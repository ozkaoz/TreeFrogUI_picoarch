/*
 * menu_font.h - TTF text rendering for the picoarch menu.
 * Ported from FrogUI (font.c) so the in-game menu matches FrogUI's look.
 * Renders RGB565 glyphs into an arbitrary 16bpp framebuffer.
 */
#ifndef MENU_FONT_H
#define MENU_FONT_H

#include <stdint.h>

#ifndef MENU_FONT_CHAR_SPACING
#define MENU_FONT_CHAR_SPACING 8
#endif

/* Load the menu font (lazy, idempotent). Pixel height in px. Returns 1 if ready. */
int  menu_font_init(float pixel_height);
/* 1 once a TTF is loaded, else 0 (callers fall back to the bitmap font). */
int  menu_font_ready(void);

/* Draw text at (x,y) = top-left of the glyph cell, in color (RGB565). */
void menu_font_draw_text(uint16_t *fb, int fb_w, int fb_h,
                         int x, int y, const char *text, uint16_t color);
/* Pixel width of text at the current scale. */
int  menu_font_measure(const char *text);
/* baseline = px from cell top to baseline; cap_height = px height of capitals. */
void menu_font_cap_metrics(int *baseline_out, int *cap_height_out);
/* Glyph-cell pixel height (the requested pixel_height). */
int  menu_font_height(void);
/* Change render size at runtime (re-scales). No-op until a font is loaded. */
void menu_font_set_px(float pixel_height);

#endif /* MENU_FONT_H */
