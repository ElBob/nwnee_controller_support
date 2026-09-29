/* Game backend: the only code that touches game functions or memory.
 * Camera (M1) uses the game's own mouse-look path (re-notes F14, F15, F18). Movement
 * is still unavailable until M3. Every entry point checks its signatures. */
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

static struct {
    bool camera;
    void **app_manager; /* address of the g_pAppManager variable */
    get_module_fn get_module;
    turn_fn turn, tilt;
} b;

void nwpad_backend_init(void) {
    b.camera = nwpad_sigs_all(camera_sigs, sizeof camera_sigs / sizeof camera_sigs[0]);
    if (!b.camera) return;
    b.app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    b.get_module = (get_module_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_MODULE);
    b.turn = (turn_fn)nwpad_sig(NWPAD_SIG_CAMERA_TURN);
    b.tilt = (turn_fn)nwpad_sig(NWPAD_SIG_CAMERA_TILT);
}

nwpad_backend_status nwpad_backend_status_get(void) {
    return (nwpad_backend_status){.camera_available = b.camera, .movement_available = false};
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

bool nwpad_backend_movement_gated(void) { return true; }
bool nwpad_backend_always_run(void) { return false; }

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

void *nwpad_backend_debug_object(const char *name) {
    void *mod = module();
    if (strcmp(name, "server_pc") == 0) return server_pc();
    if (strcmp(name, "module") == 0) return mod;
    if (strcmp(name, "camera") == 0) return mod ? camera_object(mod) : NULL;
    return NULL;
}

bool nwpad_backend_send_move(const nwpad_move_intent *intent) { (void)intent; return false; }
bool nwpad_backend_send_stop(void) { return false; }
