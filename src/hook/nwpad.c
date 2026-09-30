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
#include "sigs.h"
#include "crashtrace.h"
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

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
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
    int (*ShowCursor)(int);            /* NULL if not hooked (cursor hiding off) */
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
static int nwpad_ShowCursor(int toggle);

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
    /* Optional: hiding the cursor while the sticks are in use. */
    void *show;
    if (dynapi_hook("SDL_ShowCursor", (void *)nwpad_ShowCursor, &show))
        sdl.ShowCursor = (int (*)(int))show;
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
    nwpad_move_style move_style;
    nwpad_style_state style_state;
    nwpad_vec2 last_left, last_right; /* deadzoned sticks this frame (state reports them) */
    struct {             /* cursor hidden while the sticks are in use (Robert's request) */
        bool game_wants; /* what the game last asked SDL_ShowCursor for */
        bool stick_hidden;
        uint64_t nudges; /* edge nudges (re-notes F26) */
    } cursor;
    struct { uint64_t total, mouse_motion, keys, filtered, right_edge_fixes; } events; /* seen by the PollEvent hook */
    int last_motion_x, last_motion_xrel; /* as SDL reported them (state shows them) */
    bool right_edge_pinned; /* pointer pushed onto the last reachable column (re-notes F27;
                             * only with NWPAD_XWAYLAND_EDGE_FIX) */
    struct { uint64_t moves, stops, last_move_ms, min_gap_ms; } sends; /* rate-cap evidence */
    struct { /* per-frame cost (plan §7 budget), 10 us buckets up to 2.55 ms */
        uint32_t bucket[256], own_bucket[256]; /* all work / excluding game functions we call */
        uint64_t frames, max_ns, own_max_ns;
        uint64_t game_ns; /* this frame's time inside game functions (backend calls) */
    } cost;
    struct { /* control socket override (plan §8.3), core convention */
        bool active;
        nwpad_vec2 left, right;
        uint64_t until_ms; /* 0: until released */
    } virt;
} g;

static void load_config(void) {
    nwpad_config_defaults(&g.cfg);
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *explicit_path = getenv("NWPAD_CONFIG"); /* tests use their own file */
    char path[512];
    if (explicit_path && *explicit_path) snprintf(path, sizeof path, "%s", explicit_path);
    else if (xdg && *xdg) snprintf(path, sizeof path, "%s/nwpad/config.toml", xdg);
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

/* Upper edge (us) of the bucket holding the given percentile of frame costs. */
static unsigned cost_percentile_us(const uint32_t *bucket, double pct) {
    uint64_t need = (uint64_t)(g.cost.frames * pct / 100.0 + 0.5), seen = 0;
    for (unsigned i = 0; i < 256; i++) {
        seen += bucket[i];
        if (seen >= need && seen) return (i + 1) * 10;
    }
    return 2560;
}

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
        char sigs[1024];
        nwpad_sigs_json(sigs, sizeof sigs);
        snprintf(out, cap,
                 "{\"ok\":true,\"version\":\"%s\",\"frame\":%llu,\"hooks\":true,"
                 "\"controller\":\"%s\",\"in_game\":%s,"
                 "\"features\":{\"camera\":%s,\"movement\":%s},\"signatures\":%s}",
                 NWPAD_VERSION, (unsigned long long)g.frames, controller_state(),
                 nwpad_backend_in_game() ? "true" : "false",
                 bs.camera_available ? "true" : "false", bs.movement_available ? "true" : "false",
                 sigs);
    } else if (strcmp(cmd, "stick") == 0) {
        /* {"cmd":"stick","lx":..,"ly":..,"rx":..,"ry":..,"hold_ms":..}; +y is forward/up. */
        double v;
        g.virt.left.x = nwpad_json_get_number(request, "lx", &v) ? (float)v : 0.0f;
        g.virt.left.y = nwpad_json_get_number(request, "ly", &v) ? (float)v : 0.0f;
        g.virt.right.x = nwpad_json_get_number(request, "rx", &v) ? (float)v : 0.0f;
        g.virt.right.y = nwpad_json_get_number(request, "ry", &v) ? (float)v : 0.0f;
        g.virt.until_ms = nwpad_json_get_number(request, "hold_ms", &v) && v > 0
                              ? now_ms() + (uint64_t)v : 0;
        g.virt.active = true;
        snprintf(out, cap, "{\"ok\":true}");
    } else if (strcmp(cmd, "script_chunk") == 0) {
        /* {"cmd":"script_chunk","code":"LockCameraPitch(GetFirstPC(), TRUE);"} */
        char code[768];
        if (!nwpad_json_get_string(request, "code", code, sizeof code))
            snprintf(out, cap, "{\"ok\":false,\"error\":\"missing code\"}");
        else if (!nwpad_backend_run_script_chunk(code))
            snprintf(out, cap, "{\"ok\":false,\"error\":\"script chunks unavailable\"}");
        else
            snprintf(out, cap, "{\"ok\":true}");
    } else if (strcmp(cmd, "read") == 0) {
        /* {"cmd":"read","base":"module|camera","offset":0,"len":256} -> hex bytes (RE only) */
        char base_name[16];
        double off = 0, len = 0;
        void *base = nwpad_json_get_string(request, "base", base_name, sizeof base_name)
                         ? nwpad_backend_debug_object(base_name) : NULL;
        nwpad_json_get_number(request, "offset", &off);
        nwpad_json_get_number(request, "len", &len);
        size_t n = len > 0 ? (size_t)len : 0, max = (cap - 64) / 2;
        if (!base || off < 0 || n == 0 || n > max) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"bad base, offset, or len (max %zu)\"}", max);
        } else {
            int k = snprintf(out, cap, "{\"ok\":true,\"hex\":\"");
            const uint8_t *p = (const uint8_t *)base + (size_t)off;
            for (size_t i = 0; i < n; i++) k += snprintf(out + k, cap - (size_t)k, "%02x", p[i]);
            snprintf(out + k, cap - (size_t)k, "\"}");
        }
    } else if (strcmp(cmd, "walk_to") == 0) {
        /* {"cmd":"walk_to","x":..,"y":..,"mode":0} (M3 RE) */
        double x, y, mode = 0;
        nwpad_json_get_number(request, "mode", &mode);
        if (!nwpad_json_get_number(request, "x", &x) || !nwpad_json_get_number(request, "y", &y))
            snprintf(out, cap, "{\"ok\":false,\"error\":\"missing x or y\"}");
        else if (!nwpad_backend_debug_walk_to((float)x, (float)y, (int)mode))
            snprintf(out, cap, "{\"ok\":false,\"error\":\"walk unavailable\"}");
        else
            snprintf(out, cap, "{\"ok\":true}");
    } else if (strcmp(cmd, "drive_keys") == 0) {
        /* {"cmd":"drive_keys","w":0,"s":1,"q":0,"e":0} (M3 RE) */
        double w = 0, sk = 0, q = 0, e = 0;
        nwpad_json_get_number(request, "w", &w);
        nwpad_json_get_number(request, "s", &sk);
        nwpad_json_get_number(request, "q", &q);
        nwpad_json_get_number(request, "e", &e);
        if (!nwpad_backend_debug_drive_keys(w != 0, sk != 0, q != 0, e != 0))
            snprintf(out, cap, "{\"ok\":false,\"error\":\"drive unavailable\"}");
        else
            snprintf(out, cap, "{\"ok\":true}");
    } else if (strcmp(cmd, "always_run") == 0) {
        /* {"cmd":"always_run","on":1} (tests) */
        double on = 0;
        nwpad_json_get_number(request, "on", &on);
        if (!nwpad_backend_debug_set_always_run(on != 0))
            snprintf(out, cap, "{\"ok\":false,\"error\":\"options unavailable\"}");
        else
            snprintf(out, cap, "{\"ok\":true}");
    } else if (strcmp(cmd, "reset_cost") == 0) {
        memset(&g.cost, 0, sizeof g.cost);
        snprintf(out, cap, "{\"ok\":true}");
    } else if (strcmp(cmd, "release") == 0) {
        g.virt.active = false;
        snprintf(out, cap, "{\"ok\":true}");
    } else if (strcmp(cmd, "state") == 0) {
        nwpad_camera cam;
        nwpad_camera_limits lim;
        bool have = nwpad_backend_camera_get(&cam, &lim);
        int n = snprintf(out, cap,
                         "{\"ok\":true,\"frame\":%llu,\"t_ms\":%llu,\"in_game\":%s,"
                         "\"virtual_stick\":%s,\"camera_owner\":\"%s\","
                         "\"events\":{\"total\":%llu,\"mouse_motion\":%llu,\"keys\":%llu,"
                         "\"filtered\":%llu,\"right_edge_fixes\":%llu,\"right_edge_pinned\":%s,"
                         "\"last_motion_x\":%d,\"last_motion_xrel\":%d}",
                         (unsigned long long)g.frames, (unsigned long long)now_ms(),
                         nwpad_backend_in_game() ? "true" : "false",
                         g.virt.active ? "true" : "false",
                         g.arbiter.camera_owned_by_stick ? "stick" : "mouse",
                         (unsigned long long)g.events.total, (unsigned long long)g.events.mouse_motion,
                         (unsigned long long)g.events.keys, (unsigned long long)g.events.filtered,
                         (unsigned long long)g.events.right_edge_fixes,
                         g.right_edge_pinned ? "true" : "false", g.last_motion_x, g.last_motion_xrel);
        if (n > 0 && (size_t)n < cap)
            n += snprintf(out + n, cap - (size_t)n,
                          ",\"frame_cost_us\":{\"p50\":%u,\"p99\":%u,\"max\":%.1f,\"frames\":%llu,"
                          "\"own_p99\":%u,\"own_max\":%.1f}"
                          ",\"sends\":{\"moves\":%llu,\"stops\":%llu,\"min_gap_ms\":%llu,\"cap_ms\":%u}",
                          cost_percentile_us(g.cost.bucket, 50), cost_percentile_us(g.cost.bucket, 99),
                          (double)g.cost.max_ns / 1000.0, (unsigned long long)g.cost.frames,
                          cost_percentile_us(g.cost.own_bucket, 99), (double)g.cost.own_max_ns / 1000.0,
                          (unsigned long long)g.sends.moves,
                          (unsigned long long)g.sends.stops, (unsigned long long)g.sends.min_gap_ms,
                          (unsigned)g.send_policy.min_interval_ms);
        static const char *styles[] = {"rest", "drag", "strafe_right", "backpedal", "strafe_left"};
        static const char *modes[] = {"idle", "walk", "run"};
        float pf, px, py;
        if (n > 0 && (size_t)n < cap)
            n += snprintf(out + n, cap - (size_t)n,
                          ",\"sticks\":{\"left\":%.3f,\"right\":%.3f},"
                          "\"cursor\":{\"hooked\":%s,\"stick_hidden\":%s,\"game_wants\":%s,\"shown\":%s,"
                          "\"nudges\":%llu}",
                          nwpad_magnitude(g.last_left), nwpad_magnitude(g.last_right),
                          sdl.ShowCursor ? "true" : "false", g.cursor.stick_hidden ? "true" : "false",
                          g.cursor.game_wants ? "true" : "false",
                          sdl.ShowCursor && sdl.ShowCursor(SDL_QUERY) == SDL_ENABLE ? "true" : "false",
                          (unsigned long long)g.cursor.nudges);
        if (n > 0 && (size_t)n < cap)
            n += snprintf(out + n, cap - (size_t)n, ",\"move_style\":\"%s\",\"move_mode\":\"%s\",\"always_run\":%s",
                          styles[g.move_style], modes[g.move_mode],
                          nwpad_backend_always_run() ? "true" : "false");
        if (n > 0 && (size_t)n < cap && nwpad_backend_player_facing(&pf) &&
            nwpad_backend_player_pos(&px, &py))
            n += snprintf(out + n, cap - (size_t)n,
                          ",\"client\":{\"x\":%.4f,\"y\":%.4f,\"facing\":%.3f}", px, py, pf);
        float cx, cy, cf;
        if (n > 0 && (size_t)n < cap && nwpad_backend_creature(&cx, &cy, &cf))
            n += snprintf(out + n, cap - (size_t)n,
                          ",\"creature\":{\"x\":%.4f,\"y\":%.4f,\"facing\":%.3f}", cx, cy, cf);
        if (n > 0 && (size_t)n < cap) {
            if (have)
                snprintf(out + n, cap - (size_t)n,
                         ",\"camera\":{\"yaw\":%.4f,\"pitch\":%.4f,\"min_pitch\":%.4f,"
                         "\"max_pitch\":%.4f,\"yaw_locked\":%s,\"pitch_locked\":%s}}",
                         cam.yaw_deg, cam.pitch_deg, lim.min_pitch, lim.max_pitch,
                         lim.yaw_locked ? "true" : "false", lim.pitch_locked ? "true" : "false");
            else
                snprintf(out + n, cap - (size_t)n, ",\"camera\":null}");
        }
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
    nwpad_sigs_resolve();
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
    if (!g.frames++) {
        nwpad_log("first frame");
        nwpad_crashtrace_install(); /* after the game has installed its own handlers */
    }
    float dt = g.last_frame_ms ? (float)(t - g.last_frame_ms) / 1000.0f : 0.0f;
    if (dt > 0.1f) dt = 0.1f; /* hitch guard: never jump more than 100 ms of motion */
    g.last_frame_ms = t;

    if (g.virt.active && g.virt.until_ms && t >= g.virt.until_ms) g.virt.active = false;
    /* No input source (no pad, virtual stick ended) reads as centered sticks, so a
     * character that was moving still gets its stop. */
    nwpad_vec2 left = {0, 0}, right = {0, 0};
    if (g.virt.active) {
        left = nwpad_apply_deadzone(g.virt.left);
        right = nwpad_apply_deadzone(g.virt.right);
    } else if (controller_init()) {
        controller_ensure_open(t);
        if (g.pad) {
            /* SDL Y axes are +down; core convention is +forward / +up. */
            left = nwpad_apply_deadzone(
                (nwpad_vec2){axis(SDL_CONTROLLER_AXIS_LEFTX), -axis(SDL_CONTROLLER_AXIS_LEFTY)});
            right = nwpad_apply_deadzone(
                (nwpad_vec2){axis(SDL_CONTROLLER_AXIS_RIGHTX), -axis(SDL_CONTROLLER_AXIS_RIGHTY)});
        }
    }

    g.last_left = left;
    g.last_right = right;
    bool sticks_active = nwpad_magnitude(left) > 0 || nwpad_magnitude(right) > 0;
    if (sdl.ShowCursor && g.cfg.hide_cursor && !g.cursor.stick_hidden && sticks_active) {
        g.cursor.stick_hidden = true;
        sdl.ShowCursor(SDL_DISABLE);
    }
    /* With edge turning on, a pointer left on the outermost pixel spins the camera
     * once the stick is released; move the game's recorded pointer one pixel in. */
    /* The pin (right_edge_pinned) stays: like the left edge, the next mouse motion,
     * even purely vertical, reports the edge again and edge turning resumes. */
    NWPAD_WHERE("edge nudge");
    if (sticks_active && nwpad_backend_nudge_pointer_off_edge()) g.cursor.nudges++;
    nwpad_arbiter_update(&g.arbiter, right, t, &g.cfg);
    NWPAD_WHERE("backend tick");
    nwpad_backend_tick(t);
    NWPAD_WHERE("in_game check");
    if (!nwpad_backend_in_game()) {
        NWPAD_WHERE("");
        return;
    }

    nwpad_camera cam;
    nwpad_camera_limits lim;
    NWPAD_WHERE("camera read");
    bool have_cam = nwpad_backend_camera_get(&cam, &lim);
    if (have_cam && g.arbiter.camera_owned_by_stick &&
        nwpad_magnitude(right) >= NWPAD_SAFETY_DEADZONE) {
        nwpad_camera next = nwpad_camera_step(cam, right, dt, &g.cfg, &lim);
        uint64_t g0 = now_ns();
        NWPAD_WHERE("camera turn");
        nwpad_backend_camera_set(&next);
        g.cost.game_ns += now_ns() - g0;
        cam = next;
    }

    float facing;
    NWPAD_WHERE("player facing");
    if (!have_cam || !nwpad_backend_player_facing(&facing)) {
        NWPAD_WHERE("");
        return;
    }
    nwpad_vec2 move = nwpad_backend_movement_gated() ? (nwpad_vec2){0, 0} : left;
    nwpad_move_intent intent = nwpad_move_intent_compute(
        move, nwpad_backend_camera_forward(&cam), facing, nwpad_backend_always_run(), g.move_mode,
        &g.style_state, t, &g.cfg);
    g.move_mode = intent.mode;
    g.move_style = intent.style;
    nwpad_move_style sent_style = g.send_state.last_sent.style;
    switch (nwpad_send_decide(&g.send_state, &g.send_policy, &intent, t)) {
    case NWPAD_SEND_MOVE:
        /* Style changes are exempt from the cap (core); count only same-style gaps. */
        if (g.sends.moves && intent.style == sent_style) {
            uint64_t gap = t - g.sends.last_move_ms;
            if (!g.sends.min_gap_ms || gap < g.sends.min_gap_ms) g.sends.min_gap_ms = gap;
        }
        g.sends.moves++;
        g.sends.last_move_ms = t;
        {
            uint64_t g0 = now_ns();
            NWPAD_WHERE("move send");
            nwpad_backend_send_move(&intent);
            g.cost.game_ns += now_ns() - g0;
        }
        break;
    case NWPAD_SEND_STOP:
        g.sends.stops++;
        {
            uint64_t g0 = now_ns();
            NWPAD_WHERE("stop send");
            nwpad_backend_send_stop();
            g.cost.game_ns += now_ns() - g0;
        }
        break;
    case NWPAD_SEND_NONE: break;
    }
    NWPAD_WHERE("");
}

/* ---- Interposed SDL functions ---- */

static void nwpad_SwapWindow(SDL_Window *window) {
    uint64_t t0 = now_ns();
    g.cost.game_ns = 0;
    nwpad_frame();
    uint64_t ns = now_ns() - t0; /* control-socket servicing is debug-only; not counted */
    uint64_t own = ns > g.cost.game_ns ? ns - g.cost.game_ns : 0;
    g.cost.bucket[ns / 10000 < 255 ? ns / 10000 : 255]++;
    g.cost.own_bucket[own / 10000 < 255 ? own / 10000 : 255]++;
    g.cost.frames++;
    if (ns > g.cost.max_ns) g.cost.max_ns = ns;
    if (own > g.cost.own_max_ns) g.cost.own_max_ns = own;
    nwpad_control_service();
    sdl.SwapWindow(window);
}

/* The game shows the cursor itself whenever its shape changes (hovering a door
 * or NPC), so while the sticks hide it, remember the game's requests instead of
 * applying them (re-notes F24). Queries report the game's own view. */
static int nwpad_ShowCursor(int toggle) {
    if (toggle == SDL_ENABLE || toggle == SDL_DISABLE) g.cursor.game_wants = toggle == SDL_ENABLE;
    if (g.cursor.stick_hidden) {
        if (toggle == SDL_QUERY) return g.cursor.game_wants ? SDL_ENABLE : SDL_DISABLE;
        return sdl.ShowCursor(SDL_DISABLE) >= 0 ? toggle : -1;
    }
    return sdl.ShowCursor(toggle);
}

static int nwpad_PollEvent(SDL_Event *event) {
    for (;;) {
        int r = sdl.PollEvent(event);
        if (!r || !event) return r;
        uint32_t type = event->type;
        g.events.total++;
        if (type >= SDL_CONTROLLER_FIRST && type <= SDL_CONTROLLER_LAST) {
            g.events.filtered++;
            continue; /* the game never sees controller events; Steam Input covers buttons */
        }
        if (type == SDL_MOUSEMOTION) {
            g.last_motion_x = event->motion.x;
            g.last_motion_xrel = event->motion.xrel;
#ifdef NWPAD_XWAYLAND_EDGE_FIX
            /* Under 2x desktop scaling on XWayland, X pointer coordinates are even,
             * so the last column (width-1), where the game's right-edge turning
             * triggers, can't be reached. Moving right onto width-2 counts as reaching
             * it (re-notes F27). */
            NWPAD_WHERE("right-edge fix");
            int w = nwpad_backend_gui_width();
            if (w > 2 && event->motion.x >= w - 2) {
                /* Sticky: a mouse pressed against the edge keeps sending events on
                 * the last two columns (sub-pixel rounding lands on either), so keep
                 * reporting the edge until the pointer moves further left. */
                if (event->motion.xrel > 0 && !g.right_edge_pinned) {
                    g.right_edge_pinned = true;
                    g.events.right_edge_fixes++;
                }
                if (g.right_edge_pinned) event->motion.x = w - 1;
            } else {
                g.right_edge_pinned = false;
            }
#endif
            g.events.mouse_motion++;
            nwpad_arbiter_mouse_motion(&g.arbiter, now_ms());
            if (g.cursor.stick_hidden) { /* the mouse is back: show what the game wants */
                g.cursor.stick_hidden = false;
                sdl.ShowCursor(g.cursor.game_wants ? SDL_ENABLE : SDL_DISABLE);
            }
        } else if (type == SDL_KEYDOWN || type == SDL_KEYUP) {
            g.events.keys++;
        }
        return r;
    }
}
