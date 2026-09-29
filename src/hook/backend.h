/* Game backend: the only code that touches game functions or memory.
 * Every entry point may be unavailable (signature missing); callers must check.
 * Implementations land in M1 (camera) and M3 (movement). See docs/plan.md §5-§6. */
#ifndef NWPAD_BACKEND_H
#define NWPAD_BACKEND_H

#include <stdbool.h>
#include <stdint.h>
#include "../core/nwpad_core.h"

typedef struct {
    bool camera_available;
    bool movement_available;
} nwpad_backend_status;

void nwpad_backend_init(void);
nwpad_backend_status nwpad_backend_status_get(void);

/* True while the player is in a module with a controllable creature. */
bool nwpad_backend_in_game(void);
/* True when the game would block keyboard movement (dialog, cutscene, text focus...). */
bool nwpad_backend_movement_gated(void);
bool nwpad_backend_always_run(void);

bool nwpad_backend_camera_get(nwpad_camera *cam, nwpad_camera_limits *lim);
/* The camera's forward direction (core convention) for a camera from camera_get. */
float nwpad_backend_camera_forward(const nwpad_camera *cam);
/* The player character's current facing (core convention), client side. */
bool nwpad_backend_player_facing(float *facing_deg);
/* If the game's recorded pointer is on the leftmost or rightmost pixel column
 * (where edge turning spins the camera), move it one pixel in. The next real mouse
 * motion overwrites it. True if it moved (re-notes F26). */
bool nwpad_backend_nudge_pointer_off_edge(void);
/* The game's GUI width in pixels, or 0 if unknown (re-notes F26). */
int nwpad_backend_gui_width(void);
/* The player character's client-side position (what's on screen). */
bool nwpad_backend_player_pos(float *x, float *y);
bool nwpad_backend_camera_set(const nwpad_camera *cam);

bool nwpad_backend_send_move(const nwpad_move_intent *intent);
bool nwpad_backend_send_stop(void);
/* Once per frame, for timed follow-ups such as releasing the stop tap. */
void nwpad_backend_tick(uint64_t now_ms);

/* Debug surface: send a NWScript chunk (wrapped in main) to the server, as the
 * cheat console does (re-notes F17). The server may refuse it. */
bool nwpad_backend_run_script_chunk(const char *code);
/* Debug surface: base address of a named game object for the socket's read
 * command ("module" or "camera"), or NULL. */
void *nwpad_backend_debug_object(const char *name);
/* Server-side player creature (ground truth for tests): world position and
 * facing in degrees counter-clockwise from +X. False if there is none. */
bool nwpad_backend_creature(float *x, float *y, float *facing_deg);
/* Debug surface (M3 RE): walk the player to a world point through the client's
 * mouse walk entry, without the ring effect (re-notes F20). */
bool nwpad_backend_debug_walk_to(float x, float y, int mode);
/* Debug surface (M3 RE): set the client's drive key state as if W/S/Q/E were
 * held (re-notes F20). */
bool nwpad_backend_debug_drive_keys(bool w, bool s, bool q, bool e);
/* Debug surface (tests): set the game's Always Run option (re-notes F23). */
bool nwpad_backend_debug_set_always_run(bool on);

#endif
