"""M0 smoke test: the game starts with the library loaded and signatures resolve."""
import json
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def test_library_loaded(game):
    log = open(os.path.join(game, "game.log"), errors="replace").read()
    assert "[nwpad] version" in log


def test_sigcheck_clean():
    r = subprocess.run(["python3", os.path.join(ROOT, "tools", "sigcheck")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def nwpadctl(*args):
    r = subprocess.run([os.path.join(ROOT, "tools", "nwpadctl"), *args],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return json.loads(r.stdout)


def test_ping_frames_advance(game):
    first = nwpadctl("ping")["frame"]
    second = nwpadctl("ping")["frame"]
    assert second > first


def test_status(game):
    st = nwpadctl("status")
    assert st["hooks"] is True
