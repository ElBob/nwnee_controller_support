"""NPC conversations (dialog plan D1, re-notes F36): nwpad reads the NPC's line and
the replies of the open conversation, and answers it the way the number keys do."""
import time

import pytest

# The nearest NPC to the test character, Rules Enforcer D, has a conversation.
START = ('object pc=GetFirstPC(); object n=GetNearestCreature(CREATURE_TYPE_PLAYER_CHAR, PLAYER_CHAR_NOT_PC, pc, 1);'
         ' AssignCommand(n, ClearAllActions()); AssignCommand(n, ActionStartConversation(pc, "", FALSE, FALSE));')


@pytest.fixture(autouse=True)
def npc_home(ctl):
    """Starting a conversation walks the NPC over to the player, into the movement
    tests' way: put it back where it started afterwards."""
    ctl("script_chunk", code='object n=GetNearestCreature(CREATURE_TYPE_PLAYER_CHAR, PLAYER_CHAR_NOT_PC, GetFirstPC(), 1);'
                             ' SetLocalObject(GetModule(), "nwpad_npc", n); SetLocalLocation(GetModule(), "nwpad_npc_home", GetLocation(n));')
    yield
    if ctl("dialog")["dialog"]:
        ctl("dialog_select", end=1)
    ctl("script_chunk", code='object n=GetLocalObject(GetModule(), "nwpad_npc"); AssignCommand(n, ClearAllActions(TRUE));'
                             ' AssignCommand(n, JumpToLocation(GetLocalLocation(GetModule(), "nwpad_npc_home")));')
    time.sleep(1.0)


def _dialog(ctl, timeout=5.0, want=lambda d: d is not None):
    deadline = time.monotonic() + timeout
    while True:
        d = ctl("dialog")["dialog"]
        if want(d):
            return d
        assert time.monotonic() < deadline, f"dialog never reached the expected state: {d}"
        time.sleep(0.2)


def test_read_and_answer(game, ctl):
    if ctl("dialog")["dialog"]:
        ctl("dialog_select", end=1)
        _dialog(ctl, want=lambda d: d is None)
    ctl("script_chunk", code=START)
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


def _key(key):
    import os
    import subprocess
    env = dict(os.environ, DISPLAY=os.environ.get("NWPAD_DISPLAY", ":0"))
    win = subprocess.run(["xdotool", "search", "--name", "Neverwinter Nights: Enhanced"], env=env,
                         capture_output=True, text=True).stdout.split()
    if win:
        subprocess.run(["xdotool", "windowactivate", "--sync", win[0]], env=env, capture_output=True)
    subprocess.run(["xdotool", "key", key], env=env, check=True)
    time.sleep(0.4)


def test_window_keys(game, ctl):
    """nwpad's conversation window (D2): the arrow keys move the highlight, Enter
    answers, Escape ends the conversation; the game sees none of them meanwhile."""
    if ctl("dialog")["dialog"]:
        ctl("dialog_select", end=1)
        _dialog(ctl, want=lambda d: d is None)
    filtered0 = ctl("state")["events"]["filtered"]
    _key("Down")  # no conversation: the game's key
    assert ctl("state")["events"]["filtered"] == filtered0
    ctl("script_chunk", code=START)
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
    d = _dialog(ctl, want=lambda d: d and d["seq"] != seq and d["line"].startswith("The Contest Of Champions is"))
    assert ctl("dialog_ui")["open"]
    _key("Escape")  # ends it
    _dialog(ctl, want=lambda d: d is None)
    assert not ctl("dialog_ui")["open"]
    log = open(__import__("os").path.join(game, "game.log"), errors="replace").read()
    assert "window does not exist: 18532" not in log  # nwpad's NUI tokens never reach the server


def test_test_conversation(game, ctl, tmp_path):
    """nwpad's own test conversation (tools/make_test_dlg.py), shaped like the
    official campaigns' extremes: markup (the client turns <StartCheck> etc. into
    colour codes; nwpad's window strips them), an 83-reply list that scrolls with
    the highlight, a ~1000-character line and a long reply."""
    import os
    import subprocess
    from conftest import ROOT
    dlg = str(tmp_path / "nwpadtest.dlg")
    subprocess.run([os.path.join(ROOT, "tools", "make_test_dlg.py"), dlg], check=True)
    assert ctl("resource_publish", src=dlg, name="nwpadtest.dlg")["ok"]
    if ctl("dialog")["dialog"]:
        ctl("dialog_select", end=1)
        _dialog(ctl, want=lambda d: d is None)
    ctl("script_chunk", code=START.replace('ActionStartConversation(pc, ""', 'ActionStartConversation(pc, "nwpadtest"'))
    d = _dialog(ctl, want=lambda d: d and len(d["replies"]) == 4)
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
    ctl("dialog_select", end=1)
    _dialog(ctl, want=lambda d: d is None)
    log = open(os.path.join(game, "game.log"), errors="replace").read()
    assert "window does not exist: 18532" not in log
