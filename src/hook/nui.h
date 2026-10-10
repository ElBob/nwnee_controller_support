/* Client-side NUI windows: nwpad creates, updates and destroys NUI windows itself,
 * by running the game's own handler for the server's NUI messages on a message
 * nwpad builds (re-notes F32). No server is involved. Game thread only. */
#ifndef NWPAD_NUI_H
#define NWPAD_NUI_H

#include <stdbool.h>

/* nwpad's window tokens: far above the small numbers servers hand out. */
#define NWPAD_NUI_TOKEN_BASE 0x6e770000

/* Create (or replace) window `token` from its NUI JSON definition. */
bool nwpad_nui_create(int token, const char *id, const char *json);
/* Set bind `name` of window `token` to a JSON value. Only binds the window's
 * definition uses can be set (others would reach the server, F32). */
bool nwpad_nui_bind(int token, const char *name, const char *json_value);
bool nwpad_nui_destroy(int token);
/* Mouse input on an element of one of nwpad's windows (F37). NUI does the hit
 * testing; nwpad takes over the element's event callbacks, so nothing is queued
 * for the server. Positions are NUI's mouse position (GUI units). */
enum { NWPAD_NUI_DOWN, NWPAD_NUI_UP, NWPAD_NUI_SCROLL };
typedef struct {
    int token, tag, kind;
    int button;     /* DOWN / UP: 0 left, 1 middle, 2 right */
    float x, y;     /* DOWN / UP: where; SCROLL: the wheel's delta */
} nwpad_nui_input;
/* Take over element `id` of window `token` (an element with an "id" in the
 * definition); its input is reported with `tag`. False (and nothing changed) if
 * the element or its callbacks aren't as expected (F37). */
bool nwpad_nui_take_input(int token, const char *id, int tag);
/* The next input taken since the last call, oldest first; false when none. */
bool nwpad_nui_next_input(nwpad_nui_input *out);

/* Whether the game has NUI events queued for the server (none should be ours). */
bool nwpad_nui_events_pending(void);
/* Time spent inside the game's NUI handler since the last call (game time, not
 * nwpad's own, for the frame budget). */
#include <stdint.h>
uint64_t nwpad_nui_take_game_ns(void);
/* Add time spent in other game functions to the same game-time account. */
void nwpad_game_ns_add(uint64_t ns);
uint64_t nwpad_now_ns(void);
/* How many NUI windows the game has (debug). */
int nwpad_nui_window_count(void);

#ifdef NWPAD_DEBUG_SURFACES
#include <stddef.h>
void nwpad_nui_debug_element(int token, const char *id, char *out, size_t cap);
void nwpad_nui_debug_nuklear(char *out, size_t cap);
#endif

#endif
