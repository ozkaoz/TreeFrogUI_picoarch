#include <SDL/SDL.h>
#include "libretro.h"
#include "libpicofe/plat.h"
#include "libpicofe/input.h"
#include "libpicofe/in_sdl.h"
#include "main.h"
#include "util.h"
#include "scale.h"
#include "scaler_neon.h"
#include "plat.h"

// Video system
static SDL_Surface* screen;
static SDL_Surface* clean_screen;
//static Uint32 video_flags = SDL_SWSURFACE;

// Buffer structure
static struct GFX_Buffer buffer;

// Function prototypes
//void in_sdl_key_down(int key);
//void in_sdl_key_up(int key);

// Initialize the framebuffer buffer
/*
void buffer_init(void) {
    buffer.width = SCREEN_WIDTH;
    buffer.height = SCREEN_HEIGHT;
    buffer.depth = SCREEN_BPP;
    buffer.pitch = buffer.width * (SCREEN_BPP / 8);
    buffer.size = buffer.pitch * buffer.height;

    // Allocate memory for the buffer
    buffer.virAddr = malloc(buffer.size);
    if (!buffer.virAddr) {
        PA_ERROR("Failed to allocate memory for framebuffer\n");
        return;
    }

    // Clear the buffer
    memset(buffer.virAddr, 0, buffer.size);

    PA_INFO("buffer_init() completed for sf3000\n");
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
void buffer_scale(int w, int h, int pitch, void *data) {
    SDL_Surface *src_surface = SDL_CreateRGBSurfaceFrom(
        data, w, h, SCREEN_BPP, pitch,
        0x00FF0000,  // Red mask
        0x0000FF00,  // Green mask
        0x000000FF,  // Blue mask
        0xFF000000   // Alpha mask
    );

    if (!src_surface) {
        PA_ERROR("Failed to create source surface: %s\n", SDL_GetError());
        return;
    }

    SDL_Rect dst_rect = { 0, 0, buffer.width, buffer.height };
    if (SDL_BlitSurface(src_surface, NULL, screen, &dst_rect) < 0) {
        PA_ERROR("SDL_BlitSurface failed: %s\n", SDL_GetError());
    }

    SDL_FreeSurface(src_surface);

    if (SDL_Flip(screen) < 0) {
        PA_ERROR("SDL_Flip failed: %s\n", SDL_GetError());
    }
}
*/

// plat_sf3000.c
#ifdef PLATFORM_SF3000
#include <stdint.h>
#include "libretro.h"

/* cubevol bit → libretro button mapping.
   Bits confirmed via discovery log (press each button, note which bit flips).
   Set bit field to 0xFF to mark as unmapped / TBD.
   Mapping table: {cubevol_bit_position, RETRO_DEVICE_ID_JOYPAD_*} */
struct sf3000_btn { uint8_t bit; uint8_t retro_id; };
/* Mutable — filled in by sf3000_calibrate_input() at startup */
struct sf3000_btn sf3000_keymap[] = {
    { 4,  RETRO_DEVICE_ID_JOYPAD_UP     },
    { 6,  RETRO_DEVICE_ID_JOYPAD_DOWN   },
    { 5,  RETRO_DEVICE_ID_JOYPAD_LEFT   },
    { 7,  RETRO_DEVICE_ID_JOYPAD_RIGHT  },
    { 8,  RETRO_DEVICE_ID_JOYPAD_A      },
    { 9,  RETRO_DEVICE_ID_JOYPAD_B      },
    { 10, RETRO_DEVICE_ID_JOYPAD_X      },
    { 11, RETRO_DEVICE_ID_JOYPAD_Y      },
    { 12, RETRO_DEVICE_ID_JOYPAD_L      },
    { 13, RETRO_DEVICE_ID_JOYPAD_R      },
    { 14, RETRO_DEVICE_ID_JOYPAD_SELECT },
    { 15, RETRO_DEVICE_ID_JOYPAD_START  },
};
const int sf3000_keymap_count = (int)(sizeof(sf3000_keymap)/sizeof(sf3000_keymap[0]));

uint32_t sf3000_keys_to_buttons(uint32_t raw) {
    uint32_t result = 0;
    for (int i = 0; i < (int)(sizeof(sf3000_keymap)/sizeof(sf3000_keymap[0])); i++) {
        if (raw & (1u << sf3000_keymap[i].bit))
            result |= (1u << sf3000_keymap[i].retro_id);
    }
    return result;
}
#endif

// Input system (SDL fallback binds, not used on SF3000 but required to compile)
static const struct in_default_bind in_sdl_defbinds[] = {
    { SDLK_UP,        IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_UP },
    { SDLK_DOWN,      IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_DOWN },
    { SDLK_LEFT,      IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_LEFT },
    { SDLK_RIGHT,     IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { SDLK_LCTRL,     IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_B },
    { SDLK_SPACE,     IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_A },
    { SDLK_LSHIFT,    IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_X },
    { SDLK_LALT,      IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_Y },
    { SDLK_RETURN,    IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_START },
    { SDLK_RCTRL,     IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_SELECT },
    { SDLK_TAB,       IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_L },
    { SDLK_BACKSPACE, IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_R },
    { SDLK_q,         IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_L2 },
    { SDLK_BACKSLASH, IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_R2 },
    { SDLK_ESCAPE,    IN_BINDTYPE_EMU, EACTION_MENU },
    { 0, 0, 0 }
};

const struct menu_keymap in_sdl_key_map[] =
{
    { SDLK_UP,        PBTN_UP },
    { SDLK_DOWN,      PBTN_DOWN },
    { SDLK_LEFT,      PBTN_LEFT },
    { SDLK_RIGHT,     PBTN_RIGHT },
    { SDLK_SPACE,     PBTN_MOK },
    { SDLK_LCTRL,     PBTN_MBACK },
    { SDLK_LALT,      PBTN_MA2 },
    { SDLK_LSHIFT,    PBTN_MA3 },
    { SDLK_TAB,       PBTN_L },
    { SDLK_BACKSPACE, PBTN_R },
    { SDLK_ESCAPE,    PBTN_MENU },
};

const struct menu_keymap in_sdl_joy_map[] =
{
    { SDLK_UP,    PBTN_UP },
    { SDLK_DOWN,  PBTN_DOWN },
    { SDLK_LEFT,  PBTN_LEFT },
    { SDLK_RIGHT, PBTN_RIGHT },
    { SDLK_WORLD_0, PBTN_MOK },
    { SDLK_WORLD_1, PBTN_MBACK },
    { SDLK_WORLD_2, PBTN_MA2 },
    { SDLK_WORLD_3, PBTN_MA3 },
};

static const char * const in_sdl_key_names[SDLK_LAST] = {
    [SDLK_UP]         = "UP",
    [SDLK_DOWN]       = "DOWN",
    [SDLK_LEFT]       = "LEFT",
    [SDLK_RIGHT]      = "RIGHT",
    [SDLK_LSHIFT]     = "X",
    [SDLK_LCTRL]      = "B",
    [SDLK_SPACE]      = "A",
    [SDLK_LALT]       = "Y",
    [SDLK_RETURN]     = "START",
    [SDLK_RCTRL]      = "SELECT",
    [SDLK_TAB]        = "L",
    [SDLK_BACKSPACE]  = "R",
    [SDLK_1]          = "MENU+UP",
    [SDLK_2]          = "MENU+DOWN",
    [SDLK_3]          = "MENU+LEFT",
    [SDLK_4]          = "MENU+RIGHT",
    [SDLK_5]          = "MENU+B",
    [SDLK_6]          = "MENU+A",
    [SDLK_7]          = "MENU+X",
    [SDLK_8]          = "MENU+Y",
    [SDLK_9]          = "MENU+START",
    [SDLK_0]          = "MENU+SELECT",
    [SDLK_q]          = "MENU+L",
    [SDLK_BACKSLASH]  = "MENU+R",
    [SDLK_ESCAPE]     = "MENU",
};

// mod_keymap not available in SF3000 build
/*
static const struct mod_keymap in_sdl_mod_keymap[] = {
    { SDLK_UP,        SDLK_1 },
    { SDLK_DOWN,      SDLK_2 },
    { SDLK_LEFT,      SDLK_3 },
    { SDLK_RIGHT,     SDLK_4 },
    { SDLK_LCTRL,     SDLK_5 },
    { SDLK_SPACE,     SDLK_6 },
    { SDLK_LSHIFT,    SDLK_7 },
    { SDLK_LALT,      SDLK_8 },
    { SDLK_RETURN,    SDLK_9 },
    { SDLK_RCTRL,     SDLK_0 },
    { SDLK_TAB,       SDLK_q },
    { SDLK_BACKSPACE, SDLK_BACKSLASH },
};
*/

static const struct in_pdata in_sdl_platform_data = {
    .defbinds  = in_sdl_defbinds,
    .key_map   = in_sdl_key_map,
    .kmap_size = array_size(in_sdl_key_map),
    .joy_map   = in_sdl_joy_map,
    .jmap_size = array_size(in_sdl_joy_map),
    .key_names = in_sdl_key_names,
};

#include "plat_sdl.c"