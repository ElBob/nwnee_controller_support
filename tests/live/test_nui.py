"""Client-side NUI windows (quickbar plan, re-notes F32): nwpad creates, updates and
destroys a NUI window itself, and none of it reaches the server. The local server
logs any NUI message for a window it doesn't know, which is what a remote server
would see."""
import json
import os
import time

from conftest import ROOT, server_heard

TOKEN = 0x6E770001
PROBE = json.load(open(os.path.join(ROOT, "tools", "nui", "probe.json")))


def _create(ctl, token, wid):
    return ctl("nui_create", token=token, id=wid, json=json.dumps(PROBE, separators=(",", ":")))


def test_window_lifecycle_stays_local(game, ctl):
    r = _create(ctl, TOKEN, "nwpad_test")
    assert r["ok"] and r["windows"] >= 1 and not r["events_pending"], r
    assert ctl("nui_bind", token=TOKEN, name="icon", json='"is_MageArm"')["ok"]
    assert ctl("nui_bind", token=TOKEN, name="name", json='"Mage Armor"')["ok"]
    time.sleep(1.0)
    r = ctl("nui_destroy", token=TOKEN)
    assert r["ok"], r
    time.sleep(1.0)
    assert not TOKEN in server_heard(game), "a message for nwpad's window reached the server"


def test_server_would_hear_other_tokens(game, ctl):
    """Control for the test above: a window outside nwpad's token range still sends
    its "open" event, so the check can see a leak."""
    assert _create(ctl, 5, "nwpad_control")["ok"]
    try:
        deadline = time.monotonic() + 5
        while not 5 in server_heard(game):
            assert time.monotonic() < deadline, "the control window's open event never reached the server"
            time.sleep(0.2)
    finally:
        ctl("nui_destroy", token=5)
