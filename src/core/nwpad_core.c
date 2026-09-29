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
    /* Robert's feel test: start running above 0.85, drop back to walking below 0.725. */
    cfg->run_threshold = 0.7875f;
    cfg->run_hysteresis = 0.125f;
    cfg->mouse_idle_ms = 300;
    cfg->strafe_window_deg = 10.0f;
    cfg->hide_cursor = true;
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
            else if (!strcmp(key, "strafe_window")) { cfg->strafe_window_deg = (float)v; applied++; }
            else if (!strcmp(key, "hide_cursor")) { cfg->hide_cursor = v != 0; applied++; }
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

nwpad_move_intent nwpad_move_intent_compute(nwpad_vec2 stick, float camera_forward_deg,
                                            float facing_deg, bool always_run,
                                            nwpad_move_mode prev_mode, nwpad_move_style prev_style,
                                            const nwpad_config *cfg) {
    nwpad_move_intent in = {0};
    float m = nwpad_magnitude(stick);
    in.mode = nwpad_move_mode_update(prev_mode, m, always_run, cfg);
    in.moving = in.mode != NWPAD_MOVE_IDLE;
    if (!in.moving) return in; /* style REST */
    in.bearing_deg = nwpad_world_bearing(stick, camera_forward_deg);
    /* The same stick, re-expressed relative to the character's facing. */
    float cw = nwpad_wrap_deg(facing_deg - in.bearing_deg) * 0.017453292f;
    nwpad_vec2 rel = {sinf(cw) * m, cosf(cw) * m};
    in.style = nwpad_move_style_update(prev_style, rel, cfg);
    return in;
}

/* ---- Send-rate limiting ---- */

void nwpad_send_policy_defaults(nwpad_send_policy *p) {
    p->heading_threshold_deg = 2.0f;
    p->keepalive_ms = 150;   /* matches the mouse-drag resend cadence (re-notes F16) */
    p->min_interval_ms = 100; /* hard cap; the mouse drag re-targets about every 140 ms (re-notes F16) */
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

    bool style_changed = in->style != st->last_sent.style;
    bool want = !st->was_moving || !st->has_sent || style_changed || in->mode != st->last_sent.mode ||
                fabsf(nwpad_angle_diff(in->bearing_deg, st->last_sent.bearing_deg)) >= p->heading_threshold_deg ||
                now_ms - st->last_send_ms >= p->keepalive_ms;
    if (!want) return NWPAD_SEND_NONE;
    /* A style change is never rate-capped: the old style has to end now. */
    if (st->has_sent && st->was_moving && !style_changed && now_ms - st->last_send_ms < p->min_interval_ms)
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
}

void nwpad_arbiter_mouse_motion(nwpad_arbiter *a, uint64_t now_ms) {
    a->last_mouse_ms = now_ms;
    a->mouse_seen = true;
    a->camera_owned_by_stick = false;
}

void nwpad_arbiter_update(nwpad_arbiter *a, nwpad_vec2 right, uint64_t now_ms,
                          const nwpad_config *cfg) {
    if (!a->camera_owned_by_stick) {
        bool mouse_idle = !a->mouse_seen || now_ms - a->last_mouse_ms >= cfg->mouse_idle_ms;
        if (mouse_idle && nwpad_magnitude(right) >= NWPAD_SAFETY_DEADZONE)
            a->camera_owned_by_stick = true;
    }
}

/* Start of the value for "key" (after the colon), or NULL. */
static const char *json_value(const char *json, const char *key) {
    size_t klen = strlen(key);
    for (const char *p = json; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"') continue;
        const char *q = p + 2 + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q++ != ':') continue;
        while (*q == ' ' || *q == '\t') q++;
        return q;
    }
    return NULL;
}

bool nwpad_json_get_number(const char *json, const char *key, double *out) {
    const char *q = json_value(json, key);
    if (!q) return false;
    char *end;
    double v = strtod(q, &end);
    if (end == q) return false;
    *out = v;
    return true;
}

bool nwpad_json_get_string(const char *json, const char *key, char *out, size_t cap) {
    const char *q = json_value(json, key);
    if (!q || *q++ != '"') return false;
    size_t n = 0;
    for (; *q && *q != '"'; q++) {
        if (*q == '\\') {
            q++;
            if (*q != '"' && *q != '\\') return false;
        }
        if (n + 1 >= cap) return false;
        out[n++] = *q;
    }
    if (*q != '"') return false;
    out[n] = '\0';
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool nwpad_pattern_parse(const char *text, nwpad_pattern *out) {
    out->len = 0;
    const char *p = text;
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (out->len == NWPAD_PATTERN_MAX) return false;
        const char *tok = p;
        while (*p && *p != ' ') p++;
        size_t tlen = (size_t)(p - tok);
        if ((tlen == 1 && tok[0] == '?') || (tlen == 2 && tok[0] == '?' && tok[1] == '?')) {
            out->bytes[out->len] = 0;
            out->mask[out->len] = 0;
        } else if (tlen == 2 && hex_nibble(tok[0]) >= 0 && hex_nibble(tok[1]) >= 0) {
            out->bytes[out->len] = (uint8_t)(hex_nibble(tok[0]) << 4 | hex_nibble(tok[1]));
            out->mask[out->len] = 0xff;
        } else {
            return false;
        }
        out->len++;
    }
    return out->len > 0;
}

const uint8_t *nwpad_pattern_find(const uint8_t *hay, size_t n, const nwpad_pattern *p,
                                  int *count) {
    const uint8_t *first = NULL;
    *count = 0;
    if (p->len == 0 || n < p->len) return NULL;
    for (size_t i = 0; i + p->len <= n; i++) {
        size_t j = 0;
        while (j < p->len && (hay[i + j] & p->mask[j]) == p->bytes[j]) j++;
        if (j == p->len) {
            if (!first) first = hay + i;
            (*count)++;
        }
    }
    return first;
}

float nwpad_stick_angle_cw(nwpad_vec2 stick) {
    return nwpad_wrap_deg(atan2f(stick.x, stick.y) * 57.29577951f);
}

static bool in_window(float angle, float center, float half) {
    return fabsf(nwpad_angle_diff(angle, center)) <= half;
}

nwpad_move_style nwpad_move_style_update(nwpad_move_style prev, nwpad_vec2 stick,
                                         const nwpad_config *cfg) {
    if (nwpad_magnitude(stick) < NWPAD_SAFETY_DEADZONE) return NWPAD_STYLE_REST;
    float a = nwpad_stick_angle_cw(stick), w = cfg->strafe_window_deg;
    switch (prev) {
    case NWPAD_STYLE_REST:
        if (in_window(a, 90.0f, w)) return NWPAD_STYLE_STRAFE_RIGHT;
        if (in_window(a, 180.0f, w)) return NWPAD_STYLE_BACKPEDAL;
        if (in_window(a, 270.0f, w)) return NWPAD_STYLE_STRAFE_LEFT;
        return NWPAD_STYLE_DRAG;
    case NWPAD_STYLE_STRAFE_RIGHT: return in_window(a, 90.0f, w) ? prev : NWPAD_STYLE_DRAG;
    case NWPAD_STYLE_BACKPEDAL: return in_window(a, 180.0f, w) ? prev : NWPAD_STYLE_DRAG;
    case NWPAD_STYLE_STRAFE_LEFT: return in_window(a, 270.0f, w) ? prev : NWPAD_STYLE_DRAG;
    case NWPAD_STYLE_DRAG: break;
    }
    return NWPAD_STYLE_DRAG;
}
