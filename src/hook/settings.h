/* Where nwpad's settings come from (docs/settings-plan.md, "lite" tier):
 *  1. NWPAD_CONFIG, if set: that config.toml only (tests and development).
 *  2. The [nwpad] section of the game's settings.tml. If the section is missing,
 *     it's added once, before the game reads the file, with values imported from
 *     config.toml or the defaults; the game preserves it from then on (re-notes F29).
 *  3. ~/.config/nwpad/config.toml, if settings.tml can't be read (fallback).
 * When the values came from settings.tml, nwpad_settings_frame() also registers the
 * user-facing keys with the game's settings registry and adds them to the Options
 * window's Camera group (re-notes F30); the registry is then the live source. */
#ifndef NWPAD_SETTINGS_H
#define NWPAD_SETTINGS_H

#include "../core/nwpad_core.h"

/* Fill cfg. Call from the library constructor, before the game starts. */
void nwpad_settings_load(nwpad_config *cfg);

/* Per frame, on the game's thread: register the native entries once, add them to
 * any newly opened Options window, and copy their current values into cfg.
 * Does nothing unless the settings came from settings.tml and every F30
 * signature resolved. */
void nwpad_settings_frame(nwpad_config *cfg);

/* Whether the native entries are registered (for status). */
bool nwpad_settings_native(void);

#ifdef NWPAD_DEBUG_SURFACES
/* {"key":[working, committed], ...} or null. */
void nwpad_settings_debug_json(char *out, size_t cap);
#endif

#endif
