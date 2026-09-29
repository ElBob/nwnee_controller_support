/* Placeholder backend: reports every feature unavailable, so the library loads,
 * reads the controller and logs, but never touches the game. Replaced
 * piece by piece as research tasks resolve signatures (docs/re-notes.md). */
#include "backend.h"

void nwpad_backend_init(void) {}
nwpad_backend_status nwpad_backend_status_get(void) {
    return (nwpad_backend_status){.camera_available = false, .movement_available = false};
}
bool nwpad_backend_in_game(void) { return false; }
bool nwpad_backend_movement_gated(void) { return true; }
bool nwpad_backend_always_run(void) { return false; }
bool nwpad_backend_camera_get(nwpad_camera *cam, nwpad_camera_limits *lim) {
    (void)cam; (void)lim; return false;
}
bool nwpad_backend_camera_set(const nwpad_camera *cam) { (void)cam; return false; }
bool nwpad_backend_send_move(const nwpad_move_intent *intent) { (void)intent; return false; }
bool nwpad_backend_send_stop(void) { return false; }
