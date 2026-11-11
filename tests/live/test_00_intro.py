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


class _Intro:
    """A game started with no module, in its start-up intro."""

    def __init__(self, virtual_pad):
        if virtual_pad is None:
            pytest.skip("no virtual pad (python-evdev or /dev/uinput missing)")
        self.pad = virtual_pad
        state = os.environ.get("NWPAD_STATE", os.path.expanduser("~/.nwpad"))
        os.makedirs(state, exist_ok=True)
        with open(os.path.join(state, "config.toml"), "w") as f:
            f.write(TEST_CONFIG)
        proc = subprocess.run([os.path.join(ROOT, "tools", "run_game.sh")], capture_output=True, text=True)
        if proc.returncode != 0:
            pytest.fail(f"run_game.sh failed ({proc.returncode}): {proc.stdout}{proc.stderr}")
        self.pid = int(open(os.path.join(proc.stdout.strip().splitlines()[-1], "pid")).read().strip())
        conftest.GAME_PID = self.pid
        self.ctl = Ctl()
        deadline = time.monotonic() + 20
        while self.stage() < 1:  # the first movie is on
            assert time.monotonic() < deadline, "the intro never started"
            time.sleep(0.2)
        focus_game()

    def stage(self):
        return self.ctl("state")["events"]["intro"]["stage"]

    def stop(self):
        self.ctl.close()
        stop_game(self.pid)
        conftest.GAME_PID = None


def test_pad_button_skips_the_intro(virtual_pad):
    intro = _Intro(virtual_pad)
    try:
        started = time.monotonic()
        # The movies run ~25 s unskipped; buttons pressed a few times a second get through
        # all four and the legal screen in a few seconds.
        while intro.stage() != -1:
            assert time.monotonic() - started < 15, f"still in the intro at stage {intro.stage()}"
            virtual_pad.press("a")
            time.sleep(0.5)
        assert intro.ctl("state")["events"]["pad_buttons"] >= 2
    finally:
        intro.stop()


def test_pad_button_passes_the_legal_screen(virtual_pad):
    """The legal screen holds for several seconds once it has faded in (stage 7) unless a
    button is pressed: a button pressed only then still gets past it."""
    intro = _Intro(virtual_pad)
    try:
        started = time.monotonic()
        while intro.stage() < 5:  # through the movies
            assert time.monotonic() - started < 30, f"stuck at stage {intro.stage()}"
            virtual_pad.press("a")
            time.sleep(0.5)
        deadline = time.monotonic() + 10
        while intro.stage() != 7:  # faded in, holding; no button pressed meanwhile
            assert intro.stage() != -1 and time.monotonic() < deadline, f"stage {intro.stage()}"
            time.sleep(0.1)
        time.sleep(0.5)
        assert intro.stage() == 7
        virtual_pad.press("a")
        deadline = time.monotonic() + 2.5
        while intro.stage() != -1:
            assert time.monotonic() < deadline, f"the legal screen held at stage {intro.stage()}"
            time.sleep(0.1)
    finally:
        intro.stop()
