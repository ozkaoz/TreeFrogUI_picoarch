/* sf3000-hwdisp implementation. dlopen's driver.so, calls video_driver_*.
 *
 * Driver scales src dims → panel (854x480) via HCGE DMA with bilinear filter.
 * Two filter modes:
 *   - HW (default): pass src as-is, driver scales (bilinear).
 *   - Nearest: SW nearest-upscale src to driver native (1280x720) framing,
 *     so driver doesn't scale further → pixels stay sharp.
 *
 * Aspect-pad: when target aspect set, source is centered horizontally with
 * black pillar-bars so driver's stretch becomes uniform. */

#include "hwdisp.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
extern void dbg_log(const char *fmt, ...);
#define DBG(...) dbg_log(__VA_ARGS__)

#ifndef HW_NATIVE_W
#define HW_NATIVE_W 1280
#endif
#ifndef HW_NATIVE_H
#define HW_NATIVE_H  720
#endif
#define HW_W   HW_NATIVE_W
#define HW_H   HW_NATIVE_H
#define HW_PITCH (HW_W * 2)
#define HW_BUFSZ (HW_W * HW_H * 2)

static void   *g_handle = NULL;
static int     g_active = 0;

typedef int  (*fn_init_t)(void);
typedef void (*fn_deinit_t)(void);
typedef int  (*fn_disp_t)(void *src, int w, int h, int pitch);

static fn_init_t   p_init   = NULL;
static fn_deinit_t p_deinit = NULL;
static fn_disp_t   p_disp   = NULL;

/* Aspect-pad staging buffer (lazy alloc, resized on demand) */
static uint16_t *g_pad_buf  = NULL;
static int       g_pad_cap  = 0;
static int       g_pad_w    = 0;
static int       g_pad_h    = 0;

/* Nearest-upscale buffer (always 1280x720) */
static uint16_t *g_near_buf = NULL;

static int g_aspect_num = 0;
static int g_aspect_den = 0;
static int g_filter_nearest = 0;

/* Direct-fb present: after video_drivers_init the driver reconfigures fb0 to its
 * native landscape geometry (R36SX: 1280x720 RGB565) and programs the display
 * controller to scale fb0 → physical panel (640x480), rotate:0. The panel scans
 * fb0 continuously, so we present by writing RGB565 straight into fb0 — no
 * video_driver_disp_frame (which hard-hangs on the engine sync on this driver). */
static int       g_fbfd    = -1;
static uint16_t *g_fbmem   = NULL;
static int       g_fbw     = 0;   /* fb visible width  (px) */
static int       g_fbh     = 0;   /* fb visible height (px) */
static int       g_fbstride= 0;   /* fb row stride     (px) */
static long      g_fbsize  = 0;   /* mmap length (bytes) */

static void hwdisp_fb_open(void) {
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    g_fbfd = open("/dev/fb0", O_RDWR);
    if (g_fbfd < 0) { DBG("DBG fbwrite: open fb0 failed\n"); return; }
    if (ioctl(g_fbfd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(g_fbfd, FBIOGET_FSCREENINFO, &fi) < 0) {
        DBG("DBG fbwrite: ioctl GET info failed\n");
        close(g_fbfd); g_fbfd = -1; return;
    }
    g_fbw      = vi.xres;
    g_fbh      = vi.yres;
    g_fbstride = fi.line_length / 2;       /* bytes → px (RGB565) */
    g_fbsize   = fi.smem_len;
    g_fbmem = (uint16_t *)mmap(NULL, g_fbsize, PROT_READ|PROT_WRITE, MAP_SHARED, g_fbfd, 0);
    if (g_fbmem == MAP_FAILED) {
        DBG("DBG fbwrite: mmap failed\n");
        g_fbmem = NULL; close(g_fbfd); g_fbfd = -1; return;
    }
    DBG("DBG fbwrite: fb0 %dx%d stride=%dpx bpp=%d size=%ld OK\n",
        g_fbw, g_fbh, g_fbstride, vi.bits_per_pixel, g_fbsize);
}

/* Write src(w×h RGB565) directly to fb0, nearest-scaled to fill it.
 *   R36SX: landscape fb, rotate:0 → straight scale.
 *   SF3000: panel is 480x854 portrait-mounted; the driver's disp_frame normally
 *           rotates 90°, but we bypass it, so rotate the frame 90° CW here.
 * Returns 1 if drawn. */
static int hwdisp_fb_present(const void *src, int w, int h, int pitch_bytes) {
    if (!g_fbmem || g_fbw <= 0 || g_fbh <= 0 || w <= 0 || h <= 0) return 0;
    extern int sf3000_is_r36sx(void);
    const int sp = pitch_bytes / 2;
    const uint16_t *s = (const uint16_t *)src;
    int draw_w = g_fbw < 2048 ? g_fbw : 2048;

    if (!sf3000_is_r36sx()) {
        /* SF3000: 90° CW rotate. fb row → src x; fb col(reversed) → src y. */
        static int symap[2048];
        static int last_h = -1, last_fbw = -1;
        if (h != last_h || g_fbw != last_fbw) {
            for (int dx = 0; dx < draw_w; dx++) symap[dx] = (h - 1) - (dx * h / g_fbw);
            last_h = h; last_fbw = g_fbw;
        }
        for (int dy = 0; dy < g_fbh; dy++) {
            int sx = dy * w / g_fbh;
            uint16_t *drow = g_fbmem + (size_t)dy * g_fbstride;
            for (int dx = 0; dx < draw_w; dx++)
                drow[dx] = s[(size_t)symap[dx] * sp + sx];
        }
        return 1;
    }

    /* R36SX: straight scale. */
    static int xmap[2048];
    static int last_w = -1, last_fbw = -1;
    if (w != last_w || g_fbw != last_fbw) {
        for (int dx = 0; dx < draw_w; dx++) xmap[dx] = dx * w / g_fbw;
        last_w = w; last_fbw = g_fbw;
    }
    for (int dy = 0; dy < g_fbh; dy++) {
        int sy = dy * h / g_fbh;
        const uint16_t *srow = s + sy * sp;
        uint16_t *drow = g_fbmem + (size_t)dy * g_fbstride;
        for (int dx = 0; dx < draw_w; dx++)
            drow[dx] = srow[xmap[dx]];
    }
    return 1;
}

int hwdisp_init(void) {
    extern void sf3000_dump_fb_state(const char *);
    if (g_active) return 0;
    sf3000_dump_fb_state("hwdisp_init/pre");

    /* Device-specific driver: R36SX and SF3000 ship different driver.so builds
     * (panel init + render behavior differ). Pick by detected device; fall back
     * to a generic driver.so if the per-device file is absent. */
    extern int sf3000_is_r36sx(void);
    const char *drv = sf3000_is_r36sx()
        ? "/mnt/sdcard/cubegm/driver_r36sx.so"
        : "/mnt/sdcard/cubegm/driver_sf3000.so";
    g_handle = dlopen(drv, RTLD_NOW | RTLD_GLOBAL);
    if (!g_handle) {
        DBG("DBG hwdisp: dlopen %s failed (%s), trying driver.so\n", drv, dlerror());
        g_handle = dlopen("/mnt/sdcard/cubegm/driver.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_handle) {
        fprintf(stderr, "hwdisp: dlopen failed: %s\n", dlerror());
        return -1;
    }
    DBG("DBG hwdisp: loaded %s\n", drv);

    p_init   = (fn_init_t)  dlsym(g_handle, "video_drivers_init");
    p_deinit = (fn_deinit_t)dlsym(g_handle, "video_driver_deinit");
    p_disp   = (fn_disp_t)  dlsym(g_handle, "video_driver_disp_frame");

    if (!p_init || !p_deinit || !p_disp) {
        fprintf(stderr, "hwdisp: dlsym failed (init=%p deinit=%p disp=%p)\n",
                p_init, p_deinit, p_disp);
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }

    int rv = p_init();
    if (rv <= 0) {
        fprintf(stderr, "hwdisp: video_drivers_init returned %d\n", rv);
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }

    g_active = 1;
    fprintf(stderr, "hwdisp: HW path active (init rv=%d)\n", rv);

    /* R36SX: enable the HCGE engine via video_driver_setmode so disp_frame's
     * engine-sync doesn't hang (rkgame configures before presenting). disp_frame
     * then HW-scales src→panel (no CPU upscale). Try a few mode args; logged
     * fsync'd so a hang still tells us how far it got. */
    if (sf3000_is_r36sx()) {
        typedef int (*fn_setmode_t)(int, int);
        fn_setmode_t p_setmode = (fn_setmode_t)dlsym(g_handle, "video_driver_setmode");
        DBG("DBG hwdisp: setmode=%p calling setmode(0,0)\n", (void*)p_setmode);
        if (p_setmode) { int sr = p_setmode(0, 0); DBG("DBG hwdisp: setmode(0,0) ret=%d\n", sr); }
    }
    sf3000_dump_fb_state("hwdisp_init/post");
    /* fb0 for direct presents is mmap'd lazily by hwdisp_present_direct(). */
    return 0;
}

int hwdisp_active(void) { return g_active; }

void hwdisp_set_target_aspect(int num, int den) {
    g_aspect_num = num;
    g_aspect_den = den;
}

void hwdisp_set_filter(int nearest) {
    g_filter_nearest = nearest ? 1 : 0;
    /* If switching to nearest, ensure native buffer exists. */
    if (g_filter_nearest && !g_near_buf) {
        g_near_buf = (uint16_t*)malloc(HW_BUFSZ);
        if (g_near_buf) memset(g_near_buf, 0, HW_BUFSZ);
    }
}

/* Pad horizontally: src(w×h) → g_pad_buf(pad_w×h), src centered, sides black. */
static void pad_horizontal(const void *src, int w, int h, int pitch_bytes, int pad_w) {
    int need = pad_w * h;
    if (need > g_pad_cap) {
        free(g_pad_buf);
        g_pad_cap = need + 4096;
        g_pad_buf = (uint16_t*)malloc(g_pad_cap * sizeof(uint16_t));
        g_pad_h   = 0;
        g_pad_w   = 0;
    }
    if (!g_pad_buf) return;

    int off_x = (pad_w - w) / 2;
    if (off_x < 0) off_x = 0;

    if (pad_w != g_pad_w || h != g_pad_h) {
        memset(g_pad_buf, 0, (size_t)pad_w * h * sizeof(uint16_t));
        g_pad_w = pad_w;
        g_pad_h = h;
    }

    for (int y = 0; y < h; y++) {
        const uint16_t *srow = (const uint16_t *)((const char *)src + y * pitch_bytes);
        uint16_t *drow = g_pad_buf + (size_t)y * pad_w + off_x;
        memcpy(drow, srow, (size_t)w * sizeof(uint16_t));
    }
}

/* Nearest-upscale src into g_near_buf (1280×720). Pads with black to fit
 * target aspect if set. Otherwise full-stretch upscales to 1280×720.
 *
 * Fast paths:
 *   - Integer scale (dst_w = w*n, dst_h = h*m): unrolled replication +
 *     vertical row memcpy. Avoids per-pixel lookup tables.
 *   - Generic: xmap lookup. */
static void upscale_nearest(const void *src, int w, int h, int pitch_bytes) {
    if (!g_near_buf) return;

    int dst_w, dst_h;
    if (g_aspect_num > 0 && g_aspect_den > 0) {
        /* Integer scale preferring largest factor that still fits */
        int my = HW_H / h;
        if (my < 1) my = 1;
        int dw = w * my;
        if (dw > HW_W) {
            /* Width-limited: pick scale by width instead */
            my = HW_W / w; if (my < 1) my = 1;
            dw = w * my;
        }
        dst_h = h * my;
        dst_w = dw;
    } else {
        /* Full stretch: integer-snap to 1280×720 if possible */
        int mx = HW_W / w; if (mx < 1) mx = 1;
        int my = HW_H / h; if (my < 1) my = 1;
        dst_w = w * mx; dst_h = h * my;
    }
    int off_x = (HW_W - dst_w) / 2;
    int off_y = (HW_H - dst_h) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    /* Clear borders only when geometry changes */
    static int last_dst_w = -1, last_dst_h = -1;
    if (dst_w != last_dst_w || dst_h != last_dst_h) {
        memset(g_near_buf, 0, HW_BUFSZ);
        last_dst_w = dst_w; last_dst_h = dst_h;
    }

    const int sp = pitch_bytes / 2;
    const uint16_t *s = (const uint16_t *)src;
    const int nx = dst_w / w;    /* H replication factor (integer) */
    const int ny = dst_h / h;    /* V replication factor (integer) */

    /* Integer-scale fast path: expand one row, copy ny times.
     * Use uint32_t writes (2 px/word) where alignment permits. */
    if (nx >= 1 && ny >= 1 && nx * w == dst_w && ny * h == dst_h) {
        const int row_bytes = dst_w * 2;
        for (int sy = 0; sy < h; sy++) {
            const uint16_t *srow = s + sy * sp;
            uint16_t *drow = g_near_buf + (size_t)(sy * ny + off_y) * HW_W + off_x;

            switch (nx) {
            case 1:
                memcpy(drow, srow, (size_t)w * 2);
                break;
            case 2: {
                /* 1 src px → 1 uint32_t write (p|p<<16) */
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    d32[sx] = p | (p << 16);
                }
                break;
            }
            case 3:
                for (int sx = 0; sx < w; sx++) {
                    uint16_t p = srow[sx];
                    uint16_t *dp = drow + sx * 3;
                    dp[0] = p; dp[1] = p; dp[2] = p;
                }
                break;
            case 4: {
                /* 1 src px → 2 uint32_t writes */
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    uint32_t pp = p | (p << 16);
                    d32[sx*2  ] = pp;
                    d32[sx*2+1] = pp;
                }
                break;
            }
            case 5:
                for (int sx = 0; sx < w; sx++) {
                    uint16_t p = srow[sx];
                    uint16_t *dp = drow + sx * 5;
                    dp[0] = p; dp[1] = p; dp[2] = p; dp[3] = p; dp[4] = p;
                }
                break;
            case 6: {
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    uint32_t pp = p | (p << 16);
                    d32[sx*3  ] = pp;
                    d32[sx*3+1] = pp;
                    d32[sx*3+2] = pp;
                }
                break;
            }
            case 8: {
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    uint32_t pp = p | (p << 16);
                    d32[sx*4  ] = pp;
                    d32[sx*4+1] = pp;
                    d32[sx*4+2] = pp;
                    d32[sx*4+3] = pp;
                }
                break;
            }
            default:
                for (int sx = 0; sx < w; sx++) {
                    uint16_t p = srow[sx];
                    uint16_t *dp = drow + sx * nx;
                    for (int k = 0; k < nx; k++) dp[k] = p;
                }
                break;
            }

            /* Vertical replication: copy this row (ny-1) more times */
            for (int v = 1; v < ny; v++)
                memcpy(drow + (size_t)v * HW_W, drow, row_bytes);
        }
        return;
    }

    /* Generic fallback: xmap lookup */
    static int xmap[HW_W];
    static int last_w_map = -1, last_dst_w_map = -1;
    if (w != last_w_map || dst_w != last_dst_w_map) {
        for (int dx = 0; dx < dst_w; dx++) xmap[dx] = dx * w / dst_w;
        last_w_map = w; last_dst_w_map = dst_w;
    }
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = dy * h / dst_h;
        const uint16_t *srow = s + sy * sp;
        uint16_t *drow = g_near_buf + (size_t)(dy + off_y) * HW_W + off_x;
        for (int dx = 0; dx < dst_w; dx++)
            drow[dx] = srow[xmap[dx]];
    }
}

/* Direct present: write the frame straight into fb0; the display controller
 * scales fb0 → panel. Bypasses video_driver_disp_frame, which HANGS on R36SX
 * and ABORTS on SF3000 panel-size input. Used for FrogUI (both devices) and all
 * R36SX frames. Returns 1 if presented. */
/* Panel scale mode for R36SX disp_frame present: 0=integer(NONE), 1=aspect, 2=full.
 * disp_frame itself aspect-fits src→panel, so:
 *   aspect → pass src straight (driver aspect-fits, bars).
 *   full   → SW-stretch src into a 640x480 panel buffer → disp_frame 1:1 (fills).
 *   integer→ SW integer-replicate src centered in 640x480 buffer → disp_frame 1:1. */
#define PANEL_PW 640
#define PANEL_PH 480
static int g_panel_scale = 2;       /* default full */
static uint16_t *g_panelbuf = NULL; /* 640x480 cached staging */
void hwdisp_set_panel_scale(int m) { g_panel_scale = m; }

static uint16_t *panel_build(const void *src, int w, int h, int pitch_bytes, int integer) {
    if (!g_panelbuf) { g_panelbuf = (uint16_t*)malloc(PANEL_PW*PANEL_PH*2); if (!g_panelbuf) return NULL; }
    const int sp = pitch_bytes/2; const uint16_t *s = (const uint16_t*)src;
    if (integer) {
        int n = PANEL_PW/w; int ny = PANEL_PH/h; if (ny<n) n=ny; if (n<1) n=1;
        int dw=w*n, dh=h*n, ox=(PANEL_PW-dw)/2, oy=(PANEL_PH-dh)/2;
        memset(g_panelbuf, 0, PANEL_PW*PANEL_PH*2);
        for (int y=0; y<h; y++){ const uint16_t *sr=s+(size_t)y*sp;
            for (int ry=0; ry<n; ry++){ uint16_t *dr=g_panelbuf+(size_t)(oy+y*n+ry)*PANEL_PW+ox;
                for (int x=0;x<w;x++){ uint16_t px=sr[x]; for(int rx=0;rx<n;rx++) dr[x*n+rx]=px; } } }
    } else { /* full stretch */
        static int xm[PANEL_PW]; static int lw=-1;
        if (w!=lw){ for(int x=0;x<PANEL_PW;x++) xm[x]=x*w/PANEL_PW; lw=w; }
        for (int y=0;y<PANEL_PH;y++){ const uint16_t *sr=s+(size_t)(y*h/PANEL_PH)*sp;
            uint16_t *dr=g_panelbuf+(size_t)y*PANEL_PW; for(int x=0;x<PANEL_PW;x++) dr[x]=sr[xm[x]]; }
    }
    return g_panelbuf;
}

int hwdisp_present_direct(const void *src, int w, int h, int pitch_bytes) {
    static int s_n = 0;
    int lg = (s_n < 8);
    s_n++;
    if (!g_active || !src || !p_disp) return 0;
    /* HW present: disp_frame HW-scales src→panel (no CPU upscale). For full/integer
     * on game-size frames, pre-build a 640x480 panel buffer; aspect & panel-size
     * (FrogUI) pass straight. */
    const void *psrc = src; int pw = w, ph = h, ppitch = pitch_bytes;
    if (!(w == PANEL_PW && h == PANEL_PH) && g_panel_scale != 1 /*aspect=straight*/) {
        uint16_t *b = panel_build(src, w, h, pitch_bytes, g_panel_scale == 0);
        if (b) { psrc = b; pw = PANEL_PW; ph = PANEL_PH; ppitch = PANEL_PW*2; }
    }
    if (lg) DBG("DBG present_direct#%d: pre disp_frame %dx%d scale=%d\n", s_n, pw, ph, g_panel_scale);
    int rv = p_disp((void *)psrc, pw, ph, ppitch);
    if (lg) DBG("DBG present_direct#%d: post rv=%d\n", s_n, rv);
    return 1;
}

void hwdisp_present(const void *src, int w, int h, int pitch_bytes) {
    static int s_n = 0;
    int lg = (s_n < 8);
    s_n++;
    if (lg) DBG("DBG present#%d: src=%p w=%d h=%d pitch=%d active=%d p_disp=%p filt=%d asp=%d/%d HW=%dx%d\n",
                s_n, src, w, h, pitch_bytes, g_active, (void*)p_disp,
                g_filter_nearest, g_aspect_num, g_aspect_den, HW_W, HW_H);
    if (!g_active || !src) { if (lg) DBG("DBG present#%d: EARLY-RET\n", s_n); return; }
    if (!p_disp) { if (lg) DBG("DBG present#%d: no p_disp\n", s_n); return; }
    int rv;
    /* Nearest filter: SW upscale to 1280×720, driver does no further scale. */
    if (g_filter_nearest) {
        if (!g_near_buf) {
            g_near_buf = (uint16_t*)malloc(HW_BUFSZ);
            if (g_near_buf) memset(g_near_buf, 0, HW_BUFSZ);
        }
        if (g_near_buf) {
            upscale_nearest(src, w, h, pitch_bytes);
            if (lg) DBG("DBG present#%d: nearest pre p_disp(%p,%d,%d,%d)\n", s_n, (void*)g_near_buf, HW_W, HW_H, HW_PITCH);
            rv = p_disp(g_near_buf, HW_W, HW_H, HW_PITCH);
            if (lg) DBG("DBG present#%d: nearest post p_disp rv=%d\n", s_n, rv);
            return;
        }
        /* Fallthrough to HW path if alloc failed */
    }

    /* HW (bilinear) path: pass through, optional aspect pad. */
    if (g_aspect_num <= 0 || g_aspect_den <= 0) {
        if (lg) DBG("DBG present#%d: passthru pre p_disp(%p,%d,%d,%d)\n", s_n, src, w, h, pitch_bytes);
        rv = p_disp((void *)src, w, h, pitch_bytes);
        if (lg) DBG("DBG present#%d: passthru post p_disp rv=%d\n", s_n, rv);
        return;
    }

    int pad_w = h * g_aspect_num / g_aspect_den;
    if (pad_w <= w) {
        if (lg) DBG("DBG present#%d: nopad pre p_disp\n", s_n);
        rv = p_disp((void *)src, w, h, pitch_bytes);
        if (lg) DBG("DBG present#%d: nopad post p_disp rv=%d\n", s_n, rv);
        return;
    }

    pad_horizontal(src, w, h, pitch_bytes, pad_w);
    if (!g_pad_buf) {
        rv = p_disp((void *)src, w, h, pitch_bytes);
        if (lg) DBG("DBG present#%d: padfail post rv=%d\n", s_n, rv);
        return;
    }
    if (lg) DBG("DBG present#%d: pad pre p_disp(pad_w=%d)\n", s_n, pad_w);
    rv = p_disp(g_pad_buf, pad_w, h, pad_w * 2);
    if (lg) DBG("DBG present#%d: pad post p_disp rv=%d\n", s_n, rv);
}

/* Panel-integer present: SW nearest-upscale src by largest integer N where
 * N*w<=854 && N*h<=480, center result in 854x480 black panel buffer, send to
 * driver with filter=0 (pass-through). True integer pixel ratio on panel. */
#ifndef PANEL_W
#define PANEL_W 854
#endif
#ifndef PANEL_H
#define PANEL_H 480
#endif
#define PANEL_PITCH (PANEL_W * 2)

static uint16_t *g_panel_buf = NULL;

void hwdisp_present_integer(const void *src, int w, int h, int pitch_bytes) {
    if (!g_active || !p_disp || !src) return;
    if (w <= 0 || h <= 0) return;

    if (!g_panel_buf) {
        g_panel_buf = (uint16_t *)malloc(PANEL_W * PANEL_H * sizeof(uint16_t));
        if (!g_panel_buf) return;
        memset(g_panel_buf, 0, PANEL_W * PANEL_H * sizeof(uint16_t));
    }

    int sx = PANEL_W / w;
    int sy = PANEL_H / h;
    int n = sx < sy ? sx : sy;
    if (n < 1) n = 1;
    int dw = w * n, dh = h * n;
    if (dw > PANEL_W) dw = PANEL_W;
    if (dh > PANEL_H) dh = PANEL_H;
    int ox = (PANEL_W - dw) / 2;
    int oy = (PANEL_H - dh) / 2;

    /* Clear borders only when geometry changes */
    static int last_dw = -1, last_dh = -1;
    if (dw != last_dw || dh != last_dh) {
        memset(g_panel_buf, 0, PANEL_W * PANEL_H * sizeof(uint16_t));
        last_dw = dw; last_dh = dh;
    }

    const int sp = pitch_bytes / 2;
    const uint16_t *s = (const uint16_t *)src;

    /* Row expand (one src row → n dst rows), per-pixel replicate. */
    for (int srow_i = 0; srow_i < h; srow_i++) {
        const uint16_t *srow = s + srow_i * sp;
        uint16_t *drow = g_panel_buf + (size_t)(oy + srow_i * n) * PANEL_W + ox;

        switch (n) {
        case 1:
            memcpy(drow, srow, (size_t)w * 2);
            break;
        case 2: {
            uint32_t *d32 = (uint32_t *)drow;
            for (int x = 0; x < w; x++) {
                uint32_t p = srow[x];
                d32[x] = p | (p << 16);
            }
            break;
        }
        case 3:
            for (int x = 0; x < w; x++) {
                uint16_t p = srow[x];
                drow[x*3] = drow[x*3+1] = drow[x*3+2] = p;
            }
            break;
        case 4: {
            uint32_t *d32 = (uint32_t *)drow;
            for (int x = 0; x < w; x++) {
                uint32_t p = srow[x];
                uint32_t pp = p | (p << 16);
                d32[x*2] = pp; d32[x*2+1] = pp;
            }
            break;
        }
        default:
            for (int x = 0; x < w; x++) {
                uint16_t p = srow[x];
                uint16_t *dp = drow + x * n;
                for (int k = 0; k < n; k++) dp[k] = p;
            }
            break;
        }

        /* Vertical replication: copy this row n-1 more times */
        for (int v = 1; v < n; v++)
            memcpy(drow + (size_t)v * PANEL_W, drow, (size_t)dw * 2);
    }

    p_disp(g_panel_buf, PANEL_W, PANEL_H, PANEL_PITCH);
}

void hwdisp_deinit(void) {
    extern void sf3000_dump_fb_state(const char *);
    if (!g_active) { DBG("DBG hwdisp_deinit: not active\n"); return; }
    sf3000_dump_fb_state("hwdisp_deinit/pre");
    if (p_deinit) p_deinit();
    sf3000_dump_fb_state("hwdisp_deinit/post-p_deinit");
    if (g_fbmem) { munmap(g_fbmem, g_fbsize); g_fbmem = NULL; }
    if (g_fbfd >= 0) { close(g_fbfd); g_fbfd = -1; }
    if (g_pad_buf) { free(g_pad_buf); g_pad_buf = NULL; g_pad_cap = 0; g_pad_w = 0; g_pad_h = 0; }
    if (g_near_buf) { free(g_near_buf); g_near_buf = NULL; }
    if (g_panel_buf) { free(g_panel_buf); g_panel_buf = NULL; }
    if (g_handle) { dlclose(g_handle); g_handle = NULL; }
    p_init = NULL; p_deinit = NULL; p_disp = NULL;
    g_active = 0;
    sf3000_dump_fb_state("hwdisp_deinit/post-dlclose");
}
