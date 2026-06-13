#include <SDL/SDL.h>
#include <unistd.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <dirent.h>
#include <math.h>
#include "core.h"
#include "libpicofe/fonts.h"
#include "libpicofe/plat.h"
#include "main.h"
#include "menu.h"
#include "plat.h"
#include "scale.h"
#include "scaler_neon.h"
#include "util.h"
#include "libpicofe/in_sdl.h"

static SDL_Surface* screen;

// SF3000 raw framebuffer + input support
#ifdef PLATFORM_SF3000
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "hwdisp.h"

int sf3000_use_hwdisp = 0;

/* cubevol shared memory: ftok("/tmp/joy_key", 'a') → 4-byte key state.
   Low 16 bits = button bitmask (bit set = pressed). */
volatile uint32_t *sf3000_keys_ptr = NULL;

/* Background thread: polls cubevol and injects SDL events so menu works.
   Derives SDL keys from sf3000_keymap[] — no duplicate bit values. */
#include <pthread.h>
static pthread_t sf3000_input_thread;
extern struct sf3000_btn sf3000_keymap[];
extern const int sf3000_keymap_count;

static SDLKey sf3000_retro_to_sdlkey(int retro_id) {
    switch (retro_id) {
        case 4:  return SDLK_UP;      /* JOYPAD_UP    */
        case 5:  return SDLK_DOWN;    /* JOYPAD_DOWN  */
        case 6:  return SDLK_LEFT;    /* JOYPAD_LEFT  */
        case 7:  return SDLK_RIGHT;   /* JOYPAD_RIGHT */
        case 8:  return SDLK_SPACE;   /* JOYPAD_A → OK */
        case 0:  return SDLK_LCTRL;   /* JOYPAD_B → BACK */
        case 3:  return SDLK_ESCAPE;  /* JOYPAD_START → MENU */
        default: return SDLK_UNKNOWN;
    }
}

static void *sf3000_input_thread_fn(void *unused) {
    uint32_t prev = 0, last = 0;
    while (1) {
        usleep(16000);
        if (!sf3000_keys_ptr) continue;
        uint32_t cur = *sf3000_keys_ptr & 0xFFFF;
        /* Debounce: only act on a state that's stable for 2 polls (~32ms). Filters
         * transient cubevol shmem glitches that otherwise inject phantom presses
         * (e.g. a stray B → menu "back"). */
        if (cur != last) { last = cur; continue; }
        uint32_t changed = cur ^ prev;
        if (!changed) continue;
        for (int i = 0; i < sf3000_keymap_count; i++) {
            uint32_t bit = 1u << sf3000_keymap[i].bit;
            if (!(changed & bit)) continue;
            SDLKey k = sf3000_retro_to_sdlkey(sf3000_keymap[i].retro_id);
            if (k == SDLK_UNKNOWN) continue;
            SDL_Event ev; memset(&ev, 0, sizeof(ev));
            ev.type = (cur & bit) ? SDL_KEYDOWN : SDL_KEYUP;
            ev.key.keysym.sym = k;
            SDL_PushEvent(&ev);
        }
        prev = cur;
    }
    return NULL;
}

static void sf3000_keys_init(void) {
    key_t k = ftok("/tmp/joy_key", 'a');
    if (k == (key_t)-1) { fprintf(stderr, "SF3000 input: ftok failed\n"); return; }
    int id = shmget(k, 4, 0666);
    if (id < 0) { fprintf(stderr, "SF3000 input: shmget failed\n"); return; }
    void *p = shmat(id, NULL, 0);
    if (p == (void *)-1) { fprintf(stderr, "SF3000 input: shmat failed\n"); return; }
    sf3000_keys_ptr = (volatile uint32_t *)p;
    fprintf(stderr, "SF3000 input: cubevol shm OK, initial keys=0x%08X\n", *sf3000_keys_ptr);
    pthread_create(&sf3000_input_thread, NULL, sf3000_input_thread_fn, NULL);
}

static int sf3000_fb_fd = -1;
static uint32_t *sf3000_fb_mem = NULL;
static struct fb_var_screeninfo sf3000_vinfo;
static struct fb_fix_screeninfo sf3000_finfo;
static size_t sf3000_fb_size = 0;

// Forward declarations
int sf3000_fb_init(void);
void sf3000_fb_blit(const void *src, int width, int height, int pitch);
void sf3000_fb_finish(void);
static void sf3000_text_native(int px, int py, const char *text, uint32_t color, int page_y_offset);

#endif

// begin miyoo hardware scaling support
#ifndef PLATFORM_SF3000
// loosely based on eggs' picogpsp gfx.c
#include <mi_sys.h>
#include <mi_gfx.h>
#define	pixelsPa	unused1
#define ALIGN4K(val)	((val+4095)&(~4095))

//
//	Get GFX_ColorFmt from SDL_Surface
//
MI_GFX_ColorFmt_e	GFX_ColorFmt(SDL_Surface *surface) {
	if (surface != NULL) {
		if (surface->format->BytesPerPixel == 2) {
			if (surface->format->Amask == 0) return E_MI_GFX_FMT_RGB565;
			if (surface->format->Amask == 0x8000) return E_MI_GFX_FMT_ARGB1555;
			if (surface->format->Amask == 0xF000) {
				if (surface->format->Bmask == 0x000F) return E_MI_GFX_FMT_ARGB4444;
				return E_MI_GFX_FMT_ABGR4444;
			}
			if (surface->format->Amask == 0x000F) {
				if (surface->format->Bmask == 0x00F0) return E_MI_GFX_FMT_RGBA4444;
				return E_MI_GFX_FMT_BGRA4444;
			}
			return E_MI_GFX_FMT_RGB565;
		}
		if (surface->format->Bmask == 0x000000FF) return E_MI_GFX_FMT_ARGB8888;
		if (surface->format->Amask == 0x000000FF) {
			if (surface->format->Bmask == 0x0000FF00) return E_MI_GFX_FMT_RGBA8888;
			return E_MI_GFX_FMT_BGRA8888;
		}
		if (surface->format->Rmask == 0x000000FF) return E_MI_GFX_FMT_ABGR8888;
	}
	return E_MI_GFX_FMT_ARGB8888;
}

//
//	GFX BlitSurface (MI_GFX ver) / in place of SDL_BlitSurface
//		with scale/bpp convert and rotate/mirror
//		rotate : 1 = 90 / 2 = 180 / 3 = 270
//		mirror : 1 = Horizontal / 2 = Vertical / 3 = Both
//		nowait : 0 = wait until done / 1 = no wait
//
void GFX_BlitSurfaceExec(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect,
			 uint32_t rotate, uint32_t mirror, uint32_t nowait) {
	if ((src != NULL)&&(dst != NULL)&&(src->pixelsPa)&&(dst->pixelsPa)) {
		MI_GFX_Surface_t Src;
		MI_GFX_Surface_t Dst;
		MI_GFX_Rect_t SrcRect;
		MI_GFX_Rect_t DstRect;
		MI_GFX_Opt_t Opt;
		MI_U16 Fence;

		memset(&Opt, 0, sizeof(Opt));
		Opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
		Opt.eRotate = (MI_GFX_Rotate_e)rotate;
		Opt.eMirror = (MI_GFX_Mirror_e)mirror;

		Src.phyAddr = src->pixelsPa;
		Src.u32Width = src->w;
		Src.u32Height = src->h;
		Src.u32Stride = src->pitch;
		Src.eColorFmt = GFX_ColorFmt(src);
		if (srcrect != NULL) {
			SrcRect.s32Xpos = srcrect->x;
			SrcRect.s32Ypos = srcrect->y;
			SrcRect.u32Width = srcrect->w;
			SrcRect.u32Height = srcrect->h;
		} else {
			SrcRect.s32Xpos = 0;
			SrcRect.s32Ypos = 0;
			SrcRect.u32Width = Src.u32Width;
			SrcRect.u32Height = Src.u32Height;
		}

		Dst.phyAddr = dst->pixelsPa;
		Dst.u32Width = dst->w;
		Dst.u32Height = dst->h;
		Dst.u32Stride = dst->pitch;
		Dst.eColorFmt = GFX_ColorFmt(dst);
		if (dstrect != NULL) {
			DstRect.s32Xpos = dstrect->x;
			DstRect.s32Ypos = dstrect->y;
			if ((dstrect->w==0)||(dstrect->h==0)) {
				DstRect.u32Width = SrcRect.u32Width;
				DstRect.u32Height = SrcRect.u32Height;
			} else {
				DstRect.u32Width = dstrect->w;
				DstRect.u32Height = dstrect->h;
			}
		} else {
			DstRect.s32Xpos = 0;
			DstRect.s32Ypos = 0;
			DstRect.u32Width = Dst.u32Width;
			DstRect.u32Height = Dst.u32Height;
		}

		MI_SYS_FlushInvCache(src->pixels, ALIGN4K(src->pitch * src->h));
		MI_SYS_FlushInvCache(dst->pixels, ALIGN4K(dst->pitch * dst->h));
		MI_GFX_BitBlit(&Src, &SrcRect, &Dst, &DstRect, &Opt, &Fence);
		if (!nowait) MI_GFX_WaitAllDone(FALSE, Fence);
	} else SDL_BlitSurface(src, srcrect, dst, dstrect);
}

struct GFX_Buffer {
	MI_PHY		phyAddr;
	void*		virAddr;
	int 		width;
	int 		height;
	int 		depth;
	int 		pitch;
	uint32_t 	size;
};

typedef scale_neon_t scale_func_t;


struct GFX_Scaler {
	scale_func_t upscale;
	
	int src_w;
	int src_h;
	int src_p;
	
	int dst_w;
	int dst_h;
	int dst_p;
	
	int asp_x;
	int asp_y;
	int asp_w;
	int asp_h;
};
static struct GFX_Buffer buffer;
static struct GFX_Scaler scaler;
static SDL_Surface* scaled = NULL;

static uint16_t* dst_buf = NULL;
static size_t dst_buf_p = 0;
static void buffer_upscale(unsigned src_w, unsigned src_h, size_t src_p, const void* src,
			 unsigned dst_w, unsigned dst_h, size_t dst_p, void* dst) {
	
	unsigned scale = dst_w / src_w;
	if (scale==1) {
		memcpy(dst,src,dst_h*dst_p);
		return;
	}
	
	if (dst_p!=dst_buf_p) {
		if (dst_buf) free(dst_buf);
		dst_buf_p = dst_p;
		dst_buf = malloc(dst_p);
	}
	
	unsigned _src_w = src_w;
	unsigned _scale = scale;
	while (src_h) {
		const uint16_t* src_row = src;
		uint16_t* dst_row = dst_buf;
		while (src_w) {
			uint16_t s = *(src_row++);
			while (scale) {
				*(dst_row++) = s;
				scale -= 1;
			}
			scale = _scale;
			src_w -= 1;
		}
		src_w = _src_w;
		
		while (scale) {
			memcpy(dst, dst_buf, dst_p);
			dst += dst_p;
			scale -= 1;
		}
		scale = _scale;
		src_h -= 1;
		
		src += src_p;
	}
}

static void buffer_init(void) {
	buffer.width  = 1280;
	buffer.height =  960;
	buffer.depth  =   16;
	buffer.pitch  = buffer.width * SCREEN_BPP;
	buffer.size = buffer.pitch * buffer.height;

	if (MI_SYS_MMA_Alloc(NULL, ALIGN4K(buffer.size), &buffer.phyAddr)) return;
	MI_SYS_MemsetPa(buffer.phyAddr, 0, buffer.size);
	MI_SYS_Mmap(buffer.phyAddr, ALIGN4K(buffer.size), &buffer.virAddr, TRUE);
	
	PA_INFO("buffer_init()\n"); fflush(stdout);
	
	memset(&scaler, 0, sizeof(struct GFX_Scaler));
}
static void buffer_quit(void) {
	if (buffer.phyAddr) {
		MI_SYS_Munmap(buffer.virAddr, ALIGN4K(buffer.size));
		MI_SYS_MMA_Free(buffer.phyAddr);
	}
	if (scaled) {
		scaled->pixelsPa = 0;
		SDL_FreeSurface(scaled);
		scaled = NULL;
	}
	
	if (dst_buf) free(dst_buf);
}
static void buffer_clear(void) {
	PA_INFO("buffer_clear()\n"); fflush(stdout);
	MI_SYS_FlushInvCache(buffer.virAddr, ALIGN4K(buffer.size));
	MI_SYS_MemsetPa(buffer.phyAddr, 0, buffer.size);
}
static void buffer_renew_surface(int src_w, int src_h, int src_p) {
	if (scaled) {
		PA_INFO("freed %ix%i surface\n", scaled->w, scaled->h); fflush(stdout);
		scaled->pixelsPa = 0;
		SDL_FreeSurface(scaled);
		scaled = NULL;
	}
	
	scaler.src_w = src_w;
	scaler.src_h = src_h;
	scaler.src_p = src_p;
	
	// minimum
	// snes can barely keep up
	int sx = ceilf((float)SCREEN_WIDTH / src_w);
	int sy = ceilf((float)SCREEN_HEIGHT / src_h);
	int s = sx>sy ? sx : sy;
	
	// maximum
	// snes can't keep up (so we added max_upscale)
	// int sx = 2 * SCREEN_WIDTH / src_w;
	// int sy = 2 * SCREEN_HEIGHT / src_h;
	// int s = sx<sy ? sx : sy;
	//
	// if (s>max_upscale) s = max_upscale;
	if (s > max_upscale)
		s = max_upscale;
	if (s < 1)
		s = 1;

	switch (s) {
		case 1: scaler.upscale = scale1x_n16; break;
		case 2: scaler.upscale = scale2x_n16; break;
		case 3: scaler.upscale = scale3x_n16; break;
		case 4: scaler.upscale = scale4x_n16; break;
		case 5: scaler.upscale = scale5x_n16; break;
		case 6: scaler.upscale = scale6x_n16; break;
		// TODO: make an 8x version?
	}

	scaler.dst_w = src_w * s;
	scaler.dst_h = src_h * s;
	scaler.dst_p = scaler.dst_w * SCREEN_BPP;
	
	scaled = SDL_CreateRGBSurfaceFrom(buffer.virAddr,scaler.dst_w,scaler.dst_h,buffer.depth,scaler.dst_p,0,0,0,0);
	if (scaled != NULL) scaled->pixelsPa = buffer.phyAddr;
	PA_INFO("created %ix%i surface (%ix)\n", scaler.dst_w, scaler.dst_h, s); fflush(stdout);
	// buffer_clear();
}
static void buffer_scale(unsigned w, unsigned h, size_t pitch, const void *src) {
	bool update_asp = false;
	// static int scaler_max_upscale;
	if (w!=scaler.src_w || h!=scaler.src_h || pitch!=scaler.src_p) { //  || scaler_max_upscale!=max_upscale
		// scaler_max_upscale = max_upscale;
		buffer_renew_surface(w,h,pitch);
		update_asp = true;
	}
	
	static int scaler_mode;
	if (scaler_mode!=scale_size) {
		scaler_mode = scale_size;
		update_asp = true;
	}
	
	if (update_asp) {
		PA_INFO("update aspect ratio\n"); fflush(stdout);
		buffer_clear();
		
		if (aspect_ratio<=0) aspect_ratio = (float)scaler.src_w / scaler.src_h;
	
		if (scale_size==SCALE_SIZE_ASPECT) {
			scaler.asp_w = SCREEN_HEIGHT * aspect_ratio;
			scaler.asp_h = SCREEN_HEIGHT;
			if (scaler.asp_w>SCREEN_WIDTH) {
				scaler.asp_w = SCREEN_WIDTH;
				scaler.asp_h = SCREEN_WIDTH / aspect_ratio;
			}
		
			scaler.asp_x = (SCREEN_WIDTH - scaler.asp_w) / 2;
			scaler.asp_y = (SCREEN_HEIGHT - scaler.asp_h) / 2;
		}
	}
	
	// buffer_upscale_nn(src);
	// buffer_upscale(scaler.src_w,scaler.src_h,scaler.src_p,src,
	// 		scaler.dst_w,scaler.dst_h,scaler.dst_p,buffer.virAddr);
	scaler.upscale(src, buffer.virAddr,scaler.src_w,scaler.src_h,scaler.src_p,scaler.dst_p);
	
	if (scale_size==SCALE_SIZE_ASPECT) {
		GFX_BlitSurfaceExec(scaled, NULL, screen, &(SDL_Rect){scaler.asp_x, scaler.asp_y, scaler.asp_w, scaler.asp_h},0,0,0);
	}
	else {
		GFX_BlitSurfaceExec(scaled, NULL, screen, NULL,0,0,0);
	}
	
	// just awful
	// void* dst = screen->pixels;
	// if (scale_effect==SCALE_EFFECT_SCANLINE) {
	// 	for (int i=1; i<480; i+=2) {
	// 		memset(dst+i*SCREEN_PITCH, 0, SCREEN_PITCH);
	// 	}
	// }
}

#endif // end miyoo hardware scaling support


//begin PLATFORM_SF3000

// Declare the external event handler from in_sdl.c
//extern void in_sdl_event_handler(void *event_);

// Redirect plat_sdl_event_handler to the external handler
// Stub removed - using proper implementation below instead

struct audio_state {
	unsigned buf_w;
	unsigned max_buf_w;
	unsigned buf_r;
	size_t buf_len;
	struct audio_frame *buf;
	int in_sample_rate;
	int out_sample_rate;
};

struct audio_state audio;

static char msg[HUD_LEN];
static unsigned msg_priority = 0;
static unsigned msg_expire = 0;

// Initialize the framebuffer buffer
int buffer_init(void) {
    buffer.width = SCREEN_WIDTH;
    buffer.height = SCREEN_HEIGHT;
    buffer.depth = SCREEN_BPP;
    buffer.pitch = buffer.width * (SCREEN_BPP / 8);
    buffer.size = buffer.pitch * buffer.height;

    // Allocate memory for the buffer
    buffer.virAddr = malloc(buffer.size);
    if (!buffer.virAddr) {
        PA_ERROR("Failed to allocate memory for framebuffer\n");
        return -1;
    }

    // Clear the buffer
    memset(buffer.virAddr, 0, buffer.size);

    PA_INFO("buffer_init() completed for sf3000\n");
	return 0;
}

// Clean up the framebuffer buffer
void buffer_quit(void) {
    if (buffer.virAddr) {
        free(buffer.virAddr);
        buffer.virAddr = NULL;
    }

    buffer.width = 0;
    buffer.height = 0;
    buffer.depth = 0;
    buffer.pitch = 0;
    buffer.size = 0;

    PA_INFO("buffer_quit() completed for sf3000\n");
}

// Scale the source image to the framebuffer
int buffer_scale(int w, int h, int pitch, const void *data) {
    SDL_Surface *src_surface = SDL_CreateRGBSurfaceFrom(
        (void *)data, w, h, SCREEN_BPP, pitch,
        0x00FF0000,  // Red mask
        0x0000FF00,  // Green mask
        0x000000FF,  // Blue mask
        0xFF000000   // Alpha mask
    );
	return 0;

    if (!src_surface) {
        PA_ERROR("Failed to create source surface: %s\n", SDL_GetError());
        return -1;
    }

    SDL_Rect dst_rect = { 0, 0, buffer.width, buffer.height };
    if (SDL_BlitSurface(src_surface, NULL, screen, &dst_rect) < 0) {
        PA_ERROR("SDL_BlitSurface failed: %s\n", SDL_GetError());
		return -1;
    }

    SDL_FreeSurface(src_surface);

    if (SDL_Flip(screen) < 0) {
        PA_ERROR("SDL_Flip failed: %s\n", SDL_GetError());
		return -1;
    }
}

static void video_expire_msg(void)
{
	msg[0] = '\0';
	msg_priority = 0;
	msg_expire = 0;
}

static void video_update_msg(void)
{
	if (msg[0] && msg_expire < plat_get_ticks_ms())
		video_expire_msg();
}

static void video_clear_msg(uint16_t *dst, uint32_t h, uint32_t pitch)
{
	memset(dst + (h - 10) * pitch, 0, 10 * pitch * sizeof(uint16_t));
}

static void video_print_msg(uint16_t *dst, uint32_t h, uint32_t pitch, char *msg)
{
	basic_text_out16_nf(dst, pitch, 2, h - 10, msg);
}

static int audio_resample_nearest(struct audio_frame data) {
	static int diff = 0;
	int consumed = 0;

	if (diff < audio.out_sample_rate) {
		audio.buf[audio.buf_w++] = data;
		if (audio.buf_w >= audio.buf_len) audio.buf_w = 0;

		diff += audio.in_sample_rate;
	}

	if (diff >= audio.out_sample_rate) {
		consumed++;
		diff -= audio.out_sample_rate;
	}

	return consumed;
}

static void *fb_flip(void)
{
#ifdef PLATFORM_SF3000
	/* Only blit during menu — game blits directly in plat_video_process */
	if (screen && g_menuscreen_ptr) {
		extern void sf3000_fb_blit(const void *, int, int, int);
		sf3000_fb_blit(screen->pixels, screen->w, screen->h, screen->pitch);

	}
	return screen ? screen->pixels : NULL;
#else
	SDL_Flip(screen);
	return screen->pixels;
#endif
}

void *plat_prepare_screenshot(int *w, int *h, int *bpp)
{
	if (w) *w = SCREEN_WIDTH;
	if (h) *h = SCREEN_HEIGHT;
	if (bpp) *bpp = SCREEN_BPP;

#ifdef PLATFORM_SF3000
	return screen ? screen->pixels : NULL;
#else
	return screen->pixels;
#endif
}

int plat_dump_screen(const char *filename) {
	char imgname[MAX_PATH];
	int ret = -1;
	SDL_Surface *surface = NULL;

	snprintf(imgname, MAX_PATH, "%s.bmp", filename);

	if (g_menuscreen_ptr) {
		surface = SDL_CreateRGBSurfaceFrom(g_menubg_src_ptr,
		                                   g_menubg_src_w,
		                                   g_menubg_src_h,
		                                   16,
		                                   g_menubg_src_w * sizeof(uint16_t),
		                                   0xF800, 0x07E0, 0x001F, 0x0000);
		if (surface) {
			ret = SDL_SaveBMP(surface, imgname);
			SDL_FreeSurface(surface);
		}
	} else {
		ret = SDL_SaveBMP(screen, imgname);
	}

	return ret;
}

int plat_load_screen(const char *filename, void *buf, size_t buf_size, int *w, int *h, int *bpp) {
	int ret = -1;
	char imgname[MAX_PATH];
	SDL_Surface *imgsurface = NULL;
	SDL_Surface *surface = NULL;

	snprintf(imgname, MAX_PATH, "%s.bmp", filename);
	imgsurface = SDL_LoadBMP(imgname);
	if (!imgsurface)
		goto finish;

	/* Convert to RGB565 explicitly. SDL_DisplayFormat needs an initialised video
	 * mode (we never SetVideoMode on SF3000), so it would fail or yield 24bpp. */
	{
		SDL_PixelFormat fmt;
		memset(&fmt, 0, sizeof(fmt));
		fmt.BitsPerPixel = 16;
		fmt.BytesPerPixel = 2;
		fmt.Rmask = 0xF800; fmt.Gmask = 0x07E0; fmt.Bmask = 0x001F; fmt.Amask = 0;
		fmt.Rshift = 11; fmt.Gshift = 5; fmt.Bshift = 0;
		fmt.Rloss = 3; fmt.Gloss = 2; fmt.Bloss = 3;
		surface = SDL_ConvertSurface(imgsurface, &fmt, SDL_SWSURFACE);
	}
	if (!surface)
		goto finish;

	if (surface->w == 0 ||
	    (size_t)(surface->h * surface->pitch) > buf_size)
		goto finish;

	memcpy(buf, surface->pixels, surface->pitch * surface->h);
	*w = surface->w;
	*h = surface->h;
	*bpp = 2;

	ret = 0;

finish:
	if (imgsurface)
		SDL_FreeSurface(imgsurface);
	if (surface)
		SDL_FreeSurface(surface);
	return ret;
}


#ifdef PLATFORM_SF3000
/* Last presented frame as RGB565, captured for the menu background + the
 * per-savestate screenshot (the HW path never writes screen->pixels on SF3000). */
static const uint16_t *g_last565 = NULL;
static int g_last565_w = 0, g_last565_h = 0, g_last565_pitch = 0; /* pitch in bytes */
/* Nearest-scale the last presented game frame into the menu-bg buffer so the menu
 * background + saved screenshot (plat_dump_screen reads g_menubg_src_ptr) show the
 * game. The HW path never writes screen->pixels here, so we capture g_last565. */
static void sf3000_capture_menubg(void)
{
	uint16_t *dst = (uint16_t *)g_menubg_src_ptr;
	int dw = g_menubg_src_w, dh = g_menubg_src_h;
	if (!dst) return;
	/* clear to black first so unfilled bars match the game's letterbox */
	memset(dst, 0, (size_t)dh * g_menubg_src_pp * sizeof(uint16_t));
	if (!g_last565 || g_last565_w <= 0 || g_last565_h <= 0)
		return;
	int sw = g_last565_w, sh = g_last565_h, sp = g_last565_pitch / 2;
	/* Match how the game was displayed: FULL stretches to fill; every other
	 * mode keeps the game's aspect (letterbox/pillarbox) so the menu background
	 * lines up with the last gameplay frame for a smooth transition. */
	int ox = 0, oy = 0, ow = dw, oh = dh;
	if (scale_size != SCALE_SIZE_FULL) {
		if ((long)dw * sh > (long)dh * sw) { oh = dh; ow = (int)((long)sw * dh / sh); }
		else                               { ow = dw; oh = (int)((long)sh * dw / sw); }
		if (ow > dw) ow = dw;
		if (oh > dh) oh = dh;
		ox = (dw - ow) / 2; oy = (dh - oh) / 2;
	}
	for (int y = 0; y < oh; y++) {
		int sy = y * sh / oh;
		const uint16_t *srow = g_last565 + (size_t)sy * sp;
		uint16_t *drow = dst + (size_t)(oy + y) * g_menubg_src_pp + ox;
		for (int x = 0; x < ow; x++)
			drow[x] = srow[x * sw / ow];
	}
}
#endif

void plat_video_menu_enter(int is_rom_loaded)
{
	SDL_LockSurface(screen);
#ifdef PLATFORM_SF3000
	sf3000_capture_menubg();
	memset(screen->pixels, 0, screen->h * screen->pitch);
#else
	memcpy(g_menubg_src_ptr, screen->pixels, g_menubg_src_h * g_menubg_src_pp * sizeof(uint16_t));
#endif
	SDL_UnlockSurface(screen);
	g_menuscreen_ptr = fb_flip();
}

void plat_video_menu_begin(void)
{
	SDL_LockSurface(screen);
	menu_begin();
}

void plat_video_menu_end(void)
{
	menu_end();
	SDL_UnlockSurface(screen);
	g_menuscreen_ptr = fb_flip();
}

void plat_video_menu_leave(void)
{
	memset(g_menubg_src_ptr, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));

	SDL_LockSurface(screen);
	memset(screen->pixels, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));
	SDL_UnlockSurface(screen);
	fb_flip();
	SDL_LockSurface(screen);
	memset(screen->pixels, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));
	SDL_UnlockSurface(screen);

	g_menuscreen_ptr = NULL;
}

void plat_video_open(void)
{
}

void plat_video_set_msg(const char *new_msg, unsigned priority, unsigned msec)
{
	if (!new_msg) {
		video_expire_msg();
	} else if (priority >= msg_priority) {
		snprintf(msg, HUD_LEN, "%s", new_msg);
		string_truncate(msg, HUD_LEN - 1);
		msg_priority = priority;
		msg_expire = plat_get_ticks_ms() + msec;
	}
}


static SDL_Surface* clean_screen = NULL;
static const void* framebuffer; // NOTE: we don't own this
static int g_game_w = 256, g_game_h = 224; /* actual game dimensions from core */
void* plat_clean_screen(void) {
	return scale_clean(framebuffer, clean_screen->pixels) ? clean_screen : NULL;
}

void plat_video_process(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!data) return;
	framebuffer = data;

	static int had_msg = 0;
	SDL_LockSurface(screen);

	if (had_msg) {
		video_clear_msg(screen->pixels, screen->h, screen->pitch / (SCREEN_BPP / 8));
		had_msg = 0;
	}

#ifdef PLATFORM_SF3000
	extern int current_pixel_format;
	if (current_pixel_format != RETRO_PIXEL_FORMAT_XRGB8888) {
		/* RGB565 / 0RGB1555: blit directly to fb0 (pitch may be wider than width*2). */
		g_game_w = (int)width; g_game_h = (int)height;
		g_last565 = (const uint16_t *)data; g_last565_w = (int)width;
		g_last565_h = (int)height; g_last565_pitch = (int)pitch;
		SDL_UnlockSurface(screen);
		video_update_msg();
		sf3000_fb_blit(data, (int)width, (int)height, (int)pitch);
		return;
	}

	if (current_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
		/* RGB8888 path: convert to a core-sized RGB565 buffer, then feed the
		 * SAME blit path as native RGB565 cores so hwdisp scales core→panel
		 * (honouring scale modes). The old code center-blitted 1:1 into a
		 * panel-sized buffer, which cropped cores wider than the panel and
		 * defeated scaling. */
		static uint16_t *conv_buf = NULL;
		static size_t conv_cap = 0;
		size_t need = (size_t)width * height;
		if (need > conv_cap) {
			free(conv_buf);
			conv_cap = need + 4096;
			conv_buf = (uint16_t *)malloc(conv_cap * sizeof(uint16_t));
		}
		if (conv_buf) {
			const uint32_t *src32 = (const uint32_t *)data;
			for (unsigned y = 0; y < height; y++) {
				const uint32_t *row = src32 + y * (pitch / 4);
				uint16_t *dst = conv_buf + (size_t)y * width;
				for (unsigned x = 0; x < width; x++) {
					uint32_t p = row[x];
					uint8_t r = (p >> 16) & 0xFF;
					uint8_t g = (p >>  8) & 0xFF;
					uint8_t b = (p)       & 0xFF;
					dst[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
				}
			}
		}
		g_game_w = (int)width; g_game_h = (int)height;
		if (conv_buf) {
			g_last565 = conv_buf; g_last565_w = (int)width;
			g_last565_h = (int)height; g_last565_pitch = (int)width * 2;
		}
		SDL_UnlockSurface(screen);
		video_update_msg();
		if (conv_buf)
			sf3000_fb_blit(conv_buf, (int)width, (int)height, (int)width * 2);
		return;
	}
#endif

	if (scale_size==SCALE_SIZE_NONE) {
		scale(width, height, pitch, data, screen->pixels);
	}
	else {
		buffer_scale(width, height, pitch, data);
	}

	if (msg[0]) {
		video_print_msg(screen->pixels, screen->h, screen->pitch / (SCREEN_BPP / 8), msg);
		had_msg = 1;
	}

	SDL_UnlockSurface(screen);

	video_update_msg();
}

void plat_video_flip(void)
{
	fb_flip();
}

void plat_video_close(void)
{
	
}

unsigned plat_cpu_ticks(void)
{
	long unsigned ticks = 0;
	long ticksps = 0;
	FILE *file = NULL;

	file = fopen("/proc/self/stat", "r");
	if (!file)
		goto finish;

	if (!fscanf(file, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu", &ticks))
		goto finish;

	ticksps = sysconf(_SC_CLK_TCK);

	if (ticksps)
		ticks = ticks * 100 / ticksps;

finish:
	if (file)
		fclose(file);

	return ticks;
}

static void plat_sound_callback(void *unused, uint8_t *stream, int len)
{
	int16_t *p = (int16_t *)stream;
	if (audio.buf_len == 0)
		return;

	len /= (sizeof(int16_t) * 2);

	while (audio.buf_r != audio.buf_w && len > 0) {
		*p++ = audio.buf[audio.buf_r].left;
		*p++ = audio.buf[audio.buf_r].right;
		audio.max_buf_w = audio.buf_r;

		len--;
		audio.buf_r++;

		if (audio.buf_r >= audio.buf_len) audio.buf_r = 0;
	}

	while(len > 0) {
		*p++ = 0;
		--len;
	}
}

#ifdef PLATFORM_SF3000
static void *sf3000_sound_handle = NULL;
static int (*sf3000_sound_driver_init)(void *device_name, int sample_rate, int channels) = NULL;
static int (*sf3000_sound_driver_playframe)(const void *buffer, int bytes) = NULL;
static int (*sf3000_sound_driver_deinit)(void) = NULL;

/* Non-blocking audio: a dedicated consumer thread owns the *blocking*
 * sound_driver_playframe() DAC write. The emu thread only enqueues into this
 * SPSC ring and never blocks, so audio over/underrun can never freeze the
 * emulator. Video stays the sole frame pacer. */
#define SF3000_ARING_FRAMES 4096          /* power of two, ~93ms @ 44100Hz */
#define SF3000_ARING_MASK   (SF3000_ARING_FRAMES - 1)
#define SF3000_ACHUNK       735           /* 44100/60, one video frame's audio */

static struct audio_frame sf3000_aring[SF3000_ARING_FRAMES];
static unsigned           sf3000_aring_w = 0;   /* producer: emu thread   */
static unsigned           sf3000_aring_r = 0;   /* consumer: audio thread */
static pthread_mutex_t    sf3000_aring_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t          sf3000_audio_thread;
static volatile int       sf3000_audio_running = 0;

static void *sf3000_audio_thread_fn(void *unused)
{
	(void)unused;
	struct audio_frame chunk[SF3000_ACHUNK];

	while (sf3000_audio_running) {
		int have_chunk = 0;

		pthread_mutex_lock(&sf3000_aring_mtx);
		unsigned avail = sf3000_aring_w - sf3000_aring_r;  /* free-running */
		if (avail >= SF3000_ACHUNK) {
			for (int i = 0; i < SF3000_ACHUNK; i++) {
				chunk[i] = sf3000_aring[sf3000_aring_r & SF3000_ARING_MASK];
				sf3000_aring_r++;
			}
			have_chunk = 1;
		}
		pthread_mutex_unlock(&sf3000_aring_mtx);

		if (!have_chunk) {
			usleep(2000);  /* underrun: wait for the emu thread to refill */
			continue;
		}

		/* Blocks until the DAC drains — but on THIS thread, not the emu. */
		if (sf3000_sound_driver_playframe)
			sf3000_sound_driver_playframe(chunk, SF3000_ACHUNK);
	}
	return NULL;
}
#endif

static void plat_sound_finish(void)
{
#ifdef PLATFORM_SF3000
	if (sf3000_audio_running) {
		sf3000_audio_running = 0;
		pthread_join(sf3000_audio_thread, NULL);
	}
	if (sf3000_sound_driver_deinit) {
		sf3000_sound_driver_deinit();
	}
	if (sf3000_sound_handle) {
		dlclose(sf3000_sound_handle);
		sf3000_sound_handle = NULL;
	}
#else
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	if (audio.buf) {
		free(audio.buf);
		audio.buf = NULL;
	}
#endif
}

const char *sf3000_driver_path(void);

static int plat_sound_init(void)
{
#ifdef PLATFORM_SF3000
	if (!sf3000_sound_handle) {
		sf3000_sound_handle = dlopen(sf3000_driver_path(), RTLD_LAZY);
		if (!sf3000_sound_handle)   /* fall back to generic driver.so */
			sf3000_sound_handle = dlopen("/mnt/sdcard/cubegm/driver.so", RTLD_LAZY);
		if (!sf3000_sound_handle) {
			PA_ERROR("SF3000: Failed to load driver.so for audio: %s\n", dlerror());
			return -1;
		}

		sf3000_sound_driver_init = dlsym(sf3000_sound_handle, "sound_driver_init");
		sf3000_sound_driver_playframe = dlsym(sf3000_sound_handle, "sound_driver_playframe");
		sf3000_sound_driver_deinit = dlsym(sf3000_sound_handle, "sound_driver_deinit");

		if (!sf3000_sound_driver_init || !sf3000_sound_driver_playframe) {
			PA_ERROR("SF3000: Missing audio driver functions in driver.so\n");
			plat_sound_finish();
			return -1;
		}
	}

	if (sf3000_sound_driver_init(NULL, SAMPLE_RATE, 2) < 0) {
		PA_ERROR("SF3000: sound_driver_init failed\n");
		return -1;
	}

	audio.in_sample_rate = sample_rate;
	audio.out_sample_rate = SAMPLE_RATE;

	/* Start the non-blocking audio consumer thread. */
	sf3000_aring_w = sf3000_aring_r = 0;
	if (!sf3000_audio_running) {
		sf3000_audio_running = 1;
		if (pthread_create(&sf3000_audio_thread, NULL,
		                   sf3000_audio_thread_fn, NULL) != 0) {
			sf3000_audio_running = 0;
			PA_ERROR("SF3000: failed to start audio thread\n");
			return -1;
		}
	}

	PA_INFO("SF3000: Proprietary audio driver initialized at %d Hz\n", SAMPLE_RATE);
	return 0;
#else
	if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		return -1;
	}

	SDL_AudioSpec spec, received;

	spec.freq = SAMPLE_RATE;
	spec.format = AUDIO_S16;
	spec.channels = 2;
	spec.samples = 512;
	spec.callback = plat_sound_callback;

	if (SDL_OpenAudio(&spec, &received) < 0) {
		plat_sound_finish();
		return -1;
	}

	audio.in_sample_rate = sample_rate;
	audio.out_sample_rate = received.freq;
	plat_sound_resize_buffer();

	SDL_PauseAudio(0);
	return 0;
#endif
}

float plat_sound_capacity(void)
{
#ifdef PLATFORM_SF3000
	return 1.0;
#else
	int buffered = 0;
	if (audio.buf_len == 0)
		return 1.0;

	if (audio.buf_w != audio.buf_r) {
		buffered = audio.buf_w > audio.buf_r ?
			audio.buf_w - audio.buf_r :
			(audio.buf_w + audio.buf_len) - audio.buf_r;
	}

	return 1.0 - (float)buffered / audio.buf_len;
#endif
}

#define BATCH_SIZE 100
void plat_sound_write(const struct audio_frame *data, int frames)
{
#ifdef PLATFORM_SF3000
	/* Non-blocking enqueue: hand frames to the consumer thread and return
	 * immediately. On overrun we drop the excess rather than block the emu. */
	if (sf3000_audio_running) {
		pthread_mutex_lock(&sf3000_aring_mtx);
		for (int i = 0; i < frames; i++) {
			if (sf3000_aring_w - sf3000_aring_r >= SF3000_ARING_FRAMES)
				break;  /* ring full — drop remaining frames */
			sf3000_aring[sf3000_aring_w & SF3000_ARING_MASK] = data[i];
			sf3000_aring_w++;
		}
		pthread_mutex_unlock(&sf3000_aring_mtx);
	}
#else
	int consumed = 0;
	if (audio.buf_len == 0)
		return;

	SDL_LockAudio();

	while (frames > 0) {
		int tries = 0;
		int amount = MIN(BATCH_SIZE, frames);

		while (tries < 10 && audio.buf_w == audio.max_buf_w) {
			tries++;
			SDL_UnlockAudio();

			if (!limit_frames)
				return;

			SDL_Delay(1);
			SDL_LockAudio();
		}

		while (amount && audio.buf_w != audio.max_buf_w) {
			consumed = audio_resample_nearest(*data);
			data += consumed;
			amount -= consumed;
			frames -= consumed;
		}
	}
	SDL_UnlockAudio();
#endif
}

void plat_sound_resize_buffer(void) {
#ifdef PLATFORM_SF3000
	return;
#else
	size_t buf_size;
	SDL_LockAudio();

	audio.buf_len = frame_rate > 0
		? current_audio_buffer_size * audio.in_sample_rate / frame_rate
		: 0;

	if (audio.buf_len == 0) {
		SDL_UnlockAudio();
		return;
	}

	buf_size = audio.buf_len * sizeof(struct audio_frame);
	audio.buf = realloc(audio.buf, buf_size);

	if (!audio.buf) {
		SDL_UnlockAudio();
		PA_ERROR("Error initializing sound buffer\n");
		plat_sound_finish();
		return;
	}

	memset(audio.buf, 0, buf_size);
	audio.buf_w = 0;
	audio.buf_r = 0;
	audio.max_buf_w = audio.buf_len - 1;
	SDL_UnlockAudio();
#endif
}

void plat_sdl_event_handler(void *event_)
{
    SDL_Event *event = (SDL_Event *)event_;

    switch (event->type) {
        case SDL_QUIT:
            exit(0);
            break;

        default:
            break;
    }
}

int plat_init(void)
{
#ifdef PLATFORM_SF3000
    setenv("SDL_NOMOUSE", "1", 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        PA_ERROR("SF3000 SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    // Wire display controller to fb0 AFTER SDL_Init so SDL can't reset it
    extern int sf3000_fb_init(void);
    if (sf3000_fb_init() < 0) {
        PA_ERROR("sf3000_fb_init failed\n");
        return -1;
    }
    /* Off-screen staging buffer only — do NOT call SDL_SetVideoMode,
       it changes fb0 resolution away from 720x1280 and breaks our mmap writes. */
    screen = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_WIDTH, SCREEN_HEIGHT, 16,
                                  0xF800, 0x07E0, 0x001F, 0x0000);
    if (!screen) {
        PA_ERROR("SF3000 SDL_CreateRGBSurface failed: %s\n", SDL_GetError());
        return -1;
    }

    fprintf(stderr, "SDL surface: %dx%d bpp=%d pitch=%d\n",
            screen->w, screen->h, screen->format->BitsPerPixel, screen->pitch);
    PA_INFO("SF3000: fb0 + SDL dummy surface ready (%dx%d)\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    SDL_ShowCursor(0);

    g_menuscreen_w  = SCREEN_WIDTH;
    g_menuscreen_h  = SCREEN_HEIGHT;
    g_menuscreen_pp = SCREEN_WIDTH;
    g_menuscreen_ptr = NULL;
    g_menubg_src_w  = SCREEN_WIDTH;
    g_menubg_src_h  = SCREEN_HEIGHT;
    g_menubg_src_pp = SCREEN_WIDTH;

    if (in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler)) {
        PA_ERROR("SF3000 SDL input failed to init: %s\n", SDL_GetError());
        return -1;
    }
    in_probe();

    if (plat_sound_init()) {
        PA_ERROR("SF3000 SDL sound failed to init (continuing without audio): %s\n", SDL_GetError());
    }

    sf3000_keys_init();
    extern void sf3000_calibrate_input(void);
    extern void sf3000_load_keymap(void);
    /* Load saved mapping if exists, else run one-time calibration */
    {
        /* Keymap is hardcoded and confirmed. Write file so calibration
           never runs. Delete /mnt/sdcard/sf3000_keymap.txt to recalibrate. */
        {
            static const char *_n[] = {
                "UP","DOWN","LEFT","RIGHT","A","B","X","Y",
                "L","R","L2","R2","SELECT","START" };
            FILE *_kf = fopen("/mnt/sdcard/sf3000_keymap.txt", "r");
            if (_kf) { fclose(_kf); sf3000_load_keymap(); }
            else {
                FILE *_wf = fopen("/mnt/sdcard/sf3000_keymap.txt", "w");
                if (_wf) {
                    for (int _i = 0; _i < sf3000_keymap_count; _i++)
                        fprintf(_wf, "%s %d\n", _n[_i], sf3000_keymap[_i].bit);
                    fclose(_wf);
                }
            }
        }
    }

    return 0;
#else
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        PA_ERROR("%s, SDL_Init failed: %s\n", __func__, SDL_GetError());
        return -1;
    }
	screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 0, SDL_SWSURFACE | SDL_DOUBLEBUF);
	PA_INFO("Using Framebuffer: %s\n", getenv("SDL_FBDEV"));
	if (screen == NULL) {
		PA_ERROR("%s, failed to set video mode\n", __func__, SDL_GetError());
		return -1;
	}
	
	clean_screen = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_WIDTH, SCREEN_HEIGHT, 0,
                                    0x00FF0000,  // Red mask
                                    0x0000FF00,  // Green mask
                                    0x000000FF,  // Blue mask
                                    0xFF000000); // Alpha mask
	if (clean_screen == NULL) {
		PA_ERROR("%s, SDL_CreateRGBSurface failed: %s\n", __func__, SDL_GetError());
		return -1;
	}
	
	buffer_init();
	
	SDL_ShowCursor(0);

	g_menuscreen_w = SCREEN_WIDTH;
	g_menuscreen_h = SCREEN_HEIGHT;
	g_menuscreen_pp = SCREEN_WIDTH;
	g_menuscreen_ptr = NULL;
	g_menubg_src_w = SCREEN_WIDTH;
	g_menubg_src_h = SCREEN_HEIGHT;
	g_menubg_src_pp = SCREEN_WIDTH;

	if (in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler)) {
		PA_ERROR("SDL input failed to init: %s\n", SDL_GetError());
		return -1;
	}
	in_probe();

	if (plat_sound_init()) {
		PA_ERROR("SDL sound failed to init: %s\n", SDL_GetError());
		return -1;
	}
	printf("Framebuffer resolution: %dx%d, color depth: %d bpp\n", SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_BPP * 8);
	return 0;
#endif // PLATFORM_SF3000
}

int plat_reinit(void)
{
#ifdef PLATFORM_SF3000
	if (sf3000_sound_driver_deinit) {
		sf3000_sound_driver_deinit();
	}
	if (sf3000_sound_driver_init) {
		sf3000_sound_driver_init(NULL, sample_rate, 2);
	}
	audio.in_sample_rate = sample_rate;
	audio.out_sample_rate = sample_rate;
#else
	audio.in_sample_rate = sample_rate;
	plat_sound_resize_buffer();
#endif
	scale_update_scaler();
	return 0;
}

void plat_finish(void)
{
#ifdef PLATFORM_SF3000
	extern void sf3000_fb_finish(void);
	plat_sound_finish();
	sf3000_fb_finish();
	SDL_FreeSurface(screen);
	screen = NULL;
	SDL_Quit();
#else
	plat_sound_finish();
	buffer_quit();
	SDL_FreeSurface(clean_screen);
	SDL_FreeSurface(screen);
	screen = NULL;
	SDL_Quit();
#endif
}

// SF3000 framebuffer implementation
#ifdef PLATFORM_SF3000

/* Phase-A diagnostic: dump state of fb0/fb6/fb22 at any moment so we can
 * see which layers driver.so leaves enabled and whether display routing
 * stays on fb0 or shifts to a HW layer. */
void sf3000_dump_fb_state(const char *tag) {
    const int devs[] = {0, 6, 22};
    for (size_t i = 0; i < sizeof(devs)/sizeof(devs[0]); i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/fb%d", devs[i]);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            DBG("DBG dump[%s] %s: open failed (errno=%d)\n", tag, path, errno);
            continue;
        }
        struct fb_var_screeninfo v;
        struct fb_fix_screeninfo f;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 &&
            ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0) {
            DBG("DBG dump[%s] %s: %ux%u(v%ux%u) bpp=%u off=%u,%u rot=%u "
                            "line=%u smem_start=0x%lx smem_len=%u type=%u visual=%u\n",
                    tag, path,
                    v.xres, v.yres, v.xres_virtual, v.yres_virtual,
                    v.bits_per_pixel, v.xoffset, v.yoffset, v.rotate,
                    f.line_length, (unsigned long)f.smem_start, f.smem_len,
                    f.type, f.visual);
        } else {
            DBG("DBG dump[%s] %s: ioctl failed (errno=%d)\n", tag, path, errno);
        }
        close(fd);
    }
}

int sf3000_fb_init(void) {
    /* Detect device + export panel geometry to env before the core's retro_init
     * reads it (single binary → SF3000 854x480 or R36SX 640x480). */
    { extern void sf3000_detect_device(void); sf3000_detect_device(); }
    /* Write per-process init log to dedicated file with fsync, so it
     * survives even when stderr buffer is lost on power-cycle. */
    int initlog = open("/mnt/sdcard/picoarch_init.log",
                       O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (initlog >= 0) {
        dprintf(initlog, "\n=== picoarch init pid=%d ppid=%d ===\n",
                getpid(), getppid());
    }
    /* Dump kernel-side fb/display info once on each startup so we can see
     * what devices exist, what panel state the kernel reports, etc. */
    {
        const char *paths[] = {"/proc/fb", "/proc/iomem", "/proc/interrupts", NULL};
        for (int i = 0; paths[i]; i++) {
            FILE *pf = fopen(paths[i], "r");
            if (!pf) { DBG("DBG %s: open failed\n", paths[i]); continue; }
            char buf[512];
            int line_count = 0;
            while (fgets(buf, sizeof(buf), pf) && line_count++ < 40) {
                size_t n = strlen(buf);
                if (n && buf[n-1] == '\n') buf[n-1] = 0;
                /* Only log lines that look relevant (fb / disp / panel / hcge). */
                if (strstr(buf, "fb") || strstr(buf, "isp") ||
                    strstr(buf, "anel") || strstr(buf, "cge") ||
                    strstr(buf, "lcd") || strstr(buf, "/dev/fb") ||
                    i == 0) /* /proc/fb is small, dump fully */
                    DBG("DBG %s: %s\n", paths[i], buf);
            }
            fclose(pf);
        }
    }
    /* Wire display controller to fb0 — must happen AFTER SDL_Init so SDL
       can't reset this connection. Extracted from fbdev_init() disassembly. */
    int dis = open("/dev/dis", O_RDWR);
    if (dis >= 0) {
        struct { int a, b, c; } buf = {1, 0, 0};
        int r = ioctl(dis, 0xc00c0e0c, &buf);
        fprintf(stderr, "SF3000: /dev/dis ioctl ret=%d\n", r);
        close(dis);
    } else {
        fprintf(stderr, "SF3000: /dev/dis open failed\n");
    }

    // Open framebuffer (720x1280, 32-bit ARGB)
    sf3000_fb_fd = open("/dev/fb0", O_RDWR);
    if (sf3000_fb_fd < 0) return -1;

    ioctl(sf3000_fb_fd, FBIOGET_VSCREENINFO, &sf3000_vinfo);
    DBG("DBG sf3000_fb_init: PRE-FBIOPUT %ux%u(v%ux%u) bpp=%u off=%u,%u\n",
            sf3000_vinfo.xres, sf3000_vinfo.yres,
            sf3000_vinfo.xres_virtual, sf3000_vinfo.yres_virtual,
            sf3000_vinfo.bits_per_pixel,
            sf3000_vinfo.xoffset, sf3000_vinfo.yoffset);
    if (initlog >= 0) {
        dprintf(initlog, "PRE-FBIOPUT %ux%u(v%ux%u) bpp=%u off=%u,%u rot=%u\n",
                sf3000_vinfo.xres, sf3000_vinfo.yres,
                sf3000_vinfo.xres_virtual, sf3000_vinfo.yres_virtual,
                sf3000_vinfo.bits_per_pixel,
                sf3000_vinfo.xoffset, sf3000_vinfo.yoffset,
                sf3000_vinfo.rotate);
    }
    /* Always FBIOPUT + re-assert /dev/dis.  Even when geometry looks correct
     * after hwdisp_deinit, the display controller can still be in driver.so
     * state.  Skipping FBIOPUT in that case → FrogUI shows squished/offset
     * frames.  Cheap to run unconditionally; safe for every entry path. */
    sf3000_vinfo.xres         = 720;
    sf3000_vinfo.yres         = 1280;
    sf3000_vinfo.xres_virtual = 720;
    sf3000_vinfo.yres_virtual = 2560; /* 2 pages; fits in HCGE-leftover smem */
    sf3000_vinfo.xoffset      = 0;
    sf3000_vinfo.yoffset      = 0;
    sf3000_vinfo.bits_per_pixel = 32;
    sf3000_vinfo.rotate       = 0;   /* force panel back to portrait */
    sf3000_vinfo.red    = (struct fb_bitfield){16, 8, 0};
    sf3000_vinfo.green  = (struct fb_bitfield){ 8, 8, 0};
    sf3000_vinfo.blue   = (struct fb_bitfield){ 0, 8, 0};
    sf3000_vinfo.transp = (struct fb_bitfield){24, 8, 0};
    int r2 = ioctl(sf3000_fb_fd, FBIOPUT_VSCREENINFO, &sf3000_vinfo);
    fprintf(stderr, "sf3000_fb_init: FBIOPUT ret=%d\n", r2);
    if (initlog >= 0) dprintf(initlog, "FBIOPUT ret=%d errno=%d\n", r2, errno);
    /* FBIOPUT sets metadata but does NOT pan; driver.so may have left
     * yoffset at a large value (e.g. 3360) which keeps the display scanning
     * out the wrong region (squished/offset frames).  Force pan to (0,0). */
    sf3000_vinfo.xoffset = 0;
    sf3000_vinfo.yoffset = 0;
    int rp = ioctl(sf3000_fb_fd, FBIOPAN_DISPLAY, &sf3000_vinfo);
    DBG("DBG sf3000_fb_init: FBIOPAN_DISPLAY(0,0) ret=%d\n", rp);
    /* Match driver.so's fbdev_init blank sequence: BLANK_NORMAL then
     * UNBLANK. Forces panel to re-latch geometry from FBIOPUT. */
    int rb1 = ioctl(sf3000_fb_fd, FBIOBLANK, 1);
    int rb2 = ioctl(sf3000_fb_fd, FBIOBLANK, 0);
    DBG("DBG sf3000_fb_init: FBIOBLANK(1)=%d (0)=%d\n", rb1, rb2);
    if (initlog >= 0)
        dprintf(initlog, "FBIOBLANK(1)=%d (0)=%d errno=%d\n", rb1, rb2, errno);
    ioctl(sf3000_fb_fd, FBIOGET_VSCREENINFO, &sf3000_vinfo);
    sf3000_dump_fb_state("fb_init/post");
    if (initlog >= 0) {
        dprintf(initlog, "POST-FBIOPUT %ux%u(v%ux%u) bpp=%u off=%u,%u rot=%u\n",
                sf3000_vinfo.xres, sf3000_vinfo.yres,
                sf3000_vinfo.xres_virtual, sf3000_vinfo.yres_virtual,
                sf3000_vinfo.bits_per_pixel,
                sf3000_vinfo.xoffset, sf3000_vinfo.yoffset,
                sf3000_vinfo.rotate);
        fsync(initlog);
        close(initlog);
    }
    ioctl(sf3000_fb_fd, FBIOGET_FSCREENINFO, &sf3000_finfo);

    sf3000_fb_size = sf3000_finfo.smem_len;
    sf3000_fb_mem = mmap(NULL, sf3000_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, sf3000_fb_fd, 0);
    if (sf3000_fb_mem == MAP_FAILED) {
        close(sf3000_fb_fd);
        sf3000_fb_fd = -1;
        return -1;
    }

    fprintf(stderr, "SF3000 fb0: %ux%u (virtual %ux%u) bpp=%u line=%u smem=%u mem=%p\n",
            sf3000_vinfo.xres, sf3000_vinfo.yres,
            sf3000_vinfo.xres_virtual, sf3000_vinfo.yres_virtual,
            sf3000_vinfo.bits_per_pixel,
            sf3000_finfo.line_length, sf3000_finfo.smem_len,
            (void *)sf3000_fb_mem);
    fprintf(stderr, "SF3000 fb0: xoffset=%u yoffset=%u rotate=%u type=%u visual=%u\n",
            sf3000_vinfo.xoffset, sf3000_vinfo.yoffset, sf3000_vinfo.rotate,
            sf3000_finfo.type, sf3000_finfo.visual);
    fprintf(stderr, "SF3000 fb0: pixel r=%u/%u g=%u/%u b=%u/%u a=%u/%u\n",
            sf3000_vinfo.red.offset,   sf3000_vinfo.red.length,
            sf3000_vinfo.green.offset, sf3000_vinfo.green.length,
            sf3000_vinfo.blue.offset,  sf3000_vinfo.blue.length,
            sf3000_vinfo.transp.offset, sf3000_vinfo.transp.length);

    /* Write test pixel, read it back to verify mmap works */
    uint32_t old = sf3000_fb_mem[0];
    sf3000_fb_mem[0] = 0xDEADBEEF;
    uint32_t rb = sf3000_fb_mem[0];
    sf3000_fb_mem[0] = old;
    fprintf(stderr, "SF3000 fb0: write-readback test: wrote 0xDEADBEEF got 0x%08X (%s)\n",
            rb, rb == 0xDEADBEEF ? "OK" : "FAIL - mmap not writable?");

    /* Clear to opaque black once — blit overwrites entirely each frame */
    for (size_t i = 0; i < sf3000_fb_size / 4; i++)
        sf3000_fb_mem[i] = 0xFF000000u;

    sf3000_use_hwdisp = 0;

    /* WARM-BOOT: previous picoarch left HCGE active (game in bilinear).
     * Marker contains parent PID; we early-init hwdisp here so FrogUI
     * frames present through driver.so instead of squishing on SW path. */
    {
        FILE *mf = fopen("/tmp/picoarch_hcge_was_active", "r");
        if (mf) {
            int marker_ppid = -1;
            (void)!fscanf(mf, "%d", &marker_ppid);
            fclose(mf);
            unlink("/tmp/picoarch_hcge_was_active");
            if (marker_ppid == (int)getppid()) {
                extern int hwdisp_init(void);
                if (hwdisp_init() == 0) {
                    sf3000_use_hwdisp = 1;
                    fprintf(stderr, "sf3000_fb_init: hwdisp early-init (warm boot)\n");
                }
            } else {
                fprintf(stderr, "sf3000_fb_init: stale marker (ppid %d != %d) — ignored\n",
                        marker_ppid, (int)getppid());
            }
        }
    }
    return 0;
}

static int sf3000_last_gh = -1;
static int sf3000_current_page = 0;
static uint16_t *sf3000_rotated_src = NULL;
static int sf3000_rotated_capacity = 0;

struct sf3000_blend_data {
    int16_t gy1, gy2;
    uint8_t w1, w2;
};
static struct sf3000_blend_data sf3000_blend_lut[1024];

static inline uint32_t cvt565(uint16_t c) {
    return 0xFF000000u |
           (((c & 0xF800) << 8) | ((c & 0xE000) << 3)) |
           (((c & 0x07E0) << 5) | ((c & 0x0600) >> 1)) |
           (((c & 0x001F) << 3) | ((c & 0x001C) >> 2));
}

static void sf3000_frame_limit(void) {
    extern int g_ff_level;            /* 0=off,1=2x,2=3x */
    long long target = 16666 / (g_ff_level + 1);   /* pace emulation faster on FF */
    static struct timeval last_tv = {0, 0};
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (last_tv.tv_sec != 0) {
        long long diff_us = (tv.tv_sec - last_tv.tv_sec) * 1000000LL + (tv.tv_usec - last_tv.tv_usec);
        if (diff_us > 0 && diff_us < target) {
            long long sleep_us = target - diff_us;
            if (sleep_us > 2000)
                usleep(sleep_us - 2000);
            while (1) {
                gettimeofday(&tv, NULL);
                diff_us = (tv.tv_sec - last_tv.tv_sec) * 1000000LL + (tv.tv_usec - last_tv.tv_usec);
                if (diff_us >= target) break;
            }
        }
    }
    last_tv = tv;
}

/* Runtime device + panel detection (single binary supports both):
 *   SF3000 — 854x480 16:9, driver_sf3000.so, present via video_driver_disp_frame
 *   R36SX  — 640x480 4:3,  driver_r36sx.so,  present via direct fb0 write
 * Detected once from the device-tree panel node (R36SX panel chip = r63311).
 * Panel dims + UI scale are exported via env (TF_PANEL_W/H/UI_SCALE) so the
 * FrogUI libretro core renders at the right resolution. */
extern void dbg_log(const char *fmt, ...);
static int g_dev_r36sx = -1;   /* -1 = undetected, 0 = SF3000, 1 = R36SX */
static int g_dev_w = 854, g_dev_h = 480, g_dev_scale = 150;

/* Recursively scan /proc/device-tree for the R36SX panel node (r63311), without
 * forking — popen/fork inside the emulator process can disturb later dynarec
 * memory mappings (gpsp). Bounded depth. Sets *found if seen. */
static void dt_scan_r63311(const char *dir, int depth, int *found) {
    if (*found || depth > 5) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[512];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strstr(e->d_name, "r63311")) { *found = 1; break; }
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        /* recurse into subdirs only */
        DIR *sub = opendir(path);
        if (sub) { closedir(sub); dt_scan_r63311(path, depth + 1, found); if (*found) break; }
    }
    closedir(d);
}

void sf3000_detect_device(void) {
    if (g_dev_r36sx != -1) return;
    int found = 0;
    dt_scan_r63311("/proc/device-tree", 0, &found);
    g_dev_r36sx = found;
    /* Both panels are 480 tall → same UI scale gives a consistent layout. */
    if (found) { g_dev_w = 640; g_dev_h = 480; g_dev_scale = 150; }
    else       { g_dev_w = 854; g_dev_h = 480; g_dev_scale = 150; }
    char s[16];
    snprintf(s, sizeof s, "%d", g_dev_w);     setenv("TF_PANEL_W", s, 1);
    snprintf(s, sizeof s, "%d", g_dev_h);     setenv("TF_PANEL_H", s, 1);
    snprintf(s, sizeof s, "%d", g_dev_scale); setenv("TF_UI_SCALE", s, 1);
    dbg_log("DBG device: %s panel=%dx%d ui_scale=%d\n",
            found ? "R36SX" : "SF3000", g_dev_w, g_dev_h, g_dev_scale);
}

int sf3000_panel_w(void)  { sf3000_detect_device(); return g_dev_w; }
int sf3000_panel_h(void)  { sf3000_detect_device(); return g_dev_h; }
int sf3000_is_r36sx(void) { sf3000_detect_device(); return g_dev_r36sx > 0; }

/* Device-correct driver.so path (video AND audio must match the panel/SoC build;
 * loading the wrong one segfaults the proprietary audio init). */
const char *sf3000_driver_path(void) {
    sf3000_detect_device();
    return g_dev_r36sx > 0 ? "/mnt/sdcard/cubegm/driver_r36sx.so"
                           : "/mnt/sdcard/cubegm/driver_sf3000.so";
}

#define PANEL_W (sf3000_panel_w())
#define PANEL_H (sf3000_panel_h())
#define PANEL_ASPECT_NUM (sf3000_is_r36sx() ? 4 : 16)
#define PANEL_ASPECT_DEN (sf3000_is_r36sx() ? 3 : 9)

void sf3000_fb_blit(const void *src, int width, int height, int pitch) {
    {
        extern void dbg_log(const char *fmt, ...);
        static int s_blit = 0;
        if (s_blit < 8) {
            s_blit++;
            dbg_log("DBG blit#%d: src=%p screen=%p w=%d h=%d pitch=%d scale_filter=%d scale_size=%d use_hw=%d PANEL=%dx%d\n",
                    s_blit, src, (void*)screen->pixels, width, height, pitch,
                    scale_filter, scale_size, sf3000_use_hwdisp, PANEL_W, PANEL_H);
        }
    }
    /* R36SX: pass the user's scale-size choice (integer/aspect/full) to the HW
     * present so disp_frame fills / aspect-fits / integer-scales accordingly. */
    if (sf3000_is_r36sx()) hwdisp_set_panel_scale((int)scale_size);
    /* WARM-BOOT: hwdisp pre-init'd by sf3000_fb_init via marker file.
     * For FrogUI panel-size frames, force HW bilinear pass-through — only path
     * proven safe with panel-native input.  Cold-boot FrogUI takes SW path
     * (no marker → use_hwdisp=0). */
    /* R36SX ONLY: panel-size core frames (FrogUI) → direct fb0 write (controller
     * scales, panel is landscape-native so no rotation needed). SF3000's panel is
     * portrait-mounted (needs 90° rotation) and its driver's disp_frame ABORTS on
     * panel-size frames (only small game frames work), so SF3000 FrogUI falls
     * through to the SW transpose path below (bilinear-blended, its proven cold
     * renderer). */
    if (sf3000_is_r36sx() && src != screen->pixels &&
        width == PANEL_W && height == PANEL_H) {
        if (!sf3000_use_hwdisp && hwdisp_init() == 0) {
            sf3000_use_hwdisp = 1;
            fprintf(stderr, "sf3000_fb_blit: HW path active (panel-size)\n");
        }
        if (sf3000_use_hwdisp && hwdisp_present_direct(src, width, height, pitch))
            return;
    }
    /* Lazy-init HW: R36SX always (disp_frame hangs → fb-write for everything);
     * SF3000 only on bilinear frames (nearest games take the SW path). */
    if (!sf3000_use_hwdisp &&
        (sf3000_is_r36sx() || scale_filter == SCALE_FILTER_BILINEAR)) {
        if (hwdisp_init() == 0) {
            sf3000_use_hwdisp = 1;
            fprintf(stderr, "sf3000_fb_blit: HW path active\n");
        }
    }


if (sf3000_use_hwdisp) {
        /* Fast-forward: frame_limit paces emulation at (level+1)*60fps; here we
         * present only 1 of (level+1) frames so the display stays 60fps and the
         * game runs (level+1)x. frame_limit runs first (below) → paces every
         * frame; this only skips the present. Game frames only (not FrogUI). */
        extern int g_ff_level;
        int ff_skip = 0;
        if (g_ff_level && src != screen->pixels) {
            static unsigned ff_ctr = 0;
            if ((++ff_ctr) % (g_ff_level + 1)) ff_skip = 1;
        }
        int aspect = (src == screen->pixels) || (scale_size != SCALE_SIZE_FULL);
        hwdisp_set_target_aspect(aspect ? PANEL_ASPECT_NUM : 0, aspect ? PANEL_ASPECT_DEN : 0);
        hwdisp_set_filter(0);   /* always HW bilinear (SW nearest path removed) */

        /* FPS / overlay text: render into a copy of src (RGB565), since src
         * itself is owned by the core and may be read-only. */
        if (msg[0]) {
            static uint16_t *compose_buf = NULL;
            static int compose_cap = 0;
            int need = width * height;
            if (need > compose_cap) {
                free(compose_buf);
                compose_cap = need + 4096;
                compose_buf = (uint16_t*)malloc(compose_cap * sizeof(uint16_t));
            }
            if (compose_buf) {
                /* Copy src into compose_buf row-by-row (pitch may be wider). */
                int sp = pitch / 2;
                const uint16_t *s = (const uint16_t *)src;
                for (int y = 0; y < height; y++)
                    memcpy(compose_buf + (size_t)y * width, s + (size_t)y * sp, (size_t)width * 2);

                /* Render msg at top-left using fontdata8x8. The buffer is the
                 * game's native res and gets upscaled to the panel, so a fixed
                 * scale looks huge on low-res cores. Scale with buffer height so
                 * the on-panel size stays ~constant (≈ the menu font). */
                int len = 0; while (msg[len]) len++;
                /* The HUD msg is space-padded to a fixed width (fps left / cpu
                 * right), so size the bar to the last non-space glyph — otherwise
                 * it stretches across the whole screen. */
                int vis = len; while (vis > 0 && msg[vis-1] == ' ') vis--;
                int ts = height / 120; if (ts < 1) ts = 1;
                int tx = 4, ty = 4;
                /* Background bar */
                int bw = vis * 8 * ts + 4;
                int bh = 8 * ts + 4;
                for (int by = 0; by < bh && (ty + by) < height; by++) {
                    uint16_t *r = compose_buf + (size_t)(ty + by) * width + tx;
                    int span = bw; if (tx + span > width) span = width - tx;
                    for (int bx = 0; bx < span; bx++) r[bx] = 0;
                }
                /* Glyphs (white = 0xFFFF) */
                for (int i = 0; i < len; i++) {
                    unsigned char c = (unsigned char)msg[i];
                    for (int row = 0; row < 8; row++) {
                        unsigned char fd = fontdata8x8[c * 8 + row];
                        if (!fd) continue;
                        for (int sr = 0; sr < ts; sr++) {
                            int py = ty + 2 + row * ts + sr;
                            if (py >= height) break;
                            uint16_t *r = compose_buf + (size_t)py * width;
                            for (int col = 0; col < 8; col++) {
                                if (!(fd & (0x80 >> col))) continue;
                                int px = tx + 2 + i * 8 * ts + col * ts;
                                for (int sc = 0; sc < ts && (px + sc) < width; sc++)
                                    r[px + sc] = 0xFFFF;
                            }
                        }
                    }
                }
                sf3000_frame_limit();   /* paces emulation (target scales with FF level) */
                if (ff_skip) return;    /* FF: drop this present to keep display 60fps */
                /* R36SX: disp_frame hangs → direct fb0 write. SF3000: disp_frame. */
                if (!(sf3000_is_r36sx() && hwdisp_present_direct(compose_buf, width, height, width * 2)))
                    hwdisp_present(compose_buf, width, height, width * 2);
                return;
            }
        }
        sf3000_frame_limit();   /* paces emulation (target scales with FF level) */
        if (ff_skip) return;    /* FF: drop this present to keep display 60fps */
        if (!(sf3000_is_r36sx() && hwdisp_present_direct(src, width, height, pitch)))
            hwdisp_present(src, width, height, pitch);
        return;
    }
    /* Re-assert display controller → fb0 page.
       Virtual fb is 5120 lines (4 pages). This ioctl keeps display pointed at our page. */
    {
        int dis = open("/dev/dis", O_RDWR);
        if (dis >= 0) {
            struct { int a, b, c; } buf = {1, 0, 0};
            ioctl(dis, 0xc00c0e0c, &buf);
            close(dis);
        }
    }

    if (!sf3000_fb_mem) return;

    int dst_stride = (int)(sf3000_finfo.line_length / 4);
    const int FB_X_VIS = 180;
    const int FB_Y_TOT = 1014;   /* visible fb_y range: fb_y 0..1013 */
    const int PHYS_W   = 854;
    const int PHYS_H   = 480;
    const int PAGE_Y_SIZE = 1280;

    const int gw = width, gh = height;
    const uint16_t *s = (const uint16_t *)src;

    if (gw * gh > sf3000_rotated_capacity) {
        if (sf3000_rotated_src) free(sf3000_rotated_src);
        sf3000_rotated_capacity = gw * gh + 8192;
        sf3000_rotated_src = malloc(sf3000_rotated_capacity * sizeof(uint16_t));
    }

    /* Fast, pure-data transpose pass. Great for CPU cache. */
    for (int gy = 0; gy < gh; gy++) {
        const uint16_t *src_row = (const uint16_t *)((const char *)s + gy * pitch);
        for (int gx = 0; gx < gw; gx++) {
            sf3000_rotated_src[gx * gh + gy] = src_row[gx];
        }
    }

    int current_scale = scale_size;
    if (src == screen->pixels) {
        current_scale = SCALE_SIZE_ASPECT; /* Force menu to not stretch */
    }

    int disp_w = PHYS_W;
    int disp_h = PHYS_H;
    
    if (current_scale == SCALE_SIZE_ASPECT) {
        double ar = (aspect_ratio > 0.1) ? aspect_ratio : (double)gw / gh;
        disp_w = (int)(PHYS_H * ar + 0.5);
    } else if (current_scale == SCALE_SIZE_NONE) {
        /* Integer scaling */
        int scale_x = PHYS_W / gw;
        int scale_y = PHYS_H / gh;
        int scale = (scale_x < scale_y) ? scale_x : scale_y;
        if (scale < 1) scale = 1;
        disp_w = gw * scale;
        disp_h = gh * scale;
    }
    
    if (disp_w > PHYS_W) disp_w = PHYS_W;
    if (disp_h > PHYS_H) disp_h = PHYS_H;
    if (current_scale == SCALE_SIZE_FULL) {
        disp_w = PHYS_W;
        disp_h = PHYS_H;
    }

    static int sf3000_last_disp_h = -1;
    if (gh != sf3000_last_gh || disp_h != sf3000_last_disp_h) {
        int fb_x_len = disp_h * FB_X_VIS / PHYS_H;
        int fb_x_off = (FB_X_VIS - fb_x_len) / 2;

        for (int fb_x = 0; fb_x < FB_X_VIS; fb_x++) {
            if (fb_x < fb_x_off || fb_x >= fb_x_off + fb_x_len) {
                sf3000_blend_lut[fb_x].gy1 = -1; // Black border
                sf3000_blend_lut[fb_x].w2 = 0;
            } else {
                int active_x = fb_x - fb_x_off;
                int gy_fixed = ((fb_x_len - 1 - active_x) * gh * 256) / fb_x_len;
                int gy = gy_fixed >> 8;
                int frac = gy_fixed & 0xFF;
                
                sf3000_blend_lut[fb_x].gy1 = gy;
                sf3000_blend_lut[fb_x].gy2 = (gy + 1 < gh) ? (gy + 1) : gy;
                sf3000_blend_lut[fb_x].w2 = frac >> 1; // 0..127
                sf3000_blend_lut[fb_x].w1 = 128 - (frac >> 1);
            }
        }
        sf3000_last_gh = gh;
        sf3000_last_disp_h = disp_h;
    }

    /* Convert physical width → fb_y units, then center */
    int fb_y_len = disp_w * FB_Y_TOT / PHYS_W;
    int fb_y_off = (FB_Y_TOT - fb_y_len) / 2;

    static uint32_t row_cache[180];
    static int last_fb_y_off = -1, last_fb_y_len = -1;
    int prev_gx = -1;

    /* Cycle between page 0 and page 1 for double buffering */
    sf3000_current_page = (sf3000_current_page + 1) % 2;
    int page_y_offset = sf3000_current_page * PAGE_Y_SIZE;

    if (fb_y_off != last_fb_y_off || fb_y_len != last_fb_y_len) {
        for (int p = 0; p < 2; p++) {
            for (int fy = 0; fy < PAGE_Y_SIZE; fy++) {
                for (int fx = 0; fx < FB_X_VIS; fx++) {
                    sf3000_fb_mem[(p * PAGE_Y_SIZE + fy) * dst_stride + fx] = 0xFF000000u;
                }
            }
        }
        last_fb_y_off = fb_y_off;
        last_fb_y_len = fb_y_len;
    }

    int fb_y = fb_y_off;
    while (fb_y < fb_y_off + fb_y_len) {
        int gx = (fb_y - fb_y_off) * gw / fb_y_len;
        
        /* Count how many lines share this exact gx (nearest neighbor vertical scaling) */
        int count = 1;
        while (fb_y + count < fb_y_off + fb_y_len && ((fb_y + count - fb_y_off) * gw / fb_y_len) == gx) {
            count++;
        }
        
        /* Compute the blended 32-bit row ONLY ONCE for these `count` lines */
        uint16_t *src_col = sf3000_rotated_src + gx * gh;
        for (int fb_x = 0; fb_x < FB_X_VIS; fb_x++) {
            int gy1 = sf3000_blend_lut[fb_x].gy1;
            if (gy1 < 0) {
                row_cache[fb_x] = 0xFF000000u;
                continue;
            }
            int gy2 = sf3000_blend_lut[fb_x].gy2;
            uint32_t w1 = sf3000_blend_lut[fb_x].w1;
            uint32_t w2 = sf3000_blend_lut[fb_x].w2;

            uint16_t p1 = src_col[gy1];

            if (w2 == 0 || gy1 == gy2) {
                row_cache[fb_x] = cvt565(p1);
            } else {
                uint16_t p2 = src_col[gy2];
                if (p1 == p2) {
                    row_cache[fb_x] = cvt565(p1);
                } else {
                    uint32_t c1 = cvt565(p1);
                    uint32_t c2 = cvt565(p2);
                    uint32_t rb = (((c1 & 0xFF00FF) * w1 + (c2 & 0xFF00FF) * w2) >> 7) & 0xFF00FF;
                    uint32_t g  = (((c1 & 0x00FF00) * w1 + (c2 & 0x00FF00) * w2) >> 7) & 0x00FF00;
                    row_cache[fb_x] = 0xFF000000u | rb | g;
                }
            }
        }
        
        /* Blit the computed row `count` times to the framebuffer */
        uint32_t *dst = sf3000_fb_mem + (page_y_offset + fb_y) * dst_stride;
        for (int i = 0; i < count; i++) {
            uint32_t *d = dst;
            uint32_t *s = row_cache;
            // Manually unroll the 180-word copy (18 iterations of 10)
            // This aggressively bypasses generic libc memcpy overhead for uncached writes
            for (int k = 0; k < 18; k++) {
                *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
                *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
            }
            dst += dst_stride;
        }
        
        fb_y += count;
    }

    if (msg[0]) {
        /* Draw overlay text (FPS, CPU load, etc.) 
           px=10, py=10 is top-left in landscape */
        sf3000_text_native(10, 10, msg, 0xFFFFFFFFu, page_y_offset);
    }
    
    {
        struct fb_var_screeninfo vi = sf3000_vinfo;
        vi.xoffset = 0; vi.yoffset = page_y_offset;
        ioctl(sf3000_fb_fd, FBIOPAN_DISPLAY, &vi);

        static struct timeval last_tv = {0, 0};
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (last_tv.tv_sec != 0) {
            long long diff_us = (tv.tv_sec - last_tv.tv_sec) * 1000000LL + (tv.tv_usec - last_tv.tv_usec);
            if (diff_us > 0 && diff_us < 16666) {
                long long sleep_us = 16666 - diff_us;
                if (sleep_us > 2000) {
                    usleep(sleep_us - 2000);
                }
                while (1) {
                    gettimeofday(&tv, NULL);
                    diff_us = (tv.tv_sec - last_tv.tv_sec) * 1000000LL + (tv.tv_usec - last_tv.tv_usec);
                    if (diff_us >= 16666) break;
                }
            }
        }
        last_tv = tv;
    }
}

extern struct sf3000_btn sf3000_keymap[];
extern const int sf3000_keymap_count;

/* Native text: writes fontdata8x8 glyphs directly to fb0.
   physical_x = fb_y * 854/1014, physical_y = (179-fb_x) * 8/3.
   2 fb_y per font column (squarifies pixels), 1 fb_x per row. */
static void sf3000_text_native(int px, int py, const char *text, uint32_t color, int page_y_offset) {
    if (!sf3000_fb_mem) return;
    int dst_stride = (int)(sf3000_finfo.line_length / 4);
    
    int text_scale = 2; // 2x scale for readability
    int bg_padding = 4;
    
    int len = 0; while(text[len]) len++;
    int box_w_px = len * 8 * text_scale + bg_padding * 2;
    int box_h_px = 8 * text_scale + bg_padding * 2;
    
    // Draw solid black background
    for (int box_y = 0; box_y < box_h_px; box_y++) {
        int current_py = py + box_y;
        int fb_x = 179 - (current_py * 180 / 480);
        if (fb_x < 0 || fb_x > 179) continue;
        
        int fb_y_start = (px * 1014 / 854) + page_y_offset;
        int fb_y_end = ((px + box_w_px) * 1014 / 854) + page_y_offset;
        
        for (int fy = fb_y_start; fy < fb_y_end; fy++) {
            if (fy >= page_y_offset && fy < page_y_offset + 1280) {
                sf3000_fb_mem[fy * dst_stride + fb_x] = 0xFF000000u; 
            }
        }
    }
    
    // Draw scaled text
    px += bg_padding;
    py += bg_padding;
    for (int i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        for (int row = 0; row < 8; row++) {
            unsigned char fd = fontdata8x8[c * 8 + row];
            if (!fd) continue;
            
            for (int sr = 0; sr < text_scale; sr++) {
                int current_py = py + row * text_scale + sr;
                int fb_x = 179 - (current_py * 180 / 480);
                if (fb_x < 0 || fb_x > 179) continue;
                
                for (int col = 0; col < 8; col++) {
                    if (fd & (0x80 >> col)) {
                        int current_px = px + i * 8 * text_scale + col * text_scale;
                        int fb_y_start = (current_px * 1014 / 854) + page_y_offset;
                        int fb_y_end = ((current_px + text_scale) * 1014 / 854) + page_y_offset;
                        for (int fy = fb_y_start; fy < fb_y_end; fy++) {
                            if (fy >= page_y_offset && fy < page_y_offset + 1280) {
                                sf3000_fb_mem[fy * dst_stride + fb_x] = color;
                            }
                        }
                    }
                }
            }
        }
    }
}

static void sf3000_show_frame_native(const char **lines, int nlines) {
    if (!sf3000_fb_mem) return;
    int dst_stride = (int)(sf3000_finfo.line_length / 4);
    for (int fy = 0; fy < 1014; fy++)
        for (int fx = 0; fx < 180; fx++)
            sf3000_fb_mem[fy * dst_stride + fx] = 0xFF000000u;
    int py = 10;
    for (int i = 0; i < nlines; i++) {
        if (lines[i]) sf3000_text_native(20, py, lines[i], 0xFFFFFFFFu, 0);
        py += 30;
    }
    struct fb_var_screeninfo vi = sf3000_vinfo;
    vi.xoffset = vi.yoffset = 0;
    ioctl(sf3000_fb_fd, FBIOPAN_DISPLAY, &vi);
}

void sf3000_calibrate_input(void) {
    if (!sf3000_keys_ptr) return;

    static const char *bnames[] = {
        "UP","DOWN","LEFT","RIGHT","A","B","X","Y","L","R","L2","R2","SELECT","START" };
    const int N = 14;
    uint32_t captured[14];
    for (int i = 0; i < N; i++) captured[i] = 0xFF;

    for (int i = 0; i < N; i++) {
        char prompt[32], sub[32], hint[48];
        snprintf(prompt, sizeof(prompt), "PRESS: %s", bnames[i]);
        snprintf(sub,    sizeof(sub),    "Button %d of %d", i+1, N);
        snprintf(hint,   sizeof(hint),   "(hold 3s to skip)");

        /* flush held keys max 1s */
        for (int t = 0; t < 60 && (*sf3000_keys_ptr & 0xFFFF); t++) usleep(16000);

        uint32_t pressed = 0;
        int ticks = 0;
        const char *flines[3] = { prompt, sub, hint };
        while (!pressed && ticks < 180) {
            sf3000_show_frame_native(flines, 3);
            usleep(16000);
            ticks++;
            uint32_t cur = *sf3000_keys_ptr & 0xFFFF;
            if (cur) pressed = cur;
        }
        if (pressed) {
            captured[i] = pressed;
            for (int t = 0; t < 120 && (*sf3000_keys_ptr & pressed); t++) {
                sf3000_show_frame_native(flines, 3);
                usleep(16000);
            }
        }
    }

    /* Build summary lines and hold on screen until device rebooted */
    char lines[6][48];
    snprintf(lines[0], 48, "RESULTS (tell me these):");
    snprintf(lines[1], 48, "UP=%04X DN=%04X LT=%04X RT=%04X",
             captured[0], captured[1], captured[2], captured[3]);
    snprintf(lines[2], 48, "A=%04X B=%04X X=%04X Y=%04X",
             captured[4], captured[5], captured[6], captured[7]);
    snprintf(lines[3], 48, "L=%04X R=%04X L2=%04X R2=%04X",
             captured[8], captured[9], captured[10], captured[11]);
    snprintf(lines[4], 48, "SEL=%04X STA=%04X",
             captured[12], captured[13]);
    snprintf(lines[5], 48, "Reboot when done reading");

    for (int i = 0; i < 6; i++) fprintf(stderr, "%s\n", lines[i]);
    fflush(stderr);

    const char *slines[6] = { lines[0], lines[1], lines[2], lines[3], lines[4], lines[5] };
    while (1) {
        sf3000_show_frame_native(slines, 6);
        usleep(100000);
    }
}

void sf3000_load_keymap(void) {
    static const char *names[] = {
        "UP","DOWN","LEFT","RIGHT","A","B","X","Y","L","R","L2","R2","SELECT","START" };
    static const int retro_ids[] = { 4,5,6,7,8,0,9,1,10,11,12,13,2,3 };
    const int NMAP = 14;
    FILE *f = fopen("/mnt/sdcard/sf3000_keymap.txt", "r");
    if (!f) return;
    char name[16]; int bit;
    while (fscanf(f, "%15s %d", name, &bit) == 2) {
        for (int i = 0; i < NMAP; i++) {
            if (strcmp(name, names[i]) == 0) {
                sf3000_keymap[i].bit      = (uint8_t)bit;
                sf3000_keymap[i].retro_id = (uint8_t)retro_ids[i];
            }
        }
    }
    fclose(f);
    fprintf(stderr, "SF3000 input: loaded keymap\n");
}

void sf3000_fb_finish(void) {
    if (sf3000_use_hwdisp) {
        hwdisp_deinit();
        sf3000_use_hwdisp = 0;
    }
    if (sf3000_fb_mem) {
        munmap(sf3000_fb_mem, sf3000_fb_size);
        sf3000_fb_mem = NULL;
    }
    if (sf3000_fb_fd >= 0) {
        close(sf3000_fb_fd);
        sf3000_fb_fd = -1;
    }
    if (sf3000_rotated_src) {
        free(sf3000_rotated_src);
        sf3000_rotated_src = NULL;
        sf3000_rotated_capacity = 0;
    }
}

/* Restore fb0 to native portrait geometry (720x1280, 32bpp) using the existing
 * open fd.  Called from quit() before execl so the new process inherits a clean
 * display state.  Uses yres_virtual=2560 (2 pages) — safe when smem=11MB
 * (HCGE leftover), avoids FBIOPUT rejection that would occur with 5120. */
void sf3000_restore_fb0_geometry(void) {
    if (sf3000_fb_fd < 0) return;
    sf3000_dump_fb_state("restore/entry");
    struct fb_var_screeninfo vinfo;
    if (ioctl(sf3000_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) return;
    fprintf(stderr, "sf3000_restore_fb0_geometry: before: %ux%u(v%u) bpp=%u rot=%u\n",
            vinfo.xres, vinfo.yres, vinfo.yres_virtual, vinfo.bits_per_pixel, vinfo.rotate);
    /* Always FBIOPUT — even when geometry looks correct, the display
     * controller can still be routed to driver.so's buffer.  Pair with
     * /dev/dis ioctl to re-route display→fb0. */
    vinfo.xres = 720;
    vinfo.yres = 1280;
    vinfo.xres_virtual = 720;
    vinfo.yres_virtual = 2560; /* 2 pages; fits in HCGE-leftover smem (~11MB) */
    vinfo.xoffset = 0;
    vinfo.yoffset = 0;
    vinfo.bits_per_pixel = 32;
    vinfo.red    = (struct fb_bitfield){16, 8, 0};
    vinfo.green  = (struct fb_bitfield){ 8, 8, 0};
    vinfo.blue   = (struct fb_bitfield){ 0, 8, 0};
    vinfo.transp = (struct fb_bitfield){24, 8, 0};
    int r = ioctl(sf3000_fb_fd, FBIOPUT_VSCREENINFO, &vinfo);
    fprintf(stderr, "sf3000_restore_fb0_geometry: FBIOPUT ret=%d\n", r);
    /* Probe /dev/dis ioctl args — log all rets so we can see which combo
     * actually re-routes the display to fb0 after HCGE. */
    int dis = open("/dev/dis", O_RDWR);
    if (dis >= 0) {
        struct { int a, b, c; } b;
        b = (typeof(b)){1, 0, 0}; DBG("DBG restore: /dev/dis {1,0,0} ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        b = (typeof(b)){0, 0, 0}; DBG("DBG restore: /dev/dis {0,0,0} ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        b = (typeof(b)){1, 1, 0}; DBG("DBG restore: /dev/dis {1,1,0} ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        b = (typeof(b)){2, 0, 0}; DBG("DBG restore: /dev/dis {2,0,0} ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        b = (typeof(b)){1, 0, 0}; DBG("DBG restore: /dev/dis {1,0,0}-final ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        close(dis);
    }
    sf3000_dump_fb_state("restore/post");
}

#endif // PLATFORM_SF3000
