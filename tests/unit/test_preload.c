/* Preload smoke test: run with LD_PRELOAD=libnwpad.so against fake_sdl.
 * Checks interposition, pass-through to the real SDL, and controller-event filtering. */
#include <stdio.h>
#include "../../src/hook/sdl_min.h"

extern int fake_swap_calls;
void SDL_GL_SwapWindow(SDL_Window *w);
int SDL_PollEvent(SDL_Event *e);

int main(void) {
    int fails = 0, seen = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        seen++;
        if (e.type >= SDL_CONTROLLER_FIRST && e.type <= SDL_CONTROLLER_LAST) {
            fprintf(stderr, "FAIL: controller event leaked through\n");
            fails++;
        }
    }
    if (seen != 2) { fprintf(stderr, "FAIL: expected 2 events, saw %d\n", seen); fails++; }
    for (int i = 0; i < 3; i++) SDL_GL_SwapWindow(NULL);
    if (fake_swap_calls != 3) { fprintf(stderr, "FAIL: swap not forwarded\n"); fails++; }
    printf("preload smoke: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
