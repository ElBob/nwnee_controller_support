/* Target selection (Robert, 2026-11-10; re-notes F40): the d-pad steps through the creatures,
 * doors, placeables and items around the player, left to right as the camera looks. The
 * chosen one is lit with the game's own highlight (a creature also shows its name plate),
 * and its name is kept for the caller. B (or anything that closes it) ends the mode and
 * puts everything back. Game thread only. */
#ifndef NWPAD_TARGETS_H
#define NWPAD_TARGETS_H

#include <stdbool.h>
#include <stdint.h>

/* A spell (or other action) is waiting for a target: the game's modal "action target" panel
 * (CGuiMan+0x58, type 0x41635467) is up (re-notes F42). */
bool nwpad_targets_pending(void);
/* Give the pending action the object as its target, as a click on it does (the allowed-types
 * check, the pick callbacks, then leaving the mode). False if nothing is pending or the object
 * isn't an allowed target (the mode stays up then). */
bool nwpad_targets_complete(uint32_t id);

/* Start the mode (if it is not on) and step to the next (+1) or previous (-1) target.
 * False if there is nothing to select or the game isn't ready. */
bool nwpad_targets_step(int dir, float camera_forward_deg);
/* End the mode: un-light the selected target. */
void nwpad_targets_end(void);
bool nwpad_targets_active(void);
/* Per frame while active: ends the mode if the selected object is gone. */
void nwpad_targets_frame(void);

uint32_t nwpad_targets_selected(void); /* the selected object's id, 0 none */
#ifdef NWPAD_DEBUG_SURFACES
int nwpad_targets_count(void);         /* how many were in the list the last time it was made */
#endif

#endif
