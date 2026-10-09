/* nwpad's conversation window (dialog plan D2): while an NPC conversation is open
 * (and nwpad.dialog is on), a client-side NUI window replicates the game's dialog
 * window over it: portrait, speaker, the NPC's line and the numbered replies, one
 * highlighted. The D-pad / arrow keys or either stick move the highlight, the
 * confirm key answers, the cancel key ends the conversation. Game thread only. */
#ifndef NWPAD_DIALOGUI_H
#define NWPAD_DIALOGUI_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/nwpad_core.h"

/* Per frame. `nav` is the highlight's stick input (the larger of the two sticks'
 * up/down). Returns true while nwpad's window is up: the sticks are its. */
bool nwpad_dialogui_frame(bool enabled, float nav, uint64_t now_ms);
bool nwpad_dialogui_open(void);
/* Once, early: add nwpad's larger dialog font to the game's NUI skin (F36). */
void nwpad_dialogui_setup_font(void);
/* While open: move the highlight (-1 up, +1 down), answer, or end the conversation. */
void nwpad_dialogui_move(int step);
void nwpad_dialogui_confirm(void);
void nwpad_dialogui_cancel(void);
int nwpad_dialogui_highlight(void); /* -1 when closed */

#endif
