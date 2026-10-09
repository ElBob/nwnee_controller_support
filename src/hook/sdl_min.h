/* Minimal SDL2 ABI declarations, so the library builds without SDL headers and
 * always binds to the game's own SDL2 at runtime. Layouts match SDL2 2.x. */
#ifndef NWPAD_SDL_MIN_H
#define NWPAD_SDL_MIN_H

#include <stdint.h>

typedef struct SDL_Window SDL_Window;
typedef struct _SDL_GameController SDL_GameController;
typedef struct SDL_version { uint8_t major, minor, patch; } SDL_version;

/* SDL_Event is a 56-byte union; we only inspect the type and keyboard fields. */
typedef union SDL_Event {
    uint32_t type;
    struct {
        uint32_t type, timestamp, windowID;
        uint8_t state, repeat, padding2, padding3;
        int32_t scancode; /* SDL_Keysym.scancode */
        int32_t sym;
        uint16_t mod;
        uint32_t unused;
    } key;
    struct { /* SDL_MouseMotionEvent */
        uint32_t type, timestamp, windowID, which, state;
        int32_t x, y, xrel, yrel;
    } motion;
    struct { /* SDL_MouseButtonEvent */
        uint32_t type, timestamp, windowID, which;
        uint8_t button, state, clicks, padding1;
        int32_t x, y;
    } button;
    struct { /* SDL_MouseWheelEvent */
        uint32_t type, timestamp, windowID, which;
        int32_t x, y;
        uint32_t direction;
    } wheel;
    struct { /* SDL_WindowEvent */
        uint32_t type, timestamp, windowID;
        uint8_t event;
    } window;
    uint8_t padding[56];
} SDL_Event;

enum {
    SDL_WINDOWEVENT = 0x200,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP = 0x301,
    SDL_MOUSEMOTION = 0x400,
    SDL_MOUSEBUTTONDOWN = 0x401,
    SDL_MOUSEBUTTONUP = 0x402,
    SDL_MOUSEWHEEL = 0x403,
    SDL_CONTROLLER_FIRST = 0x650, /* SDL_CONTROLLERAXISMOTION */
    SDL_CONTROLLER_LAST = 0x6FF,  /* covers button, device, touchpad, sensor events */
};

enum { SDL_INIT_GAMECONTROLLER = 0x00002000u };
enum { SDL_QUERY = -1, SDL_DISABLE = 0, SDL_ENABLE = 1 };

enum {
    SDL_CONTROLLER_AXIS_LEFTX = 0,
    SDL_CONTROLLER_AXIS_LEFTY = 1,
    SDL_CONTROLLER_AXIS_RIGHTX = 2,
    SDL_CONTROLLER_AXIS_RIGHTY = 3,
};

enum { SDL_WINDOWEVENT_FOCUS_LOST = 13 };
enum { SDL_BUTTON_LEFT = 1 };


#endif
