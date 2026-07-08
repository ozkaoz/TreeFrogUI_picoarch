#include "overrides.h"

/* Handheld-tuned defaults: the core's stock defaults target the Vita
 * (960x544 render, auto framerate/samplerate) and are a slideshow on the
 * 74Kc. These match the settings proven playable on device. */
static const struct core_override_option vitaquake2_core_option_overrides[] = {
	{
		.key = "vitaquakeii_resolution",
		.default_value = "320x240",
	},
	{
		.key = "vitaquakeii_framerate",
		.default_value = "30",
	},
	{
		.key = "vitaquakeii_sw_dithered_filtering",
		.default_value = "enabled",
	},
	{
		.key = "vitaquakeii_sound_samplerate",
		.default_value = "32000",
	},
	{ NULL }
};

#define vitaquake2_overrides {                                     \
	.core_name = "vitaquake2",                                 \
	.options = vitaquake2_core_option_overrides,               \
}
