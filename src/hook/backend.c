/* Game backend: the only code that touches game functions or memory.
 * Camera (M1) uses the game's own mouse-look path (re-notes F14, F15, F18).
 * Movement (M3) walks like a mouse drag and strafes/backpedals through the
 * keyboard handler (F20, F21). Every entry point checks its signatures. */
#include "backend.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sigs.h"

/* Struct offsets on nwmain-linux 6d19c39b (re-notes F15). */
#define MODULE_CAMERA_HOLDER 0xe8  /* object whose vcalls lead to the camera */
#define HOLDER_VCALL_LOOKUP 0xf0   /* vcall(-1) -> intermediate object */
#define LOOKUP_VCALL_CAMERA 0x48   /* vcall() -> camera object */
#define CAMERA_YAW 0x54            /* float, degrees */
#define CAMERA_PITCH 0x5c          /* float, degrees; 1 top-down .. 89 head-on */
/* Effective limits (re-notes F18). A script lock collapses a range to one value. */
#define CAMERA_YAW_MIN 0x68
#define CAMERA_YAW_MAX 0x6c
#define CAMERA_PITCH_MIN 0x74
#define CAMERA_PITCH_MAX 0x78
#define LOCKED_RANGE 1e-3f

typedef void *(*get_module_fn)(void *client_app);
typedef void (*turn_fn)(void *module, float delta_deg, int direct);
typedef void *(*vcall_int_fn)(void *self, int arg);
typedef void *(*vcall_fn)(void *self);

static const int camera_sigs[] = {
    NWPAD_SIG_APP_MANAGER, NWPAD_SIG_CLIENT_GET_MODULE, NWPAD_SIG_CAMERA_TURN,
    NWPAD_SIG_CAMERA_TILT,
};

static const int movement_sigs[] = {
    NWPAD_SIG_APP_MANAGER, NWPAD_SIG_CLIENT_GET_PLAYER_CREATURE,
    NWPAD_SIG_CLIENT_WALK_PLAYER_TO_POINT, NWPAD_SIG_CLIENT_HANDLE_INPUT_EVENT,
    NWPAD_SIG_CLIENT_GET_NWC_MESSAGE, NWPAD_SIG_CLIENT_STOP_DRAG_MODE,
};

static struct {
    bool camera, movement;
    void **app_manager; /* address of the g_pAppManager variable */
    get_module_fn get_module;
    turn_fn turn, tilt;
} b;

void nwpad_backend_init(void) {
    b.movement = nwpad_sigs_all(movement_sigs, sizeof movement_sigs / sizeof movement_sigs[0]);
    b.camera = nwpad_sigs_all(camera_sigs, sizeof camera_sigs / sizeof camera_sigs[0]);
    b.app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    if (!b.camera) return;
    b.get_module = (get_module_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_MODULE);
    b.turn = (turn_fn)nwpad_sig(NWPAD_SIG_CAMERA_TURN);
    b.tilt = (turn_fn)nwpad_sig(NWPAD_SIG_CAMERA_TILT);
}

nwpad_backend_status nwpad_backend_status_get(void) {
    return (nwpad_backend_status){.camera_available = b.camera, .movement_available = b.movement};
}

static void *vcall_slot(void *obj, size_t offset) {
    void **vtable = *(void ***)obj;
    return vtable[offset / sizeof(void *)];
}

/* The client module, or NULL outside a module (main menu, loading). */
static void *module(void) {
    if (!b.camera) return NULL;
    void *mgr = *b.app_manager;
    void *app = mgr ? *(void **)mgr : NULL;
    return app ? b.get_module(app) : NULL;
}

static void *camera_object(void *mod) {
    void *holder = *(void **)((char *)mod + MODULE_CAMERA_HOLDER);
    if (!holder) return NULL;
    void *lookup = ((vcall_int_fn)vcall_slot(holder, HOLDER_VCALL_LOOKUP))(holder, -1);
    if (!lookup) return NULL;
    return ((vcall_fn)vcall_slot(lookup, LOOKUP_VCALL_CAMERA))(lookup);
}

static float read_float(void *base, size_t offset) {
    float v;
    memcpy(&v, (char *)base + offset, sizeof v);
    return v;
}

bool nwpad_backend_in_game(void) {
    void *mod = module();
    return mod && camera_object(mod);
}

/* Gating (dialog, cutscene, text focus) is M4. The keyboard handler already
 * applies its own checks to strafe/backpedal. */
bool nwpad_backend_movement_gated(void) { return false; }
typedef void *(*client_options_fn)(void *client_app);
typedef void (*set_always_run_fn)(void *client_options, int on);
#define OPTIONS_ALWAYS_RUN 0x4 /* int (re-notes F23) */

static void *client_options(void) {
    client_options_fn get = (client_options_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_CLIENT_OPTIONS);
    if (!get || !b.app_manager || !*b.app_manager) return NULL;
    void *app = *(void **)*b.app_manager;
    return app ? get(app) : NULL;
}

bool nwpad_backend_always_run(void) {
    void *opt = client_options();
    int32_t v = 0;
    if (opt) memcpy(&v, (char *)opt + OPTIONS_ALWAYS_RUN, sizeof v);
    return v != 0;
}

bool nwpad_backend_debug_set_always_run(bool on) {
    void *opt = client_options();
    set_always_run_fn set = (set_always_run_fn)nwpad_sig(NWPAD_SIG_CLIENT_SET_ALWAYS_RUN);
    if (!opt || !set) return false;
    set(opt, on);
    return true;
}

bool nwpad_backend_camera_get(nwpad_camera *cam, nwpad_camera_limits *lim) {
    void *mod = module();
    void *c = mod ? camera_object(mod) : NULL;
    if (!c) return false;
    cam->yaw_deg = read_float(c, CAMERA_YAW);
    cam->pitch_deg = read_float(c, CAMERA_PITCH);
    lim->min_pitch = read_float(c, CAMERA_PITCH_MIN);
    lim->max_pitch = read_float(c, CAMERA_PITCH_MAX);
    lim->yaw_locked = read_float(c, CAMERA_YAW_MAX) - read_float(c, CAMERA_YAW_MIN) < LOCKED_RANGE;
    lim->pitch_locked = lim->max_pitch - lim->min_pitch < LOCKED_RANGE;
    return true;
}

/* Apply the difference as one mouse-look step; the game consumes it next frame. */
bool nwpad_backend_camera_set(const nwpad_camera *next) {
    void *mod = module();
    void *c = mod ? camera_object(mod) : NULL;
    if (!c) return false;
    float dyaw = nwpad_angle_diff(next->yaw_deg, read_float(c, CAMERA_YAW));
    float dpitch = next->pitch_deg - read_float(c, CAMERA_PITCH);
    if (dyaw != 0.0f) b.turn(mod, dyaw, 1);
    if (dpitch != 0.0f) b.tilt(mod, dpitch, 1);
    return true;
}

/* CExoString as the game lays it out (re-notes F17). */
typedef struct {
    const char *str;
    uint32_t len;
} exo_string;

typedef void *(*get_nwc_message_fn)(void *client_app);
typedef void (*run_script_chunk_fn)(void *nwc_message, const exo_string *code, uint32_t oid, int wrap);

#define OBJECT_INVALID 0x7f000000u

bool nwpad_backend_run_script_chunk(const char *code) {
    get_nwc_message_fn get_msg = (get_nwc_message_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_NWC_MESSAGE);
    run_script_chunk_fn send = (run_script_chunk_fn)nwpad_sig(NWPAD_SIG_CHEAT_RUN_SCRIPT_CHUNK);
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    if (!get_msg || !send || !app_manager || !*app_manager) return false;
    void *app = *(void **)*app_manager;
    void *msg = app ? get_msg(app) : NULL;
    if (!msg) return false;
    exo_string s = {code, (uint32_t)strlen(code)};
    send(msg, &s, OBJECT_INVALID, 1); /* the sender copies the string */
    return true;
}

typedef uint32_t (*first_pc_fn)(void *server_app);
typedef void *(*creature_by_id_fn)(void *server_app, uint32_t oid);

/* The first player's creature in the in-process server (re-notes F19), or NULL. */
static void *server_pc(void) {
    first_pc_fn first = (first_pc_fn)nwpad_sig(NWPAD_SIG_SERVER_FIRST_PC);
    creature_by_id_fn by_id = (creature_by_id_fn)nwpad_sig(NWPAD_SIG_SERVER_CREATURE_BY_ID);
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    if (!first || !by_id || !app_manager || !*app_manager) return NULL;
    void *server = ((void **)*app_manager)[1]; /* CAppManager: client app, then server app */
    if (!server) return NULL;
    uint32_t oid = first(server);
    return oid == OBJECT_INVALID ? NULL : by_id(server, oid);
}

/* CNWSCreature offsets (re-notes F19). */
#define CREATURE_POS_X 0xa4
#define CREATURE_POS_Y 0xa8
#define CREATURE_FACING_X 0xb0 /* unit facing vector */
#define CREATURE_FACING_Y 0xb4

bool nwpad_backend_creature(float *x, float *y, float *facing_deg) {
    void *c = server_pc();
    if (!c) return false;
    *x = read_float(c, CREATURE_POS_X);
    *y = read_float(c, CREATURE_POS_Y);
    *facing_deg = nwpad_wrap_deg(atan2f(read_float(c, CREATURE_FACING_Y),
                                        read_float(c, CREATURE_FACING_X)) * 57.29577951f);
    return true;
}

typedef void *(*player_creature_fn)(void *client_app);
typedef int (*walk_to_point_fn)(void *client_internal, float x, float y, float z, int mode,
                                uint32_t target_oid, int ring);

/* CNWCCreature position (re-notes F20). */
#define CLIENT_CREATURE_POS_Z 0x40

bool nwpad_backend_debug_walk_to(float x, float y, int mode) {
    player_creature_fn get_pc = (player_creature_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_PLAYER_CREATURE);
    walk_to_point_fn walk = (walk_to_point_fn)nwpad_sig(NWPAD_SIG_CLIENT_WALK_PLAYER_TO_POINT);
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    if (!get_pc || !walk || !app_manager || !*app_manager) return false;
    void *app = *(void **)*app_manager;
    void *pc = app ? get_pc(app) : NULL;
    if (!pc) return false;
    void *internal = *(void **)((char *)app + 0x8); /* CClientExoApp -> internal (F14) */
    walk(internal, x, y, read_float(pc, CLIENT_CREATURE_POS_Z), mode, OBJECT_INVALID, 0);
    return true;
}

/* CClientExoAppInternal drive key state (re-notes F20): one int per key. */
#define DRIVE_KEY_W 0x1b8
#define DRIVE_KEY_S 0x1bc
#define DRIVE_KEY_Q 0x1c8
#define DRIVE_KEY_E 0x1cc

static void *client_internal(void) {
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    void *app = app_manager && *app_manager ? *(void **)*app_manager : NULL;
    return app ? *(void **)((char *)app + 0x8) : NULL;
}

static void write_int(void *base, size_t offset, int32_t v) { memcpy((char *)base + offset, &v, sizeof v); }

bool nwpad_backend_debug_drive_keys(bool w, bool s, bool q, bool e) {
    void *in = client_internal();
    if (!in) return false;
    write_int(in, DRIVE_KEY_W, w);
    write_int(in, DRIVE_KEY_S, s);
    write_int(in, DRIVE_KEY_Q, q);
    write_int(in, DRIVE_KEY_E, e);
    return true;
}

static void *player_creature(void);

void *nwpad_backend_debug_object(const char *name) {
    void *mod = module();
    if (strcmp(name, "server_pc") == 0) return server_pc();
    if (strcmp(name, "client_internal") == 0) return client_internal();
    if (strcmp(name, "client_pc") == 0) return player_creature();
    if (strcmp(name, "module") == 0) return mod;
    if (strcmp(name, "camera") == 0) return mod ? camera_object(mod) : NULL;
    return NULL;
}

/* ---- Movement (M3) ---- */

float nwpad_backend_camera_forward(const nwpad_camera *cam) {
    return nwpad_wrap_deg(cam->yaw_deg + 90.0f); /* yaw field = forward - 90 (re-notes F18) */
}

/* CNWCCreature (re-notes F20): position, and a facing vector the drive uses. */
#define CLIENT_CREATURE_POS_X 0x38
#define CLIENT_CREATURE_POS_Y 0x3c
#define CLIENT_CREATURE_FACING_X 0x44
#define CLIENT_CREATURE_FACING_Y 0x48

/* HandleInputEvent actions (re-notes F21) and WalkPlayerToPoint modes (F20). */
#define ACTION_STRAFE_LEFT 0x57  /* Q */
#define ACTION_STRAFE_RIGHT 0x58 /* E */
#define ACTION_BACKPEDAL 0x5b    /* S */
#define ACTION_FORWARD 0x5a      /* W */
#define STOP_TAP_MS 60           /* long enough for one drive packet to go out */
#define WALK_MODE_WALK 1
#define WALK_MODE_RUN 2
#define DRAG_LOOKAHEAD 2.0f      /* metres ahead of the character, like a held cursor */
/* CClientExoAppInternal input mode byte: 1 while the mouse drags (re-notes F22). */
#define INPUT_MODE 0x140
#define INPUT_MODE_NONE 0
#define INPUT_MODE_DRAG 1

typedef unsigned (*handle_input_fn)(void *client_internal, int action, int pressed, int p3, int p4);
typedef void (*stop_drag_fn)(void *nwc_message);

static nwpad_move_style active_style; /* what we're currently driving */
static uint64_t now_cache_ms, stop_tap_release_ms; /* 0: no tap pending */

static void *player_creature(void) {
    if (!b.movement) return NULL;
    void *app = *b.app_manager ? *(void **)*b.app_manager : NULL;
    return app ? ((player_creature_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_PLAYER_CREATURE))(app) : NULL;
}

bool nwpad_backend_player_facing(float *facing_deg) {
    void *pc = player_creature();
    if (!pc) return false;
    *facing_deg = nwpad_wrap_deg(atan2f(read_float(pc, CLIENT_CREATURE_FACING_Y),
                                        read_float(pc, CLIENT_CREATURE_FACING_X)) * 57.29577951f);
    return true;
}

bool nwpad_backend_player_pos(float *x, float *y) {
    void *pc = player_creature();
    if (!pc) return false;
    *x = read_float(pc, CLIENT_CREATURE_POS_X);
    *y = read_float(pc, CLIENT_CREATURE_POS_Y);
    return true;
}

static int style_action(nwpad_move_style style) {
    switch (style) {
    case NWPAD_STYLE_STRAFE_LEFT: return ACTION_STRAFE_LEFT;
    case NWPAD_STYLE_STRAFE_RIGHT: return ACTION_STRAFE_RIGHT;
    case NWPAD_STYLE_BACKPEDAL: return ACTION_BACKPEDAL;
    default: return 0;
    }
}

/* Press or release a movement key through the game's own input handler. */
static void key(void *in, int action, int pressed) {
    ((handle_input_fn)nwpad_sig(NWPAD_SIG_CLIENT_HANDLE_INPUT_EVENT))(in, action, pressed, 0, 0);
}

static void walk_to(void *in, void *pc, float x, float y, int mode) {
    ((walk_to_point_fn)nwpad_sig(NWPAD_SIG_CLIENT_WALK_PLAYER_TO_POINT))(
        in, x, y, read_float(pc, CLIENT_CREATURE_POS_Z), mode, OBJECT_INVALID, 0 /* no ring */);
}

static void end_drag(void *in) {
    void *app = *(void **)*b.app_manager;
    void *msg = ((get_nwc_message_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_NWC_MESSAGE))(app);
    if (msg) ((stop_drag_fn)nwpad_sig(NWPAD_SIG_CLIENT_STOP_DRAG_MODE))(msg);
    *((uint8_t *)in + INPUT_MODE) = INPUT_MODE_NONE;
}

/* Release a pending stop tap (re-notes F22). */
static void finish_stop_tap(void *in) {
    if (!stop_tap_release_ms) return;
    key(in, ACTION_FORWARD, 0); /* the handler sends AbortDriveControl */
    stop_tap_release_ms = 0;
}

void nwpad_backend_tick(uint64_t now_ms) {
    now_cache_ms = now_ms;
    if (stop_tap_release_ms && now_ms >= stop_tap_release_ms) {
        void *in = client_internal();
        if (in) finish_stop_tap(in);
    }
}

bool nwpad_backend_send_move(const nwpad_move_intent *intent) {
    void *pc = player_creature(), *in = client_internal();
    if (!pc || !in) return false;
    finish_stop_tap(in); /* moving again cancels the tap early */
    if (active_style != intent->style) { /* end the old style before anything else */
        if (style_action(active_style)) key(in, style_action(active_style), 0);
        else if (active_style == NWPAD_STYLE_DRAG) end_drag(in);
    }
    if (intent->style == NWPAD_STYLE_DRAG) {
        *((uint8_t *)in + INPUT_MODE) = INPUT_MODE_DRAG; /* as a held mouse button */
        float r = intent->bearing_deg * 0.017453292f;
        walk_to(in, pc, read_float(pc, CLIENT_CREATURE_POS_X) + DRAG_LOOKAHEAD * cosf(r),
                read_float(pc, CLIENT_CREATURE_POS_Y) + DRAG_LOOKAHEAD * sinf(r),
                intent->mode == NWPAD_MOVE_RUN ? WALK_MODE_RUN : WALK_MODE_WALK);
    } else if (style_action(intent->style)) {
        key(in, style_action(intent->style), 1); /* repeated like key autorepeat */
    }
    active_style = intent->style;
    return true;
}

bool nwpad_backend_send_stop(void) {
    void *pc = player_creature(), *in = client_internal();
    if (!pc || !in) return false;
    if (style_action(active_style)) {
        key(in, style_action(active_style), 0); /* the handler sends AbortDriveControl */
    } else if (active_style == NWPAD_STYLE_DRAG) {
        /* A walk keeps going to its target, so hand the stop to the keyboard drive:
         * end the drag like a mouse release, then tap forward. The character already
         * faces its path, so the tap doesn't turn it (Robert; re-notes F22). */
        end_drag(in);
        key(in, ACTION_FORWARD, 1);
        stop_tap_release_ms = now_cache_ms + STOP_TAP_MS;
    }
    active_style = NWPAD_STYLE_REST;
    return true;
}
