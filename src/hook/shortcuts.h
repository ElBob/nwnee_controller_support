/* The shortcuts wheel (Robert, 2026-11-10): a ring of the game's panels and Rest, opened
 * from the controller's right trigger. The right stick picks one, A opens it (the game's
 * own key action, so its guards apply), B or another trigger press closes the wheel.
 * A client-side NUI window like the quickbar picker (re-notes F32, F39). Game thread
 * only. */
#ifndef NWPAD_SHORTCUTS_H
#define NWPAD_SHORTCUTS_H

#include <stdbool.h>

#include "../core/nwpad_core.h"

#define NWPAD_SHORTCUTS 8

/* Per frame: open the wheel while `want` (and in a game), track the highlight, close it
 * without acting when no longer wanted. Returns true while open: the right stick is the
 * wheel's, not the camera's. */
bool nwpad_shortcuts_frame(bool want, nwpad_vec2 right, bool in_game);
bool nwpad_shortcuts_open(void);
/* Close the open wheel now, doing the highlighted shortcut if `use`. */
void nwpad_shortcuts_close(bool use);

#ifdef NWPAD_DEBUG_SURFACES
int nwpad_shortcuts_selected(void);   /* the highlighted slot, -1 none or closed */
int nwpad_shortcuts_last_action(void); /* the game action last done, 0 none */
#endif

#endif
