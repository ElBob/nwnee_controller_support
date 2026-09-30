/* Where nwpad's settings come from (docs/settings-plan.md, "lite" tier):
 *  1. NWPAD_CONFIG, if set: that config.toml only (tests and development).
 *  2. The [nwpad] section of the game's settings.tml. If the section is missing,
 *     it's added once, before the game reads the file, with values imported from
 *     config.toml or the defaults; the game preserves it from then on (re-notes F29).
 *  3. ~/.config/nwpad/config.toml, if settings.tml can't be read (fallback). */
#ifndef NWPAD_SETTINGS_H
#define NWPAD_SETTINGS_H

#include "../core/nwpad_core.h"

/* Fill cfg. Call from the library constructor, before the game starts. */
void nwpad_settings_load(nwpad_config *cfg);

#endif
