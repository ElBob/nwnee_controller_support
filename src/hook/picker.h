/* The quickbar picker (quickbar plan Q2): while the picker input is held, a ring of
 * the visible quickbar bank's 12 buttons is shown in a client-side NUI window; the
 * right stick (the Deck's right trackpad, through Steam Input) highlights one, and
 * releasing the input uses it (hold mode), or a press opens it and the confirm or
 * cancel key closes it (toggle mode). The input is a keyboard key (`picker-key`, default
 * Scroll Lock) that Steam Input sends from whatever button or grip the layout
 * chooses. Game thread only. */
#ifndef NWPAD_PICKER_H
#define NWPAD_PICKER_H

#include <stdbool.h>

#include "../core/nwpad_core.h"

/* Per frame. Returns true while the picker is open: the right stick is the
 * picker's, not the camera's. */
bool nwpad_picker_frame(bool held, nwpad_vec2 right, bool in_game);

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
