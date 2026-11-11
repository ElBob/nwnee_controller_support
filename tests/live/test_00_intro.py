"""The start-up intro movies (re-notes F43): any controller button counts as the key press
that skips a movie and passes the "press any key" legal screen.

This starts its own game, with no module, so it must run before the session's `game`
fixture launches the shared one: the file name sorts first on purpose."""
import os
import subprocess
import time

import pytest

import conftest
from conftest import Ctl, ROOT, TEST_CONFIG, focus_game, stop_game


def test_pad_button_skips_the_intro(virtual_pad):
    if virtual_pad is None:
        pytest.skip("no virtual pad (python-evdev or /dev/uinput missing)")
    state = os.environ.get("NWPAD_STATE", os.path.expanduser("~/.nwpad"))
    os.makedirs(state, exist_ok=True)
    with open(os.path.join(state, "config.toml"), "w") as f:
        f.write(TEST_CONFIG)
    proc = subprocess.run([os.path.join(ROOT, "tools", "run_game.sh")], capture_output=True, text=True)
    if proc.returncode != 0:
        pytest.fail(f"run_game.sh failed ({proc.returncode}): {proc.stdout}{proc.stderr}")
    pid = int(open(os.path.join(proc.stdout.strip().splitlines()[-1], "pid")).read().strip())
    conftest.GAME_PID = pid
    try:
        ctl = Ctl()

        def stage():
            return ctl("state")["events"]["intro"]["stage"]

        deadline = time.monotonic() + 20
        while stage() < 1:  # the first movie is on
            assert time.monotonic() < deadline, "the intro never started"
            time.sleep(0.2)
        focus_game()
        started = time.monotonic()
        # The movies run ~25 s unskipped; buttons pressed a few times a second get through
        # all four and the legal screen in a few seconds.
        while stage() != -1:
            assert time.monotonic() - started < 15, f"still in the intro at stage {stage()}"
            virtual_pad.press("a")
            time.sleep(0.5)
        assert ctl("state")["events"]["pad_buttons"] >= 2
        ctl.close()
    finally:
        stop_game(pid)
        conftest.GAME_PID = None
