"""Camera arbitration (plan §3, §8.6): mouse input suspends the stick camera
until the mouse has been idle for mouse_idle_ms, then the stick takes over."""
import os
import subprocess
import time

from conftest import ROOT, game_window_center

MOUSE_IDLE_MS = 300  # config default (plan §6.3)


def test_mouse_suspends_stick_camera(ctl):
    x, y = game_window_center()
    ctl("stick", rx=1.0)
    motion0 = ctl("state")["events"]["mouse_motion"]
    mouse = subprocess.Popen([os.path.join(ROOT, "tools", "uinput_mouse.py"), "--to", str(x), str(y),
                              "--jiggle", "60", "--interval", "0.02"])
    try:
        deadline = time.monotonic() + 5
        while ctl("state")["events"]["mouse_motion"] < motion0 + 5:
            assert time.monotonic() < deadline, "no mouse motion reached the game"
            time.sleep(0.02)
        # Mid-jiggle: the mouse owns the camera and the stick has no effect.
        a = ctl("state")
        time.sleep(0.4)
        b = ctl("state")
        assert a["camera_owner"] == b["camera_owner"] == "mouse", (a, b)
        assert abs(b["camera"]["yaw"] - a["camera"]["yaw"]) < 0.5, (a["camera"], b["camera"])
        assert mouse.wait(timeout=10) == 0
    finally:
        if mouse.poll() is None:
            mouse.kill()
    # After the idle timeout, the held stick takes the camera back.
    time.sleep(MOUSE_IDLE_MS / 1000 + 0.2)
    c = ctl("state")
    time.sleep(0.3)
    d = ctl("state")
    assert c["camera_owner"] == d["camera_owner"] == "stick", (c, d)
    assert d["camera"]["yaw"] != c["camera"]["yaw"]
