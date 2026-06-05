/*
 * menu_font.c - TTF text rendering for the picoarch menu.
 * Ported from FrogUI's font.c. Loads GamePocket (FrogUI's default) from the
 * FrogUI font dirs and renders uppercase RGB565 glyphs, matching FrogUI's look.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "menu_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static stbtt_fontinfo font_info;
static unsigned char *font_buffer = NULL;
static float font_scale = 0.0f;
static int   font_loaded = 0;
static float font_px = 20.0f;

static int load_font_file(const char *fname) {
    char paths[4][256];
    snprintf(paths[0], sizeof(paths[0]), "/mnt/sdcard/cubegm/fonts/%s", fname);
    snprintf(paths[1], sizeof(paths[1]), "/mnt/sdcard/frogui/fonts/%s", fname);
    snprintf(paths[2], sizeof(paths[2]), "fonts/%s", fname);
    snprintf(paths[3], sizeof(paths[3]), "%s", fname);

    FILE *fp = NULL;
    for (int i = 0; i < 4; i++) { fp = fopen(paths[i], "rb"); if (fp) break; }
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return 0; }

    unsigned char *buf = (unsigned char *)malloc(sz);
    if (!buf) { fclose(fp); return 0; }
    if (fread(buf, 1, sz, fp) != (size_t)sz) { free(buf); fclose(fp); return 0; }
    fclose(fp);

    if (!stbtt_InitFont(&font_info, buf, stbtt_GetFontOffsetForIndex(buf, 0))) {
        free(buf);
        return 0;
    }
    if (font_buffer) free(font_buffer);
    font_buffer = buf;
    font_scale  = stbtt_ScaleForPixelHeight(&font_info, font_px);
    font_loaded = 1;
    return 1;
}

int menu_font_init(float pixel_height) {
    if (pixel_height > 0) font_px = pixel_height;
    if (font_loaded) return 1;
    /* FrogUI's default font first, then monogram as fallback. */
    if (load_font_file("GamePocket-Regular-ZeroKern.ttf")) return 1;
    if (load_font_file("monogram.ttf")) return 1;
    return 0;
}

int menu_font_ready(void) { return font_loaded; }
int menu_font_height(void) { return (int)font_px; }

void menu_font_set_px(float pixel_height) {
    if (pixel_height <= 0 || !font_loaded) return;
    font_px = pixel_height;
    font_scale = stbtt_ScaleForPixelHeight(&font_info, font_px);
}

static void draw_char(uint16_t *fb, int fb_w, int fb_h, int x, int y, char c, uint16_t color) {
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    int gi = stbtt_FindGlyphIndex(&font_info, c);
    if (gi == 0) return;

    int w, h, xoff, yoff;
    unsigned char *bmp = stbtt_GetGlyphBitmap(&font_info, 0, font_scale, gi, &w, &h, &xoff, &yoff);
    if (!bmp) return;

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);
    int baseline = (int)(ascent * font_scale);

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            unsigned char a = bmp[row * w + col];
            if (!a) continue;
            int px = x + xoff + col;
            int py = y + baseline + yoff + row;
            if (px < 0 || px >= fb_w || py < 0 || py >= fb_h) continue;
            uint16_t *dst = &fb[py * fb_w + px];
            if (a >= 255) {
                *dst = color;
            } else {
                uint16_t bg = *dst;
                int fr = (color >> 11) & 0x1F, fg = (color >> 5) & 0x3F, fbl = color & 0x1F;
                int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
                int ia = 255 - a;
                int rr = (fr * a + br * ia) / 255;
                int rg = (fg * a + bgc * ia) / 255;
                int rb = (fbl * a + bb * ia) / 255;
                *dst = (uint16_t)((rr << 11) | (rg << 5) | rb);
            }
        }
    }
    stbtt_FreeBitmap(bmp, NULL);
}

void menu_font_cap_metrics(int *baseline_out, int *cap_height_out) {
    int baseline = 0, cap = 0;
    if (font_loaded) {
        int ascent, descent, line_gap;
        stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);
        baseline = (int)(ascent * font_scale);
        int gi = stbtt_FindGlyphIndex(&font_info, 'H');
        int x0, y0, x1, y1;
        if (gi && stbtt_GetGlyphBox(&font_info, gi, &x0, &y0, &x1, &y1))
            cap = (int)((y1 - y0) * font_scale);
        else
            cap = baseline;
    }
    if (baseline_out)   *baseline_out = baseline;
    if (cap_height_out) *cap_height_out = cap;
}

void menu_font_draw_text(uint16_t *fb, int fb_w, int fb_h,
                         int x, int y, const char *text, uint16_t color) {
    if (!font_loaded || !fb || !text) return;
    int prev = 0;
    for (; *text; text++) {
        char c = *text;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        int gi = stbtt_FindGlyphIndex(&font_info, c);
        if (gi != 0) {
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&font_info, gi, &adv, &lsb);
            if (prev) x += (int)(stbtt_GetGlyphKernAdvance(&font_info, prev, gi) * font_scale);
            draw_char(fb, fb_w, fb_h, x, y, c, color);
            x += (int)(adv * font_scale);
            prev = gi;
        } else {
            x += MENU_FONT_CHAR_SPACING;
            prev = 0;
        }
    }
}

int menu_font_measure(const char *text) {
    if (!font_loaded || !text) return 0;
    int width = 0, prev = 0;
    for (; *text; text++) {
        char c = *text;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        int gi = stbtt_FindGlyphIndex(&font_info, c);
        if (gi != 0) {
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&font_info, gi, &adv, &lsb);
            if (prev) width += (int)(stbtt_GetGlyphKernAdvance(&font_info, prev, gi) * font_scale);
            width += (int)(adv * font_scale);
            prev = gi;
        } else {
            width += MENU_FONT_CHAR_SPACING;
            prev = 0;
        }
    }
    return width;
}
