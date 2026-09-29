"""M3 movement scenarios (plan §8.6), measured against the in-process server's
creature position and facing (re-notes F19).

Drag walks along the stick and faces it; strafe/backpedal keep the character's
facing, with the windows measured from that facing (plan §3). Each test starts
from the arena's spawn point (the `home` fixture).
"""
import math
import os
import time

import pytest

HEADING_TOL = 5.0   # degrees (plan §8.6)
FACING_TOL = 5.0
STOP_MS = 450  # raised from 300 by Robert to match the game's own keys (plan decision log)


def snapshot(ctl):
    d = ctl("state")
    return d, d["creature"], (d["camera"]["yaw"] + 90.0) % 360


def ang_diff(a, b):
    return (a - b + 180.0) % 360.0 - 180.0


def stick_for_world(ctl, world_deg, mag=1.0):
    """Stick (camera frame) that points at world direction world_deg."""
    _, _, fwd = snapshot(ctl)
    a = math.radians(world_deg - fwd)
    return {"lx": -math.sin(a) * mag, "ly": math.cos(a) * mag}


def move(ctl, stick, secs=1.0, settle=0.6):
    """Hold the stick, release, and return (start, end, style while moving).
    start["client_facing"] is the facing the game's drive uses."""
    d0, c0, _ = snapshot(ctl)
    c0 = {**c0, "client_facing": d0.get("client", {}).get("facing", c0["facing"])}
    ctl("stick", **stick)
    time.sleep(secs)
    style = ctl("state")["move_style"]
    ctl("release")
    time.sleep(settle)
    _, c1, _ = snapshot(ctl)
    return c0, c1, style


def displacement(c0, c1):
    dx, dy = c1["x"] - c0["x"], c1["y"] - c0["y"]
    return math.hypot(dx, dy), math.degrees(math.atan2(dy, dx)) % 360


STRAFE_WINDOW = 10.0  # config default (plan §6.3)


HOME = (20.0, 20.0, 90.0)  # the arena's spawn point and facing
CLIENT_FACING_SETTLE = 10.0  # reset check only; not a test tolerance


@pytest.fixture
def home(ctl):
    """Put the character back at the spawn point facing 90°, with the camera facing
    the same way (NWScript over the control socket, re-notes F17)."""
    reset(ctl)
    return ctl


def reset(ctl):
    ctl("release")
    # Let a stick release finish first: the stop tap's drive packet carries the
    # client's position, and landing after the jump it would put the character back.
    time.sleep(0.5)
    x, y, f = HOME

    def jump(facing):
        # Recover from anything the arena can do to the character first: death,
        # combat, or being made uncommandable.
        ctl("script_chunk", code=(
            f"object pc = GetFirstPC();"
            f"if (GetIsDead(pc)) ApplyEffectToObject(DURATION_TYPE_INSTANT, EffectResurrection(), pc);"
            f"ApplyEffectToObject(DURATION_TYPE_INSTANT, EffectHeal(GetMaxHitPoints(pc)), pc);"
            f"SetCommandable(TRUE, pc); AssignCommand(pc, ClearAllActions(TRUE));"
            f"AssignCommand(pc, JumpToLocation(Location(GetArea(pc), Vector({x}, {y}, 0.0), {facing})));"
            f"AssignCommand(pc, SetCameraFacing({f}, -1.0, -1.0, CAMERA_TRANSITION_TYPE_SNAP));"))
        deadline = time.monotonic() + 5
        while True:
            d, c, fwd = snapshot(ctl)
            cl = d.get("client", {"x": 1e9, "y": 1e9, "facing": 1e9})
            # The client's facing can settle a few degrees off the server's; the
            # drive uses the client's, so expectations below use it (see move()).
            if (math.hypot(c["x"] - x, c["y"] - y) < 0.05 and abs(ang_diff(c["facing"], facing)) < 1.0
                    and math.hypot(cl["x"] - x, cl["y"] - y) < 0.05
                    and abs(ang_diff(cl["facing"], facing)) < CLIENT_FACING_SETTLE
                    and abs(ang_diff(fwd, f)) < 1.0):
                return
            if time.monotonic() > deadline:
                shot = capture_failure(ctl, "reset")
                pytest.fail(f"couldn't reset: server {c}, client {cl}, camera forward {fwd:.1f}; screenshot {shot}")
            time.sleep(0.1)

    # Jump twice so the facing always changes; an unchanged server facing isn't
    # re-sent, and the client's copy can differ by a few degrees.
    jump(f + 45)
    jump(f)


def capture_failure(ctl, label):
    """Screenshot and state dump into artifacts/ (pulled back by remote.sh; never committed)."""
    import json
    import subprocess
    from conftest import ROOT
    out = os.path.join(ROOT, "artifacts", f"{label}-{int(time.time())}")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "state.json"), "w") as f:
        json.dump(ctl("state"), f, indent=1)
    subprocess.run(["spectacle", "-b", "-n", "-f", "-o", os.path.join(out, "screen.png")],
                   timeout=15, check=False, capture_output=True)
    return out


def expected_direction(stick_world, facing, style):
    """Where the character should go: along the stick when dragging; snapped to
    facing ±90/180 in a strafe/backpedal window (plan §3)."""
    snap = {"strafe_right": -90.0, "backpedal": 180.0, "strafe_left": 90.0}
    if style in snap:
        target = (facing + snap[style]) % 360
        assert abs(ang_diff(stick_world, target)) <= STRAFE_WINDOW + 0.5, (stick_world, target, style)
        return target
    return stick_world


def test_movement_available(ctl):
    assert ctl("status")["features"]["movement"] is True


@pytest.mark.parametrize("step", range(16))
def test_heading_accuracy(home, step):
    """Displacement direction matches the stick at 22.5° steps (also covers non-quantization)."""
    world = (snapshot(home)[2] + step * 22.5) % 360
    c0, c1, style = move(home, stick_for_world(home, world))
    dist, dirn = displacement(c0, c1)
    assert dist > 0.5, (c0, c1)
    want = expected_direction(world, c0["client_facing"], style)
    assert abs(ang_diff(dirn, want)) <= HEADING_TOL, f"stick {world:.1f} ({style}), moved {dirn:.1f}, expected {want:.1f}"


def test_facing_follows_drag(home):
    _, c, _ = snapshot(home)
    world = (c["facing"] - 45) % 360  # 45° right of facing: drag
    c0, c1, style = move(home, stick_for_world(home, world))
    assert style == "drag"
    _, dirn = displacement(c0, c1)
    assert abs(ang_diff(c1["facing"], dirn)) <= FACING_TOL, (c1, dirn)


@pytest.mark.parametrize("rel, style", [(90, "strafe_right"), (180, "backpedal"), (270, "strafe_left")])
def test_strafe_and_backpedal_keep_facing(home, rel, style):
    _, c, _ = snapshot(home)
    world = (c["facing"] - rel) % 360
    c0, c1, got = move(home, stick_for_world(home, world))
    assert got == style
    dist, dirn = displacement(c0, c1)
    want = expected_direction(world, c0["client_facing"], style)
    assert dist > 0.5 and abs(ang_diff(dirn, want)) <= HEADING_TOL, (dirn, want)
    assert abs(ang_diff(c1["facing"], c0["facing"])) <= FACING_TOL, (c0, c1)


def test_stop(home):
    home("stick", ly=1.0)
    time.sleep(0.8)
    home("release")
    t_release = time.monotonic()
    positions = []
    while time.monotonic() - t_release < 1.3:
        c = home("state")["creature"]
        positions.append((time.monotonic() - t_release, c["x"], c["y"]))
        time.sleep(0.02)
    # Stopped within STOP_MS: nothing moves more than 1 cm after that point.
    after = [(x, y) for t, x, y in positions if t >= STOP_MS / 1000]
    drift = max(math.hypot(x - after[0][0], y - after[0][1]) for x, y in after)
    assert drift < 0.01, f"still moving after {STOP_MS} ms: {drift:.3f} m"


def test_direction_change_without_stop(home):
    """Rotating the stick while dragging turns the path with no stop in between."""
    _, c, fwd = snapshot(home)
    home("stick", ly=1.0)
    time.sleep(0.5)
    samples = []
    for i in range(10):  # sweep 90° to the right over 1 s
        a = math.radians(-9.0 * (i + 1))
        home("stick", lx=-math.sin(a), ly=math.cos(a))
        time.sleep(0.1)
        s = home("state")
        samples.append((s["creature"]["x"], s["creature"]["y"], s["move_style"]))
    home("release")
    time.sleep(0.6)
    assert all(st == "drag" for _, _, st in samples)
    steps = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(samples, samples[1:])]
    assert min(steps) > 0.1, f"stalled during the turn: {steps}"


def test_strafe_turns_into_drag_and_drag_stays_drag(home):
    _, c, _ = snapshot(home)
    right = (c["facing"] - 90) % 360
    home("stick", **stick_for_world(home, right))
    time.sleep(0.3)
    assert home("state")["move_style"] == "strafe_right"
    home("stick", **stick_for_world(home, (right + 40) % 360))  # out of the window
    time.sleep(0.3)
    assert home("state")["move_style"] == "drag"
    _, c, _ = snapshot(home)
    home("stick", **stick_for_world(home, (c["facing"] - 90) % 360))  # into a window while dragging
    time.sleep(0.3)
    assert home("state")["move_style"] == "drag"
    home("release")
    time.sleep(0.6)
    assert home("state")["move_style"] == "rest"


def steady_speed(ctl, stick=None, walk_to_mode=None, secs=1.4):
    """Server-side speed between 0.6 s and secs, pushing straight ahead from the
    spawn point (reset first, so the arena wall is never in the way)."""
    reset(ctl)
    _, c0, _ = snapshot(ctl)
    if stick is not None:
        ctl("stick", **stick)
    else:
        f = math.radians(c0["facing"])
        ctl("walk_to", x=c0["x"] + 8 * math.cos(f), y=c0["y"] + 8 * math.sin(f), mode=walk_to_mode)
    time.sleep(0.6)
    d, a, _ = snapshot(ctl)
    t_a = d["t_ms"]
    time.sleep(secs - 0.6)
    d, b, _ = snapshot(ctl)
    mode = d["move_mode"]
    ctl("release")
    time.sleep(0.6)
    return math.hypot(b["x"] - a["x"], b["y"] - a["y"]) / ((d["t_ms"] - t_a) / 1000.0), mode


@pytest.mark.parametrize("always_run", [False, True])
def test_walk_run(home, always_run):
    """Stick magnitude picks walk or run at the game's own speeds; Always Run runs
    at any deflection (plan §3, re-notes F21, F23)."""
    ctl = home
    ctl("always_run", on=int(always_run))
    time.sleep(0.3)
    assert ctl("state")["always_run"] is always_run
    try:
        walk_ref, _ = steady_speed(ctl, walk_to_mode=1)
        run_ref, _ = steady_speed(ctl, walk_to_mode=2)
        assert walk_ref < 0.7 * run_ref, (walk_ref, run_ref)
        low, low_mode = steady_speed(ctl, stick={"ly": 0.3})
        high, high_mode = steady_speed(ctl, stick={"ly": 0.9})
        assert high_mode == "run" and high == pytest.approx(run_ref, rel=0.10), (high, run_ref)
        if always_run:
            assert low_mode == "run" and low == pytest.approx(run_ref, rel=0.10), (low, run_ref)
        else:
            assert low_mode == "walk" and low == pytest.approx(walk_ref, rel=0.10), (low, walk_ref)
    finally:
        ctl("always_run", on=0)
