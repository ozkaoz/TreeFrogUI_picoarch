#include <sys/stat.h>
#include "core.h"
#include "main.h"
#include "menu.h"
#include "menu_font.h"
#include "options.h"
#include "overrides.h"
#include "plat.h"
#include "scale.h"
#include "util.h"

static int drew_alt_bg = 0;

static char cores_path[MAX_PATH];
static struct dirent **corelist = NULL;
static int corelist_len = 0;

#define MENU_ALIGN_LEFT 0
#define MENU_X2 0

#define MENU_ITEMS_PER_PAGE 11

typedef enum
{
	MA_NONE = 1,
	MA_MAIN_RESUME_GAME,
	MA_MAIN_SAVE_STATE,
	MA_MAIN_LOAD_STATE,
	MA_MAIN_DISC_CTRL,
	MA_MAIN_CHEATS,
	MA_MAIN_CORE_SEL,
	MA_MAIN_CONTENT_SEL,
	MA_MAIN_RESET_GAME,
	MA_MAIN_CREDITS,
	MA_MAIN_EXIT,
	MA_OPT_CORE_OPTS,
	MA_OPT_SAVECFG,
	MA_OPT_SAVECFG_GAME,
	MA_OPT_RMCFG_GAME,
	MA_CTRL_PLAYER1,
	MA_CTRL_EMU,
	MA_VID_FX,
	MA_VID_BLANK,
	MA_VID_SCALE_SIZE,
	MA_VID_FILTER,
} menu_id;

me_bind_action me_ctrl_actions[] =
{
	{ "UP       ",  1 << RETRO_DEVICE_ID_JOYPAD_UP},
	{ "DOWN     ",  1 << RETRO_DEVICE_ID_JOYPAD_DOWN },
	{ "LEFT     ",  1 << RETRO_DEVICE_ID_JOYPAD_LEFT },
	{ "RIGHT    ",  1 << RETRO_DEVICE_ID_JOYPAD_RIGHT },
	{ "A BUTTON ",  1 << RETRO_DEVICE_ID_JOYPAD_A },
	{ "B BUTTON ",  1 << RETRO_DEVICE_ID_JOYPAD_B },
	{ "X BUTTON ",  1 << RETRO_DEVICE_ID_JOYPAD_X },
	{ "Y BUTTON ",  1 << RETRO_DEVICE_ID_JOYPAD_Y },
	{ "START    ",  1 << RETRO_DEVICE_ID_JOYPAD_START },
	{ "SELECT   ",  1 << RETRO_DEVICE_ID_JOYPAD_SELECT },
	{ "L BUTTON ",  1 << RETRO_DEVICE_ID_JOYPAD_L },
	{ "R BUTTON ",  1 << RETRO_DEVICE_ID_JOYPAD_R },
	{ "L2 BUTTON ", 1 << RETRO_DEVICE_ID_JOYPAD_L2 },
	{ "R2 BUTTON ", 1 << RETRO_DEVICE_ID_JOYPAD_R2 },
	{ NULL,       0 }
};

/* Must be a superset of all possible actions. This is used when
 * saving config, and if an entry isn't here, the saver won't see
 * it. */
me_bind_action emuctrl_actions[] =
{
	{ "Save State       ", 1 << EACTION_SAVE_STATE },
	{ "Load State       ", 1 << EACTION_LOAD_STATE },
	{ "Toggle FPS/CPU%  ", 1 << EACTION_TOGGLE_HUD },
	{ "Toggle FF        ", 1 << EACTION_TOGGLE_FF },
	// { "Take Screenshot  ", 1 << EACTION_SCREENSHOT },
	{ NULL,                0 }
};

static int emu_check_save_file(int slot, int *time)
{
	char fname[MAX_PATH];
	struct stat status;
	int ret;

	state_file_name(fname, sizeof(fname), slot);

	ret = stat(fname, &status);
	if (ret != 0)
		return 0;

	return 1;
}

static int emu_save_load_game(int load, int unused)
{
	int ret;

	if (load)
		ret = pa_state_read();
	else
		ret = pa_state_write();

	return ret;
}

// RGB565
static unsigned short fname2color(const char *fname)
{
	return 0xFFFF;
}

/* MinUI draw helpers — defined later in this file, but forward-declared here so
 * the savestate menu (inside the libpicofe/menu.c included below) can use them. */
static void minui_fill(int x, int y, int w, int h, uint16_t c);
static void minui_border(int x, int y, int w, int h, int t, uint16_t c);
static void minui_thumb(const uint16_t *src, int sw, int sh, int spp,
                        int dx, int dy, int dw, int dh);

#include "libpicofe/menu.c"

static void draw_menu_message(const char *msg, void (*draw_more)(void))  __attribute__((unused));

static const char *mgn_saveloadcfg(int id, int *offs)
{
	return "";
}

static int mh_restore_defaults(int id, int keys)
{
	set_defaults();
	menu_update_msg("defaults restored");
	return 1;
}

static int mh_savecfg(int id, int keys)
{
	if (save_config(id == MA_OPT_SAVECFG_GAME ? 1 : 0) == 0)
		menu_update_msg("config saved");
	else
		menu_update_msg("failed to write config");

	return 1;
}

static int mh_rmcfg(int id, int keys)
{
	if (remove_config(id == MA_OPT_RMCFG_GAME ? 1 : 0) == 0)
		menu_update_msg("config removed");
	else
		menu_update_msg("failed to remove config");

	return 1;
}

static void draw_src_bg(void) {
	memcpy(g_menubg_ptr, g_menubg_src_ptr, g_menubg_src_h * g_menubg_src_pp * sizeof(uint16_t));
	menu_darken_bg(g_menubg_ptr, g_menubg_src_ptr, g_menubg_src_h * g_menubg_src_pp, 0);
}

static int mh_set_core(int id, int keys) {
	if (corelist && id < corelist_len)
		snprintf(core_path, sizeof(core_path), "%s/%s", cores_path, corelist[id]->d_name);

	return 1;
}

static int core_selector(const struct dirent *ent) {
	return has_suffix_i(ent->d_name, "_libretro.so");
}

static int menu_loop_core_page(int offset, int keys) {
	static int sel = 0;
	menu_entry e_menu_cores[MENU_ITEMS_PER_PAGE + 2] = {0}; /* +2 for Next, NULL */
	size_t menu_idx = 0;
	int i;
	char names[MENU_ITEMS_PER_PAGE][MAX_PATH];

	for (i = offset, menu_idx = 0; i < corelist_len && menu_idx < MENU_ITEMS_PER_PAGE; i++) {
		menu_entry *option;
		struct dirent *ent = corelist[i];
		option = &e_menu_cores[menu_idx];
		core_extract_name(ent->d_name, names[menu_idx], sizeof(names[menu_idx]));

		option->name = names[menu_idx];
		option->beh = MB_OPT_CUSTOM;
		option->id = i;
		option->enabled = 1;
		option->selectable = 1;
		option->handler = mh_set_core;
		menu_idx++;
	}

	if (i < corelist_len) {
		menu_entry *option;
		option = &e_menu_cores[menu_idx];
		option->name = "Next page";
		option->beh = MB_OPT_CUSTOM;
		option->id = i;
		option->enabled = 1;
		option->selectable = 1;
		option->handler = menu_loop_core_page;
	}

	return me_loop(e_menu_cores, &sel);
}

int menu_select_core(void) {
	int ret = -1;
	getcwd(cores_path, MAX_PATH);

	corelist_len = scandir(cores_path, &corelist, core_selector, alphasort);
	if (!corelist_len) return -1;

	plat_video_menu_enter(1);

	if (menu_loop_core_page(0, 0) < 0)
		goto finish;

	if (core_path[0] == '\0')
		goto finish;

	ret = 0;
finish:
	/* wait until menu, ok, back is released */
	while (in_menu_wait_any(NULL, 50) & (PBTN_MENU|PBTN_MOK|PBTN_MBACK))
		;

	plat_video_menu_leave();

	if (corelist_len > 0) {
		while (corelist_len--)
			free(corelist[corelist_len]);
		free(corelist);
		corelist = NULL;
	}
	return ret;
}

int hidden_file_filter(struct dirent **namelist, int count, const char *basedir) {
	int newcount = 0;

	for (int i = 0; i < count; i++) {
		if (namelist[i]->d_name[0] == '.' && namelist[i]->d_name[1] != '.') {
			free(namelist[i]);
			namelist[i] = NULL;
		}
	}

	for (int i = 0; i < count; i++) {
		if (namelist[i] != NULL)
			namelist[newcount++] = namelist[i];
	}

	return newcount;
}

const char *select_content(void) {
	const char *fname = NULL;
	char content_path[MAX_PATH];
	const char **extensions = core_extensions();
	const char **exts_with_zip = NULL;
	int i = 0, size = 0;

	if (content && content->path) {
		strncpy(content_path, content->path, sizeof(content_path) - 1);
	} else if (getenv("CONTENT_DIR")) {
		strncpy(content_path, getenv("CONTENT_DIR"), sizeof(content_path) - 1);
#ifdef CONTENT_DIR
	} else {
		strncpy(content_path, CONTENT_DIR, sizeof(content_path) - 1);
#else
	} else if (getenv("HOME")) {
		strncpy(content_path, getenv("HOME"), sizeof(content_path) - 1);
#endif
	}

	if (extensions) {
		for (size = 0; extensions[size]; size++)
			;
	}

	exts_with_zip = calloc(size + 2, sizeof (char *)); /* add 2 for "zip", NULL */

	if (exts_with_zip) {
		for (i = 0; extensions[i]; i++) {
			exts_with_zip[i] = extensions[i];
		}
		exts_with_zip[i] = "zip";
	} else {
		exts_with_zip = extensions;
	}

	fname = menu_loop_romsel(content_path, sizeof(content_path), exts_with_zip, hidden_file_filter);

	if (exts_with_zip != extensions)
		free(exts_with_zip);

	return fname;
}

int menu_select_content(char *filename, size_t len) {
	const char *fname = NULL;
	int ret = -1;

	plat_video_menu_enter(1);
	fname = select_content();
	if (!fname)
		goto finish;

	strncpy(filename, fname, len - 1);
	if (g_autostateld_opt)
		resume_slot = 0;
	ret = 0;

finish:
        /* wait until menu, ok, back is released */
	while (in_menu_wait_any(NULL, 50) & (PBTN_MENU|PBTN_MOK|PBTN_MBACK))
		;

	plat_video_menu_leave();
	return ret;
}

static int menu_loop_select_content(int id, int keys) {
	const char *fname = select_content();

	if (fname == NULL)
		return -1;

	core_unload_content();

	content = content_init(fname);
	if (!content) {
		PA_ERROR("Couldn't allocate memory for content\n");
		quit(-1);
	}

	set_defaults();

	if (core_load_content(content)) {
		quit(-1);
	}

	load_config();
	load_config_keys();

	if (g_autostateld_opt) {
		resume_slot = 0;
		state_resume();
	}

	return 1;
}

static int menu_loop_disc(int id, int keys)
{
	static int sel = 0;
	menu_entry e_menu_disc_options[2] = {0};
	unsigned disc = disc_get_index() + 1;
	menu_entry *option = &e_menu_disc_options[0];

	option->name = "Disc";
	option->beh = MB_OPT_RANGE;
	option->var = &disc;
	option->min = 1;
	option->max = disc_get_count();
	option->enabled = 1;
	option->need_to_save = 1;
	option->selectable = 1;

	me_loop(e_menu_disc_options, &sel);

	if (disc_get_index() + 1 != disc)
		disc_switch_index(disc - 1);

	return 0;
}

static int menu_loop_cheats_page(int offset, int keys) {
	static int sel = 0;
	menu_entry *e_menu_cheats;
	size_t i, menu_idx;

	/* cheats + 2 for possible "Next page" +  NULL */
	e_menu_cheats = (menu_entry *)calloc(cheats->count + 2, sizeof(menu_entry));

	if (!e_menu_cheats) {
		PA_ERROR("Error allocating cheats\n");
		return 0;
	}

	for (i = offset, menu_idx = 0; i < cheats->count && menu_idx < MENU_ITEMS_PER_PAGE; i++) {
		struct cheat *cheat = &cheats->cheats[i];
		menu_entry *option;

		option = &e_menu_cheats[menu_idx];

		option->name = cheat->name;
		option->beh = MB_OPT_ONOFF;
		option->var = &cheat->enabled;
		option->enabled = 1;
		option->mask = 1;
		option->need_to_save = 1;
		option->selectable = 1;
		option->help = cheat->info;
		menu_idx++;
	}

	if (i < cheats->count) {
		menu_entry *option;
		option = &e_menu_cheats[menu_idx];
		option->name = "Next page";
		option->beh = MB_OPT_CUSTOM;
		option->id = i;
		option->enabled = 1;
		option->selectable = 1;
		option->handler = menu_loop_cheats_page;
	}

	me_loop(e_menu_cheats, &sel);
	free(e_menu_cheats);

	return 0;
}

static int menu_loop_cheats(int id, int keys)
{
	int ret = menu_loop_cheats_page(0, keys);
	core_apply_cheats(cheats);
	return ret;
}

static int menu_loop_core_options_page(int offset, int keys) {
	static int sel = 0;
	menu_entry *e_menu_core_options;
	size_t i, menu_idx;

	/* core_option + 2 for possible "Next page" +  NULL */
	e_menu_core_options = (menu_entry *)calloc(core_options.visible_len + 2, sizeof(menu_entry));

	if (!e_menu_core_options) {
		PA_ERROR("Error allocating core options\n");
		return 0;
	}

	for (i = offset, menu_idx = 0; i < core_options.len && menu_idx < MENU_ITEMS_PER_PAGE; i++) {
		struct core_option_entry *entry = &core_options.entries[i];
		menu_entry *option;
		const char *key = entry->key;

		if (entry->blocked || !entry->visible)
			continue;

		option = &e_menu_core_options[menu_idx];

		option->name = entry->desc;
		option->beh = MB_OPT_ENUM;
		option->var = options_get_value_ptr(key);
		option->enabled = 1;
		option->need_to_save = 1;
		option->selectable = 1;
		option->data = options_get_options(key);
		option->help = entry->info;
		menu_idx++;
	}

	if (i < core_options.len) {
		menu_entry *option;
		option = &e_menu_core_options[menu_idx];
		option->name = "Next page";
		option->beh = MB_OPT_CUSTOM;
		option->id = i;
		option->enabled = 1;
		option->selectable = 1;
		option->handler = menu_loop_core_options_page;
	}

	me_loop(e_menu_core_options, &sel);

	options_update_changed();

	free(e_menu_core_options);

	return 0;
}

static int menu_loop_core_options(int id, int keys)
{
	return menu_loop_core_options_page(0, keys);
}

static const char h_rm_config_game[]  = "Removes game-specific config file";

static const char h_restore_def[]     = "Switches back to default settings";

static const char h_show_fps[]        = "Shows frames and vsyncs per second";
static const char h_show_cpu[]        = "Shows CPU usage";
static const char h_ff_enabled[]      = "Allow fast-forward (SELECT+Y). Off by default.";
static const char h_rewind_enabled[]  = "Allow rewind (hold SELECT+B). Uses RAM and slows frames; off by default.";

static const char h_audio_buffer_size[]        =
	"The size of the audio buffer, in frames. Higher\n"
	"values reduce the risk of audio crackling at the\n"
	"cost of delayed sound.";

static const char h_scale_size[]        =
	"How much to stretch the screen when scaling. Native\n"
	"does no stretching. Aspect uses the correct aspect\n"
	"ratio. Full uses the whole screen.";

// static const char h_max_upscale[]       =
// 	"When stretching the screen, the maximum integer\n"
// 	"step to scale up to before hardware scaling. Higher\n"
// 	"values produce crisper scaled pixels but increase\n"
// 	"the risk of audio crackling and frameskip.";

// static const char h_scale_filter[]        =
// 	"When stretching, how missing pixels are filled.\n"
// 	"Nearest copies the last pixel. Sharp keeps pixels\n"
// 	"aligned where possible. Smooth adds a blur effect.";

static const char h_scale_effect[]        =
	"When scaling is possible, which visual effect\n"
	"to apply. None is simple integer scaling. DMG is\n"
	"for Gameboy. LCD simulates RGB pixels. Scanline\n"
	"interleaves black rows. Some effects are only\n"
	"available at certain scales and resolutions.";

static const char h_optimize_text[]        =
	"When non-integer scaling is required,\n"
	"prioritize a consistent stroke size for\n"
	"text. May affect non-text content.";

static const char *men_scale_size[] = { "Integer", "Aspect", "Full", NULL};
static const char *men_scale_filter[] = { "Nearest", "Bilinear", NULL};

/* Custom name+cycle for screen size — hides "Integer" when filter=BILINEAR
 * (HW path: SW-upscale to 854×480 panel buffer is too memory-heavy and lags
 * at 60fps). User can still cycle Aspect/Full freely in that mode. */
static const char *mgn_scale_size(int id, int *offs) {
    (void)id; (void)offs;
    switch (scale_size) {
    case SCALE_SIZE_NONE:   return "Integer";
    case SCALE_SIZE_ASPECT: return "Aspect";
    case SCALE_SIZE_FULL:   return "Full";
    default: return "?";
    }
}

static int mh_scale_size(int id, int keys) {
    (void)id;
    int dir = (keys & (PBTN_RIGHT|PBTN_R)) ? 1 : -1;
    int v = (int)scale_size + dir;
    /* Integer/Aspect/Full all work via HW present now (any filter). */
    if (v < 0) v = SCALE_SIZE_FULL;
    if (v > SCALE_SIZE_FULL) v = SCALE_SIZE_NONE;
    scale_size = (enum scale_size)v;
    return 0;
}
static const char *men_scale_effect[] = { "None", "DMG", "LCD", "Scanline", NULL};

#ifdef PLATFORM_SF3000
extern int sf3000_snd_gain_pct;            /* SW output gain percent (plat_sdl.c) */
extern void sf3000_apply_snd_gain(void);
static const char h_sf3000_volume[] =
	"Lowers this app's audio output so the system's low\n"
	"volume steps are actually quiet (the stock volume\n"
	"curve is loud). 100 = full. Saved to sndgain.txt.";
#endif

static menu_entry e_menu_video_options[] =
{
	mee_onoff_h      ("Show FPS",                 0, show_fps, 1, h_show_fps),
	mee_onoff_h      ("Show CPU %",               0, show_cpu, 1, h_show_cpu),
	mee_onoff_h      ("Fast forward (SELECT+Y)",  0, ff_enabled, 1, h_ff_enabled),
	mee_onoff_h      ("Rewind (hold SELECT+B)",   0, rewind_enabled, 1, h_rewind_enabled),
	mee_cust_s_h     ("Screen size", MA_VID_SCALE_SIZE, 1, mh_scale_size, mgn_scale_size, h_scale_size),
	mee_enum_h       ("Filter",        MA_VID_FILTER, scale_filter, men_scale_filter, NULL),
	// mee_range_h      ("Max upscale",              0, max_upscale, 1, 8, h_max_upscale),
	mee_enum_h       ("Screen effect",    MA_VID_FX, scale_effect, men_scale_effect, h_scale_effect),
	mee_handler_id_h (             "", MA_VID_BLANK, NULL, NULL),
	mee_onoff_h      ("Optimize text",            0, optimize_text, 1, h_optimize_text),
	mee_range_h      ("Audio buffer",             0, audio_buffer_size, 1, 15, h_audio_buffer_size),
#ifdef PLATFORM_SF3000
	mee_range_h      ("Volume %",                 0, sf3000_snd_gain_pct, 0, 100, h_sf3000_volume),
#endif
	mee_end,
};

	// only show effects on native scale
static void menu_loop_video_prep(void) {
	/* Integer (NONE) works on the HW present regardless of filter — no reset. */
	/* Filter is owned by FrogUI settings — hide from in-game menu. */
	me_enable(e_menu_video_options, MA_VID_FILTER, false);
	/* Screen size always visible; effects only meaningful in nearest. */
	me_enable(e_menu_video_options, MA_VID_SCALE_SIZE, true);
	me_enable(e_menu_video_options, MA_VID_FX,
	          scale_filter == SCALE_FILTER_NEAREST && scale_size == SCALE_SIZE_NONE);
	me_enable(e_menu_video_options, MA_VID_BLANK, scale_size!=SCALE_SIZE_NONE);
}

static int menu_loop_video_options(int id, int keys)
{
	static int sel = 0;

	menu_loop_video_prep();
	
	me_loop_d(e_menu_video_options, &sel, menu_loop_video_prep, NULL);
	scale_update_scaler();

#ifdef PLATFORM_SF3000
	sf3000_apply_snd_gain();   /* recompute + persist the volume gain on exit */
#endif
	return 0;
}

static int key_config_loop_wrap(int id, int keys)
{
	const struct core_override *override = get_overrides();
	me_bind_action *actions = CORE_OVERRIDE(override, actions, me_ctrl_actions);
	size_t action_size = CORE_OVERRIDE(override, action_size, array_size(me_ctrl_actions));
	me_bind_action *emu_actions = emuctrl_actions;
	size_t emu_action_size = array_size(emuctrl_actions);

	switch (id) {
	case MA_CTRL_PLAYER1:
		key_config_loop(actions, action_size - 1, 0);
		break;
	case MA_CTRL_EMU:
		key_config_loop(emu_actions, emu_action_size - 1, -1);
		break;
	default:
		break;
	}
	return 0;
}

const char *config_label(int id, int *offs) {
	return config_override ? "Loaded: game config" : "Loaded: global config";
}

static menu_entry e_menu_config_options[] =
{
	mee_cust_nosave  ("Save global config",       MA_OPT_SAVECFG,      mh_savecfg, mgn_saveloadcfg),
	mee_cust_nosave  ("Save game config",         MA_OPT_SAVECFG_GAME, mh_savecfg, mgn_saveloadcfg),
	mee_handler_id_h ("Delete game config",       MA_OPT_RMCFG_GAME,   mh_rmcfg,   h_rm_config_game),
	mee_handler_h    ("Restore defaults",         mh_restore_defaults, h_restore_def),
	mee_label        (""),
	mee_label_mk     (0,                          config_label),
	mee_end,
};

static int menu_loop_config_options(int id, int keys)
{
	static int sel = 0;
	me_enable(e_menu_config_options, MA_OPT_RMCFG_GAME, config_override == 1);

	me_loop(e_menu_config_options, &sel);

	return 0;
}

static menu_entry e_menu_options[] =
{
	mee_handler   ("Audio and video",    menu_loop_video_options),
	mee_handler_id("Emulator options",   MA_OPT_CORE_OPTS,    menu_loop_core_options),
	mee_handler_id("Player controls",    MA_CTRL_PLAYER1,     key_config_loop_wrap),
	mee_handler_id("Emulator controls",  MA_CTRL_EMU,         key_config_loop_wrap),
	mee_handler   ("Save config",        menu_loop_config_options),
	mee_end,
};

static int menu_loop_options(int id, int keys)
{
	static int sel = 0;
	me_loop(e_menu_options, &sel);

	return 0;
}

static int main_menu_handler(int id, int keys)
{
	switch (id)
	{
	case MA_MAIN_RESUME_GAME:
		return 1;
	case MA_MAIN_SAVE_STATE:
		return menu_loop_savestate(0);
	case MA_MAIN_LOAD_STATE:
		return menu_loop_savestate(1);
	case MA_MAIN_RESET_GAME:
		current_core.retro_reset();
		return 1;
	case MA_MAIN_EXIT:
		should_quit = 1;
		return 1;
	default:
		lprintf("%s: something unknown selected\n", __FUNCTION__);
		break;
	}

	return 0;
}

static menu_entry e_menu_main[] =
{
	mee_handler_id("Resume game",        MA_MAIN_RESUME_GAME, main_menu_handler),
	mee_handler_id("Save state",         MA_MAIN_SAVE_STATE,  main_menu_handler),
	mee_handler_id("Load state",         MA_MAIN_LOAD_STATE,  main_menu_handler),
	mee_handler_id("Disc control",       MA_MAIN_DISC_CTRL,   menu_loop_disc),
	mee_handler_id("Cheats",             MA_MAIN_CHEATS,      menu_loop_cheats),
	mee_handler   ("Options",            menu_loop_options),
	mee_handler_id("Reset game",         MA_MAIN_RESET_GAME,  main_menu_handler),
	mee_handler_id("Load new game",      MA_MAIN_CONTENT_SEL, menu_loop_select_content),
	mee_handler_id("Exit",               MA_MAIN_EXIT,        main_menu_handler),
	mee_end,
};

static void draw_savestate_bg(int slot)
{
	char filename[MAX_PATH];
	int w, h, bpp;
	size_t bufsize = SCREEN_PITCH * SCREEN_HEIGHT;
	void *buf = calloc(bufsize, sizeof(char));

	if (!buf) {
		PA_WARN("Couldn't allocate savestate background");
		goto finish;
	}
	state_file_name(filename, MAX_PATH, slot);

	if (plat_load_screen(filename, buf, bufsize, &w, &h, &bpp))
		goto finish;

	if (bpp == sizeof(uint16_t)) {
		menu_darken_bg(g_menubg_ptr, buf, w * h, 0);
		drew_alt_bg = 1;
	}

finish:
	if (buf)
		free(buf);
}

void menu_begin(void)
{
	if (!drew_alt_bg)
		draw_src_bg();
}

void menu_end(void)
{
	drew_alt_bg = 0;
}

/* ------------------------- MinUI-style main pause menu ------------------------- */

static void minui_game_title(char *out, size_t n) {
	const char *p = (content && content->path[0]) ? content->path : "";
	const char *base = strrchr(p, '/');
	base = base ? base + 1 : p;
	snprintf(out, n, "%s", base);
	char *dot = strrchr(out, '.');
	if (dot && dot != out) *dot = '\0';
	if (!out[0]) snprintf(out, n, "picoarch");
}

static void minui_fill(int x, int y, int w, int h, uint16_t c) {
	uint16_t *fb = (uint16_t *)g_menuscreen_ptr;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > g_menuscreen_w) w = g_menuscreen_w - x;
	if (y + h > g_menuscreen_h) h = g_menuscreen_h - y;
	for (int j = 0; j < h; j++) {
		uint16_t *r = fb + (size_t)(y + j) * g_menuscreen_pp + x;
		for (int i = 0; i < w; i++) r[i] = c;
	}
}

static void minui_border(int x, int y, int w, int h, int t, uint16_t c) {
	minui_fill(x, y, w, t, c);
	minui_fill(x, y + h - t, w, t, c);
	minui_fill(x, y, t, h, c);
	minui_fill(x + w - t, y, t, h, c);
}

/* Nearest-scale an RGB565 source into a box of g_menuscreen. */
static void minui_thumb(const uint16_t *src, int sw, int sh, int spp,
                        int dx, int dy, int dw, int dh) {
	if (!src || sw <= 0 || sh <= 0) { minui_fill(dx, dy, dw, dh, 0x0000); return; }
	uint16_t *fb = (uint16_t *)g_menuscreen_ptr;
	for (int y = 0; y < dh; y++) {
		int sy = y * sh / dh;
		const uint16_t *srow = src + (size_t)sy * spp;
		uint16_t *drow = fb + (size_t)(dy + y) * g_menuscreen_pp + dx;
		for (int x = 0; x < dw; x++) drow[x] = srow[x * sw / dw];
	}
}

static void minui_draw_main(menu_entry *menu, int sel) {
	/* Colours come from skin.txt (written by FrogUI from the active theme):
	 * TEXT = theme select-text, PILL = theme select-bg. */
	extern int menu_text_color, menu_sel_color, menu_sel_text_color;
	const uint16_t WHITE = (uint16_t)menu_text_color;                       /* item/title text */
	const uint16_t PILL  = (menu_sel_color >= 0) ? (uint16_t)menu_sel_color : 0xFFFF; /* selection pill */
	const uint16_t DARK  = (uint16_t)menu_sel_text_color;                   /* selected text on pill */
	int W = g_menuscreen_w, H = g_menuscreen_h;
	int pad = W / 20;
	int rh  = me_mfont_h;                   /* 36 — matches FrogUI ITEM_HEIGHT */
	int tdy = (rh - MENU_TTF_PX) / 2;       /* vertical text centering in a row */

	menu_draw_begin(1, 0);                  /* darkened game frame as background */
	menu_font_set_px((float)MENU_TTF_PX);

	char title[128];
	minui_game_title(title, sizeof(title));
	menu_font_draw_text((uint16_t *)g_menuscreen_ptr, W, H, pad, pad + tdy, title, WHITE);

	/* thumbnail box (right) = bright captured frame */
	int tw = (W * 42) / 100, th = (tw * 3) / 4;
	int tx = W - tw - pad, ty = pad + rh + rh / 2;
	minui_thumb((const uint16_t *)g_menubg_src_ptr, g_menubg_src_w, g_menubg_src_h,
	            g_menubg_src_pp, tx, ty, tw, th);
	minui_border(tx - 3, ty - 3, tw + 6, th + 6, 3, WHITE);

	/* item list (left), highlight the selected entry with a rounded white pill */
	int ly = pad + rh * 2;
	int barw = tx - (pad - 6) - pad / 2;
	for (menu_entry *e = menu; e->name; e++) {
		if (!e->enabled) continue;
		int idx = (int)(e - menu);
		if (idx == sel) {
			menu_round_fill(pad - 6, ly, barw, rh, rh / 4, PILL);
			menu_font_draw_text((uint16_t *)g_menuscreen_ptr, W, H, pad, ly + tdy, e->name, DARK);
		} else {
			menu_font_draw_text((uint16_t *)g_menuscreen_ptr, W, H, pad, ly + tdy, e->name, WHITE);
		}
		ly += rh;
	}

	/* core library name + version (from retro_system_info), under the thumbnail */
	extern char core_lib_name[64], core_lib_version[32];
	if (core_lib_name[0]) {
		char cinfo[96];
		if (core_lib_version[0])
			snprintf(cinfo, sizeof(cinfo), "%s %s", core_lib_name, core_lib_version);
		else
			snprintf(cinfo, sizeof(cinfo), "%s", core_lib_name);
		menu_font_draw_text((uint16_t *)g_menuscreen_ptr, W, H, tx, ty + th + 6 + tdy, cinfo, WHITE);
	}

	const char *leg = "B-BACK   A-OKAY";
	int lw = menu_font_measure(leg);
	menu_font_draw_text((uint16_t *)g_menuscreen_ptr, W, H, W - lw - pad, H - rh + tdy, leg, WHITE);

	menu_draw_end();
}

static int minui_main_loop(menu_entry *menu, int *menu_sel) {
	int ret = 0, inp, sel = *menu_sel, smax = me_count(menu) - 1;
	if (smax < 0) return 0;
	while ((!menu[sel].enabled || !menu[sel].selectable) && sel < smax) sel++;

	minui_draw_main(menu, sel);
	while (in_menu_wait_any(NULL, 50) & (PBTN_MOK | PBTN_MBACK | PBTN_MENU))
		;

	for (;;) {
		minui_draw_main(menu, sel);
		inp = in_menu_wait(PBTN_UP | PBTN_DOWN | PBTN_MOK | PBTN_MBACK | PBTN_MENU, NULL, 70);
		if (inp & (PBTN_MENU | PBTN_MBACK))
			break;
		if (inp & PBTN_UP) {
			do { sel--; if (sel < 0) sel = smax; }
			while (!menu[sel].enabled || !menu[sel].selectable);
		}
		if (inp & PBTN_DOWN) {
			do { sel++; if (sel > smax) sel = 0; }
			while (!menu[sel].enabled || !menu[sel].selectable);
		}
		if (inp & PBTN_MOK) {
			if (menu[sel].handler) {
				ret = menu[sel].handler(menu[sel].id, inp);
				if (ret) break;
				smax = me_count(menu) - 1;   /* enables may have changed */
			}
		}
	}
	*menu_sel = sel;
	return ret;
}

void menu_loop(void)
{
	static int sel = 0;
	bool needs_disc_ctrl = disc_get_count() > 1;
	// const struct core_override *override = get_overrides();

	plat_video_menu_enter(1);

	me_enable(e_menu_options, MA_OPT_CORE_OPTS, core_options.visible_len > 0);

	me_enable(e_menu_main, MA_MAIN_SAVE_STATE, state_allowed());
	me_enable(e_menu_main, MA_MAIN_LOAD_STATE, state_allowed());
	me_enable(e_menu_main, MA_MAIN_CHEATS, cheats != NULL);
	
	me_enable(e_menu_main, MA_MAIN_DISC_CTRL, needs_disc_ctrl);

	me_enable(e_menu_main, MA_MAIN_CONTENT_SEL, false);

	// if (override)
	// 	me_enable(e_menu_main, MA_MAIN_CONTENT_SEL, !override->block_load_content);

#ifdef MMENU
	if (state_allowed()) {
		me_enable(e_menu_main, MA_MAIN_SAVE_STATE, mmenu == NULL);
		me_enable(e_menu_main, MA_MAIN_LOAD_STATE, mmenu == NULL);
	}
#endif
	minui_main_loop(e_menu_main, &sel);

	/* wait until menu, ok, back is released */
	while (in_menu_wait_any(NULL, 50) & (PBTN_MENU|PBTN_MOK|PBTN_MBACK))
		;

	/* Force the hud to clear */
	plat_video_set_msg(NULL, 0, 0);
	plat_video_menu_leave();
}

int menu_init(void)
{
	menu_init_base();

	/* Load FrogUI's TTF for the menu (no-op + bitmap fallback if unavailable). */
	menu_font_init((float)MENU_TTF_PX);

	g_menubg_src_ptr = calloc(g_menubg_src_pp * g_menubg_src_h, sizeof(uint16_t));
	g_menubg_ptr = calloc(g_menuscreen_w * g_menuscreen_pp, sizeof(uint16_t));
	if (g_menubg_src_ptr == NULL || g_menubg_ptr == NULL) {
		fprintf(stderr, "OOM\n");
		return -1;
	}
	return 0;
}

void menu_finish(void)
{
	if (g_menubg_src_ptr) {
		free(g_menubg_src_ptr);
		g_menubg_src_ptr = NULL;
	}

	if (g_menubg_ptr) {
		free(g_menubg_ptr);
		g_menubg_ptr = NULL;
	}
}

static void debug_menu_loop(void)
{
}

void menu_update_msg(const char *msg)
{
	strncpy(menu_error_msg, msg, sizeof(menu_error_msg));
	menu_error_msg[sizeof(menu_error_msg) - 1] = 0;

	menu_error_time = plat_get_ticks_ms();
	PA_INFO("%s\n", menu_error_msg);
}
