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


def _stealth_off(ctl):
    """Leave the test character out of stealth (its slot then reads "Cancel ...")."""
    slot = ctl("quickbar")["slots"][STEALTH]
    if slot["name"].startswith("Cancel"):
        ctl("quickbar_use", slot=STEALTH)
        time.sleep(0.5)


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
    _stealth_off(ctl)
    during, after = _pick(ctl, 1.0, 0.0)
    assert during["selected"] == STEALTH, during
    assert not after["open"] and after["last_used"] == STEALTH, after
    _stealth_off(ctl)
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
    """action: keydown, keyup or key (a tap)."""
    """Press/release a key through X (XTEST keyboard events reach the game)."""
    env = dict(os.environ, DISPLAY=os.environ.get("NWPAD_DISPLAY", ":0"))
    win = subprocess.run(["xdotool", "search", "--name", "Neverwinter Nights: Enhanced"], env=env,
                         capture_output=True, text=True).stdout.split()
    if win:
        subprocess.run(["xdotool", "windowactivate", "--sync", win[0]], env=env, capture_output=True)
    subprocess.run(["xdotool", action, key], env=env, check=True)


def test_picker_key_toggle(game, ctl):
    """Toggle mode (the default, like Baldur's Gate 3's radial): a press of the picker
    key (ScrollLock) opens the picker and it stays open; the confirm key (Enter) uses
    the highlighted button, the cancel key (Escape) or another press closes it. The
    game sees none of these keys while the picker is open, and Enter/Escape otherwise."""
    _stealth_off(ctl)
    filtered0 = ctl("state")["events"]["filtered"]
    _key("key", "Return")  # closed: the game's (opens the chat bar) ...
    time.sleep(0.3)
    assert ctl("state")["events"]["filtered"] == filtered0
    _key("key", "Escape")  # ... and closes it again
    time.sleep(0.3)
    _key("key")  # open
    time.sleep(0.5)
    p = ctl("state")["picker"]
    assert p["open"], p
    ctl("stick", rx=1.0)  # pointing, as the trackpad would
    time.sleep(0.5)
    ctl("release")
    time.sleep(0.3)
    assert ctl("state")["picker"]["open"], "the picker closed when the stick let go"
    _key("key", "Return")  # confirm: uses Stealth Mode
    time.sleep(0.6)
    p = ctl("state")["picker"]
    assert not p["open"] and p["last_used"] == STEALTH, p
    _stealth_off(ctl)
    for close in ("Escape", "Scroll_Lock"):  # cancel, or the picker key again
        before = ctl("state")["picker"]["last_used"]
        _key("key")
        time.sleep(0.4)
        ctl("stick", rx=1.0)
        time.sleep(0.4)
        ctl("release")
        _key("key", close)
        time.sleep(0.5)
        p = ctl("state")["picker"]
        assert not p["open"] and p["last_used"] == before, (close, p)
    assert not _server_heard_nwpad(game)


def test_bank_wheels(game, ctl):
    """Three wheels: the bank keys (default "[" / "]") change the active one while the
    picker is open, and pass through to the game otherwise; a button can be used
    from a bank the quickbar isn't showing."""
    _stealth_off(ctl)
    filtered0 = ctl("state")["events"]["filtered"]
    _key("key", "bracketright")  # picker closed: the game's key
    time.sleep(0.3)
    assert ctl("state")["events"]["filtered"] == filtered0
    assert ctl("quickbar_bank", bank=1)["ok"]  # the quickbar shows bank 1
    try:
        ctl("stick", picker=1)
        time.sleep(0.4)
        assert ctl("state")["picker"]["bank"] == 1
        _key("key", "bracketright")  # open: next bank (wraps to 2)
        time.sleep(0.4)
        assert ctl("state")["picker"]["bank"] == 2
        assert ctl("state")["events"]["filtered"] >= filtered0 + 2  # its down and up
        _key("key", "bracketleft")
        _key("key", "bracketleft")
        time.sleep(0.4)
        assert ctl("state")["picker"]["bank"] == 0
        ctl("stick", picker=1, rx=1.0)  # bank 0, slot 3: Stealth Mode
        time.sleep(0.5)
        ctl("release")
        time.sleep(0.6)
        assert ctl("state")["picker"]["last_used"] == STEALTH  # bank 0 * 12 + 3
    finally:
        ctl("release")
        ctl("quickbar_bank", bank=0)
        _stealth_off(ctl)
    assert not _server_heard_nwpad(game)
