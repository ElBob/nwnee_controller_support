"""Quickbar picker (quickbar plan Q2): holding the picker input shows the visible
bank as a ring, the right stick highlights a button, releasing uses it; the right
stick doesn't move the camera meanwhile, and nothing reaches the server."""
import os
import subprocess
import time

STEALTH = 3  # the test character's slot 3 (test_quickbar.py); ring slot 3 is due right


def _server_heard_nwpad(game):
    log = open(os.path.join(game, "game.log"), errors="replace").read()
    return "window does not exist: 18532" in log  # nwpad tokens 0x6e77xxxx


def _pick(ctl, rx, ry):
    ctl("stick", picker=1)
    time.sleep(0.4)
    p = ctl("state")["picker"]
    assert p["open"] and p["selected"] == -1, p
    yaw0 = ctl("state")["camera"]["yaw"]
    ctl("stick", picker=1, rx=rx, ry=ry)
    time.sleep(0.5)
    s = ctl("state")
    assert abs(s["camera"]["yaw"] - yaw0) < 0.5, "the right stick turned the camera while picking"
    ctl("release")
    time.sleep(0.6)
    return s["picker"], ctl("state")["picker"]


def test_pick_and_use(game, ctl):
    during, after = _pick(ctl, 1.0, 0.0)
    assert during["selected"] == STEALTH, during
    assert not after["open"] and after["last_used"] == STEALTH, after
    _pick(ctl, 1.0, 0.0)  # stealth back off
    assert not _server_heard_nwpad(game)


def test_untouched_cancels(game, ctl):
    before = ctl("state")["picker"]["last_used"]
    ctl("stick", picker=1)
    time.sleep(0.4)
    ctl("release")
    time.sleep(0.4)
    p = ctl("state")["picker"]
    assert not p["open"] and p["last_used"] == before, p


def _key(action, key="Scroll_Lock"):
    """Press/release a key through X (XTEST keyboard events reach the game)."""
    env = dict(os.environ, DISPLAY=os.environ.get("NWPAD_DISPLAY", ":0"))
    win = subprocess.run(["xdotool", "search", "--name", "Neverwinter Nights: Enhanced"], env=env,
                         capture_output=True, text=True).stdout.split()
    if win:
        subprocess.run(["xdotool", "windowactivate", "--sync", win[0]], env=env, capture_output=True)
    subprocess.run(["xdotool", action, key], env=env, check=True)


def test_picker_key(game, ctl):
    """The picker key (picker-key, default Scroll Lock) holds the picker, as Steam
    Input would send it; the game doesn't see the key."""
    filtered0 = ctl("state")["events"]["filtered"]
    _key("keydown")
    try:
        time.sleep(0.5)
        p = ctl("state")["picker"]
        assert p["key_held"] and p["open"], p
        ctl("stick", rx=1.0)  # pointing, as the trackpad would
        time.sleep(0.5)
        assert ctl("state")["picker"]["selected"] == STEALTH
        ctl("release")
    finally:
        _key("keyup")
    time.sleep(0.6)
    p = ctl("state")["picker"]
    assert not p["open"] and p["last_used"] == STEALTH, p
    assert ctl("state")["events"]["filtered"] >= filtered0 + 2  # down and up swallowed
    _pick(ctl, 1.0, 0.0)  # stealth back off
