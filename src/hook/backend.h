/* Game backend: the only code that touches game functions or memory.
 * Every entry point may be unavailable (signature missing); callers must check.
 * Implementations land in M1 (camera) and M3 (movement). See docs/plan.md §5-§6. */
#ifndef NWPAD_BACKEND_H
#define NWPAD_BACKEND_H

#include <stdbool.h>
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
bool nwpad_backend_camera_set(const nwpad_camera *cam);

bool nwpad_backend_send_move(const nwpad_move_intent *intent);
bool nwpad_backend_send_stop(void);

/* Debug surface: send a NWScript chunk (wrapped in main) to the server, as the
 * cheat console does (re-notes F17). The server may refuse it. */
bool nwpad_backend_run_script_chunk(const char *code);
/* Debug surface: base address of a named game object for the socket's read
 * command ("module" or "camera"), or NULL. */
void *nwpad_backend_debug_object(const char *name);

#endif
