"""NPC conversations (dialog plan D1, re-notes F36): nwpad reads the NPC's line and
the replies of the open conversation, and answers it the way the number keys do."""
import time

# The nearest NPC to the test character, Rules Enforcer D, has a conversation.
START = ('object pc=GetFirstPC(); object n=GetNearestCreature(CREATURE_TYPE_PLAYER_CHAR, PLAYER_CHAR_NOT_PC, pc, 1);'
         ' AssignCommand(n, ClearAllActions()); AssignCommand(n, ActionStartConversation(pc, "", FALSE, FALSE));')


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
