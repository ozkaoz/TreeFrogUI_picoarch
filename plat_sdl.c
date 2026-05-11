#include <SDL/SDL.h>
#include <unistd.h>
#include <math.h>
#include "core.h"
#include "libpicofe/fonts.h"
#include "libpicofe/plat.h"
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
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/ipc.h>
#include <sys/shm.h>

/* cubevol shared memory: ftok("/tmp/joy_key", 'a') → 4-byte key state.
   Low 16 bits = button bitmask (bit set = pressed). */
volatile uint32_t *sf3000_keys_ptr = NULL;

static void sf3000_keys_init(void) {
    key_t k = ftok("/tmp/joy_key", 'a');
    if (k == (key_t)-1) { fprintf(stderr, "SF3000 input: ftok failed\n"); return; }
    int id = shmget(k, 4, 0666);
    if (id < 0) { fprintf(stderr, "SF3000 input: shmget failed\n"); return; }
    void *p = shmat(id, NULL, 0);
    if (p == (void *)-1) { fprintf(stderr, "SF3000 input: shmat failed\n"); return; }
    sf3000_keys_ptr = (volatile uint32_t *)p;
    fprintf(stderr, "SF3000 input: cubevol shm OK, initial keys=0x%08X\n", *sf3000_keys_ptr);
}

static int sf3000_fb_fd = -1;
static uint32_t *sf3000_fb_mem = NULL;
static struct fb_var_screeninfo sf3000_vinfo;
static struct fb_fix_screeninfo sf3000_finfo;
static size_t sf3000_fb_size = 0;

// Forward declarations
int sf3000_fb_init(void);
void sf3000_fb_blit(const void *src, int width, int height);
void sf3000_fb_finish(void);

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
	/* Blit already done in plat_video_process directly from libretro data */
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

	surface = SDL_DisplayFormat(imgsurface);
	if (!surface)
		goto finish;

	if (surface->pitch > SCREEN_PITCH ||
	    surface->h > SCREEN_HEIGHT ||
	    surface->w == 0 ||
	    surface->h * surface->pitch > buf_size)
		goto finish;

	memcpy(buf, surface->pixels, surface->pitch * surface->h);
	*w = surface->w;
	*h = surface->h;
	*bpp = surface->pitch / surface->w;

	ret = 0;

finish:
	if (imgsurface)
		SDL_FreeSurface(imgsurface);
	if (surface)
		SDL_FreeSurface(surface);
	return ret;
}


void plat_video_menu_enter(int is_rom_loaded)
{
	SDL_LockSurface(screen);
	memcpy(g_menubg_src_ptr, screen->pixels, g_menubg_src_h * g_menubg_src_pp * sizeof(uint16_t));
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
	if (pitch == width * 2) {
		/* RGB565: blit directly to fb0, bypass staging buffer entirely.
		   Staging had x0/y0 offsets and wrong stride=320 instead of gw. */
		g_game_w = (int)width; g_game_h = (int)height;
		SDL_UnlockSurface(screen);
		video_update_msg();
		sf3000_fb_blit(data, (int)width, (int)height);
		return;
	}

	if (pitch == width * 4) {
		int x0 = ((int)SCREEN_WIDTH  - (int)width)  / 2;
		int y0 = ((int)SCREEN_HEIGHT - (int)height) / 2;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		memset(screen->pixels, 0, SCREEN_HEIGHT * (screen->pitch));
		const uint32_t *src32 = (const uint32_t *)data;
		for (unsigned y = 0; y < height && (y + y0) < SCREEN_HEIGHT; y++) {
			const uint32_t *row = src32 + y * (pitch / 4);
			uint16_t *dst = (uint16_t *)screen->pixels
			                + (y + y0) * (screen->pitch / 2) + x0;
			for (unsigned x = 0; x < width && (x + x0) < SCREEN_WIDTH; x++) {
				uint32_t p = row[x];
				uint8_t r = (p >> 16) & 0xFF;
				uint8_t g = (p >>  8) & 0xFF;
				uint8_t b = (p)       & 0xFF;
				*dst++ = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
			}
		}
		SDL_UnlockSurface(screen);
		video_update_msg();
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

static void plat_sound_finish(void)
{
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	if (audio.buf) {
		free(audio.buf);
		audio.buf = NULL;
	}
}

static int plat_sound_init(void)
{
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
}

float plat_sound_capacity(void)
{
	int buffered = 0;
	if (audio.buf_len == 0)
		return 1.0;

	if (audio.buf_w != audio.buf_r) {
		buffered = audio.buf_w > audio.buf_r ?
			audio.buf_w - audio.buf_r :
			(audio.buf_w + audio.buf_len) - audio.buf_r;
	}

	return 1.0 - (float)buffered / audio.buf_len;
}

#define BATCH_SIZE 100
void plat_sound_write(const struct audio_frame *data, int frames)
{
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
}

void plat_sound_resize_buffer(void) {
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
	audio.in_sample_rate = sample_rate;
	plat_sound_resize_buffer();
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

int sf3000_fb_init(void) {
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
    ioctl(sf3000_fb_fd, FBIOGET_FSCREENINFO, &sf3000_finfo);

    sf3000_fb_size = sf3000_finfo.line_length * sf3000_vinfo.yres;
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
    fprintf(stderr, "SF3000 fb0: xoffset=%u yoffset=%u type=%u visual=%u\n",
            sf3000_vinfo.xoffset, sf3000_vinfo.yoffset,
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

    return 0;
}

void sf3000_fb_blit(const void *src, int width, int height) {
    /* Re-assert display controller → fb0 page 0 every frame.
       Virtual fb is 5120 lines (4 pages); something may flip yoffset
       after init. This ioctl keeps display pointed at our page 0. */
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
    /* Confirmed FB mapping (hardware-tested via calibration bars, 2025-05):
         physical_x = fb_y * 854 / 1014   (visible fb_y 0..1013)
         physical_y = (179-fb_x) * 8/3    (visible fb_x 0..179 → 480 physical px)
       Derived: GREEN bar (fb_y=763) at ~75% px; BLUE (fb_y=1014) first off-screen. */
    const int FB_X_VIS = 180;
    const int FB_Y_TOT = 1014;   /* visible fb_y range: fb_y 0..1013 */
    const int PHYS_W   = 854;
    const int PHYS_H   = 480;

    const int gw = width, gh = height;
    const uint16_t *s = (const uint16_t *)src;

    /* AR from core; fallback to pixel AR */
    double ar = (aspect_ratio > 0.1) ? aspect_ratio : (double)gw / gh;
    int disp_w = (int)(PHYS_H * ar + 0.5);  /* game width in physical_x px */
    if (disp_w > PHYS_W) disp_w = PHYS_W;

    /* Convert physical width → fb_y units, then center */
    int fb_y_len = disp_w * FB_Y_TOT / PHYS_W;
    int fb_y_off = (FB_Y_TOT - fb_y_len) / 2;

    static uint32_t row_cache[180];
    static int last_fb_y_off = -1, last_fb_y_len = -1;
    int prev_gx = -1;

    /* Clear full fb0 to black once on layout change */
    if (fb_y_off != last_fb_y_off || fb_y_len != last_fb_y_len) {
        int fb_h_full = (int)sf3000_vinfo.yres;  /* 1280 */
        for (int fy = 0; fy < fb_h_full; fy++)
            for (int fx = 0; fx < FB_X_VIS; fx++)
                sf3000_fb_mem[fy * dst_stride + fx] = 0xFF000000u;
        last_fb_y_off = fb_y_off;
        last_fb_y_len = fb_y_len;
    }

    for (int fb_y = fb_y_off; fb_y < fb_y_off + fb_y_len; fb_y++) {
        int gx = (fb_y - fb_y_off) * gw / fb_y_len;
        if (gx != prev_gx) {
            /* Iterate gy (source rows) not fb_x — ensures ALL gh rows are
               sampled. With gh=224 > FB_X_VIS=180 the old loop skipped ~44
               rows; iterating gy gives nearest-neighbour correctness. */
            for (int gy = 0; gy < gh; gy++) {
                int fb_x = (FB_X_VIS - 1) - gy * FB_X_VIS / gh;
                if (fb_x < 0 || fb_x >= FB_X_VIS) continue;
                uint16_t p = s[gy * gw + gx];
                uint32_t r = (p >> 11) & 0x1F; r = (r << 3) | (r >> 2);
                uint32_t g = (p >>  5) & 0x3F; g = (g << 2) | (g >> 4);
                uint32_t b = (p)       & 0x1F; b = (b << 3) | (b >> 2);
                row_cache[fb_x] = 0xFF000000u | (r<<16) | (g<<8) | b;
            }
            prev_gx = gx;
        }
        memcpy(sf3000_fb_mem + fb_y * dst_stride, row_cache, FB_X_VIS * 4);
    }
    {
        struct fb_var_screeninfo vi = sf3000_vinfo;
        vi.xoffset = 0; vi.yoffset = 0;
        ioctl(sf3000_fb_fd, FBIOPAN_DISPLAY, &vi);
    }
}

extern struct sf3000_btn sf3000_keymap[];
extern const int sf3000_keymap_count;

/* Native text: writes fontdata8x8 glyphs directly to fb0.
   physical_x = fb_y * 854/1014, physical_y = (179-fb_x) * 8/3.
   2 fb_y per font column (squarifies pixels), 1 fb_x per row. */
static void sf3000_text_native(int px, int py, const char *text, uint32_t color) {
    if (!sf3000_fb_mem) return;
    int dst_stride = (int)(sf3000_finfo.line_length / 4);
    int fb_y_base = px * 1014 / 854;
    for (int i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        for (int row = 0; row < 8; row++) {
            unsigned char fd = fontdata8x8[c * 8 + row];
            if (!fd) continue;
            int fb_x = 179 - (py + row) * 180 / 480;
            if (fb_x < 0 || fb_x > 179) continue;
            for (int col = 0; col < 8; col++) {
                if (fd & (0x80 >> col)) {
                    int fy = fb_y_base + col * 2;
                    if (fy >= 0 && fy < 1013) {
                        sf3000_fb_mem[fy * dst_stride + fb_x] = color;
                        sf3000_fb_mem[(fy+1) * dst_stride + fb_x] = color;
                    }
                }
            }
        }
        fb_y_base += 18;  /* 8 cols × 2 + 2 spacing */
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
        if (lines[i]) sf3000_text_native(20, py, lines[i], 0xFFFFFFFFu);
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
    if (sf3000_fb_mem) {
        munmap(sf3000_fb_mem, sf3000_fb_size);
        sf3000_fb_mem = NULL;
    }
    if (sf3000_fb_fd >= 0) {
        close(sf3000_fb_fd);
        sf3000_fb_fd = -1;
    }
}

#endif // PLATFORM_SF3000
