/* The player's quickbar: what's in each of its 36 buttons (3 banks of 12), with
 * the name and icon the game shows for it, and using a button as a click would
 * (quickbar plan; re-notes F31). Game thread only. */
#ifndef NWPAD_QUICKBAR_H
#define NWPAD_QUICKBAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NWPAD_QB_SLOTS 36

typedef struct {
    uint8_t type;   /* CGuiQuickButton type: 0 empty, 1 item, 2 spell, 3 skill, 4 feat, ... (F31) */
    uint64_t data;  /* type-specific id word (spell: id | class and metamagic bits) */
    uint32_t item;  /* item object id, for items */
    char icon[17];  /* icon resref (not items) */
    char parts[3][17]; /* items: icon images, bottom to top (one, or three for composite icons); "" unused */
    char name[128]; /* as the game names it ("Empowered Fireball", item name, feat name) */
} nwpad_qb_slot;

/* Fill all 36 slots. Returns false outside a game (no quickbar). */
bool nwpad_quickbar_read(nwpad_qb_slot out[NWPAD_QB_SLOTS]);
/* The visible bank (0-2), or -1. */
int nwpad_quickbar_bank(void);
#ifdef NWPAD_DEBUG_SURFACES
void nwpad_quickbar_debug_equipped_icon(unsigned slot_bit, char *out, size_t cap);
bool nwpad_quickbar_debug_show_bank(int bank); /* as Shift/Ctrl would */
#endif
/* Use slot 0-35 as clicking it would. */
bool nwpad_quickbar_use(int slot);

#endif
