// #include <png.h>
#include <stdarg.h>
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#define FROGUI_CORE "/mnt/sdcard/cubegm/cores/frogui_libretro.so"
#define PICOARCH_BIN    "/mnt/sdcard/cubegm/picoarch"
#define PICOARCH_HI_BIN "/mnt/sdcard/cubegm/picoarch_hi"

/* gpsp (GBA) and pcsx_rearmed (PS1) dynarecs need the low 512MB of address space
 * free for their fixed memory maps — run them via the high-linked picoarch_hi
 * (text @0x20000000). Everything else uses the normal picoarch. */
static const char *picoarch_for_core(const char *core) {
	if (core && (strstr(core, "gpsp") || strstr(core, "pcsx") || strstr(core, "ps1")))
		if (access(PICOARCH_HI_BIN, F_OK) == 0)   /* vfat has no exec bit → F_OK */
			return PICOARCH_HI_BIN;
	return PICOARCH_BIN;
}
#define LAUNCH_FILE "/tmp/frogui_launch.txt"
#include "core.h"
#include "config.h"
#include "content.h"
#include "libpicofe/config_file.h"
#include "libpicofe/input.h"
#include "main.h"
#include "menu.h"
#include "overrides.h"
#include "plat.h"
#include "util.h"

#ifdef MMENU
#include <dlfcn.h>
#include <mmenu.h>
#include <SDL/SDL.h>
void* mmenu = NULL;
char save_template_path[MAX_PATH];
#endif

bool should_quit = false;
unsigned current_audio_buffer_size;
char core_name[MAX_PATH];
int config_override = 0;
static int last_screenshot = 0;
int g_debug_frame = 0;
static int g_filter_on_menu_enter = -1;

#ifdef PLATFORM_SF3000
/* FrogUI owns the nearest/bilinear filter choice (single setting, applies to
 * every game).  Picoarch reads /mnt/sdcard/frogui/settings.txt at startup and
 * overrides scale_filter accordingly.  No in-game menu option to change. */
#define FROGUI_SETTINGS_FILE "/mnt/sdcard/frogui/settings.txt"
#define LAST_GAME_FILE       "/mnt/sdcard/picoarch/last_game.txt"
static int g_auto_resume = 0;
static void load_frogui_settings(void) {
	FILE *f = fopen(FROGUI_SETTINGS_FILE, "r");
	if (!f) { DBG("DBG load_frogui_settings: no settings file\n"); return; }
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char *val = eq + 1;
		char *nl = strchr(val, '\n'); if (nl) *nl = '\0';
		char *cr = strchr(val, '\r'); if (cr) *cr = '\0';
		if (strcmp(line, "filter") == 0) {
			if (strcmp(val, "bilinear") == 0)      scale_filter = SCALE_FILTER_BILINEAR;
			else if (strcmp(val, "nearest") == 0)  scale_filter = SCALE_FILTER_NEAREST;
			DBG("DBG load_frogui_settings: filter=%s → scale_filter=%d\n",
			        val, scale_filter);
		} else if (strcmp(line, "auto_resume") == 0) {
			g_auto_resume = (strcmp(val, "on") == 0) ? 1 : 0;
			DBG("DBG load_frogui_settings: auto_resume=%d\n", g_auto_resume);
		}
	}
	fclose(f);
}
static void load_frogui_filter(void) { load_frogui_settings(); }

static int read_last_game(char *core, size_t cs, char *rom, size_t rs) {
	FILE *f = fopen(LAST_GAME_FILE, "r");
	if (!f) { DBG("DBG read_last_game: file missing (good)\n"); return 0; }
	core[0] = rom[0] = 0;
	if (fgets(core, cs, f)) core[strcspn(core, "\r\n")] = 0;
	if (fgets(rom,  rs, f)) rom[strcspn(rom, "\r\n")]   = 0;
	fclose(f);
	DBG("DBG read_last_game: core=%s rom=%s\n", core, rom);
	return (core[0] && rom[0]);
}
static void write_last_game(const char *core, const char *rom) {
	FILE *f = fopen(LAST_GAME_FILE, "w");
	if (!f) return;
	fprintf(f, "%s\n%s\n", core, rom);
	fflush(f); fsync(fileno(f));
	fclose(f);
	sync();
}
static void clear_last_game(void) {
	int r = unlink(LAST_GAME_FILE);
	DBG("DBG clear_last_game: unlink ret=%d errno=%d\n", r, errno);
	sync();
}
#endif

void dbg_log(const char *fmt, ...) {
	/* Write directly to log.txt (append) so every picoarch process — including
	 * the game process exec'd from FrogUI — is captured, regardless of whether
	 * its stderr is redirected. Gated on log.txt existing (debug mode). */
	static int enabled = -1;
	static FILE *lf = NULL;
	if (enabled == -1) {
		enabled = (access("/mnt/sdcard/log.txt", F_OK) == 0) ? 1 : 0;
		if (enabled) lf = fopen("/mnt/sdcard/log.txt", "a");
	}
	if (!enabled || !lf) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(lf, fmt, ap);
	va_end(ap);
	fflush(lf);
	fsync(fileno(lf));   /* debug: persist each line (survives crash/power-cut) */
}

static void sig_hex(char *b, int *n, unsigned long v) {
	b[(*n)++] = '0'; b[(*n)++] = 'x';
	for (int i = 28; i >= 0; i -= 4) {
		int d = (v >> i) & 0xf;
		b[(*n)++] = d < 10 ? ('0' + d) : ('a' + d - 10);
	}
}

static void sigsegv_handler(int sig, siginfo_t *si, void *ucv) {
	/* async-signal-safe: write() + manual formatting only */
	char buf[160];
	int n = 0;
	buf[n++] = 'S'; buf[n++] = 'I'; buf[n++] = 'G'; buf[n++] = '=';
	if (sig >= 10) buf[n++] = '0' + sig / 10;
	buf[n++] = '0' + sig % 10;
	buf[n++] = ' '; buf[n++] = 'f';
	int f = g_debug_frame;
	if (f >= 1000) buf[n++] = '0' + f / 1000;
	if (f >= 100)  buf[n++] = '0' + (f / 100) % 10;
	if (f >= 10)   buf[n++] = '0' + (f / 10) % 10;
	buf[n++] = '0' + f % 10;
	const char *as = " addr="; for (const char *p = as; *p; p++) buf[n++] = *p;
	sig_hex(buf, &n, si ? (unsigned long)si->si_addr : 0);
	const char *ps = " pc="; for (const char *p = ps; *p; p++) buf[n++] = *p;
	unsigned long pc = 0;
	if (ucv) pc = (unsigned long)((ucontext_t *)ucv)->uc_mcontext.pc;
	sig_hex(buf, &n, pc);
	buf[n++] = '\n';
	write(2, buf, n);
	signal(sig, SIG_DFL);
	raise(sig);
}

static uint32_t vsyncs;
static uint32_t renders;

/* Fast-forward speed level, cycled by SELECT+Y: 0=off, 1=2x, 2=3x.
 * The frame limiter paces emulation at (level+1)*60fps and the SF3000/R36SX
 * present skips to keep display at 60fps (see plat_sdl.c). Audio mutes while FF. */
int g_ff_level = 0;

static void toggle_fast_forward(int force_off)
{
	static int enable_audio_was = 1;

	if (force_off) {
		if (g_ff_level) { g_ff_level = 0; enable_audio = enable_audio_was; }
		return;
	}
	if (g_ff_level == 0) enable_audio_was = enable_audio;  /* snapshot on entry */
	g_ff_level = (g_ff_level + 1) % 3;                     /* off → 2x → 3x → off */
	enable_audio = g_ff_level ? 0 : enable_audio_was;      /* mute while FF */
}

// static int screenshot_file_name(char *name, size_t len) {
// 	char suffix[MAX_PATH];
//
// 	for (int i = last_screenshot; i < 10000; i++) {
// 		snprintf(suffix, MAX_PATH, "IMG_%04d.png", i);
// 		save_relative_path(name, len, suffix);
//
// 		if (access(name, F_OK) == -1) {
// 			last_screenshot = i;
// 			return 0;
// 		}
// 	}
// 	*name = '\0';
// 	return -1;
// }
//
// static int png_write_rgb565(const uint16_t *buf, png_structp png_ptr, int w, int h) {
// 	png_byte *row_pointer = calloc(w * 3, sizeof(png_byte));
// 	int ret = -1;
//
// 	if (!row_pointer)
// 		return ret;
//
// 	if (setjmp(png_jmpbuf(png_ptr)))
// 		goto finish;
//
// 	for (int i = 0; i < h; i++) {
// 		uint16_t *pbuf = &((uint16_t *)buf)[i * w];
// 		png_byte *prow = row_pointer;
//
// 		for (int j = 0; j < w; j++) {
// 			uint16_t px = *pbuf++;
// 			*prow++ = ((((px & 0xF800) >> 11) * 255 + 15) / 31);
// 			*prow++ = ((((px & 0x07E0) >> 5)  * 255 + 31) / 63);
// 			*prow++ = ((((px & 0x001F))       * 255 + 15) / 31);
// 		}
// 		png_write_row(png_ptr, row_pointer);
// 	}
// 	ret = 0;
//
// finish:
// 	if (row_pointer)
// 		free(row_pointer);
//
// 	return ret;
// }
//
// static int write_png(const uint16_t *buf, int w, int h, FILE *file) {
// 	png_structp png_ptr = NULL;
// 	png_infop info_ptr = NULL;
// 	int ret = -1;
//
// 	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
// 	if (!png_ptr)
// 		goto finish;
//
// 	info_ptr = png_create_info_struct(png_ptr);
// 	if (!info_ptr)
// 		goto finish;
//
// 	if (setjmp(png_jmpbuf(png_ptr)))
// 		goto finish;
//
// 	png_init_io(png_ptr, file);
//
// 	png_set_IHDR(png_ptr, info_ptr, w, h, 8,
// 	             PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
// 	             PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
//
// 	png_write_info(png_ptr, info_ptr);
//
// 	if (png_write_rgb565(buf, png_ptr, w, h))
// 		goto finish;
//
// 	if (setjmp(png_jmpbuf(png_ptr)))
// 		goto finish;
//
// 	png_write_end(png_ptr, info_ptr);
// 	ret = 0;
//
// finish:
// 	png_destroy_write_struct(&png_ptr, &info_ptr);
// 	return ret;
// }

int screenshot(void) {
	return 0;
// 	FILE *fp;
// 	char filename[MAX_PATH];
// 	int w, h;
// 	void *buf = plat_prepare_screenshot(&w, &h, NULL);
// 	int ret = -1;
//
// 	if (screenshot_file_name(filename, MAX_PATH)) {
// 		PA_ERROR("No available filename for screenshot\n");
// 		return -1;
// 	}
//
// 	fp = fopen(filename, "wb");
// 	if (!fp)
// 		goto finish;
//
// 	if (write_png(buf, w, h, fp))
// 		goto finish;
//
// 	PA_INFO("Wrote screenshot to %s\n", filename);
// 	ret = 0;
//
// finish:
// 	if (fp)
// 		fclose(fp);
// 	return ret;
}

/* OSD overlay (battery, volume) is drawn by cubevol on /dev/fb1 (ARGB plane).
 * Hide: mmap fb1, memset zeros (cubevol has no signal handler so it won't
 *       redraw on its own — clear once is enough until next restart).
 * Show: restart cubevol via killall + spawn; the new instance draws battery
 *       and volume on init. cubevol shmem (/tmp/joy_key) is kernel-owned so
 *       readers (game cores) survive the restart. */
#include <sys/mman.h>
#include <linux/fb.h>
#include <stdlib.h>
static int g_is_frogui = 0;
static void fb1_clear(void) {
	int fd = open("/dev/fb1", O_RDWR);
	if (fd < 0) return;
	struct fb_fix_screeninfo finfo;
	if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0 && finfo.smem_len > 0) {
		void *mem = mmap(NULL, finfo.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (mem != MAP_FAILED) {
			memset(mem, 0, finfo.smem_len);
			munmap(mem, finfo.smem_len);
		}
	}
	close(fd);
}
/* Only FrogUI restores OSD (via its own retro_init restart). picoarch's
 * menu and game-resume keep fb1 cleared. */
static void fb1_blank(int blank) { if (blank) fb1_clear(); }
static void fb1_menu_enter(void) {
	if (g_is_frogui) return;
	/* Filter is locked per-process (set from FrogUI settings at startup);
	 * no need to track or react to filter changes mid-game. */
}
static void fb1_menu_exit(void) {
	if (g_is_frogui) return;
}

void set_defaults(void)
{
	show_fps = 0;
	show_cpu = 0;
	show_hud = 1;
	limit_frames = 1;
	enable_audio = 1;
	audio_buffer_size = 5;
	scale_size = SCALE_SIZE_NONE;
	scale_filter = SCALE_FILTER_NEAREST;
	scale_effect = default_scale_effect;
	optimize_text = 1;
	// max_upscale = 8;
	
	scale_update_scaler();

	if (current_audio_buffer_size < audio_buffer_size)
		current_audio_buffer_size = audio_buffer_size;

	for (size_t i = 0; i < core_options.len; i++) {
		const char *key = options_get_key(i);
		if (key)
			core_options.entries[i].value = core_options.entries[i].default_value;
	}

	options_update_changed();
}

int save_config(int is_game)
{
	char config_filename[MAX_PATH];
	FILE *config_file;

	config_file_name(config_filename, MAX_PATH, is_game);
	DBG("DBG save_config(is_game=%d) → %s\n", is_game, config_filename);
	config_file = fopen(config_filename, "wb");
	if (!config_file) {
		fprintf(stderr, "Could not write config to %s (errno=%d)\n", config_filename, errno);
		return -1;
	}

	config_write(config_file);
	config_write_keys(config_file);

	fflush(config_file);
	fsync(fileno(config_file));
	fclose(config_file);
	DBG("DBG save_config: fclose done\n");

	if (is_game)
		config_override = 1;

	return 0;
}

static void alloc_config_buffer(char **config_ptr) {
	char config_filename[MAX_PATH];
	FILE *config_file;
	size_t length;
	config_override = 0;

	config_file_name(config_filename, MAX_PATH, 1);
	DBG("DBG alloc_config_buffer: try game-cfg=%s\n", config_filename);
	config_file = fopen(config_filename, "rb");
	if (config_file) {
		config_override = 1;
		DBG("DBG alloc_config_buffer: game-cfg HIT\n");
	} else {
		config_file_name(config_filename, MAX_PATH, 0);
		DBG("DBG alloc_config_buffer: try global-cfg=%s\n", config_filename);
		config_file = fopen(config_filename, "rb");
		DBG("DBG alloc_config_buffer: global-cfg %s\n", config_file ? "HIT" : "MISS");
	}

	if (!config_file)
		return;

	PA_INFO("Loading config from %s\n", config_filename);

	fseek(config_file, 0, SEEK_END);
	length = ftell(config_file);
	fseek(config_file, 0, SEEK_SET);

	*config_ptr = calloc(1, length);

	fread(*config_ptr, 1, length, config_file);
	fclose(config_file);
}

void load_config(void)
{
	char *config = NULL;
	alloc_config_buffer(&config);

	if (config) {
		config_read(config);
		free(config);
	}
	plat_reinit();
}

void load_config_keys(void)
{
	char *config = NULL;
	int kcount = 0;
	const int *defbinds = NULL;

	alloc_config_buffer(&config);

	if (config) {
		config_read_keys(config);
		free(config);

		/* Force input device 0 menu to be bound to the default key */
		in_get_config(0, IN_CFG_BIND_COUNT, &kcount);
		defbinds = in_get_dev_def_binds(0);

		for(int i = 0; i < kcount; i++) {
			if (defbinds[IN_BIND_OFFS(i, IN_BINDTYPE_EMU)] == 1 << EACTION_MENU) {
				in_bind_key(0, i, 1 << EACTION_MENU, IN_BINDTYPE_EMU, 0);
			}
		}
	}
}

int remove_config(int is_game) {
	char config_filename[MAX_PATH];
	int ret;

	config_file_name(config_filename, MAX_PATH, is_game);
	ret = remove(config_filename);
	if (ret == 0)
		config_override = 0;

	return ret;
}

void handle_emu_action(emu_action action)
{
	static emu_action prev_action = EACTION_NONE;
	if (prev_action != EACTION_NONE && prev_action == action) return;

	switch (action)
	{
	case EACTION_NONE:
		break;
	case EACTION_MENU:
	{
#ifdef PLATFORM_SF3000
		extern int sf3000_use_hwdisp;
		DBG("DBG EACTION_MENU: use_hwdisp=%d filter=%d\n",
		        sf3000_use_hwdisp, scale_filter);
#endif
		toggle_fast_forward(1); /* Force FF off */
		sram_write();
		DBG("DBG EACTION_MENU: sram_write done\n");
#ifdef PLATFORM_SF3000
		/* Auto-resume snapshot on menu open — only safe point to save
		 * mid-game (game is paused while menu is up). */
		if (g_auto_resume && !g_is_frogui) {
			int prev = state_slot;
			state_slot = 99;
			state_write();
			state_slot = prev;
			sync();
			DBG("DBG auto-resume: state saved to slot 99 (menu open)\n");
		}
#endif
	}
#ifdef MMENU
		if (mmenu && content && content->path) {
			ShowMenu_t ShowMenu = (ShowMenu_t)dlsym(mmenu, "ShowMenu");
			MenuReturnStatus status = ShowMenu(content->path, state_allowed() ? save_template_path : NULL, plat_clean_screen());
			char disc_path[256];
			ChangeDisc_t ChangeDisc = (ChangeDisc_t)dlsym(mmenu, "ChangeDisc");

			if (status == kStatusExitGame) {
				should_quit = 1;
				plat_video_menu_leave();
			} else if (status == kStatusChangeDisc && ChangeDisc(disc_path)) {
				disc_replace_index(0, disc_path);
			} else if (status == kStatusOpenMenu) {
				plat_video_flip();
				fb1_menu_enter();
				menu_loop();
				fb1_menu_exit();
			} else if (status >= kStatusLoadSlot) {
				state_slot = status - kStatusLoadSlot;
				state_read();
			} else if (status >= kStatusSaveSlot) {
				state_slot = status - kStatusSaveSlot;
				state_write();
			}

			// release that menu key
			SDL_Event sdlevent;
			sdlevent.type = SDL_KEYUP;
			sdlevent.key.keysym.sym = SDLK_ESCAPE;
			SDL_PushEvent(&sdlevent);
		}
		else {
#endif
			fb1_menu_enter();
			DBG("DBG menu_loop: ENTER\n");
			menu_loop();
			DBG("DBG menu_loop: EXIT\n");
			fb1_menu_exit();
#ifdef MMENU
		}
#endif
		break;
	case EACTION_TOGGLE_HUD:
		show_hud = !show_hud;
		/* Force the hud to clear */
		plat_video_set_msg(NULL, 0, 0);
		break;
	case EACTION_SAVE_STATE:
		state_write();
		break;
	case EACTION_LOAD_STATE:
		state_read();
		break;
	case EACTION_TOGGLE_FF:
		toggle_fast_forward(0);
		break;
	case EACTION_SCREENSHOT:
		screenshot();
		break;
	case EACTION_QUIT:
		should_quit = 1;
		break;
	default:
		break;
	}

	prev_action = action;
}

void pa_log(enum retro_log_level level, const char *fmt, ...) {
	char buf[1024] = {0};
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	switch(level) {
#ifdef DEBUG
	case RETRO_LOG_DEBUG:
		printf("DEBUG: %s", buf);
		break;
#endif
	case RETRO_LOG_INFO:
		printf("INFO: %s", buf);
		break;
	case RETRO_LOG_WARN:
		fprintf(stderr, "WARN: %s", buf);
		break;
	case RETRO_LOG_ERROR:
		fprintf(stderr, "ERROR: %s", buf);
		break;
	default:
		break;
	}
}

static void show_startup_message(void) {
	const struct core_override *override = get_overrides();
	if (override && override->startup_msg) {
		plat_video_set_msg(override->startup_msg->msg, 2, override->startup_msg->msec);
	}
}

void pa_track_render(void) {
	renders++;
}

#define CPU_MSG_LEN 10
static void count_fps(void)
{
	char msg[HUD_LEN];
	static unsigned int nextsec = 0;
	static unsigned last_cpu_ticks = 0;
	unsigned int ticks = 0;

	if (show_hud && (show_fps || show_cpu)) {
		ticks = plat_get_ticks_ms();
		if (ticks > nextsec) {
			float last_time = (ticks - nextsec + 1000) / 1000.0f;

			if (last_time > 0) {
				char cpu_msg[CPU_MSG_LEN];
				char fps_msg[HUD_LEN - CPU_MSG_LEN];

				cpu_msg[0] = fps_msg[0] = '\0';
				nextsec = ticks + 1000;

				if (show_fps) {
					float vsyncsps = vsyncs / last_time;
					float rendersps = renders / last_time;
					vsyncs = 0;
					renders = 0;
					snprintf(fps_msg, sizeof(fps_msg), "FPS: %.1f (%.0f)", rendersps, vsyncsps);
				}

				if (show_cpu) {
					unsigned cpu_ticks = plat_cpu_ticks();
					if (cpu_ticks && last_cpu_ticks) {
						float cpu_percent = (cpu_ticks - last_cpu_ticks) / last_time;
						snprintf(cpu_msg, sizeof(cpu_msg), "%.1f%%", cpu_percent);
					}
					last_cpu_ticks = cpu_ticks;
				}

				snprintf(msg, HUD_LEN, "%-*s%*s",
				         (HUD_LEN - CPU_MSG_LEN - 1), fps_msg,
				         CPU_MSG_LEN - 1, cpu_msg);
				plat_video_set_msg(msg, 1, 1100);
			}
		}
		vsyncs++;

	}
}

static void adjust_audio(void) {
	static unsigned prev_audio_buffer_size = 0;

	if (!prev_audio_buffer_size)
		prev_audio_buffer_size = current_audio_buffer_size;

	current_audio_buffer_size = MAX(audio_buffer_size, audio_buffer_size_override);

	if (prev_audio_buffer_size != current_audio_buffer_size) {
		PA_INFO("Resizing audio buffer from %d to %d frames\n",
			prev_audio_buffer_size,
			current_audio_buffer_size);

		plat_sound_resize_buffer();
		prev_audio_buffer_size = current_audio_buffer_size;
	}

	if (current_core.retro_audio_buffer_status) {
		float occupancy = 1.0 - plat_sound_capacity();
		current_core.retro_audio_buffer_status(true, (int)(occupancy * 100), occupancy < 0.50);
	}
}

static void get_tag_name(const char* in_path, char* out_tag) {
	char tmp[MAX_PATH];
	char *slash, *paren;

	strncpy(tmp, in_path, MAX_PATH - 1);
	tmp[MAX_PATH - 1] = '\0';

	/* strip filename: remove everything after last '/' */
	slash = strrchr(tmp, '/');
	if (slash) *slash = '\0';

	/* parent dir name is the tag (e.g. "gb" from .../roms/gb/game.gbc) */
	slash = strrchr(tmp, '/');
	strncpy(out_tag, slash ? slash + 1 : tmp, MAX_PATH - 1);
	out_tag[MAX_PATH - 1] = '\0';

	/* extract pak name from parentheses if present: "PS1 (PCSX)" → "PCSX" */
	paren = strchr(out_tag, '(');
	if (paren) {
		char *close = strchr(paren + 1, ')');
		if (close) {
			*close = '\0';
			memmove(out_tag, paren + 1, strlen(paren + 1) + 1);
		}
	}
}

#ifdef PLATFORM_SF3000
/* ---- Rewind: per-frame serialized-state ring. Hold SELECT+B to rewind. ----
 * Each normal frame we serialize the core state into a ring. While rewinding we
 * drop the newest slot and unserialize the now-newest, then retro_run() renders
 * it (its +1 advance is discarded by the next step) → motion goes backward.
 * Capped by a RAM budget; disabled for cores whose state is too big. */
#define REWIND_BUDGET (16 * 1024 * 1024)
#define REWIND_INTERVAL 6              /* capture every 6 frames (~10Hz) */
static unsigned char *g_rw_buf  = NULL;
static size_t         g_rw_ssize = 0;
static int            g_rw_cap = 0, g_rw_head = 0, g_rw_count = 0;

static void rewind_init(void) {
	if (g_is_frogui || !current_core.retro_serialize_size) return;
	size_t s = current_core.retro_serialize_size();
	if (s == 0 || s > REWIND_BUDGET) return;
	int cap = (int)(REWIND_BUDGET / s);
	if (cap < 2) return;
	if (cap > 1200) cap = 1200;          /* ~20s @60fps */
	g_rw_buf = malloc((size_t)cap * s);
	if (!g_rw_buf) return;
	g_rw_ssize = s; g_rw_cap = cap; g_rw_head = 0; g_rw_count = 0;
	DBG("DBG rewind: state=%zu cap=%d (%.1fs)\n", s, cap, cap / 60.0);
}

static void rewind_capture(void) {
	if (!g_rw_buf) return;
	if (current_core.retro_serialize(g_rw_buf + (size_t)g_rw_head * g_rw_ssize, g_rw_ssize)) {
		g_rw_head = (g_rw_head + 1) % g_rw_cap;
		if (g_rw_count < g_rw_cap) g_rw_count++;
	}
}

static int rewind_back(void) {
	if (!g_rw_buf || g_rw_count <= 1) return 0;
	g_rw_head = (g_rw_head - 1 + g_rw_cap) % g_rw_cap;   /* drop newest */
	g_rw_count--;
	int slot = (g_rw_head - 1 + g_rw_cap) % g_rw_cap;    /* now-newest */
	current_core.retro_unserialize(g_rw_buf + (size_t)slot * g_rw_ssize, g_rw_ssize);
	return 1;
}

static int rewind_held(void) {
	extern volatile uint32_t *sf3000_keys_ptr;
	if (!sf3000_keys_ptr) return 0;
	uint32_t r = *sf3000_keys_ptr;
	return (r & ((1u << 0) | (1u << 14))) == ((1u << 0) | (1u << 14)); /* SELECT+B */
}
#endif

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_sigaction = sigsegv_handler;
		sa.sa_flags = SA_SIGINFO;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS,  &sa, NULL);
		sigaction(SIGABRT, &sa, NULL);
		sigaction(SIGILL,  &sa, NULL);
		sigaction(SIGFPE,  &sa, NULL);
	}
#ifdef PLATFORM_SF3000
	dbg_log("DBG picoarch start: text~%p (hi if 0x2x) argv1=%s next-bin=%s\n",
	        (void *)&picoarch_for_core, argc > 1 ? argv[1] : "?",
	        argc > 1 ? picoarch_for_core(argv[1]) : "?");
	/* AUTO-RESUME: if FrogUI launch and a last-game marker exists with
	 * auto_resume=on, redirect to that game with state restore.  Marker
	 * cleared on clean exit to FrogUI (Quit). */
	if (argc > 1 && strcmp(argv[1], FROGUI_CORE) == 0) {
		load_frogui_settings();
		if (g_auto_resume) {
			char lg_core[512], lg_rom[512];
			if (read_last_game(lg_core, sizeof(lg_core), lg_rom, sizeof(lg_rom))) {
				DBG("DBG main: auto-resume redirect → %s + %s\n",
				        lg_core, lg_rom);
				setenv("PICOARCH_AUTO_RESUME", "1", 1);
				execl(picoarch_for_core(lg_core), "picoarch", lg_core, lg_rom, NULL);
				/* fall through on execl failure */
			}
		}
	}
#endif
	char content_path[MAX_PATH];
	char tag_name[MAX_PATH];

	if (argc > 1) {
		if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
			printf("Usage: picoarch libretro_core content [NONE DMG LCD SCANLINE]\n");
			return 0;
		}
	}

	if (plat_init()) {
		quit(-1);
	}

	if (menu_init()) {
		quit(-1);
	}

	if (argc > 1 && argv[1]) {
		strncpy(core_path, argv[1], sizeof(core_path) - 1);
	} else {
		quit(-1);
	}

	if (argc > 2 && argv[2]) {
		strncpy(content_path, argv[2], sizeof(content_path) - 1);
	} else {
		quit(-1);
	}
	
	if (argc > 3 && argv[3]) {
		if (!strcmp(argv[3],"NONE")) default_scale_effect = SCALE_EFFECT_NONE;
		else if (!strcmp(argv[3],"DMG")) default_scale_effect = SCALE_EFFECT_DMG;
		else if (!strcmp(argv[3],"LCD")) default_scale_effect = SCALE_EFFECT_LCD;
		else default_scale_effect = SCALE_EFFECT_SCANLINE;
	}
	
	get_tag_name(content_path, tag_name);
	core_extract_name(core_path, core_name, sizeof(core_name));

	if (core_open(core_path, tag_name)) {
		quit(-1);
	}

	content = content_init(content_path);
	if (!content) {
		PA_ERROR("Couldn't allocate memory for content path\n");
		quit(-1);
	}

	set_defaults();
	load_config();
	DBG("DBG main: post-load_config scale_filter=%d scale_size=%d override=%d\n",
	        scale_filter, scale_size, config_override);
#ifdef PLATFORM_SF3000
	/* Filter + auto_resume from FrogUI settings (games only; FrogUI stays SW). */
	if (strcmp(core_path, FROGUI_CORE) != 0) {
		load_frogui_settings();
		/* Record current game as "last game" so a power-cycle resumes it. */
		if (g_auto_resume) {
			write_last_game(core_path, content_path);
			/* Always try slot 99 auto state on game launch when auto_resume
			 * is on (boot redirect OR manual FrogUI launch).  state_resume
			 * silently falls through if slot 99 file doesn't exist. */
			resume_slot = 99;
			DBG("DBG main: auto-resume → resume_slot=99\n");
		}
	}
	DBG("DBG main: post-FrogUI-override scale_filter=%d auto_resume=%d\n",
	        scale_filter, g_auto_resume);
#endif
	core_load();

	if (core_load_content(content)) {
		quit(-1);
	}

	/* Hide cubevol's battery/volume OSD (/dev/fb1) during gameplay. Skipped
	 * for FrogUI (the menu core) so the menu still shows battery. */
	g_is_frogui = (strcmp(core_path, FROGUI_CORE) == 0);
	if (!g_is_frogui) fb1_blank(1);

	load_config_keys();

#ifdef MMENU

	mmenu = dlopen("libmmenu.so", RTLD_LAZY);
	if (mmenu) {
		ResumeSlot_t ResumeSlot = (ResumeSlot_t)dlsym(mmenu, "ResumeSlot");
		if (ResumeSlot) resume_slot = ResumeSlot();
	}
#endif
	show_startup_message();
	state_resume();
#ifdef PLATFORM_SF3000
	rewind_init();
#endif

	do {
		count_fps();
		adjust_audio();
#ifdef PLATFORM_SF3000
		if (g_rw_buf && rewind_held() && rewind_back()) {
			current_core.retro_run();          /* render restored frame */
		} else {
			static int rw_fc = 0;
			current_core.retro_run();
			/* capture every REWIND_INTERVAL frames — per-frame serialize is too
			 * heavy (chops audio) and drains the ring too fast. */
			if (g_rw_buf && (++rw_fc % REWIND_INTERVAL) == 0)
				rewind_capture();
		}
#else
		current_core.retro_run();
#endif
		if (!should_quit)
			plat_video_flip();
	} while (!should_quit);

	return quit(0);
}

/* moved to top of file */

int quit(int code) {
	menu_finish();
#ifdef PLATFORM_SF3000
	/* Final auto-resume save before unloading core. */
	if (g_auto_resume && !g_is_frogui && current_core.retro_unload_game) {
		int prev = state_slot;
		state_slot = 99;
		state_write();
		state_slot = prev;
		sync();
		DBG("DBG quit: auto-resume final save to slot 99\n");
	}
#endif
	core_unload();
	fb1_blank(0);   /* restore OSD overlay; next process re-blanks if needed */

#ifdef PLATFORM_SF3000
	/* exec() BEFORE plat_finish() — keeps fb0/dis fds open across exec.
	 * But first tear down HW display path: if hwdisp was active the
	 * display controller is pointed at driver.so's buffer, not fb0.
	 * video_driver_deinit restores fb0 so the next process can use it. */
	extern int sf3000_use_hwdisp;
	extern int  hwdisp_init(void);
	extern void hwdisp_deinit(void);

	/* Read launch.txt first to know the next process. */
	int next_is_standalone = 0;
	char core_path[512] = "", rom_path[512] = "", standalone_rom[512] = "";
	FILE *lf = fopen(LAUNCH_FILE, "r");
	if (lf) {
		if (fgets(core_path,      sizeof(core_path),      lf)) core_path[strcspn(core_path,           "\n")] = 0;
		if (fgets(rom_path,       sizeof(rom_path),       lf)) rom_path[strcspn(rom_path,             "\n")] = 0;
		if (fgets(standalone_rom, sizeof(standalone_rom), lf)) standalone_rom[strcspn(standalone_rom, "\n")] = 0;
		fclose(lf);
		unlink(LAUNCH_FILE);
		next_is_standalone = (strcmp(core_path, "standalone") == 0 && rom_path[0]);
	}

	DBG("DBG quit: use_hwdisp=%d next_standalone=%d core_path[0]=%d rom_path[0]=%d\n",
	        sf3000_use_hwdisp, next_is_standalone, !!core_path[0], !!rom_path[0]);
	/* Clean exit to FrogUI: clear last-game marker so next boot lands on
	 * FrogUI instead of auto-resuming.  Going to another game/standalone
	 * doesn't clear — that new launch will overwrite the marker. */
	if (!core_path[0] && !rom_path[0]) {
		clear_last_game();
		DBG("DBG quit: cleared last_game marker (going to FrogUI)\n");
	}
	/* WARM-BOOT marker: if we leave HCGE active, the next picoarch process
	 * (same PPID = same icube shell child) early-inits hwdisp in
	 * sf3000_fb_init so FrogUI doesn't try to SW-render onto a fb0 still
	 * routed through driver.so (would squish). */
	if (sf3000_use_hwdisp || next_is_standalone) {
		FILE *m = fopen("/tmp/picoarch_hcge_was_active", "w");
		if (m) { fprintf(m, "%d", (int)getppid()); fclose(m); }
	}
	if (!next_is_standalone) {
		hwdisp_deinit();
		DBG("DBG quit: hwdisp_deinit done\n");
	}
	/* Keep fb0/dis fds open across exec — closing them breaks the panel
	 * state recovery (was working in commit 6537239). */
	fflush(stderr);
	fsync(STDERR_FILENO);
	if (core_path[0] && rom_path[0]) {
		if (next_is_standalone) {
			chmod(rom_path, 0755);
			execl(rom_path, rom_path, standalone_rom[0] ? standalone_rom : NULL, NULL);
		} else {
			execl(picoarch_for_core(core_path), "picoarch", core_path, rom_path, NULL);
		}
	}
	execl(PICOARCH_BIN, "picoarch", FROGUI_CORE, FROGUI_CORE, NULL);
	/* execl failed — fall through to normal cleanup and exit */
#endif

	plat_finish();
	exit(code);
}
