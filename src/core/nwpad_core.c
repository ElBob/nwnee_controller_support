#include "nwpad_core.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG_PER_RAD 57.29577951308232f

/* ---- Configuration ---- */

void nwpad_config_defaults(nwpad_config *cfg) {
    cfg->camera_yaw_speed = 180.0f;
    cfg->camera_pitch_speed = 90.0f;
    cfg->run_threshold = 0.6f;
    cfg->run_hysteresis = 0.05f;
    cfg->mouse_idle_ms = 300;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

int nwpad_config_parse(nwpad_config *cfg, const char *text) {
    int applied = 0;
    const char *line = text;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[256];
        if (len >= sizeof buf) return -1;
        memcpy(buf, line, len);
        buf[len] = '\0';
        char *hash = strchr(buf, '#');
        if (hash) *hash = '\0';

        const char *p = skip_ws(buf);
        if (*p) {
            char key[64];
            size_t k = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && k < sizeof key - 1)
                key[k++] = *p++;
            key[k] = '\0';
            p = skip_ws(p);
            if (k == 0 || *p != '=') return -1;
            p = skip_ws(p + 1);
            char *num_end;
            double v = strtod(p, &num_end);
            if (num_end == p) return -1;
            if (*skip_ws(num_end) && *skip_ws(num_end) != '\r') return -1;

            if (!strcmp(key, "camera_yaw_speed")) { cfg->camera_yaw_speed = (float)v; applied++; }
            else if (!strcmp(key, "camera_pitch_speed")) { cfg->camera_pitch_speed = (float)v; applied++; }
            else if (!strcmp(key, "run_threshold")) { cfg->run_threshold = (float)v; applied++; }
            else if (!strcmp(key, "run_hysteresis")) { cfg->run_hysteresis = (float)v; applied++; }
            else if (!strcmp(key, "mouse_idle_ms")) { cfg->mouse_idle_ms = (uint32_t)v; applied++; }
        }
        line = end ? end + 1 : NULL;
    }
    return applied;
}

/* ---- Stick processing ---- */

float nwpad_magnitude(nwpad_vec2 v) { return sqrtf(v.x * v.x + v.y * v.y); }

nwpad_vec2 nwpad_apply_deadzone(nwpad_vec2 raw) {
    float m = nwpad_magnitude(raw);
    if (m < NWPAD_SAFETY_DEADZONE) return (nwpad_vec2){0.0f, 0.0f};
    if (m > 1.0f) return (nwpad_vec2){raw.x / m, raw.y / m};
    return raw;
}

/* ---- Movement ---- */

float nwpad_wrap_deg(float deg) {
    float r = fmodf(deg, 360.0f);
    if (r < 0.0f) r += 360.0f;
    if (r >= 360.0f) r -= 360.0f;
    return r;
}

float nwpad_angle_diff(float a_deg, float b_deg) {
    float d = nwpad_wrap_deg(a_deg - b_deg);
    return d > 180.0f ? d - 360.0f : d;
}

float nwpad_world_bearing(nwpad_vec2 stick, float camera_yaw_deg) {
    /* Stick angle relative to "forward" (+y), counter-clockwise positive:
     * forward=0, left=+90, right=-90, back=180. */
    float rel = atan2f(-stick.x, stick.y) * DEG_PER_RAD;
    return nwpad_wrap_deg(camera_yaw_deg + rel);
}

nwpad_move_mode nwpad_move_mode_update(nwpad_move_mode prev, float magnitude,
                                       bool always_run, const nwpad_config *cfg) {
    if (magnitude < NWPAD_SAFETY_DEADZONE) return NWPAD_MOVE_IDLE;
    if (always_run) return NWPAD_MOVE_RUN;
    float half = cfg->run_hysteresis * 0.5f;
    if (prev == NWPAD_MOVE_RUN)
        return magnitude >= cfg->run_threshold - half ? NWPAD_MOVE_RUN : NWPAD_MOVE_WALK;
    return magnitude > cfg->run_threshold + half ? NWPAD_MOVE_RUN : NWPAD_MOVE_WALK;
}

nwpad_move_intent nwpad_move_intent_compute(nwpad_vec2 stick, float camera_yaw_deg,
                                            bool always_run, nwpad_move_mode prev_mode,
                                            const nwpad_config *cfg) {
    nwpad_move_intent in = {0};
    float m = nwpad_magnitude(stick);
    in.mode = nwpad_move_mode_update(prev_mode, m, always_run, cfg);
    in.moving = in.mode != NWPAD_MOVE_IDLE;
    in.facing_deg = nwpad_wrap_deg(camera_yaw_deg);
    in.bearing_deg = in.moving ? nwpad_world_bearing(stick, camera_yaw_deg) : in.facing_deg;
    return in;
}

/* ---- Send-rate limiting ---- */

void nwpad_send_policy_defaults(nwpad_send_policy *p) {
    p->heading_threshold_deg = 2.0f;
    p->keepalive_ms = 250;   /* placeholder until measured from msglog (plan §5.2) */
    p->min_interval_ms = 33; /* hard cap, ~30 Hz */
}

nwpad_send_action nwpad_send_decide(nwpad_send_state *st, const nwpad_send_policy *p,
                                    const nwpad_move_intent *in, uint64_t now_ms) {
    if (!in->moving) {
        if (st->was_moving) {
            st->was_moving = false;
            st->last_sent = *in;
            st->last_send_ms = now_ms;
            st->has_sent = true;
            return NWPAD_SEND_STOP;
        }
        return NWPAD_SEND_NONE;
    }

    bool want = !st->was_moving || !st->has_sent || in->mode != st->last_sent.mode ||
                fabsf(nwpad_angle_diff(in->bearing_deg, st->last_sent.bearing_deg)) >= p->heading_threshold_deg ||
                fabsf(nwpad_angle_diff(in->facing_deg, st->last_sent.facing_deg)) >= p->heading_threshold_deg ||
                now_ms - st->last_send_ms >= p->keepalive_ms;
    if (!want) return NWPAD_SEND_NONE;
    if (st->has_sent && st->was_moving && now_ms - st->last_send_ms < p->min_interval_ms)
        return NWPAD_SEND_NONE;

    st->was_moving = true;
    st->has_sent = true;
    st->last_sent = *in;
    st->last_send_ms = now_ms;
    return NWPAD_SEND_MOVE;
}

/* ---- Camera ---- */

nwpad_camera nwpad_camera_step(nwpad_camera cam, nwpad_vec2 stick, float dt_s,
                               const nwpad_config *cfg, const nwpad_camera_limits *lim) {
    if (!lim->yaw_locked)
        cam.yaw_deg = nwpad_wrap_deg(cam.yaw_deg - stick.x * cfg->camera_yaw_speed * dt_s);
    if (!lim->pitch_locked) {
        cam.pitch_deg += stick.y * cfg->camera_pitch_speed * dt_s;
        if (cam.pitch_deg < lim->min_pitch) cam.pitch_deg = lim->min_pitch;
        if (cam.pitch_deg > lim->max_pitch) cam.pitch_deg = lim->max_pitch;
    }
    return cam;
}

/* ---- Arbitration ---- */

void nwpad_arbiter_init(nwpad_arbiter *a) {
    memset(a, 0, sizeof *a);
    a->camera_owned_by_stick = true;
    a->movement_owned_by_stick = true;
}

void nwpad_arbiter_mouse_motion(nwpad_arbiter *a, uint64_t now_ms) {
    a->last_mouse_ms = now_ms;
    a->mouse_seen = true;
    a->camera_owned_by_stick = false;
}

void nwpad_arbiter_move_key(nwpad_arbiter *a, uint8_t key_bit, bool down) {
    if (down) {
        a->move_keys_held |= key_bit;
        a->movement_owned_by_stick = false;
    } else {
        a->move_keys_held &= (uint8_t)~key_bit;
    }
}

void nwpad_arbiter_update(nwpad_arbiter *a, nwpad_vec2 left, nwpad_vec2 right,
                          uint64_t now_ms, const nwpad_config *cfg) {
    if (!a->camera_owned_by_stick) {
        bool mouse_idle = !a->mouse_seen || now_ms - a->last_mouse_ms >= cfg->mouse_idle_ms;
        if (mouse_idle && nwpad_magnitude(right) >= NWPAD_SAFETY_DEADZONE)
            a->camera_owned_by_stick = true;
    }
    if (!a->movement_owned_by_stick) {
        if (a->move_keys_held == 0 && nwpad_magnitude(left) >= NWPAD_SAFETY_DEADZONE)
            a->movement_owned_by_stick = true;
    }
}
