"""Soak and overhead (plan §8.6).

test_soak: 10 minutes of randomized stick input; opt-in with NWPAD_SOAK=1.
test_overhead: the library's per-frame cost over the whole session so far. It
runs last (file order), so it covers every other live test.
"""
import collections
import os
import random
import time

import pytest

from test_movement import reset

SOAK_S = float(os.environ.get("NWPAD_SOAK_SECONDS", "600"))
BUDGET_P99_US = 100  # plan §7: under 0.1 ms per frame


@pytest.mark.skipif(os.environ.get("NWPAD_SOAK") != "1", reason="10-minute soak: set NWPAD_SOAK=1")
def test_soak(ctl, game):
    rng = random.Random(0x5eed)
    pid = int(open(os.path.join(game, "pid")).read())
    end = time.monotonic() + SOAK_S
    next_reset = 0.0
    recent = collections.deque(maxlen=50)  # replayable input history for failures
    while time.monotonic() < end:
        if time.monotonic() >= next_reset:
            try:
                reset(ctl)  # keep away from walls and NPC clusters
            except BaseException:
                diagnose_desync(ctl, recent)
                raise
            next_reset = time.monotonic() + 30
        r = rng.random()
        if r < 0.15:
            ctl("release")
            recent.append((time.monotonic(), "release"))
        else:
            stick = {"lx": rng.uniform(-1, 1), "ly": rng.uniform(-1, 1)}
            if rng.random() < 0.4:
                stick.update(rx=rng.uniform(-1, 1), ry=rng.uniform(-1, 1))
            ctl("stick", **stick)
            recent.append((time.monotonic(), stick))
        time.sleep(rng.uniform(0.05, 1.2))
        os.kill(pid, 0)  # the game is still alive
    ctl("release")
    time.sleep(1.0)
    s = ctl("state")
    a = s["creature"]
    time.sleep(1.0)
    b = ctl("state")["creature"]
    assert s["move_style"] == "rest", s
    assert abs(a["x"] - b["x"]) < 0.01 and abs(a["y"] - b["y"]) < 0.01, "still moving after release"
    sends = s["sends"]
    print(f"soak: {sends['moves']} moves, {sends['stops']} stops, min gap {sends['min_gap_ms']} ms, frame cost {s['frame_cost_us']}")
    assert sends["moves"] > 100 and sends["stops"] > 10, sends
    assert sends["min_gap_ms"] >= sends["cap_ms"], sends


def diagnose_desync(ctl, recent):
    """Record why a reset failed: input history, input-mode and key fields, and
    whether the client resyncs by itself or after another jump."""
    import json
    import struct
    from conftest import ROOT
    out = os.path.join(ROOT, "artifacts", f"desync-{int(time.time())}")
    os.makedirs(out, exist_ok=True)
    t0 = recent[-1][0] if recent else time.monotonic()
    log = {"inputs": [(round(t - t0, 3), i) for t, i in recent]}
    mode = bytes.fromhex(ctl("read", base="client_internal", offset=0x140, len=1)["hex"])[0]
    keys = struct.unpack("<8I", bytes.fromhex(ctl("read", base="client_internal", offset=0x1b0, len=0x20)["hex"]))
    log["input_mode"], log["drive_keys_1b0"] = mode, keys
    log["state_at_failure"] = ctl("state")
    time.sleep(30)
    log["state_after_30s"] = ctl("state")
    try:
        reset(ctl)
        log["second_reset"] = "ok"
    except BaseException as e:  # noqa: BLE001 - diagnostics only
        log["second_reset"] = f"failed: {e}"
    log["state_final"] = ctl("state")
    with open(os.path.join(out, "desync.json"), "w") as f:
        json.dump(log, f, indent=1, default=str)
    print(f"desync diagnostics: {out}")


def test_overhead(ctl):
    cost = ctl("state")["frame_cost_us"]
    print(f"frame cost: {cost}")
    assert cost["frames"] > 1000, cost
    assert cost["p99"] <= BUDGET_P99_US, cost
