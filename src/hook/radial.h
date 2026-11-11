/* The game's own radial menu (Robert, 2026-11-10; re-notes F41): A on a selected target opens
 * the object's menu as a right click on it does, the right stick picks an entry, A does it
 * (a sub menu opens as it does for a click, and is driven the same way), B cancels. Game
 * thread only. */
#ifndef NWPAD_RADIAL_H
#define NWPAD_RADIAL_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/nwpad_core.h"

/* Open the object's radial menu, centred on the screen. False if it can't be tried. */
bool nwpad_radial_open_for(uint32_t id);
/* A radial menu (or a sub menu of one) is open. */
bool nwpad_radial_open(void);
/* Per frame while open: the stick picks the entry under its direction. */
void nwpad_radial_frame(nwpad_vec2 stick);
/* A: do the picked entry (nothing if none is picked). B: cancel the top menu. */
void nwpad_radial_confirm(void);
void nwpad_radial_cancel(void);

#ifdef NWPAD_DEBUG_SURFACES
int nwpad_radial_depth(void);
int nwpad_radial_picked(void); /* the top menu's picked entry (0-7), -1 none */
#endif

#endif
