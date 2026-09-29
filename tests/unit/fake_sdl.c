/* Stand-in for the game's statically linked SDL2, used by the preload smoke test.
 * It mimics SDL_DYNAPI the way nwmain-linux has it: each exported SDL_Foo is a
 * stub that jumps through a writable table, and the table starts out pointing
 * at DEFAULT functions that fill it on first use. Linked into test_preload,
 * which exports its symbols like the game does. x86-64 only. */
#include <stdint.h>
#include <string.h>
#include "../../src/hook/sdl_min.h"

int fake_swap_calls, fake_dynapi_fills;
static uint32_t queue[] = {SDL_CONTROLLER_FIRST, SDL_MOUSEMOTION, SDL_CONTROLLER_FIRST + 1, SDL_KEYDOWN};
static unsigned qpos;

static void real_GetVersion(SDL_version *v) { v->major = 2; v->minor = 0; v->patch = 99; }
static void real_GL_SwapWindow(SDL_Window *w) { (void)w; fake_swap_calls++; }
static int real_PollEvent(SDL_Event *e) {
    if (qpos >= sizeof queue / sizeof queue[0]) return 0;
    if (e) { memset(e, 0, sizeof *e); e->type = queue[qpos]; }
    qpos++;
    return 1;
}

static void default_GetVersion(SDL_version *v);
static void default_GL_SwapWindow(SDL_Window *w);
static int default_PollEvent(SDL_Event *e);

/* The jump table. Referenced by name from the stubs below. */
void (*jt_SDL_GetVersion)(SDL_version *) = default_GetVersion;
void (*jt_SDL_GL_SwapWindow)(SDL_Window *) = default_GL_SwapWindow;
int (*jt_SDL_PollEvent)(SDL_Event *) = default_PollEvent;

static void dynapi_fill(void) {
    jt_SDL_GetVersion = real_GetVersion;
    jt_SDL_GL_SwapWindow = real_GL_SwapWindow;
    jt_SDL_PollEvent = real_PollEvent;
    fake_dynapi_fills++;
}
static void default_GetVersion(SDL_version *v) { dynapi_fill(); jt_SDL_GetVersion(v); }
static void default_GL_SwapWindow(SDL_Window *w) { dynapi_fill(); jt_SDL_GL_SwapWindow(w); }
static int default_PollEvent(SDL_Event *e) { dynapi_fill(); return jt_SDL_PollEvent(e); }

#define DYNAPI_STUB(name)                                                        \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n" \
            "  push %rbp\n  mov %rsp,%rbp\n  pop %rbp\n"                         \
            "  jmp *jt_" #name "(%rip)\n.size " #name ",.-" #name "\n");
DYNAPI_STUB(SDL_GetVersion)
DYNAPI_STUB(SDL_GL_SwapWindow)
DYNAPI_STUB(SDL_PollEvent)

/* The game never calls the GameController API, so these need no stubs. */
int SDL_InitSubSystem(uint32_t f) { (void)f; return 0; }
int SDL_NumJoysticks(void) { return 0; }
int SDL_IsGameController(int i) { (void)i; return 0; }
SDL_GameController *SDL_GameControllerOpen(int i) { (void)i; return 0; }
int SDL_GameControllerGetAttached(SDL_GameController *c) { (void)c; return 0; }
int16_t SDL_GameControllerGetAxis(SDL_GameController *c, int a) { (void)c; (void)a; return 0; }
