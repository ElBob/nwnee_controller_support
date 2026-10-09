/* NPC conversations: reading the open conversation and answering it the way the
 * game's number keys do (dialog plan; re-notes F36). Game thread only. */
#ifndef NWPAD_DIALOG_H
#define NWPAD_DIALOG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef NWPAD_DEBUG_SURFACES
/* Research: describe the open dialog object as JSON (fields and text it points at). */
void nwpad_dialog_debug_json(char *out, size_t cap);
/* Research: pointer paths from the dialog object to text starting with needle. */
void nwpad_dialog_debug_find(const char *needle, char *out, size_t cap);
#endif

#endif
