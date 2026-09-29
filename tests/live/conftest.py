"""Live scenario tests run against the real game on the test box (plan §8.6).

They are skipped unless NWPAD_LIVE=1, so `pytest` is safe to run anywhere.
tools/remote.sh test sets NWPAD_LIVE=1 on the box.
"""
import os
import subprocess

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
    pid = open(os.path.join(run_dir, "pid")).read().strip()
    subprocess.run(["kill", pid], check=False)
