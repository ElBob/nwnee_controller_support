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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/nwpad_core.h"
#include "backend.h"
#include "control.h"
#include "sigs.h"
#include "settings.h"
#include "quickbar.h"
#include "nui.h"
#include "picker.h"
#include "icons.h"
#include "dialog.h"
#include "dialogui.h"
extern unsigned nwpad_nui_last_size;
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
    int32_t (*GetKeyFromName)(const char *);
} sdl;

#define PICKER_KEYUP_MS 80 /* a picker key-up counts once no key-down follows this soon */

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
        uint64_t sticks_since_ms; /* sticks continuously active since (0: centered) */
        uint64_t last_mouse_ms;   /* last mouse motion (0: none yet) */
        uint64_t nudges; /* edge nudges (re-notes F26) */
    } cursor;
    struct { uint64_t total, mouse_motion, keys, filtered, right_edge_fixes; } events; /* seen by the PollEvent hook */
    struct {        /* the picker key (cfg.picker_key): Steam Input maps a grip or button to it */
        int32_t sym, prev_sym, next_sym, confirm_sym, cancel_sym; /* SDL_Keycodes; 0 none */
        bool resolved, held;             /* held: the picker key is down (debounced) */
        bool was_held, latched;          /* last frame's key, and open by a press */
        int action;                      /* 1 confirm, 2 cancel: applied on the next frame */
        bool confirm_down, cancel_down;
        int32_t up_sym, down_sym;        /* the dialog window's highlight keys */
        bool up_down, down_down;
        int dialog_step, dialog_action;  /* queued for the next frame: highlight, 1 answer / 2 end */
        uint64_t up_ms;                  /* a key-up waiting out PICKER_KEYUP_MS (0 none) */
        bool prev_down, next_down;       /* swallowed key-downs whose key-ups are ours too */
        int shift;                       /* bank change to apply on the next frame */
    } picker;
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
        char sigs[1024], native[512];
        nwpad_sigs_json(sigs, sizeof sigs);
        nwpad_settings_debug_json(native, sizeof native);
        snprintf(out, cap,
                 "{\"ok\":true,\"version\":\"%s\",\"frame\":%llu,\"hooks\":true,"
                 "\"controller\":\"%s\",\"in_game\":%s,"
                 "\"features\":{\"camera\":%s,\"movement\":%s},\"signatures\":%s,"
                 "\"config\":{\"enabled\":%s,\"turn_speed\":%.1f,\"tilt_speed\":%.1f,"
                 "\"run_point\":%.4f,\"hide_cursor\":%s},\"native\":%s}",
                 NWPAD_VERSION, (unsigned long long)g.frames, controller_state(),
                 nwpad_backend_in_game() ? "true" : "false",
                 bs.camera_available ? "true" : "false", bs.movement_available ? "true" : "false",
                 sigs, g.cfg.enabled ? "true" : "false", g.cfg.camera_yaw_speed,
                 g.cfg.camera_pitch_speed, g.cfg.run_threshold, g.cfg.hide_cursor ? "true" : "false", native);
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
    } else if (strcmp(cmd, "quickbar") == 0) {
        /* {"cmd":"quickbar"}: all 36 buttons (bank * 12 + slot) with name and icon. */
        static nwpad_qb_slot slots[NWPAD_QB_SLOTS];
        if (!nwpad_quickbar_read(slots)) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"no quickbar (not in a game?)\"}");
            return;
        }
        int n = snprintf(out, cap, "{\"ok\":true,\"bank\":%d,\"slots\":[", nwpad_quickbar_bank());
        for (int i = 0; i < NWPAD_QB_SLOTS && n > 0 && (size_t)n < cap; i++) {
            char name[300], icon[120], parts[3][120];
            for (int k = 0; k < 3; k++) nwpad_json_escape(parts[k], sizeof parts[k], slots[i].parts[k]);
            nwpad_json_escape(name, sizeof name, slots[i].name);
            nwpad_json_escape(icon, sizeof icon, slots[i].icon);
            n += snprintf(out + n, cap - (size_t)n,
                          "%s{\"slot\":%d,\"type\":%u,\"data\":%llu,\"item\":%u,\"icon\":\"%s\",\"name\":\"%s\","
                          "\"parts\":[\"%s\",\"%s\",\"%s\"]}",
                          i ? "," : "", i, slots[i].type, (unsigned long long)slots[i].data, slots[i].item, icon,
                          name, parts[0], parts[1], parts[2]);
        }
        if (n > 0 && (size_t)n < cap) snprintf(out + n, cap - (size_t)n, "]}");
    } else if (strcmp(cmd, "nui_create") == 0 || strcmp(cmd, "nui_bind") == 0 ||
               strcmp(cmd, "nui_destroy") == 0) {
        /* {"cmd":"nui_create","token":n,"id":"..","json":"<definition>"},
         * {"cmd":"nui_bind","token":n,"name":"..","json":"<value>"}, {"cmd":"nui_destroy","token":n} */
        static char text[32768], name[128];
        double token;
        bool ok = nwpad_json_get_number(request, "token", &token);
        if (ok && cmd[4] == 'c')
            ok = nwpad_json_get_string(request, "id", name, sizeof name) &&
                 nwpad_json_get_string(request, "json", text, sizeof text) && nwpad_nui_create((int)token, name, text);
        else if (ok && cmd[4] == 'b')
            ok = nwpad_json_get_string(request, "name", name, sizeof name) &&
                 nwpad_json_get_string(request, "json", text, sizeof text) && nwpad_nui_bind((int)token, name, text);
        else if (ok)
            ok = nwpad_nui_destroy((int)token);
        snprintf(out, cap, "{\"ok\":%s,\"events_pending\":%s,\"windows\":%d,\"size\":%u}", ok ? "true" : "false",
                 nwpad_nui_events_pending() ? "true" : "false", nwpad_nui_window_count(), nwpad_nui_last_size);
    } else if (strcmp(cmd, "cost_reset") == 0) {
        /* {"cmd":"cost_reset"}: start the frame-cost histogram again (the overhead
         * test on its own: leave out the game's first frames) */
        memset(g.cost.bucket, 0, sizeof g.cost.bucket);
        memset(g.cost.own_bucket, 0, sizeof g.cost.own_bucket);
        g.cost.frames = g.cost.max_ns = g.cost.own_max_ns = 0;
        snprintf(out, cap, "{\"ok\":true}");
    } else if (strcmp(cmd, "nuklear") == 0) {
        /* {"cmd":"nuklear"}: Nuklear's windows and whether a click would stay off the world (research, F37) */
        static char desc[4096];
        nwpad_nui_debug_nuklear(desc, sizeof desc);
        snprintf(out, cap, "{\"ok\":true,\"result\":%s}", desc);
    } else if (strcmp(cmd, "nui_element") == 0) {
        /* {"cmd":"nui_element","token":n,"id":".."}: an element's callback slots (research, F37) */
        double token = 0;
        char id[32] = "";
        nwpad_json_get_number(request, "token", &token);
        nwpad_json_get_string(request, "id", id, sizeof id);
        static char desc[8192];
        nwpad_nui_debug_element((int)token, id, desc, sizeof desc);
        snprintf(out, cap, "{\"ok\":true,\"result\":%s}", desc);
    } else if (strcmp(cmd, "equipped_icon") == 0) {
        /* {"cmd":"equipped_icon","slot_bit":1}: the player's equipped item's icon (debug, F34) */
        double v;
        char desc[1024];
        nwpad_quickbar_debug_equipped_icon(nwpad_json_get_number(request, "slot_bit", &v) ? (unsigned)v : 2, desc, sizeof desc);
        snprintf(out, cap, "{\"ok\":true,\"icon\":%s}", desc);
    } else if (strcmp(cmd, "nui_bench") == 0) {
        /* {"cmd":"nui_bench","token":n,"name":"geo","count":n,"x0":..,"dx":..,"y":..,"w":..,"h":..}:
         * time `count` geometry binds (quickbar plan: animation spike) */
        double token = 0, count = 1, x0 = 0, dx = 1, y = 0, w = 100, h = 100;
        char name[64] = "geo", value[160];
        nwpad_json_get_number(request, "token", &token);
        nwpad_json_get_number(request, "count", &count);
        nwpad_json_get_number(request, "x0", &x0);
        nwpad_json_get_number(request, "dx", &dx);
        nwpad_json_get_number(request, "y", &y);
        nwpad_json_get_number(request, "w", &w);
        nwpad_json_get_number(request, "h", &h);
        nwpad_json_get_string(request, "name", name, sizeof name);
        uint64_t t0 = now_ns(), worst = 0;
        int ok = 0;
        for (int i = 0; i < (int)count; i++) {
            snprintf(value, sizeof value, "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", x0 + dx * i, y, w, h);
            uint64_t a = now_ns();
            ok += nwpad_nui_bind((int)token, name, value);
            if (now_ns() - a > worst) worst = now_ns() - a;
        }
        snprintf(out, cap, "{\"ok\":true,\"binds_ok\":%d,\"avg_us\":%.1f,\"worst_us\":%.1f}", ok,
                 (double)(now_ns() - t0) / 1000.0 / (count > 0 ? count : 1), (double)worst / 1000.0);
    } else if (strcmp(cmd, "quickbar_bank") == 0) {
        double v;
        bool ok = nwpad_json_get_number(request, "bank", &v) && nwpad_quickbar_debug_show_bank((int)v);
        snprintf(out, cap, "{\"ok\":%s,\"bank\":%d}", ok ? "true" : "false", nwpad_quickbar_bank());
    } else if (strcmp(cmd, "dialog") == 0) {
        static char d[12000];
        nwpad_dialog_debug_json(d, sizeof d);
        snprintf(out, cap, "{\"ok\":true,\"dialog\":%s}", d);
    } else if (strcmp(cmd, "dialog_ui") == 0) {
        /* {"cmd":"dialog_ui","step":-1|1} / {"action":"confirm"|"cancel"}: as the keys do */
        double v;
        char action[16] = "";
        if (nwpad_json_get_number(request, "step", &v)) g.picker.dialog_step += v < 0 ? -1 : 1;
        if (nwpad_json_get_number(request, "scroll", &v)) nwpad_dialogui_scroll((int)v);
        nwpad_json_get_string(request, "action", action, sizeof action);
        if (!strcmp(action, "confirm")) g.picker.dialog_action = 1;
        if (!strcmp(action, "cancel")) g.picker.dialog_action = 2;
        extern bool nwpad_dialogui_dry;
        extern nwpad_nui_input nwpad_dialogui_last_input;
        extern unsigned nwpad_dialogui_inputs, nwpad_dialogui_place_tries, nwpad_dialogui_place_tries_max;
        if (nwpad_json_get_number(request, "dry", &v)) nwpad_dialogui_dry = v != 0;
        int first, last;
        nwpad_dialogui_range(&first, &last);
        extern unsigned nwpad_dialogui_builds;
        extern uint64_t nwpad_dialogui_build_ns, nwpad_dialogui_build_max_ns;
        snprintf(out, cap,
                 "{\"ok\":true,\"open\":%s,\"highlight\":%d,\"first\":%d,\"last\":%d,\"text_top\":%d,"
                 "\"rebuilds\":%u,\"rebuild_avg_us\":%.1f,\"rebuild_max_us\":%.1f,\"mouse\":%s,\"inputs\":%u,"
                 "\"last_input\":{\"tag\":%d,\"kind\":%d,\"button\":%d,\"x\":%.1f,\"y\":%.1f},\"events_pending\":%s,"
                 "\"place_tries\":%u,\"place_tries_max\":%u}",
                 nwpad_dialogui_open() ? "true" : "false", nwpad_dialogui_highlight(), first, last,
                 nwpad_dialogui_text_top(), nwpad_dialogui_builds,
                 nwpad_dialogui_builds ? (double)nwpad_dialogui_build_ns / nwpad_dialogui_builds / 1000.0 : 0.0,
                 (double)nwpad_dialogui_build_max_ns / 1000.0, nwpad_dialogui_mouse() ? "true" : "false",
                 nwpad_dialogui_inputs, nwpad_dialogui_last_input.tag, nwpad_dialogui_last_input.kind,
                 nwpad_dialogui_last_input.button, nwpad_dialogui_last_input.x, nwpad_dialogui_last_input.y,
                 nwpad_nui_events_pending() ? "true" : "false", nwpad_dialogui_place_tries, nwpad_dialogui_place_tries_max);
    } else if (strcmp(cmd, "resource_publish") == 0) {
        /* {"cmd":"resource_publish","src":"/tmp/x.dlg","name":"x.dlg"}: serve a file as a game resource */
        char src[512] = "", name[64] = "";
        nwpad_json_get_string(request, "src", src, sizeof src);
        nwpad_json_get_string(request, "name", name, sizeof name);
        bool ok = false;
        FILE *f = src[0] && name[0] ? fopen(src, "rb") : NULL;
        if (f) {
            static char data[1 << 20];
            size_t n = fread(data, 1, sizeof data, f);
            fclose(f);
            ok = nwpad_resource_publish(name, data, n);
        }
        snprintf(out, cap, "{\"ok\":%s}", ok ? "true" : "false");
    } else if (strcmp(cmd, "dialog_preview") == 0) {
        /* {"cmd":"dialog_preview","path":"/tmp/x.txt"} or {"cmd":"dialog_preview"} to close */
        char path[512] = "";
        nwpad_json_get_string(request, "path", path, sizeof path);
        bool ok = nwpad_dialogui_preview(path[0] ? path : NULL);
        snprintf(out, cap, "{\"ok\":%s}", ok ? "true" : "false");
    } else if (strcmp(cmd, "dialog_select") == 0) {
        /* {"cmd":"dialog_select","index":n} (0-based), or {"cmd":"dialog_select","end":1} */
        double v;
        bool ok = nwpad_json_get_number(request, "end", &v) && v ? nwpad_dialog_end()
                  : nwpad_json_get_number(request, "index", &v) ? nwpad_dialog_select((int)v)
                                                                : false;
        snprintf(out, cap, "{\"ok\":%s}", ok ? "true" : "false");
    } else if (strcmp(cmd, "picker") == 0) {
        /* {"cmd":"picker","action":"open"|"confirm"|"cancel"}: as the picker keys do */
        char action[16] = "";
        nwpad_json_get_string(request, "action", action, sizeof action);
        if (!strcmp(action, "open")) g.picker.latched = true;
        else if (!strcmp(action, "confirm")) g.picker.action = 1;
        else if (!strcmp(action, "cancel")) g.picker.action = 2;
        snprintf(out, cap, "{\"ok\":%s}", action[0] ? "true" : "false");
    } else if (strcmp(cmd, "picker_shift") == 0) {
        /* {"cmd":"picker_shift","dir":-1|1}: as the bank keys do */
        double v;
        g.picker.shift += nwpad_json_get_number(request, "dir", &v) && v < 0 ? -1 : 1;
        snprintf(out, cap, "{\"ok\":%s}", nwpad_picker_open() ? "true" : "false");
    } else if (strcmp(cmd, "quickbar_use") == 0) {
        /* {"cmd":"quickbar_use","slot":0-35} */
        double v;
        bool ok = nwpad_json_get_number(request, "slot", &v) && nwpad_quickbar_use((int)v);
        snprintf(out, cap, "{\"ok\":%s}", ok ? "true" : "false");
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
                          "\"nudges\":%llu,\"sticks_for_ms\":%lld,\"mouse_still_ms\":%lld},"
                          "\"picker\":{\"open\":%s,\"bank\":%d,\"selected\":%d,\"last_used\":%d,\"key_held\":%s}",
                          nwpad_magnitude(g.last_left), nwpad_magnitude(g.last_right),
                          sdl.ShowCursor ? "true" : "false", g.cursor.stick_hidden ? "true" : "false",
                          g.cursor.game_wants ? "true" : "false",
                          sdl.ShowCursor && sdl.ShowCursor(SDL_QUERY) == SDL_ENABLE ? "true" : "false",
                          (unsigned long long)g.cursor.nudges,
                          g.cursor.sticks_since_ms ? (long long)(now_ms() - g.cursor.sticks_since_ms) : -1LL,
                          g.cursor.last_mouse_ms ? (long long)(now_ms() - g.cursor.last_mouse_ms) : -1LL,
                          nwpad_picker_open() ? "true" : "false", nwpad_picker_bank(), nwpad_picker_selected(),
                          nwpad_picker_last_used(), g.picker.held ? "true" : "false");
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
    nwpad_settings_load(&g.cfg);
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

    NWPAD_WHERE("settings");
    nwpad_settings_frame(&g.cfg); /* live values from the Options window, if native */
    if ((!g.cfg.enabled || !g.cfg.hide_cursor) && g.cursor.stick_hidden) { /* turned off live */
        g.cursor.stick_hidden = false;
        if (sdl.ShowCursor) sdl.ShowCursor(g.cursor.game_wants ? SDL_ENABLE : SDL_DISABLE);
    }
    if (g.virt.active && g.virt.until_ms && t >= g.virt.until_ms) g.virt.active = false;
    /* No input source (no pad, virtual stick ended) reads as centered sticks, so a
     * character that was moving still gets its stop. So does "Controller support"
     * off, which otherwise does nothing. */
    nwpad_vec2 left = {0, 0}, right = {0, 0};
    if (!g.cfg.enabled) {
    } else if (g.virt.active) {
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
    if (g.picker.up_ms && t - g.picker.up_ms >= PICKER_KEYUP_MS) {
        g.picker.held = false;
        g.picker.up_ms = 0;
    }
    /* nwpad's conversation window: the sticks scroll its text (and move nothing else). */
    NWPAD_WHERE("dialog");
    float nav = fabsf(left.y) > fabsf(right.y) ? left.y : right.y;
    bool talking = nwpad_dialogui_frame(g.cfg.enabled && g.cfg.dialog, nav, t);
    if (talking) {
        for (; g.picker.dialog_step < 0; g.picker.dialog_step++) nwpad_dialogui_move(-1);
        for (; g.picker.dialog_step > 0; g.picker.dialog_step--) nwpad_dialogui_move(1);
        if (g.picker.dialog_action == 1) nwpad_dialogui_confirm();
        else if (g.picker.dialog_action == 2) nwpad_dialogui_cancel();
        left = right = (nwpad_vec2){0, 0};
        g.picker.latched = false;
    }
    g.picker.dialog_step = g.picker.dialog_action = 0;
    /* Each press of the picker key opens the picker, or closes it without using. */
    bool key = g.cfg.enabled && g.picker.held;
    if (key && !g.picker.was_held) g.picker.latched = !(g.picker.latched && nwpad_picker_open());
    g.picker.was_held = key;
    if (g.picker.action) { /* confirm / cancel */
        nwpad_picker_close(g.picker.action == 1);
        g.picker.action = 0;
        g.picker.latched = false;
    }
    if (g.frames == 2) { /* after the first frame's settings registration */
        nwpad_dialog_init();
        if (g.cfg.dialog) nwpad_dialogui_setup_font();
    }
    NWPAD_WHERE("picker");
    for (; g.picker.shift < 0; g.picker.shift++) nwpad_picker_shift(-1);
    for (; g.picker.shift > 0; g.picker.shift--) nwpad_picker_shift(1);
    bool picking = nwpad_picker_frame(g.cfg.enabled && g.picker.latched, right, nwpad_backend_in_game());
    if (!picking) g.picker.latched = false; /* e.g. left the game */
    if (picking) right = (nwpad_vec2){0, 0};

    g.last_left = left;
    g.last_right = right;
    bool sticks_active = nwpad_magnitude(left) > 0 || nwpad_magnitude(right) > 0;
    if (!sticks_active) g.cursor.sticks_since_ms = 0;
    else if (!g.cursor.sticks_since_ms) g.cursor.sticks_since_ms = t;
    /* Moving the mouse shows the cursor; it hides again once the mouse is still and
     * the sticks have been held for cursor_rehide_ms (at once if the mouse was
     * already still that long). */
    if (sdl.ShowCursor && g.cfg.hide_cursor && !g.cursor.stick_hidden &&
        nwpad_cursor_should_hide(t, g.cursor.sticks_since_ms, g.cursor.last_mouse_ms, &g.cfg)) {
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
    g.cost.game_ns += nwpad_nui_take_game_ns(); /* the game's NUI handler: game time */
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
            g.cursor.last_mouse_ms = now_ms();
            nwpad_arbiter_mouse_motion(&g.arbiter, g.cursor.last_mouse_ms);
            if (g.cursor.stick_hidden) { /* the mouse is back: show what the game wants */
                g.cursor.stick_hidden = false;
                sdl.ShowCursor(g.cursor.game_wants ? SDL_ENABLE : SDL_DISABLE);
            }
        } else if (type == SDL_KEYDOWN || type == SDL_KEYUP) {
            g.events.keys++;
            if (!g.picker.resolved) { /* the key names, through the game's own SDL */
                g.picker.resolved = true;
                RESOLVE_ANY(GetKeyFromName, "SDL_GetKeyFromName");
                const char *names[7] = {g.cfg.picker_key, g.cfg.picker_prev_key, g.cfg.picker_next_key,
                                        g.cfg.picker_confirm_key, g.cfg.picker_cancel_key, "Up", "Down"};
                int32_t *syms[7] = {&g.picker.sym, &g.picker.prev_sym, &g.picker.next_sym, &g.picker.confirm_sym,
                                    &g.picker.cancel_sym, &g.picker.up_sym, &g.picker.down_sym};
                for (int k = 0; k < 7; k++) {
                    *syms[k] = names[k][0] && sdl.GetKeyFromName ? sdl.GetKeyFromName(names[k]) : 0;
                    if (names[k][0] && !*syms[k]) nwpad_log("picker key \"%s\" is not a key name; ignored", names[k]);
                }
                nwpad_log("picker keys: open %s, banks %s / %s, confirm %s, cancel %s", g.cfg.picker_key, g.cfg.picker_prev_key,
                          g.cfg.picker_next_key, g.cfg.picker_confirm_key, g.cfg.picker_cancel_key);
            }
            int32_t sym = event->key.sym;
            /* nwpad's conversation window: Up/Down, confirm and cancel are its while it's up. */
            bool *dk = !sym                         ? NULL
                       : sym == g.picker.up_sym     ? &g.picker.up_down
                       : sym == g.picker.down_sym   ? &g.picker.down_down
                       : sym == g.picker.confirm_sym ? &g.picker.confirm_down
                       : sym == g.picker.cancel_sym ? &g.picker.cancel_down
                                                    : NULL;
            if (dk && type == SDL_KEYDOWN && nwpad_dialogui_open()) {
                if (dk == &g.picker.up_down) g.picker.dialog_step--;
                else if (dk == &g.picker.down_down) g.picker.dialog_step++;
                else if (!event->key.repeat) g.picker.dialog_action = dk == &g.picker.confirm_down ? 1 : 2;
                *dk = true;
                g.events.filtered++;
                continue;
            }
            if (dk && type == SDL_KEYUP && *dk && (dk == &g.picker.up_down || dk == &g.picker.down_down)) {
                *dk = false;
                g.events.filtered++;
                continue;
            }
            if (g.picker.sym && sym == g.picker.sym && nwpad_dialogui_open()) { /* no picker in a conversation */
                g.events.filtered++;
                continue;
            }
            if (g.picker.sym && sym == g.picker.sym) {
                /* Held keys autorepeat, which can come as key-up + repeat key-down pairs:
                 * any key-down holds, and a key-up only counts if no key-down follows
                 * within PICKER_KEYUP_MS (checked per frame). */
                if (type == SDL_KEYDOWN) {
                    g.picker.held = true;
                    g.picker.up_ms = 0;
                } else {
                    g.picker.up_ms = now_ms();
                }
                g.events.filtered++;
                continue; /* nwpad's key: the game never sees it */
            }
            /* Bank, confirm and cancel keys are nwpad's only while the picker is open
             * (their key-ups follow); otherwise the game's (Enter: chat, Esc: menu). */
            bool *down = sym && sym == g.picker.prev_sym      ? &g.picker.prev_down
                         : sym && sym == g.picker.next_sym    ? &g.picker.next_down
                         : sym && sym == g.picker.confirm_sym ? &g.picker.confirm_down
                         : sym && sym == g.picker.cancel_sym  ? &g.picker.cancel_down
                                                              : NULL;
            if (down && type == SDL_KEYDOWN && nwpad_picker_open()) {
                if (!event->key.repeat) {
                    if (down == &g.picker.prev_down || down == &g.picker.next_down)
                        g.picker.shift += down == &g.picker.prev_down ? -1 : 1;
                    else
                        g.picker.action = down == &g.picker.confirm_down ? 1 : 2;
                }
                *down = true;
                g.events.filtered++;
                continue;
            }
            if (down && type == SDL_KEYUP && *down) {
                *down = false;
                g.events.filtered++;
                continue;
            }
        } else if (type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            g.picker.held = false; /* the key-up may never come */
            g.picker.up_ms = 0;
            g.picker.latched = false;
        }
        return r;
    }
}
