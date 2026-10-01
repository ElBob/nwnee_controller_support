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
/* Whether the game has NUI events queued for the server (none should be ours). */
bool nwpad_nui_events_pending(void);
/* How many NUI windows the game has (debug). */
int nwpad_nui_window_count(void);

#endif
