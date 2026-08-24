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
/* Full button map confirmed via on-screen calibration 2026-05:
   UP=0x0010(4) DN=0x0040(6) LT=0x0080(7) RT=0x0020(5)
   A=0x2000(13) B=0x4000(14) X=0x1000(12) Y=0x8000(15)
   L=0x0400(10) R=0x0800(11) L2=0x0100(8) R2=0x0200(9)
   SEL=0x0001(0) STA=0x0008(3)
   L3=0x0002(1) R3=0x0004(2) — bits 1,2 are L3/R3 per Stock rkgame, not FN.
   FN=0x00010000(16) per Stock rkgame disassembly (Witali/r36sx_disasm) and verified SHA 57d8... on user's SD. */
struct sf3000_btn sf3000_keymap[] = {
    { 4,  RETRO_DEVICE_ID_JOYPAD_UP     },
    { 6,  RETRO_DEVICE_ID_JOYPAD_DOWN   },
    { 7,  RETRO_DEVICE_ID_JOYPAD_LEFT   },
    { 5,  RETRO_DEVICE_ID_JOYPAD_RIGHT  },
    { 13, RETRO_DEVICE_ID_JOYPAD_A      },
    { 14, RETRO_DEVICE_ID_JOYPAD_B      },
    { 12, RETRO_DEVICE_ID_JOYPAD_X      },
    { 15, RETRO_DEVICE_ID_JOYPAD_Y      },
    { 10, RETRO_DEVICE_ID_JOYPAD_L      },
    { 11, RETRO_DEVICE_ID_JOYPAD_R      },
    { 8,  RETRO_DEVICE_ID_JOYPAD_L2     },
    { 9,  RETRO_DEVICE_ID_JOYPAD_R2     },
    { 0,  RETRO_DEVICE_ID_JOYPAD_SELECT },
    { 3,  RETRO_DEVICE_ID_JOYPAD_START  },
};
const int sf3000_keymap_count = (int)(sizeof(sf3000_keymap)/sizeof(sf3000_keymap[0]));

/* ===== FN BUTTON (PHYSICAL) — Feature C1 — R36SX V2.6 CONFIRMED =====
 * FN is a PHYSICAL input, NOT a libretro virtual action.
 * Must NOT create RETRO_DEVICE_ID_JOYPAD_FN.
 * Model: PHYSICAL_FN (SDLK_F11) → binding → existing virtual action (SELECT/START/L3/etc)
 * Default binding = UNBOUND (no entry in in_sdl_defbinds).
 * Raw cubevol bit for FN is CONFIRMED: bit 16 (0x00010000) per Stock firmware evidence:
 *   - G:\cubegm\setting.xml: <quicksavehotkey>FN+A</quicksavehotkey>, <quickloadhotkey>FN+B</quickloadhotkey>, <quicksnaphotkey>FN+START</quicksnaphotkey> (SHA d7e8..., identical to backup)
 *   - G:\cubegm\rkgame SHA 57d8b4fd85e0aab44d17a51d209879c3f98130d066b622142736b42ad08ddcb9 matches Witali/r36sx_disasm disk_image rkgame; disassembly shows joykey_explaned FN 0x00010000 (bit 16), L3 0x00000002 (bit1), R3 0x00000004 (bit2)
 *   - Public wiki https://github.com/LiamJ74/R36S-V2.6_Wiki documents FN+A=quicksave, FN+B=quickload
 *   - Device tree /panel/key-fn = 0x00000002 in same repo is for L3, not FN — Stock rkgame mapping at 0x00010000 is authoritative for /tmp/joy_key
 * Confidence: HIGH for R36SX V2.6 (direct Stock firmware/config/disassembly + matching SHA). Other devices remain UNKNOWN.
 * Device capability: sf3000_has_fn auto-enabled for R36SX via TF_DEVICE detection (see plat_sdl.c), no user file required.
 * Visible name in UI: "FN".
 */
#define SF3000_FN_BIT 16
#define SF3000_FN_SDLKEY SDLK_F11
/* HAS_FN capability: runtime flag, auto-enabled for R36SX V2.6 via TF_DEVICE=R36SX (see sf3000_fn_capability_init), otherwise env/file override.
   R36SX V2.6 YES (HIGH), other 7 devices UNKNOWN/NO until evidence. */
extern int sf3000_has_fn;

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
    [SDLK_F11]        = "FN",
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