"""M1 camera scenarios (plan §8.6): linearity, pitch limits, and script locks.

Locks are applied with NWScript chunks over the control socket (re-notes F17, F18).
"""
import time

import pytest

YAW_SPEED = 180.0   # config defaults (plan §6.3)
PITCH_SPEED = 90.0


def unwrap_step(prev, cur):
    """Smallest signed step from prev to cur, in degrees."""
    return (cur - prev + 180.0) % 360.0 - 180.0


def yaw_rate(ctl, rx, seconds=1.5, sample_s=0.05):
    """Hold the right stick at rx and measure the camera yaw rate in deg/s."""
    ctl("stick", rx=rx)
    time.sleep(0.2)  # let the first frames apply
    s = ctl("state")
    t0, prev, total = s["t_ms"], s["camera"]["yaw"], 0.0
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        time.sleep(sample_s)
        s = ctl("state")
        total += unwrap_step(prev, s["camera"]["yaw"])
        prev = s["camera"]["yaw"]
    ctl("release")
    return total / ((s["t_ms"] - t0) / 1000.0)


def test_camera_available(ctl):
    s = ctl("status")
    assert s["features"]["camera"] is True, s
    assert ctl("state")["camera"] is not None


def test_yaw_linearity(ctl):
    full = yaw_rate(ctl, 1.0)
    half = yaw_rate(ctl, 0.5)
    print(f"yaw rate: full {full:.1f} deg/s, half {half:.1f} deg/s")
    assert full < 0, "stick right should decrease yaw, like the mouse (re-notes F14)"
    assert half / full == pytest.approx(0.5, rel=0.05)
    assert abs(full) == pytest.approx(YAW_SPEED, rel=0.05)


@pytest.mark.parametrize("ry, limit", [(1.0, "max_pitch"), (-1.0, "min_pitch")])
def test_pitch_stays_within_limits(ctl, ry, limit):
    cam = ctl("state")["camera"]
    travel = cam["max_pitch"] - cam["min_pitch"]
    ctl("stick", ry=ry)
    time.sleep(1.5 * travel / PITCH_SPEED)  # long enough to overshoot by half the range
    for _ in range(5):
        cam = ctl("state")["camera"]
        assert cam["min_pitch"] - 1e-3 <= cam["pitch"] <= cam["max_pitch"] + 1e-3, cam
        time.sleep(0.05)
    ctl("release")
    assert cam["pitch"] == pytest.approx(cam[limit], abs=1e-3)


LOCKS = {
    "yaw": ("LockCameraDirection", {"rx": 1.0}, "yaw"),
    "pitch": ("LockCameraPitch", {"ry": 1.0}, "pitch"),
}


@pytest.mark.parametrize("axis", ["yaw", "pitch"])
def test_script_lock_blocks_stick(ctl, axis):
    fn, stick, field = LOCKS[axis]
    # Start pitch mid-range so an unlocked stick would visibly move it.
    ctl("script_chunk", code="AssignCommand(GetFirstPC(), SetCameraFacing(-1.0, -1.0, 45.0, CAMERA_TRANSITION_TYPE_SNAP));")
    time.sleep(0.5)
    ctl("script_chunk", code=f"{fn}(GetFirstPC(), TRUE);")
    try:
        time.sleep(0.5)
        before = ctl("state")["camera"]
        assert before[f"{axis}_locked"], before
        ctl("stick", **stick)
        time.sleep(0.5)
        after = ctl("state")["camera"]
        ctl("release")
        assert after[field] == pytest.approx(before[field], abs=0.01), (before, after)
    finally:
        ctl("script_chunk", code=f"{fn}(GetFirstPC(), FALSE);")
    time.sleep(0.5)
    unlocked = ctl("state")["camera"]
    assert not unlocked[f"{axis}_locked"], unlocked
    ctl("stick", **stick)
    time.sleep(0.3)
    moved = ctl("state")["camera"]
    ctl("release")
    assert moved[field] != pytest.approx(unlocked[field], abs=0.5), (unlocked, moved)
