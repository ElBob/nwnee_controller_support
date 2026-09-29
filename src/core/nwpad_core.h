/* nwpad core: pure, game-independent logic. No SDL, no game memory, no I/O.
 * Everything here is unit-tested in tests/unit. See docs/plan.md §3 and §6. */
#ifndef NWPAD_CORE_H
#define NWPAD_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define NWPAD_SAFETY_DEADZONE 0.05f /* fixed, not configurable (plan §6.3) */

typedef struct { float x, y; } nwpad_vec2;

/* ---- Configuration (plan §6.3) ---- */
typedef struct {
    float camera_yaw_speed;   /* degrees per second at full deflection */
    float camera_pitch_speed; /* degrees per second at full deflection */
    float run_threshold;      /* raw magnitude where walk becomes run */
    float run_hysteresis;     /* total band width around the threshold */
    uint32_t mouse_idle_ms;   /* mouse idle time before the stick regains the camera */
} nwpad_config;

void nwpad_config_defaults(nwpad_config *cfg);
/* Parse a minimal TOML subset ("key = number", '#' comments). Unknown keys are
 * ignored. Returns the number of keys applied, or -1 on a malformed line. */
int nwpad_config_parse(nwpad_config *cfg, const char *text);

/* ---- Stick processing ---- */
/* Radial safety deadzone: zero inside, raw passthrough outside (no rescaling;
 * shaping belongs to Steam Input). Magnitude is clamped to 1. */
nwpad_vec2 nwpad_apply_deadzone(nwpad_vec2 raw);
float nwpad_magnitude(nwpad_vec2 v);

/* ---- Movement (plan §3, §5) ----
 * Angles are in degrees, counter-clockwise from world +X, in [0, 360).
 * Stick convention: +x right, +y forward (callers flip SDL's +down Y). */
float nwpad_wrap_deg(float deg);
/* World movement bearing for a stick direction given the camera's forward yaw. */
float nwpad_world_bearing(nwpad_vec2 stick, float camera_yaw_deg);
/* Signed smallest difference a-b in (-180, 180]. */
float nwpad_angle_diff(float a_deg, float b_deg);

typedef enum { NWPAD_MOVE_IDLE = 0, NWPAD_MOVE_WALK, NWPAD_MOVE_RUN } nwpad_move_mode;

/* Walk/run selection with hysteresis; Always Run forces RUN when deflected. */
nwpad_move_mode nwpad_move_mode_update(nwpad_move_mode prev, float magnitude,
                                       bool always_run, const nwpad_config *cfg);

typedef struct {
    bool moving;
    nwpad_move_mode mode;
    float bearing_deg; /* world movement direction */
    float facing_deg;  /* character facing: camera forward while moving */
} nwpad_move_intent;

nwpad_move_intent nwpad_move_intent_compute(nwpad_vec2 stick_after_deadzone,
                                            float camera_yaw_deg, bool always_run,
                                            nwpad_move_mode prev_mode,
                                            const nwpad_config *cfg);

/* ---- Send-rate limiting (plan §5.2) ---- */
typedef struct {
    float heading_threshold_deg; /* resend when bearing/facing moves this much */
    uint32_t keepalive_ms;       /* resend while moving at least this often */
    uint32_t min_interval_ms;    /* hard cap: never send more often than this */
} nwpad_send_policy;

typedef enum { NWPAD_SEND_NONE = 0, NWPAD_SEND_MOVE, NWPAD_SEND_STOP } nwpad_send_action;

typedef struct {
    bool has_sent;
    bool was_moving;
    nwpad_move_intent last_sent;
    uint64_t last_send_ms;
} nwpad_send_state;

void nwpad_send_policy_defaults(nwpad_send_policy *p);
/* Decide what (if anything) to send this frame, and update state if sending.
 * STOP is emitted exactly once per moving->idle transition and is never rate-capped. */
nwpad_send_action nwpad_send_decide(nwpad_send_state *st, const nwpad_send_policy *p,
                                    const nwpad_move_intent *intent, uint64_t now_ms);

/* ---- Camera (plan §6) ---- */
typedef struct {
    float min_pitch, max_pitch; /* current game limits, degrees */
    bool yaw_locked, pitch_locked;
} nwpad_camera_limits;

typedef struct { float yaw_deg, pitch_deg; } nwpad_camera;

/* Apply right-stick deltas: linear in raw deflection, scaled by dt, clamped and
 * lock-aware. Stick +y means pitch up. */
nwpad_camera nwpad_camera_step(nwpad_camera cam, nwpad_vec2 stick_after_deadzone,
                               float dt_s, const nwpad_config *cfg,
                               const nwpad_camera_limits *lim);

/* ---- Device arbitration: last-used device wins (plan §3) ---- */
typedef struct {
    uint64_t last_mouse_ms;
    bool mouse_seen;
    uint8_t move_keys_held; /* bitmask of WASDQE held */
    bool camera_owned_by_stick;
    bool movement_owned_by_stick;
} nwpad_arbiter;

enum { NWPAD_KEY_W = 1, NWPAD_KEY_A = 2, NWPAD_KEY_S = 4, NWPAD_KEY_D = 8,
       NWPAD_KEY_Q = 16, NWPAD_KEY_E = 32 };

void nwpad_arbiter_init(nwpad_arbiter *a);
void nwpad_arbiter_mouse_motion(nwpad_arbiter *a, uint64_t now_ms);
void nwpad_arbiter_move_key(nwpad_arbiter *a, uint8_t key_bit, bool down);
/* Call once per frame with the deadzoned sticks; updates ownership flags. */
void nwpad_arbiter_update(nwpad_arbiter *a, nwpad_vec2 left, nwpad_vec2 right,
                          uint64_t now_ms, const nwpad_config *cfg);

#endif
