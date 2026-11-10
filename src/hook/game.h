/* The client objects several features reach, through the signatures. Each lookup
 * returns NULL (or a default) when a signature or object is missing. Game thread
 * only. */
#ifndef NWPAD_GAME_H
#define NWPAD_GAME_H

#include <stdbool.h>
#include <stddef.h>
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
#ifdef NWPAD_DEBUG_SURFACES
/* Research (F40): JSON list of the client area's objects: id, type byte, +0xa8 flag, +0x11c flag,
 * name, position when readable. */
void nwpad_game_debug_area_objects(char *out, size_t cap, int max);
/* Whether the game's options panel / area map is open (CGuiInGame+0xd8 non-null /
 * the map panel at +0xb8 active; re-notes F38), for tests. */
bool nwpad_game_menu_open(void);
bool nwpad_game_map_open(void);
#endif
/* The screen's size in GUI units (NUI geometry): g_pGuiMan's size in pixels over
 * the GUI scale; 1280 x 720 if unknown. */
void nwpad_gui_size(float *w, float *h);

#endif
