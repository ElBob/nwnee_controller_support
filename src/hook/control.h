/* Control socket (plan §8.3): JSON lines over $XDG_RUNTIME_DIR/nwpad.sock.
 * Debug builds only (NWPAD_DEBUG_SURFACES), and only when NWPAD_SOCKET=1.
 *
 * A background thread owns the socket. Each request is handed to the game's
 * main thread, which answers it from the frame hook, so a reply also proves the
 * frame loop is running. The frame-side cost is one atomic load, plus a
 * trylock when a request is waiting. */
#ifndef NWPAD_CONTROL_H
#define NWPAD_CONTROL_H

#include <stddef.h>

/* Runs on the main thread. Writes one JSON object (no newline) into out. */
typedef void (*nwpad_control_handler)(const char *request, char *out, size_t cap);

#ifdef NWPAD_DEBUG_SURFACES
void nwpad_control_start(nwpad_control_handler handler);
void nwpad_control_service(void);
#else
static inline void nwpad_control_service(void) {}
#endif

#endif
