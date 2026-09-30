"""Camera arbitration (plan §3, §8.6): mouse input suspends the stick camera
until the mouse has been idle for mouse_idle_ms, then the stick takes over."""
import os
import subprocess
import time

from conftest import ROOT, game_window_center

MOUSE_IDLE_MS = 300  # config default (plan §6.3)
REHIDE_MS = 1000     # cursor_rehide_ms in the test config


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
    time.sleep(REHIDE_MS / 1000)  # mouse still that long: the stick hides at once
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


def _jiggle(ctl):
    """Jiggle the mouse and make sure the game saw it (a new uinput device can
    miss its first events while the compositor adds it)."""
    x, y = game_window_center()
    motion0 = ctl("state")["events"]["mouse_motion"]
    subprocess.run([os.path.join(ROOT, "tools", "uinput_mouse.py"), "--to", str(x), str(y), "--jiggle", "40",
                    "--settle", "1.5"], check=True, capture_output=True)
    assert ctl("state")["events"]["mouse_motion"] > motion0 + 5, "no mouse motion reached the game"


def test_mouse_shows_cursor_while_sticks_held(ctl):
    """Moving the mouse while a stick is held shows the cursor; it hides again once
    the mouse is still and the stick has been held for cursor_rehide_ms, counting
    only continuous stick use (Robert's request)."""
    time.sleep(REHIDE_MS / 1000 + 0.2)
    ctl("stick", rx=0.3)
    try:
        time.sleep(0.3)
        assert ctl("state")["cursor"]["stick_hidden"], "stick didn't hide the cursor"
        _jiggle(ctl)
        c = ctl("state")["cursor"]
        assert not c["stick_hidden"], c  # shown while the mouse moves
        time.sleep(REHIDE_MS / 1000 * 0.5)
        c = ctl("state")["cursor"]
        assert not c["stick_hidden"], c  # still shown: the mouse only just stopped
        time.sleep(REHIDE_MS / 1000 * 0.5 + 0.3)
        c = ctl("state")["cursor"]
        assert c["stick_hidden"], c      # mouse still and stick held long enough
        # Letting go restarts the count: stick use must be continuous.
        # (The stick must come back while the mouse is still recent: a mouse
        # already still for cursor_rehide_ms hides the cursor at once.)
        _jiggle(ctl)
        assert not ctl("state")["cursor"]["stick_hidden"]
        time.sleep(REHIDE_MS / 1000 * 0.2)
        ctl("release")
        time.sleep(0.15)
        ctl("stick", rx=0.3)
        time.sleep(REHIDE_MS / 1000 * 0.8)  # past cursor_rehide_ms since the mouse stopped
        c = ctl("state")["cursor"]
        assert not c["stick_hidden"], str(c)  # but not since the stick came back
        time.sleep(REHIDE_MS / 1000 * 0.2 + 0.3)
        c = ctl("state")["cursor"]
        assert c["stick_hidden"], str(c)
    finally:
        ctl("release")
