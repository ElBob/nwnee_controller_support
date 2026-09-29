/* Unit tests for src/core. Run via ctest; no game or SDL required. */
#include "nwpad_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b, eps) CHECK(fabsf((float)(a) - (float)(b)) <= (eps))
#define ANGLE_NEAR(a, b, eps) CHECK(fabsf(nwpad_angle_diff((a), (b))) <= (eps))

static nwpad_config cfg;

static void test_deadzone(void) {
    nwpad_vec2 z = nwpad_apply_deadzone((nwpad_vec2){0.03f, 0.02f});
    CHECK(z.x == 0.0f && z.y == 0.0f);
    nwpad_vec2 p = nwpad_apply_deadzone((nwpad_vec2){0.3f, -0.4f});
    NEAR(p.x, 0.3f, 1e-6); NEAR(p.y, -0.4f, 1e-6); /* raw passthrough, no rescale */
    nwpad_vec2 c = nwpad_apply_deadzone((nwpad_vec2){1.0f, 1.0f});
    NEAR(nwpad_magnitude(c), 1.0f, 1e-5);          /* clamped to unit circle */
}

static void test_bearing(void) {
    /* camera yaw 90 (facing +Y world) */
    ANGLE_NEAR(nwpad_world_bearing((nwpad_vec2){0, 1}, 90), 90, 1e-3);   /* forward */
    ANGLE_NEAR(nwpad_world_bearing((nwpad_vec2){-1, 0}, 90), 180, 1e-3); /* strafe left */
    ANGLE_NEAR(nwpad_world_bearing((nwpad_vec2){1, 0}, 90), 0, 1e-3);    /* strafe right */
    ANGLE_NEAR(nwpad_world_bearing((nwpad_vec2){0, -1}, 90), 270, 1e-3); /* backpedal */
    /* wraparound */
    ANGLE_NEAR(nwpad_world_bearing((nwpad_vec2){1, 0}, 10), 280, 1e-3);
    ANGLE_NEAR(nwpad_world_bearing((nwpad_vec2){0, 1}, 359.5f), 359.5f, 1e-3);
    /* non-quantization: 16 distinct directions stay distinct */
    for (int i = 0; i < 16; i++) {
        float a = i * 22.5f * 0.017453292f;
        nwpad_vec2 s = {-sinf(a), cosf(a)};
        ANGLE_NEAR(nwpad_world_bearing(s, 0), i * 22.5f, 1e-2);
    }
    NEAR(nwpad_angle_diff(10, 350), 20, 1e-4);
    NEAR(nwpad_angle_diff(350, 10), -20, 1e-4);
}

static void test_free_facing(void) {
    nwpad_move_intent in = nwpad_move_intent_compute((nwpad_vec2){1, 0}, 45, false,
                                                     NWPAD_MOVE_IDLE, &cfg);
    CHECK(in.moving);
    ANGLE_NEAR(in.facing_deg, 45, 1e-3);   /* faces camera forward */
    ANGLE_NEAR(in.bearing_deg, 315, 1e-3); /* but moves right */
    nwpad_move_intent idle = nwpad_move_intent_compute((nwpad_vec2){0, 0}, 45, false,
                                                       NWPAD_MOVE_RUN, &cfg);
    CHECK(!idle.moving && idle.mode == NWPAD_MOVE_IDLE);
}

static void test_walk_run(void) {
    nwpad_move_mode m = NWPAD_MOVE_IDLE;
    m = nwpad_move_mode_update(m, 0.4f, false, &cfg);  CHECK(m == NWPAD_MOVE_WALK);
    m = nwpad_move_mode_update(m, 0.61f, false, &cfg); CHECK(m == NWPAD_MOVE_WALK); /* inside band */
    m = nwpad_move_mode_update(m, 0.63f, false, &cfg); CHECK(m == NWPAD_MOVE_RUN);
    m = nwpad_move_mode_update(m, 0.59f, false, &cfg); CHECK(m == NWPAD_MOVE_RUN);  /* hysteresis */
    m = nwpad_move_mode_update(m, 0.57f, false, &cfg); CHECK(m == NWPAD_MOVE_WALK);
    /* noisy signal around threshold must not flicker */
    int flips = 0; nwpad_move_mode prev = NWPAD_MOVE_WALK;
    for (int i = 0; i < 200; i++) {
        float mag = 0.6f + ((i % 2) ? 0.02f : -0.02f);
        nwpad_move_mode n = nwpad_move_mode_update(prev, mag, false, &cfg);
        if (n != prev) flips++;
        prev = n;
    }
    CHECK(flips == 0);
    CHECK(nwpad_move_mode_update(NWPAD_MOVE_IDLE, 0.3f, true, &cfg) == NWPAD_MOVE_RUN);
    CHECK(nwpad_move_mode_update(NWPAD_MOVE_RUN, 0.01f, true, &cfg) == NWPAD_MOVE_IDLE);
}

static void test_send_rate(void) {
    nwpad_send_policy p; nwpad_send_policy_defaults(&p);
    nwpad_send_state st = {0};
    nwpad_move_intent idle = {0};
    nwpad_move_intent mv = {.moving = true, .mode = NWPAD_MOVE_RUN, .bearing_deg = 90, .facing_deg = 90};

    CHECK(nwpad_send_decide(&st, &p, &idle, 0) == NWPAD_SEND_NONE); /* never moved */
    CHECK(nwpad_send_decide(&st, &p, &mv, 100) == NWPAD_SEND_MOVE); /* start */
    CHECK(nwpad_send_decide(&st, &p, &mv, 110) == NWPAD_SEND_NONE); /* unchanged */
    nwpad_move_intent small = mv; small.bearing_deg = 91;
    CHECK(nwpad_send_decide(&st, &p, &small, 200) == NWPAD_SEND_NONE); /* below threshold */
    nwpad_move_intent turn = mv; turn.bearing_deg = 100;
    CHECK(nwpad_send_decide(&st, &p, &turn, 110 + 5) == NWPAD_SEND_NONE); /* rate cap */
    CHECK(nwpad_send_decide(&st, &p, &turn, 140) == NWPAD_SEND_MOVE);
    CHECK(nwpad_send_decide(&st, &p, &turn, 140 + p.keepalive_ms) == NWPAD_SEND_MOVE); /* keepalive */
    /* stop exactly once, even immediately after a send */
    uint64_t t = 140 + p.keepalive_ms + 1;
    CHECK(nwpad_send_decide(&st, &p, &idle, t) == NWPAD_SEND_STOP);
    CHECK(nwpad_send_decide(&st, &p, &idle, t + 1000) == NWPAD_SEND_NONE);
    CHECK(nwpad_send_decide(&st, &p, &mv, t + 1001) == NWPAD_SEND_MOVE); /* restart not capped */
}

static void test_camera(void) {
    nwpad_camera_limits lim = {.min_pitch = -10, .max_pitch = 60};
    nwpad_camera c0 = {.yaw_deg = 0, .pitch_deg = 30};
    nwpad_camera full = nwpad_camera_step(c0, (nwpad_vec2){1, 0}, 0.5f, &cfg, &lim);
    nwpad_camera half = nwpad_camera_step(c0, (nwpad_vec2){0.5f, 0}, 0.5f, &cfg, &lim);
    float d_full = fabsf(nwpad_angle_diff(full.yaw_deg, 0));
    float d_half = fabsf(nwpad_angle_diff(half.yaw_deg, 0));
    NEAR(d_full, 90, 1e-3);            /* 180 deg/s * 0.5 s */
    NEAR(d_half / d_full, 0.5f, 1e-4); /* linear */
    /* frame-rate independence: 10 x 0.05 s == 1 x 0.5 s */
    nwpad_camera c = c0;
    for (int i = 0; i < 10; i++) c = nwpad_camera_step(c, (nwpad_vec2){1, 0}, 0.05f, &cfg, &lim);
    ANGLE_NEAR(c.yaw_deg, full.yaw_deg, 1e-2);
    /* pitch clamp */
    nwpad_camera up = nwpad_camera_step(c0, (nwpad_vec2){0, 1}, 5.0f, &cfg, &lim);
    NEAR(up.pitch_deg, 60, 1e-4);
    nwpad_camera down = nwpad_camera_step(c0, (nwpad_vec2){0, -1}, 5.0f, &cfg, &lim);
    NEAR(down.pitch_deg, -10, 1e-4);
    /* locks */
    nwpad_camera_limits locked = lim; locked.yaw_locked = true; locked.pitch_locked = true;
    nwpad_camera l = nwpad_camera_step(c0, (nwpad_vec2){1, 1}, 1.0f, &cfg, &locked);
    NEAR(l.yaw_deg, 0, 0); NEAR(l.pitch_deg, 30, 0);
}

static void test_arbitration(void) {
    nwpad_arbiter a; nwpad_arbiter_init(&a);
    nwpad_vec2 zero = {0, 0}, push = {0.8f, 0};
    CHECK(a.camera_owned_by_stick && a.movement_owned_by_stick);

    nwpad_arbiter_mouse_motion(&a, 1000);
    CHECK(!a.camera_owned_by_stick);
    nwpad_arbiter_update(&a, zero, push, 1100, &cfg); /* mouse not idle yet */
    CHECK(!a.camera_owned_by_stick);
    nwpad_arbiter_update(&a, zero, zero, 1400, &cfg); /* idle but stick centered */
    CHECK(!a.camera_owned_by_stick);
    nwpad_arbiter_update(&a, zero, push, 1400, &cfg); /* idle + stick deflected */
    CHECK(a.camera_owned_by_stick);

    nwpad_arbiter_move_key(&a, NWPAD_KEY_W, true);
    CHECK(!a.movement_owned_by_stick);
    nwpad_arbiter_update(&a, push, zero, 2000, &cfg); /* key still held */
    CHECK(!a.movement_owned_by_stick);
    nwpad_arbiter_move_key(&a, NWPAD_KEY_W, false);
    nwpad_arbiter_update(&a, push, zero, 2010, &cfg);
    CHECK(a.movement_owned_by_stick);
}

static void test_config(void) {
    nwpad_config c; nwpad_config_defaults(&c);
    const char *toml =
        "# nwpad config\n"
        "camera_yaw_speed = 240   # faster\n"
        "\n"
        "run_threshold=0.7\n"
        "unknown_key = 3\n"
        "mouse_idle_ms = 500\n";
    CHECK(nwpad_config_parse(&c, toml) == 3);
    NEAR(c.camera_yaw_speed, 240, 0); NEAR(c.run_threshold, 0.7f, 1e-6);
    CHECK(c.mouse_idle_ms == 500);
    NEAR(c.camera_pitch_speed, 90, 0); /* untouched default */
    nwpad_config bad; nwpad_config_defaults(&bad);
    CHECK(nwpad_config_parse(&bad, "camera_yaw_speed 240\n") == -1);
    CHECK(nwpad_config_parse(&bad, "camera_yaw_speed = fast\n") == -1);
}

static void test_json(void) {
    char v[16];
    CHECK(nwpad_json_get_string("{\"cmd\":\"ping\"}", "cmd", v, sizeof v) && strcmp(v, "ping") == 0);
    CHECK(nwpad_json_get_string("{ \"x\": 1, \"cmd\" : \"status\" }", "cmd", v, sizeof v) &&
          strcmp(v, "status") == 0);
    CHECK(nwpad_json_get_string("{\"cmd\":\"a\\\"b\"}", "cmd", v, sizeof v) && strcmp(v, "a\"b") == 0);
    CHECK(!nwpad_json_get_string("{\"cmdx\":\"ping\"}", "cmd", v, sizeof v));  /* key prefix */
    CHECK(!nwpad_json_get_string("{\"cmd\":1}", "cmd", v, sizeof v));           /* not a string */
    CHECK(!nwpad_json_get_string("{\"cmd\":\"ping", "cmd", v, sizeof v));        /* unterminated */
    CHECK(!nwpad_json_get_string("{\"cmd\":\"0123456789abcdefg\"}", "cmd", v, sizeof v)); /* too long */
    CHECK(nwpad_json_get_string("{\"a\":\"cmd\",\"cmd\":\"ok\"}", "cmd", v, sizeof v) &&
          strcmp(v, "ok") == 0); /* "cmd" as a value is skipped */
    double d;
    CHECK(nwpad_json_get_number("{\"cmd\":\"stick\",\"rx\": -0.5, \"hold_ms\":1000}", "rx", &d) && d == -0.5);
    CHECK(nwpad_json_get_number("{\"hold_ms\":1000}", "hold_ms", &d) && d == 1000);
    CHECK(!nwpad_json_get_number("{\"rx\":\"x\"}", "rx", &d));
    CHECK(!nwpad_json_get_number("{\"ry\":1}", "rx", &d));
}

static void test_pattern(void) {
    nwpad_pattern p;
    CHECK(nwpad_pattern_parse("55 48 ?? e5", &p) && p.len == 4 && p.mask[2] == 0 && p.bytes[3] == 0xe5);
    CHECK(nwpad_pattern_parse("  AB ?  cd ", &p) && p.len == 3 && p.bytes[0] == 0xab && p.mask[1] == 0);
    CHECK(!nwpad_pattern_parse("", &p));
    CHECK(!nwpad_pattern_parse("5", &p));
    CHECK(!nwpad_pattern_parse("zz", &p));
    CHECK(!nwpad_pattern_parse("555", &p));
    static const uint8_t hay[] = {0x00, 0x55, 0x48, 0x89, 0xe5, 0x55, 0x48, 0x00, 0xe5, 0x55};
    int count;
    nwpad_pattern_parse("55 48 ?? e5", &p);
    CHECK(nwpad_pattern_find(hay, sizeof hay, &p, &count) == hay + 1 && count == 2);
    nwpad_pattern_parse("e5 55", &p);
    CHECK(nwpad_pattern_find(hay, sizeof hay, &p, &count) == hay + 4 && count == 2); /* match at the end */
    nwpad_pattern_parse("12 34", &p);
    CHECK(nwpad_pattern_find(hay, sizeof hay, &p, &count) == NULL && count == 0);
    CHECK(nwpad_pattern_find(hay, 1, &p, &count) == NULL && count == 0); /* shorter than pattern */
}

int main(void) {
    nwpad_config_defaults(&cfg);
    test_deadzone();
    test_bearing();
    test_free_facing();
    test_walk_run();
    test_send_rate();
    test_camera();
    test_arbitration();
    test_config();
    test_json();
    test_pattern();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
