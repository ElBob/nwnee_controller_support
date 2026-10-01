/* The quickbar picker (quickbar plan Q2): while the picker input is held, a ring of
 * the visible quickbar bank's 12 buttons is shown in a client-side NUI window; the
 * right stick (the Deck's right trackpad, through Steam Input) highlights one, and
 * releasing the input uses it. Game thread only. */
#ifndef NWPAD_PICKER_H
#define NWPAD_PICKER_H

#include <stdbool.h>

#include "../core/nwpad_core.h"

/* Per frame. Returns true while the picker is open: the right stick is the
 * picker's, not the camera's. */
bool nwpad_picker_frame(bool held, nwpad_vec2 right, bool in_game);

/* For status: the open picker's highlighted ring slot (-1 none), and whether open. */
int nwpad_picker_selected(void);
bool nwpad_picker_open(void);
/* The quickbar slot (0-35) the picker last used, -1 none (debug status). */
int nwpad_picker_last_used(void);

#endif
