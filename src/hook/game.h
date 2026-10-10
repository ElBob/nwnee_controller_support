/* The client objects several features reach, through the signatures. Each lookup
 * returns NULL (or a default) when a signature or object is missing. Game thread
 * only. */
#ifndef NWPAD_GAME_H
#define NWPAD_GAME_H

#include <stdint.h>

/* CExoString as the game lays it out: 16 bytes. */
typedef struct {
    char *ptr;
    uint32_t len, cap;
} nwpad_exo_string;

/* The CClientExoApp (g_pAppManager's first field). */
void *nwpad_client_app(void);
/* The in-game GUI (CGuiInGame), outside a module NULL. */
void *nwpad_in_game_gui(void);
/* The screen's size in GUI units (NUI geometry): g_pGuiMan's size in pixels over
 * the GUI scale; 1280 x 720 if unknown. */
void nwpad_gui_size(float *w, float *h);

#endif
