"""Live scenario tests run against the real game on the test box (plan §8.6).

They are skipped unless NWPAD_LIVE=1, so `pytest` is safe to run anywhere.
tools/remote.sh test sets NWPAD_LIVE=1 on the box.
"""
import os
import signal
import subprocess
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def pytest_collection_modifyitems(config, items):
    if os.environ.get("NWPAD_LIVE") == "1":
        return
    skip = pytest.mark.skip(reason="live test: set NWPAD_LIVE=1 on the test box")
    for item in items:
        item.add_marker(skip)


@pytest.fixture(scope="session")
def game():
    """Launch the game once per session via tools/run_game.sh and stop it afterwards."""
    proc = subprocess.run([os.path.join(ROOT, "tools", "run_game.sh")],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        pytest.fail(f"run_game.sh failed ({proc.returncode}): {proc.stdout}{proc.stderr}")
    run_dir = proc.stdout.strip().splitlines()[-1]
    yield run_dir
    pid = int(open(os.path.join(run_dir, "pid")).read().strip())
    stop_game(pid)


def stop_game(pid, grace_s=15.0):
    """SIGTERM the game we started, wait for it to exit, SIGKILL after grace_s."""
    try:
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + grace_s
        while time.monotonic() < deadline:
            os.kill(pid, 0)
            time.sleep(0.25)
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
