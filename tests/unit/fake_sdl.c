/* Stand-in for the game's SDL2, used by the preload smoke test. */
#include <stdint.h>
#include <string.h>
#include "../../src/hook/sdl_min.h"

int fake_swap_calls;
static uint32_t queue[] = {SDL_CONTROLLER_FIRST, SDL_MOUSEMOTION, SDL_CONTROLLER_FIRST + 1, SDL_KEYDOWN};
static unsigned qpos;

void SDL_GL_SwapWindow(SDL_Window *w) { (void)w; fake_swap_calls++; }
int SDL_PollEvent(SDL_Event *e) {
    if (qpos >= sizeof queue / sizeof queue[0]) return 0;
    if (e) { memset(e, 0, sizeof *e); e->type = queue[qpos]; }
    qpos++;
    return 1;
}
int SDL_InitSubSystem(uint32_t f) { (void)f; return 0; }
int SDL_NumJoysticks(void) { return 0; }
int SDL_IsGameController(int i) { (void)i; return 0; }
SDL_GameController *SDL_GameControllerOpen(int i) { (void)i; return 0; }
int SDL_GameControllerGetAttached(SDL_GameController *c) { (void)c; return 0; }
int16_t SDL_GameControllerGetAxis(SDL_GameController *c, int a) { (void)c; (void)a; return 0; }
