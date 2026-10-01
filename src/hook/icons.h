/* Item icons the game builds from palette textures (PLT: cloaks, helmets, armor),
 * which NUI can't draw, rendered to ordinary images nwpad serves to the game's
 * resource manager (re-notes F35). Game thread only. */
#ifndef NWPAD_ICONS_H
#define NWPAD_ICONS_H

#include <stdbool.h>

/* For a CLayeredIcon or CArmorIcon (`icon`, `kind` 'L' or 'A'), write the coloured,
 * layered image and put its resref into out (17 bytes). False if it can't. */
bool nwpad_icon_render_plt(const char *icon, char kind, char out[17]);

#ifdef NWPAD_DEBUG_SURFACES
#include <stddef.h>
size_t nwpad_icon_debug_fetch(const char *name); /* bytes, 0 if not served */
#endif

#endif
