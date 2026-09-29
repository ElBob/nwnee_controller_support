/* Game backend: the only code that touches game functions or memory.
 * Camera (M1) uses the game's own mouse-look path (re-notes F14, F15). Movement
 * is still unavailable until M3. Every entry point checks its signatures. */
#include "backend.h"

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
#define MODULE_CAMERA_LOCKS 0x2bc  /* dword: 0x10 yaw locked, 0x20 pitch locked */
#define LOCK_YAW 0x10u
#define LOCK_PITCH 0x20u

typedef void *(*get_module_fn)(void *client_app);
typedef void (*turn_fn)(void *module, float delta_deg, int direct);
typedef float (*pitch_limit_fn)(void *module);
typedef void *(*vcall_int_fn)(void *self, int arg);
typedef void *(*vcall_fn)(void *self);

static const int camera_sigs[] = {
    NWPAD_SIG_APP_MANAGER, NWPAD_SIG_CLIENT_GET_MODULE, NWPAD_SIG_CAMERA_TURN,
    NWPAD_SIG_CAMERA_TILT, NWPAD_SIG_CAMERA_MIN_PITCH, NWPAD_SIG_CAMERA_MAX_PITCH,
};

static struct {
    bool camera;
    void **app_manager; /* address of the g_pAppManager variable */
    get_module_fn get_module;
    turn_fn turn, tilt;
    pitch_limit_fn min_pitch, max_pitch;
} b;

void nwpad_backend_init(void) {
    b.camera = nwpad_sigs_all(camera_sigs, sizeof camera_sigs / sizeof camera_sigs[0]);
    if (!b.camera) return;
    b.app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    b.get_module = (get_module_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_MODULE);
    b.turn = (turn_fn)nwpad_sig(NWPAD_SIG_CAMERA_TURN);
    b.tilt = (turn_fn)nwpad_sig(NWPAD_SIG_CAMERA_TILT);
    b.min_pitch = (pitch_limit_fn)nwpad_sig(NWPAD_SIG_CAMERA_MIN_PITCH);
    b.max_pitch = (pitch_limit_fn)nwpad_sig(NWPAD_SIG_CAMERA_MAX_PITCH);
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
    uint32_t locks;
    memcpy(&locks, (char *)mod + MODULE_CAMERA_LOCKS, sizeof locks);
    cam->yaw_deg = read_float(c, CAMERA_YAW);
    cam->pitch_deg = read_float(c, CAMERA_PITCH);
    lim->min_pitch = b.min_pitch(mod);
    lim->max_pitch = b.max_pitch(mod);
    lim->yaw_locked = locks & LOCK_YAW;
    lim->pitch_locked = locks & LOCK_PITCH;
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

bool nwpad_backend_send_move(const nwpad_move_intent *intent) { (void)intent; return false; }
bool nwpad_backend_send_stop(void) { return false; }
