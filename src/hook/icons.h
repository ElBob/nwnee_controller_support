/* Item icons the game builds from palette textures (PLT: cloaks, helmets, armor),
 * which NUI can't draw, rendered to ordinary images nwpad serves to the game's
 * resource manager (re-notes F35). Game thread only. */
#ifndef NWPAD_ICONS_H
#define NWPAD_ICONS_H

#include <stdbool.h>

/* For a CLayeredIcon or CArmorIcon (`icon`, `kind` 'L' or 'A'), write the coloured,
 * layered image and put its resref into out (17 bytes). False if it can't. */
bool nwpad_icon_render_plt(const char *icon, char kind, char out[17]);

/* Raw bytes of a game resource (malloc'd; free it), through the game's resource
 * manager so overrides and haks apply. */
#include <stddef.h>
#include <stdint.h>
uint8_t *nwpad_resource_get(const char *name, unsigned short type, size_t *size);
/* Serve `filename` (e.g. "nui_skin.tml") with these bytes from nwpad's resource
 * folder, which outranks the game's own files (F35). */
bool nwpad_resource_publish(const char *filename, const void *data, size_t size);

#ifdef NWPAD_DEBUG_SURFACES
#include <stddef.h>
size_t nwpad_icon_debug_fetch(const char *name); /* bytes, 0 if not served */
#endif

#endif
