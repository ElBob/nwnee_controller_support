/* nwpad core: pure, game-independent logic. No SDL, no game memory, no I/O.
 * Everything here is unit-tested in tests/unit. See docs/plan.md §3 and §6. */
#ifndef NWPAD_CORE_H
#define NWPAD_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NWPAD_SAFETY_DEADZONE 0.15f /* fixed, not configurable (plan §6.3); covers an Xbox stick that settles off-centre */

typedef struct { float x, y; } nwpad_vec2;

/* ---- Configuration (plan §6.3) ---- */
typedef struct {
    float camera_yaw_speed;   /* degrees per second at full deflection */
    float camera_pitch_speed; /* degrees per second at full deflection */
    float run_threshold;      /* raw magnitude where walk becomes run */
    float run_hysteresis;     /* total band width around the threshold */
    uint32_t mouse_idle_ms;   /* mouse idle time before the stick regains the camera */
    float strafe_window_deg;  /* half-width of the strafe/backpedal windows (plan §3) */
    bool hide_cursor;         /* hide the mouse cursor while the sticks are in use */
    uint32_t cursor_rehide_ms; /* after mouse motion: still mouse + held stick this long re-hides */
    uint32_t strafe_exit_ms;  /* how long outside a strafe window before it becomes a drag */
    bool enabled;             /* "Controller support": off leaves the library idle */
} nwpad_config;

void nwpad_config_defaults(nwpad_config *cfg);
/* Parse a minimal TOML subset ("key = number", '#' comments). Unknown keys are
 * ignored. Returns the number of keys applied, or -1 on a malformed line. */
int nwpad_config_parse(nwpad_config *cfg, const char *text);

/* ---- settings.tml [nwpad] section (docs/settings-plan.md §5) ----
 * Keys: nwpad.enabled, nwpad.hide-cursor, nwpad.mouse-idle-ms, nwpad.camera.turn-speed,
 * nwpad.camera.tilt-speed, nwpad.movement.run-point, nwpad.movement.strafe-window,
 * nwpad.movement.strafe-exit-ms. run-point is the centre of the walk/run band; the
 * band width stays NWPAD_RUN_BAND. Values are clamped to the plan's ranges. */
#define NWPAD_RUN_BAND 0.125f
/* Apply the [nwpad] tables of a settings.tml text. Returns the number of keys applied,
 * or -1 if the text has no [nwpad] table. Other tables and unknown keys are ignored. */
int nwpad_settings_parse(nwpad_config *cfg, const char *toml);
/* Write cfg as a [nwpad] section in the game's style (tabs, one table per group).
 * Returns the length, or -1 if cap is too small. */
/* Apply one numeric key (full path, e.g. "nwpad.camera.turn-speed"), clamped as when
 * parsed. Returns 1 if the key is known. */
int nwpad_settings_set_number(nwpad_config *cfg, const char *key, double value);
/* Should the cursor be hidden for the sticks? sticks_since_ms: when the sticks became
 * continuously active (0: not active). last_mouse_ms: last mouse motion (0: none).
 * Hidden at once if the mouse was already still for cfg->cursor_rehide_ms when the
 * sticks started; after mouse motion, only once the mouse has been still and the
 * sticks held for that long (Robert's request). */
bool nwpad_cursor_should_hide(uint64_t now_ms, uint64_t sticks_since_ms, uint64_t last_mouse_ms,
                              const nwpad_config *cfg);

int nwpad_settings_format(const nwpad_config *cfg, char *out, size_t cap);

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

/* ---- Movement style (plan §3, decided at the M2 gate) ----
 * DRAG walks along the stick and faces it (like holding the mouse on the
 * ground). The others keep the character's facing, as the E / S / Q keys do. */
typedef enum {
    NWPAD_STYLE_REST = 0,
    NWPAD_STYLE_DRAG,
    NWPAD_STYLE_STRAFE_RIGHT,
    NWPAD_STYLE_BACKPEDAL,
    NWPAD_STYLE_STRAFE_LEFT,
} nwpad_move_style;

/* Stick angle clockwise from forward, in [0, 360): right 90, back 180, left 270. */
float nwpad_stick_angle_cw(nwpad_vec2 stick);
/* stick is relative to the character's facing (+y = the way it faces).
 * From rest, the first direction picks the style (inside a window: strafe or
 * backpedal; otherwise drag). Strafe/backpedal becomes drag when the stick
 * leaves its window; drag stays drag; the deadzone returns to rest. */
nwpad_move_style nwpad_move_style_update(nwpad_move_style prev, nwpad_vec2 stick_after_deadzone,
                                         const nwpad_config *cfg);

/* nwpad_move_style_update with a debounce on leaving a strafe/backpedal window:
 * the stick must stay outside it for cfg->strafe_exit_ms before it becomes a
 * drag. A released stick springing back to centre passes through other angles
 * for a moment, which would otherwise turn the character around just before it
 * stops (Robert's feel test). */
typedef struct {
    nwpad_move_style style;
    uint64_t outside_since_ms; /* 0: inside its window (or not in a key style) */
} nwpad_style_state;

nwpad_move_style nwpad_move_style_step(nwpad_style_state *st, nwpad_vec2 stick_after_deadzone,
                                       uint64_t now_ms, const nwpad_config *cfg);

typedef struct {
    bool moving;
    nwpad_move_mode mode;   /* walk or run */
    nwpad_move_style style; /* drag or strafe/backpedal */
    float bearing_deg;      /* world movement direction */
} nwpad_move_intent;

/* camera_forward_deg: the camera's forward direction. facing_deg: the character's
 * current facing; the strafe/backpedal windows are measured from it (plan §3). */
nwpad_move_intent nwpad_move_intent_compute(nwpad_vec2 stick_after_deadzone,
                                            float camera_forward_deg, float facing_deg,
                                            bool always_run, nwpad_move_mode prev_mode,
                                            nwpad_style_state *style, uint64_t now_ms,
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

/* ---- Camera arbitration: last-used device wins (plan §3) ----
 * Only the camera is arbitrated. Keyboard vs stick movement is left to the
 * engine, which gets both through its own entry points (decision log). */
typedef struct {
    uint64_t last_mouse_ms;
    bool mouse_seen;
    bool camera_owned_by_stick;
} nwpad_arbiter;

void nwpad_arbiter_init(nwpad_arbiter *a);
void nwpad_arbiter_mouse_motion(nwpad_arbiter *a, uint64_t now_ms);
/* Call once per frame with the deadzoned right stick; updates camera ownership. */
void nwpad_arbiter_update(nwpad_arbiter *a, nwpad_vec2 right, uint64_t now_ms,
                          const nwpad_config *cfg);

/* ---- Byte patterns (signatures/ee.yaml) ---- */
#define NWPAD_PATTERN_MAX 64
typedef struct {
    uint8_t bytes[NWPAD_PATTERN_MAX];
    uint8_t mask[NWPAD_PATTERN_MAX]; /* 0xff = must match, 0 = wildcard */
    size_t len;
} nwpad_pattern;

/* Parse "55 48 ?? e5" ("?" or "??" is a wildcard). False on bad syntax, empty
 * input, or more than NWPAD_PATTERN_MAX bytes. */
bool nwpad_pattern_parse(const char *text, nwpad_pattern *out);
/* Count matches in hay[0..n) and return the first, or NULL if none. */
const uint8_t *nwpad_pattern_find(const uint8_t *hay, size_t n, const nwpad_pattern *p,
                                  int *count);

/* ---- Control socket protocol helpers (plan §8.3) ---- */
/* Copy the string value of "key" from a flat JSON object into out. Handles \"
 * and \\ escapes only. Returns false if the key is missing, not a string, or
 * doesn't fit. */
bool nwpad_json_get_string(const char *json, const char *key, char *out, size_t cap);
/* Read the numeric value of "key". Returns false if missing or not a number. */
bool nwpad_json_get_number(const char *json, const char *key, double *out);
/* Write in as the body of a JSON string (no quotes) into out: escapes quotes,
 * backslashes and control characters, and bytes >= 0x80 as \u00XX (the game's
 * strings are Windows-1252; Latin-1 is close enough here). Returns the length
 * written, or -1 if it doesn't fit (out is then empty). */
int nwpad_json_escape(char *out, size_t cap, const char *in);

/* ---- Quickbar picker (quickbar plan Q2) ---- */
#define NWPAD_PICKER_SLOTS 12
#define NWPAD_PICKER_MIN 0.5f /* stick/trackpad deflection that picks a slot */
/* The ring slot the stick points at: 0 at the top, clockwise, NWPAD_PICKER_SLOTS
 * sectors. Below NWPAD_PICKER_MIN, `current` is kept (lifting the thumb keeps the
 * highlight). -1 means nothing picked yet. */
int nwpad_picker_select(nwpad_vec2 stick, int current);

/* ---- UBJSON (the game's NUI wire format, re-notes F32) ---- */
/* Convert JSON text to UBJSON: integers as the smallest of i/U/I/l/L, other
 * numbers as D, strings and keys with the smallest length marker, no optimized
 * containers. Returns the length written, or -1 on bad JSON or if out is too small. */
int nwpad_ubjson_from_json(const char *json, uint8_t *out, size_t cap);

#endif
