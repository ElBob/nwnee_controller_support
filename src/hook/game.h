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
/* A movie is playing (the start-up intro, an ending or credits movie, a cutscene). */
bool nwpad_movie_playing(void);
#ifdef NWPAD_DEBUG_SURFACES
/* Start the named movie (no extension), as the intro does: tests only. */
bool nwpad_game_debug_play_movie(const char *name);
#endif
/* The start-up intro's stage (CPregameIntro+0x98): 1-4 its movies, 5-6 the "press any key"
 * legal screen, later it hands over to the main menu; -1 when it isn't running (re-notes F43). */
int nwpad_intro_stage(void);
/* Press a key on the focused GUI control, as a keyboard key would: skips a movie (each one
 * puts up a "press any key" control) and passes the intro's legal screen. */
bool nwpad_intro_press_any_key(void);
/* The in-game GUI (CGuiInGame), outside a module NULL. */
void *nwpad_in_game_gui(void);
/* The client's current area (a CNWCArea*), or NULL: CClientExoApp -> the internal app ->
 * module at +0x30 -> area at module+0xf8 (re-notes F40). */
char *nwpad_client_area(void);
#ifdef NWPAD_DEBUG_SURFACES
/* Research (F40): JSON list of the client area's objects: id, type byte, +0xa8 flag, +0x11c flag,
 * name, position when readable. */
void nwpad_game_debug_area_objects(char *out, size_t cap, int max);
/* Research (F40): light up (on) or un-light the client object with this id the way the game's
 * highlight-all does (vslot 0x160 / 0x168, with the creature's MouseOver). rgb 0-1. */
int nwpad_game_debug_hilite(uint32_t id, bool on, float r, float g, float b);
/* Research (F40): open the object's own radial menu, as a right click on it does
 * (vslot 0x150 of the CNWCObject; the menu appears at the GUI's recorded mouse position).
 * Returns the object's type byte, -1 if it couldn't try. */
int nwpad_game_debug_radial(uint32_t id);
/* Research (F40): do the game's click on the client object with this id, as a mouse click on it
 * would (PerformDefaultActionOnObject). Returns the game's result, -1 if it couldn't try. */
int nwpad_game_debug_click_object(uint32_t id);
/* Whether the game's options panel / area map is open (CGuiInGame+0xd8 non-null /
 * the map panel at +0xb8 active; re-notes F38), for tests. */
bool nwpad_game_menu_open(void);
bool nwpad_game_map_open(void);
#endif
/* The screen's size in GUI units (NUI geometry): g_pGuiMan's size in pixels over
 * the GUI scale; 1280 x 720 if unknown. */
void nwpad_gui_size(float *w, float *h);

#endif
