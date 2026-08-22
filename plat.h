#ifndef __PLAT_H__
#define __PLAT_H__

#include "libpicofe/plat.h"

// Define sample rate for audio after SDL1.2 conversion
#define SAMPLE_RATE 48000
// Define Screen Width and Height and Bits Per Pixel
// Using 320x240 for SDL 1.2 fbcon compatibility
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define SCREEN_BPP 16
#define SCREEN_PITCH (SCREEN_WIDTH * (SCREEN_BPP / 8))

struct audio_frame {
	int16_t left;
	int16_t right;
};

#define HUD_LEN 41

struct GFX_Buffer {
    void* virAddr;
    int width;
    int height;
    int depth;
    int pitch;
    size_t size;
};

int  plat_init(void);
int  plat_reinit(void);
void plat_finish(void);
void plat_minimize(void);

void *plat_prepare_screenshot(int *w, int *h, int *bpp);
int  plat_dump_screen(const char *filename);
int  plat_load_screen(const char *filename, void *buf, size_t buf_size, int *w, int *h, int *bpp);

void plat_video_open(void);
void plat_video_set_msg(const char *new_msg, unsigned priority, unsigned msec);
void plat_video_process(const void *data, unsigned width, unsigned height, size_t pitch);
void plat_video_flip(void);
void plat_video_close(void);

unsigned plat_cpu_ticks(void);

float plat_sound_capacity(void);
void plat_sound_write(const struct audio_frame *data, int frames);
void plat_sound_resize_buffer(void);
void plat_sound_pause_for_menu(void);
void plat_sound_resume_from_menu(void);

void* plat_clean_screen(void);

#endif /* __PLAT_H__ */
