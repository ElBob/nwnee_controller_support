/* libnwpad.so: LD_PRELOAD entry point. Hooks SDL_GL_SwapWindow (per-frame
 * callback on the main thread) and SDL_PollEvent (controller-event filtering and
 * mouse/keyboard observation for arbitration). See docs/plan.md §7.
 *
 * The game links SDL2 statically and calls it directly, so symbol interposition
 * can't reach it. We hook through SDL's dynamic API jump table instead
 * (docs/re-notes.md F8). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/nwpad_core.h"
#include "backend.h"
#include "control.h"
#include "sdl_min.h"

#ifndef NWPAD_VERSION
#define NWPAD_VERSION "0.0.0-dev"
#endif

/* ---- Logging ---- */

static void nwpad_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[nwpad] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- Game SDL entry points, resolved from the game's own SDL2 ---- */

static struct {
    void (*SwapWindow)(SDL_Window *);  /* original jump-table targets */
    int (*PollEvent)(SDL_Event *);
    int (*InitSubSystem)(uint32_t);
    int (*NumJoysticks)(void);
    int (*IsGameController)(int);
    SDL_GameController *(*GameControllerOpen)(int);
    int (*GameControllerGetAttached)(SDL_GameController *);
    int16_t (*GameControllerGetAxis)(SDL_GameController *, int);
} sdl;

#define RESOLVE_ANY(field, name) \
    (sdl.field = (__typeof__(sdl.field))dlsym(RTLD_DEFAULT, name))

/* ---- SDL dynamic API hooking ----
 * With SDL_DYNAPI, each exported SDL_Foo is a stub that jumps through a writable
 * table: [endbr64] [push %rbp; mov %rsp,%rbp; pop %rbp] jmp *slot(%rip).
 * The table starts out pointing at stubs that fill it on first use, so we force
 * that fill first, then swap our wrappers into the slots. */

static void **dynapi_slot(void *fn) {
    static const uint8_t endbr64[] = {0xf3, 0x0f, 0x1e, 0xfa};
    static const uint8_t frame[] = {0x55, 0x48, 0x89, 0xe5, 0x5d};
    const uint8_t *p = fn;
    if (!p) return NULL;
    if (memcmp(p, endbr64, sizeof endbr64) == 0) p += sizeof endbr64;
    if (memcmp(p, frame, sizeof frame) == 0) p += sizeof frame;
    if (p[0] != 0xff || p[1] != 0x25) return NULL; /* jmp *disp32(%rip) */
    int32_t disp;
    memcpy(&disp, p + 2, sizeof disp);
    return (void **)(uintptr_t)(p + 6 + disp);
}

static bool dynapi_hook(const char *name, void *hook, void **orig) {
    void **slot = dynapi_slot(dlsym(RTLD_DEFAULT, name));
    if (!slot) {
        nwpad_log("%s is not an SDL dynamic API stub; not hooked", name);
        return false;
    }
    *orig = *slot;
    *slot = hook;
    return true;
}

static void nwpad_SwapWindow(SDL_Window *window);
static int nwpad_PollEvent(SDL_Event *event);

static bool install_hooks(void) {
    void (*get_version)(SDL_version *) =
        (void (*)(SDL_version *))dlsym(RTLD_DEFAULT, "SDL_GetVersion");
    if (!get_version) {
        nwpad_log("game SDL2 not found; hooks not installed");
        return false;
    }
    SDL_version v;
    get_version(&v); /* the first SDL call fills the jump table */
    nwpad_log("game SDL %u.%u.%u", v.major, v.minor, v.patch);

    void *swap, *poll;
    if (!dynapi_hook("SDL_GL_SwapWindow", (void *)nwpad_SwapWindow, &swap)) return false;
    if (!dynapi_hook("SDL_PollEvent", (void *)nwpad_PollEvent, &poll)) {
        dynapi_hook("SDL_GL_SwapWindow", swap, &swap); /* put the original back */
        return false;
    }
    sdl.SwapWindow = (void (*)(SDL_Window *))swap;
    sdl.PollEvent = (int (*)(SDL_Event *))poll;
    return true;
}

/* ---- State ---- */

static struct {
    bool disabled;
    bool controller_ready;
    bool controller_init_failed;
    SDL_GameController *pad;
    uint64_t frames;
    uint64_t last_frame_ms, last_scan_ms;
    nwpad_config cfg;
    nwpad_send_policy send_policy;
    nwpad_send_state send_state;
    nwpad_arbiter arbiter;
    nwpad_move_mode move_mode;
} g;

static void load_config(void) {
    nwpad_config_defaults(&g.cfg);
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[512];
    if (xdg && *xdg) snprintf(path, sizeof path, "%s/nwpad/config.toml", xdg);
    else if (home) snprintf(path, sizeof path, "%s/.config/nwpad/config.toml", home);
    else return;

    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    int applied = nwpad_config_parse(&g.cfg, buf);
    if (applied < 0) {
        nwpad_config_defaults(&g.cfg);
        nwpad_log("config %s is malformed; using defaults", path);
    } else {
        nwpad_log("config %s: %d setting(s) applied", path, applied);
    }
}

#ifdef NWPAD_DEBUG_SURFACES
/* ---- Control socket commands (plan §8.3); main thread only ---- */

static const char *controller_state(void) {
    if (g.controller_init_failed) return "unavailable";
    return g.pad ? "open" : "none";
}

static void control_handler(const char *request, char *out, size_t cap) {
    char cmd[32];
    if (!nwpad_json_get_string(request, "cmd", cmd, sizeof cmd)) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"missing cmd\"}");
    } else if (strcmp(cmd, "ping") == 0) {
        snprintf(out, cap, "{\"ok\":true,\"version\":\"%s\",\"frame\":%llu}", NWPAD_VERSION,
                 (unsigned long long)g.frames);
    } else if (strcmp(cmd, "status") == 0) {
        nwpad_backend_status bs = nwpad_backend_status_get();
        snprintf(out, cap,
                 "{\"ok\":true,\"version\":\"%s\",\"frame\":%llu,\"hooks\":true,"
                 "\"controller\":\"%s\",\"in_game\":%s,"
                 "\"features\":{\"camera\":%s,\"movement\":%s},\"signatures\":{}}",
                 NWPAD_VERSION, (unsigned long long)g.frames, controller_state(),
                 nwpad_backend_in_game() ? "true" : "false",
                 bs.camera_available ? "true" : "false", bs.movement_available ? "true" : "false");
    } else {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"unknown cmd\"}");
    }
}
#endif

__attribute__((constructor)) static void nwpad_init(void) {
    const char *dis = getenv("NWPAD_DISABLE");
    g.disabled = dis && *dis && strcmp(dis, "0") != 0;
    if (g.disabled) {
        nwpad_log("version %s loaded (disabled by NWPAD_DISABLE)", NWPAD_VERSION);
        return;
    }
    if (!install_hooks()) {
        g.disabled = true;
        nwpad_log("version %s loaded; inactive", NWPAD_VERSION);
        return;
    }
    load_config();
    nwpad_send_policy_defaults(&g.send_policy);
    nwpad_arbiter_init(&g.arbiter);
    nwpad_backend_init();
    nwpad_backend_status bs = nwpad_backend_status_get();
    nwpad_log("version %s loaded; camera backend: %s, movement backend: %s", NWPAD_VERSION,
              bs.camera_available ? "available" : "unavailable",
              bs.movement_available ? "available" : "unavailable");
#ifdef NWPAD_DEBUG_SURFACES
    nwpad_control_start(control_handler);
#endif
}

/* ---- Controller ---- */

static bool controller_init(void) {
    if (g.controller_ready) return true;
    if (g.controller_init_failed) return false;
    RESOLVE_ANY(InitSubSystem, "SDL_InitSubSystem");
    RESOLVE_ANY(NumJoysticks, "SDL_NumJoysticks");
    RESOLVE_ANY(IsGameController, "SDL_IsGameController");
    RESOLVE_ANY(GameControllerOpen, "SDL_GameControllerOpen");
    RESOLVE_ANY(GameControllerGetAttached, "SDL_GameControllerGetAttached");
    RESOLVE_ANY(GameControllerGetAxis, "SDL_GameControllerGetAxis");
    if (!sdl.InitSubSystem || !sdl.NumJoysticks || !sdl.IsGameController ||
        !sdl.GameControllerOpen || !sdl.GameControllerGetAttached || !sdl.GameControllerGetAxis) {
        nwpad_log("game SDL2 lacks GameController API; controller support disabled");
        g.controller_init_failed = true;
        return false;
    }
    if (sdl.InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        nwpad_log("SDL_InitSubSystem(GAMECONTROLLER) failed; controller support disabled");
        g.controller_init_failed = true;
        return false;
    }
    g.controller_ready = true;
    return true;
}

static void controller_ensure_open(uint64_t t) {
    if (g.pad && sdl.GameControllerGetAttached(g.pad)) return;
    g.pad = NULL;
    if (t - g.last_scan_ms < 1000) return; /* rescan at most once a second */
    g.last_scan_ms = t;
    int n = sdl.NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (sdl.IsGameController(i) && (g.pad = sdl.GameControllerOpen(i))) {
            nwpad_log("opened gamepad index %d", i);
            return;
        }
    }
}

static float axis(int a) {
    int16_t v = sdl.GameControllerGetAxis(g.pad, a);
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

/* ---- Per-frame work ---- */

static void nwpad_frame(void) {
    uint64_t t = now_ms();
    if (!g.frames++) nwpad_log("first frame");
    float dt = g.last_frame_ms ? (float)(t - g.last_frame_ms) / 1000.0f : 0.0f;
    if (dt > 0.1f) dt = 0.1f; /* hitch guard: never jump more than 100 ms of motion */
    g.last_frame_ms = t;

    if (!controller_init()) return;
    controller_ensure_open(t);
    if (!g.pad) return;

    /* SDL Y axes are +down; core convention is +forward / +up. */
    nwpad_vec2 left = nwpad_apply_deadzone(
        (nwpad_vec2){axis(SDL_CONTROLLER_AXIS_LEFTX), -axis(SDL_CONTROLLER_AXIS_LEFTY)});
    nwpad_vec2 right = nwpad_apply_deadzone(
        (nwpad_vec2){axis(SDL_CONTROLLER_AXIS_RIGHTX), -axis(SDL_CONTROLLER_AXIS_RIGHTY)});

    nwpad_arbiter_update(&g.arbiter, left, right, t, &g.cfg);
    if (!nwpad_backend_in_game()) return;

    nwpad_camera cam;
    nwpad_camera_limits lim;
    bool have_cam = nwpad_backend_camera_get(&cam, &lim);
    if (have_cam && g.arbiter.camera_owned_by_stick &&
        nwpad_magnitude(right) >= NWPAD_SAFETY_DEADZONE) {
        nwpad_camera next = nwpad_camera_step(cam, right, dt, &g.cfg, &lim);
        nwpad_backend_camera_set(&next);
        cam = next;
    }

    if (!have_cam || !g.arbiter.movement_owned_by_stick) return;
    nwpad_vec2 move = nwpad_backend_movement_gated() ? (nwpad_vec2){0, 0} : left;
    nwpad_move_intent intent = nwpad_move_intent_compute(
        move, cam.yaw_deg, nwpad_backend_always_run(), g.move_mode, &g.cfg);
    g.move_mode = intent.mode;
    switch (nwpad_send_decide(&g.send_state, &g.send_policy, &intent, t)) {
    case NWPAD_SEND_MOVE: nwpad_backend_send_move(&intent); break;
    case NWPAD_SEND_STOP: nwpad_backend_send_stop(); break;
    case NWPAD_SEND_NONE: break;
    }
}

/* ---- Interposed SDL functions ---- */

static void nwpad_SwapWindow(SDL_Window *window) {
    nwpad_frame();
    nwpad_control_service();
    sdl.SwapWindow(window);
}

static uint8_t move_key_bit(int32_t scancode) {
    switch (scancode) {
    case SDL_SCANCODE_W: return NWPAD_KEY_W;
    case SDL_SCANCODE_A: return NWPAD_KEY_A;
    case SDL_SCANCODE_S: return NWPAD_KEY_S;
    case SDL_SCANCODE_D: return NWPAD_KEY_D;
    case SDL_SCANCODE_Q: return NWPAD_KEY_Q;
    case SDL_SCANCODE_E: return NWPAD_KEY_E;
    default: return 0;
    }
}

static int nwpad_PollEvent(SDL_Event *event) {
    for (;;) {
        int r = sdl.PollEvent(event);
        if (!r || !event) return r;
        uint32_t type = event->type;
        if (type >= SDL_CONTROLLER_FIRST && type <= SDL_CONTROLLER_LAST)
            continue; /* the game never sees controller events; Steam Input covers buttons */
        if (type == SDL_MOUSEMOTION) {
            nwpad_arbiter_mouse_motion(&g.arbiter, now_ms());
        } else if ((type == SDL_KEYDOWN || type == SDL_KEYUP) && !event->key.repeat) {
            uint8_t bit = move_key_bit(event->key.scancode);
            if (bit) nwpad_arbiter_move_key(&g.arbiter, bit, type == SDL_KEYDOWN);
        }
        return r;
    }
}
