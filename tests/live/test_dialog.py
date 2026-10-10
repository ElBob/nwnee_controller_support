"""NPC conversations (dialog plan D1, re-notes F36): nwpad reads the NPC's line and
the replies of the open conversation, and answers it the way the number keys do."""
import os
import subprocess
import time

import pytest

from conftest import ROOT, mouse, server_heard_nwpad, xkey

# The nearest NPC to the test character, Rules Enforcer D, has a conversation.
START = ('object pc=GetFirstPC(); object n=GetNearestCreature(CREATURE_TYPE_PLAYER_CHAR, PLAYER_CHAR_NOT_PC, pc, 1);'
         ' AssignCommand(n, ClearAllActions()); AssignCommand(n, ActionStartConversation(pc, "%s", FALSE, FALSE));')


@pytest.fixture(autouse=True)
def npc_home(ctl):
    """Starting a conversation walks the NPC over to the player, into the movement
    tests' way: put it back where it started afterwards."""
    ctl("script_chunk", code='object n=GetNearestCreature(CREATURE_TYPE_PLAYER_CHAR, PLAYER_CHAR_NOT_PC, GetFirstPC(), 1);'
                             ' SetLocalObject(GetModule(), "nwpad_npc", n); SetLocalLocation(GetModule(), "nwpad_npc_home", GetLocation(n));')
    _end(ctl)
    yield
    if ctl("dialog")["dialog"]:
        ctl("dialog_select", end=1)
    ctl("script_chunk", code='object n=GetLocalObject(GetModule(), "nwpad_npc"); AssignCommand(n, ClearAllActions(TRUE));'
                             ' AssignCommand(n, JumpToLocation(GetLocalLocation(GetModule(), "nwpad_npc_home")));'
                             ' DestroyObject(GetObjectByTag("nwpad_speaker"));')
    time.sleep(1.0)


@pytest.fixture
def test_dlg(ctl, tmp_path):
    """nwpad's own test conversation (tools/make_test_dlg.py), served to the game."""
    dlg = str(tmp_path / "nwpadtest.dlg")
    subprocess.run([os.path.join(ROOT, "tools", "make_test_dlg.py"), dlg], check=True)
    assert ctl("resource_publish", src=dlg, name="nwpadtest.dlg")["ok"]
    return "nwpadtest"


def _dialog(ctl, timeout=5.0, want=lambda d: d is not None):
    deadline = time.monotonic() + timeout
    while True:
        d = ctl("dialog")["dialog"]
        if want(d):
            return d
        assert time.monotonic() < deadline, f"dialog never reached the expected state: {d}"
        time.sleep(0.2)


def _end(ctl):
    """End the open conversation, if any."""
    if ctl("dialog")["dialog"]:
        ctl("dialog_select", end=1)
        _dialog(ctl, want=lambda d: d is None)


def _key(key):
    xkey(key)
    time.sleep(0.4)


def test_read_and_answer(game, ctl):
    ctl("script_chunk", code=START % "")
    d = _dialog(ctl, want=lambda d: d and d["replies"])
    assert d["line"] == "Welcome to the Contest Of Champions!", d
    assert [r["text"] for r in d["replies"]] == ["What is the Contest Of Champions?", "CONTINUE"], d
    assert all(r["selectable"] for r in d["replies"])
    seq = d["seq"]
    assert ctl("dialog_select", index=0)["ok"]
    d = _dialog(ctl, want=lambda d: d and d["seq"] != seq and d["line"].startswith("The Contest Of Champions is"))
    assert [r["text"] for r in d["replies"]] == ["CONTINUE"], d
    assert ctl("dialog_select", end=1)["ok"]
    _dialog(ctl, want=lambda d: d is None)


def test_window_keys(game, ctl):
    """nwpad's conversation window (D2): the arrow keys move the highlight, Enter
    answers, Escape ends the conversation; the game sees none of them meanwhile."""
    filtered0 = ctl("state")["events"]["filtered"]
    _key("Down")  # no conversation: the game's key
    assert ctl("state")["events"]["filtered"] == filtered0
    ctl("script_chunk", code=START % "")
    _dialog(ctl, want=lambda d: d and d["replies"])
    time.sleep(0.5)
    ui = ctl("dialog_ui")
    assert ui["open"] and ui["highlight"] == 0, ui
    _key("Down")
    assert ctl("dialog_ui")["highlight"] == 1
    _key("Down")  # wraps
    assert ctl("dialog_ui")["highlight"] == 0
    _key("Up")
    _key("Up")
    assert ctl("dialog_ui")["highlight"] == 0
    seq = ctl("dialog")["dialog"]["seq"]
    _key("Return")  # answers reply 1
    _dialog(ctl, want=lambda d: d and d["seq"] != seq and d["line"].startswith("The Contest Of Champions is"))
    assert ctl("dialog_ui")["open"]
    _key("Escape")  # ends it
    _dialog(ctl, want=lambda d: d is None)
    assert not ctl("dialog_ui")["open"]
    assert not server_heard_nwpad(game)  # nwpad's NUI tokens never reach the server


def test_test_conversation(game, ctl, test_dlg):
    """nwpad's own test conversation, shaped like the official campaigns' extremes:
    markup (the client turns <StartCheck> etc. into colour codes; nwpad's window
    strips them), an 83-reply list that scrolls with the highlight, a ~1000-character
    line and a long reply."""
    ctl("script_chunk", code=START % test_dlg)
    d = _dialog(ctl, want=lambda d: d and len(d["replies"]) == 5)
    assert d["line"].startswith("<c") and "[Nods]</c> Hello, " in d["line"], d  # tokens filled in, colour codes
    assert ctl("dialog_ui")["open"]
    seq = d["seq"]
    ctl("dialog_select", index=1)  # "Many replies."
    d = _dialog(ctl, want=lambda d: d and d["seq"] != seq and len(d["replies"]) == 83)
    time.sleep(0.5)
    ui = ctl("dialog_ui")
    assert ui["first"] == 0 and 0 < ui["last"] < 82, ui  # not all fit
    for _ in range(40):
        ctl("dialog_ui", step=1)
    time.sleep(0.5)
    ui = ctl("dialog_ui")
    assert ui["highlight"] == 40 and ui["first"] <= 40 <= ui["last"] and ui["first"] > 0, ui
    for _ in range(41):  # 40 -> 0, then wrap past the top to the last
        ctl("dialog_ui", step=-1)
    time.sleep(0.5)
    ui = ctl("dialog_ui")
    assert ui["highlight"] == 82 and ui["last"] == 82, ui
    # The long line: a stick scrolls the text (the highlight stays put)
    seq = ctl("dialog")["dialog"]["seq"]
    ctl("dialog_ui", action="confirm")  # Option 83 -> back to the start
    d = _dialog(ctl, want=lambda d: d and d["seq"] != seq and len(d["replies"]) == 5)
    seq = d["seq"]
    ctl("dialog_select", index=0)  # "The long line, please."
    _dialog(ctl, want=lambda d: d and d["seq"] != seq and d["line"].startswith("This is sentence 1"))
    time.sleep(0.5)
    assert ctl("dialog_ui")["text_top"] == 0
    ctl("stick", ly=-1.0)  # down
    time.sleep(1.0)
    ctl("release")
    ui = ctl("dialog_ui")
    assert ui["text_top"] > 2 and ui["highlight"] == 0, ui
    _end(ctl)
    # Another speaker (the campaigns' Speaker tags), cp1252 text, an empty reply and
    # a line without replies (the game fills in "Continue" / "End Dialog")
    ctl("script_chunk", code='object pc=GetFirstPC(); vector v=GetPosition(pc);'
                             ' object c=CreateObject(OBJECT_TYPE_CREATURE, "nw_bandit001",'
                             ' Location(GetArea(pc), Vector(v.x+2.0, v.y+2.0, v.z), 0.0), FALSE, "nwpad_speaker");'
                             ' ChangeToStandardFaction(c, STANDARD_FACTION_COMMONER);')
    time.sleep(1.0)
    ctl("script_chunk", code='object n=GetLocalObject(GetModule(), "nwpad_npc"); AssignCommand(n, ClearAllActions());'
                             ' AssignCommand(n, ActionStartConversation(GetFirstPC(), "nwpadtest", FALSE, FALSE));')
    d = _dialog(ctl, want=lambda d: d and len(d["replies"]) == 5)
    assert d["name"] == "Rules Enforcer D"
    seq = d["seq"]
    ctl("dialog_select", index=3)  # "Somebody else, please."
    d = _dialog(ctl, want=lambda d: d and d["seq"] != seq and "Another speaker" in d["line"])
    assert d["name"] == "Bandit" and d["portrait"], d
    assert [r["text"] for r in d["replies"]] == ["Continue", "Back."], d
    seq = d["seq"]
    ctl("dialog_select", index=0)
    d = _dialog(ctl, want=lambda d: d and d["seq"] != seq and d["line"].startswith("The end"))
    assert [r["text"] for r in d["replies"]] == ["End Dialog"], d
    _end(ctl)
    assert not server_heard_nwpad(game)


def test_mouse(game, ctl, test_dlg):
    """The mouse on nwpad's conversation window (F37): NUI does the hit testing and
    nwpad takes the rows' input, so a click answers with the reply under it and the
    wheel moves the highlight; nothing reaches the server, the game's covered
    window or the world."""

    def row_y(k):
        """Where row k is, as NUI reports it: dry clicks (recorded, not answered)
        down the window until one lands on row k."""
        ctl("dialog_ui", dry=1)
        # Motion first, as a real mouse sends: NUI (Nuklear) knows where the pointer
        # is only from motion events, and a click it doesn't see goes to the world.
        # A new virtual tablet's first jump can arrive as the click alone (F37).
        mouse(100, 40, "--jiggle", "2")
        assert ctl("state")["events"]["mouse_motion"] > 0
        try:
            for y in range(40, 500, 12):
                n = ctl("dialog_ui")["inputs"]
                mouse(100, y, "--jiggle", "2", "--click")
                ui = ctl("dialog_ui")
                if ui["inputs"] > n and ui["last_input"]["tag"] == k:
                    return y
            pytest.fail(f"no row {k}")
        finally:
            ctl("dialog_ui", dry=0)

    ctl("script_chunk", code=START % test_dlg)
    d = _dialog(ctl, want=lambda d: d and len(d["replies"]) == 5)
    time.sleep(0.5)
    assert ctl("dialog_ui")["mouse"]
    where = ctl("state").get("client")
    y = row_y(1)
    assert ctl("dialog")["dialog"]["seq"] == d["seq"]  # the dry clicks answered nothing, here or in the game's window
    for x, yy in ((5, 4), (40, 60), (357, 128), (357, 280)):  # the frame, the portrait, the line, a corner
        mouse(x, yy, "--jiggle", "2", "--click")
        assert ctl("dialog")["dialog"] and ctl("state").get("client") == where, (x, yy)  # not the world's
    seq = d["seq"]
    mouse(100, y, "--jiggle", "2", "--click")  # "Many replies."
    _dialog(ctl, want=lambda d: d and d["seq"] != seq and len(d["replies"]) == 83)
    time.sleep(0.5)
    deadline = time.monotonic() + 3
    while not ctl("dialog_ui")["mouse"]:  # the new window's rows
        assert time.monotonic() < deadline
        time.sleep(0.1)
    assert ctl("dialog_ui")["highlight"] == 0
    mouse(100, y, "--jiggle", "2", "--wheel", "-3")  # three down
    assert ctl("dialog_ui")["highlight"] == 3
    assert ctl("state").get("client") == where  # no click reached the world
    _end(ctl)
    assert not server_heard_nwpad(game)  # nothing of nwpad's reached the server
