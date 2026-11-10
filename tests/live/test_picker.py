"""Quickbar picker (quickbar plan Q2), like Baldur's Gate 3's radial: opening it
shows the quickbar banks as wheels, the right stick highlights a button, confirming
uses it; the right stick doesn't move the camera meanwhile, and nothing reaches the
server."""
import time

import pytest

from conftest import focus_game, server_heard_nwpad, xkey

STEALTH = 3  # the test character's slot 3 (test_quickbar.py); ring slot 3 is due right


def _stealth_off(ctl):
    """Leave the test character out of stealth (its slot then reads "Cancel ...")."""
    slot = ctl("quickbar")["slots"][STEALTH]
    if slot["name"].startswith("Cancel"):
        ctl("quickbar_use", slot=STEALTH)
        time.sleep(0.5)


def _pick(ctl, rx, ry):
    """The picker opens on the middle wheel, the plain bank (0), where Stealth Mode is."""
    ctl("picker", action="open")
    time.sleep(0.4)
    assert ctl("state")["picker"]["bank"] == 0
    p = ctl("state")["picker"]
    assert p["open"] and p["selected"] == -1, p
    yaw0 = ctl("state")["camera"]["yaw"]
    ctl("stick", rx=rx, ry=ry)
    time.sleep(0.5)
    s = ctl("state")
    assert abs(s["camera"]["yaw"] - yaw0) < 0.5, "the right stick turned the camera while picking"
    ctl("release")
    ctl("picker", action="confirm")
    time.sleep(0.6)
    return s["picker"], ctl("state")["picker"]


@pytest.fixture(autouse=True)
def picker_closed(ctl):
    """Each test starts and ends with the picker closed and the character out of
    stealth (a failed test would otherwise leave them for the next one)."""
    _stealth_off(ctl)
    yield
    ctl("picker", action="cancel")
    time.sleep(0.3)
    _stealth_off(ctl)


def test_pick_and_use(game, ctl):
    _stealth_off(ctl)
    during, after = _pick(ctl, 1.0, 0.0)
    assert during["selected"] == STEALTH, during
    assert not after["open"] and after["last_used"] == STEALTH, after
    _stealth_off(ctl)
    assert not server_heard_nwpad(game)


def test_confirm_without_pick_uses_nothing(game, ctl):
    before = ctl("state")["picker"]["last_used"]
    ctl("picker", action="open")
    time.sleep(0.4)
    ctl("picker", action="confirm")
    time.sleep(0.4)
    p = ctl("state")["picker"]
    assert not p["open"] and p["last_used"] == before, p


def _key(action, key="Scroll_Lock"):
    """action: keydown, keyup or key (a tap)."""
    xkey(key, action)


def test_picker_key_toggle(game, ctl):
    """A press of the picker key (ScrollLock) opens the picker and it stays open; the confirm key (Enter) uses
    the highlighted button, the cancel key (Escape) or another press closes it. The
    game sees none of these keys while the picker is open, and Enter/Escape otherwise."""
    _stealth_off(ctl)
    filtered0 = ctl("state")["events"]["filtered"]
    _key("key", "Return")  # closed: the game's (opens the chat bar) ...
    time.sleep(0.3)
    assert ctl("state")["events"]["filtered"] == filtered0
    _key("key", "Escape")  # ... and closes it again
    time.sleep(0.3)
    _key("key")  # open: on the middle wheel, the plain bank, where Stealth Mode is
    time.sleep(0.5)
    p = ctl("state")["picker"]
    assert p["open"] and p["bank"] == 0, p
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
    assert not server_heard_nwpad(game)


def test_bank_wheels(game, ctl):
    """Three wheels, left to right Shift (bank 1), Plain (0), Ctrl (2); it opens on the
    middle one. The bank keys (default "[" / "]") move the big wheel one place left /
    right while the picker is open, do nothing past either end (and are still the
    picker's keys), and pass through to the game otherwise. A button can be used from
    a bank the quickbar isn't showing."""
    _stealth_off(ctl)
    filtered0 = ctl("state")["events"]["filtered"]
    _key("key", "bracketright")  # picker closed: the game's key
    time.sleep(0.3)
    assert ctl("state")["events"]["filtered"] == filtered0
    assert ctl("quickbar_bank", bank=2)["ok"]  # the quickbar shows bank 2 ...
    try:
        ctl("picker", action="open")
        time.sleep(0.4)
        assert ctl("state")["picker"]["bank"] == 0  # ... and the picker still opens on the middle wheel, Plain
        _key("key", "bracketright")  # the wheel to the right: Ctrl
        time.sleep(0.4)
        assert ctl("state")["picker"]["bank"] == 2
        assert ctl("state")["events"]["filtered"] >= filtered0 + 2  # its down and up
        filtered = ctl("state")["events"]["filtered"]
        _key("key", "bracketright")  # the right end: no wrap, and still the picker's key
        time.sleep(0.4)
        assert ctl("state")["picker"]["bank"] == 2
        assert ctl("state")["events"]["filtered"] >= filtered + 2
        _key("key", "bracketleft")  # back to the middle, then to Shift on the left
        _key("key", "bracketleft")
        time.sleep(0.4)
        assert ctl("state")["picker"]["bank"] == 1
        _key("key", "bracketleft")  # the left end: no wrap either
        time.sleep(0.4)
        assert ctl("state")["picker"]["bank"] == 1
        assert not ctl("state")["picker"]["sliding"]
        _key("key", "bracketright")  # back to Plain, where Stealth Mode is
        time.sleep(0.4)
        assert ctl("state")["picker"]["bank"] == 0
        ctl("stick", rx=1.0)  # slot 3: Stealth Mode
        time.sleep(0.5)
        ctl("release")
        ctl("picker", action="confirm")
        time.sleep(0.6)
        assert ctl("state")["picker"]["last_used"] == STEALTH  # bank 0 * 12 + 3
    finally:
        ctl("release")
        ctl("picker", action="cancel")
        ctl("quickbar_bank", bank=0)
        _stealth_off(ctl)
    assert not server_heard_nwpad(game)


def test_pad_bumpers(game, ctl, virtual_pad):
    """Straight from the controller (gamepad buttons, no keys): either bumper opens the
    picker, on the middle wheel (Plain); once it is open LB / RB move the big wheel one
    place, and do nothing past either end."""
    if virtual_pad is None:
        pytest.skip("no virtual pad (python-evdev or /dev/uinput missing)")
    pad = virtual_pad
    focus_game()  # SDL gives a pad's buttons only to the focused window
    assert ctl("status")["controller"] == "open"
    n0 = ctl("state")["events"]["pad_buttons"]
    assert not ctl("state")["picker"]["open"]
    pad.press("rb")  # either bumper opens it ...
    time.sleep(0.6)
    p = ctl("state")["picker"]
    assert p["open"] and p["bank"] == 0, p  # ... on Plain, and the opening press moves nothing
    ctl("picker", action="cancel")
    time.sleep(0.4)
    assert not ctl("state")["picker"]["open"]
    pad.press("lb")
    time.sleep(0.6)
    p = ctl("state")["picker"]
    assert p["open"] and p["bank"] == 0, p
    pad.press("rb")  # Plain -> Ctrl
    time.sleep(0.5)
    assert ctl("state")["picker"]["bank"] == 2
    pad.press("rb")  # the right end
    time.sleep(0.4)
    assert ctl("state")["picker"]["bank"] == 2
    pad.press("lb,lb")  # Ctrl -> Plain -> Shift
    time.sleep(0.6)
    assert ctl("state")["picker"]["bank"] == 1
    pad.press("lb")  # the left end
    time.sleep(0.4)
    assert ctl("state")["picker"]["bank"] == 1
    pad.press("rb")  # back to Plain, where Stealth Mode is
    time.sleep(0.5)
    assert ctl("state")["picker"]["bank"] == 0
    ctl("stick", rx=1.0)
    time.sleep(0.5)
    ctl("release")
    ctl("picker", action="confirm")
    time.sleep(0.6)
    assert ctl("state")["picker"]["last_used"] == STEALTH
    assert ctl("state")["events"]["pad_buttons"] == n0 + 8
    assert not server_heard_nwpad(game)


def test_pad_confirm_and_cancel(game, ctl, virtual_pad):
    """A uses the highlighted button and B closes the picker without using anything,
    straight from the controller, like Enter and Escape. With the picker closed they do
    nothing of nwpad's. (Start and Select are the game's menu and map: another test.)"""
    if virtual_pad is None:
        pytest.skip("no virtual pad (python-evdev or /dev/uinput missing)")
    pad = virtual_pad
    focus_game()
    n0 = ctl("state")["events"]["pad_buttons"]
    pad.press("a,b")  # closed: nwpad takes no action
    time.sleep(0.3)
    assert not ctl("state")["picker"]["open"]
    assert ctl("state")["events"]["pad_buttons"] == n0 + 2
    pad.press("lb")  # open it with a bumper; B closes it without using anything
    time.sleep(0.6)
    assert ctl("state")["picker"]["open"]
    last = ctl("state")["picker"]["last_used"]
    ctl("stick", rx=1.0)
    time.sleep(0.4)
    ctl("release")
    pad.press("back,start")  # Select and Start are not the picker's: it stays open
    time.sleep(0.4)
    assert ctl("state")["picker"]["open"]
    pad.press("b")
    time.sleep(0.5)
    p = ctl("state")["picker"]
    assert not p["open"] and p["last_used"] == last, p
    pad.press("rb")  # A uses the highlighted button: Stealth Mode
    time.sleep(0.6)
    ctl("stick", rx=1.0)
    time.sleep(0.5)
    ctl("release")
    pad.press("a")
    time.sleep(0.6)
    p = ctl("state")["picker"]
    assert not p["open"] and p["last_used"] == STEALTH, p
    assert not server_heard_nwpad(game)


def test_pad_menu_and_map(game, ctl, virtual_pad):
    """Start opens the game's menu and Select toggles its map, straight from the
    controller (the game's own key handling, so its guards apply); a second press closes
    each; neither does anything while the picker is open."""
    if virtual_pad is None:
        pytest.skip("no virtual pad (python-evdev or /dev/uinput missing)")
    pad = virtual_pad
    focus_game()

    def state(key):
        return ctl("state")["events"][key]

    try:
        assert not state("game_map_open") and not state("game_menu_open")
        pad.press("back")  # Select: the map
        time.sleep(1.0)
        assert state("game_map_open"), "the map didn't open"
        pad.press("back")
        time.sleep(1.0)
        assert not state("game_map_open"), "the map didn't close"
        ctl("picker", action="open")  # with the picker open they do nothing
        time.sleep(0.5)
        pad.press("back,start")
        time.sleep(0.8)
        assert not state("game_map_open") and not state("game_menu_open")
        assert ctl("state")["picker"]["open"]
        ctl("picker", action="cancel")
        time.sleep(0.5)
        pad.press("start")  # Start: the game menu (the options panel)
        time.sleep(1.5)
        assert state("game_menu_open"), "the game menu didn't open"
        pad.press("start")
        time.sleep(1.5)
        assert not state("game_menu_open"), "the game menu didn't close"
    finally:  # leave nothing open for the next test
        if state("game_menu_open"):
            pad.press("start")
            time.sleep(1.0)
        if state("game_map_open"):
            pad.press("back")
            time.sleep(1.0)
    assert not server_heard_nwpad(game)
