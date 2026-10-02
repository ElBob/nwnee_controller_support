/* The quickbar picker (quickbar plan Q2), like Baldur's Gate 3's radial: a press of
 * the picker key (`picker-key`, default Scroll Lock, which Steam Input sends from any
 * button) opens a client-side NUI window with the quickbar banks as three wheels;
 * the right stick (the Deck's right trackpad, through Steam Input) highlights a
 * button on the active one; the confirm key uses it and closes, the cancel key or
 * another press closes. Game thread only. */
#ifndef NWPAD_PICKER_H
#define NWPAD_PICKER_H

#include <stdbool.h>

#include "../core/nwpad_core.h"

/* Per frame: open the picker while `want` (and in a game), track the highlight, and
 * close it without using when no longer wanted. Returns true while open: the right
 * stick is the picker's, not the camera's. */
bool nwpad_picker_frame(bool want, nwpad_vec2 right, bool in_game);

/* While open: show the previous (-1) or next (+1) quickbar bank as the active wheel.
 * False if the picker isn't open (the key is then the game's). */
bool nwpad_picker_shift(int direction);

/* For status: the open picker's active bank (-1 closed) and highlighted ring slot
 * (-1 none), and whether open. */
int nwpad_picker_bank(void);

/* Close the open picker now, using the highlighted button if `use` (the confirm
 * key) or not (the cancel key). */
void nwpad_picker_close(bool use);
int nwpad_picker_selected(void);
bool nwpad_picker_open(void);
/* The quickbar slot (0-35) the picker last used, -1 none (debug status). */
int nwpad_picker_last_used(void);

#endif
