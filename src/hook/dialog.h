/* NPC conversations: reading the open conversation (the NPC's line and the replies)
 * and answering it the way the game's number keys do (dialog plan; re-notes F36).
 * Game thread only. */
#ifndef NWPAD_DIALOG_H
#define NWPAD_DIALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NWPAD_DIALOG_REPLIES 128 /* the official campaigns have up to 83 (dialog plan) */

typedef struct {
    bool open;
    uint32_t seq;   /* changes whenever the NPC's line or the replies change */
    uint32_t speaker; /* object id */
    char speaker_name[128];
    char portrait[24];  /* the speaker's portrait resref ("" none) */
    float panel_h;      /* the game's dialog window height, GUI pixels (F36) */
    bool busy;
    char line[4096]; /* the NPC's line, as the game shows it (markup and all) */
    int count;
    struct {
        uint32_t id;
        bool selectable;
        char text[512];
    } replies[NWPAD_DIALOG_REPLIES];
} nwpad_dialog;

/* The game's text (its install language's 8-bit encoding) as UTF-8, as the game
 * converts it for NUI; Latin-1 if the converter isn't available. */
void nwpad_text_utf8(const char *in, char *out, size_t cap);

/* Install the text capture (once, when signatures are resolved). False if unavailable. */
bool nwpad_dialog_init(void);
/* Cheap per-frame check: the conversation's change counter, or 0 if none is open. */
uint32_t nwpad_dialog_seq(void);
/* The open conversation, if any. */
bool nwpad_dialog_read(nwpad_dialog *out);
/* Answer with reply `index` (0-based), exactly as pressing its number key. */
bool nwpad_dialog_select(int index);
/* End the conversation (as the dialog's own close). */
bool nwpad_dialog_end(void);

#ifdef NWPAD_DEBUG_SURFACES
void nwpad_dialog_debug_json(char *out, size_t cap);
#endif

#endif
