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


def test_stick_hides_cursor_until_mouse_moves(ctl):
    """The cursor hides while a stick is used and returns on mouse motion, back to
    whatever the game last asked for (Robert's request; re-notes F24)."""
    assert ctl("state")["cursor"]["hooked"], "SDL_ShowCursor not hooked"
    x, y = game_window_center()
    subprocess.run([os.path.join(ROOT, "tools", "uinput_mouse.py"), "--to", str(x), str(y), "--jiggle", "4"],
                   check=True, capture_output=True)
    time.sleep(0.3)
    c = ctl("state")["cursor"]
    assert c["shown"] and not c["stick_hidden"], c
    ctl("stick", rx=0.5)
    time.sleep(0.3)
    ctl("release")
    time.sleep(0.3)
    c = ctl("state")["cursor"]
    assert c["stick_hidden"] and not c["shown"], c  # stays hidden after release
    subprocess.run([os.path.join(ROOT, "tools", "uinput_mouse.py"), "--to", str(x), str(y), "--jiggle", "4"],
                   check=True, capture_output=True)
    time.sleep(0.3)
    c = ctl("state")["cursor"]
    assert not c["stick_hidden"] and c["shown"] == c["game_wants"], c
