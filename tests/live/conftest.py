"""Live scenario tests run against the real game on the test box (plan §8.6).

They are skipped unless NWPAD_LIVE=1, so `pytest` is safe to run anywhere.
tools/remote.sh test sets NWPAD_LIVE=1 on the box.
"""
import json
import os
import signal
import socket
import subprocess
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# A stock module is enough until nwpad_test.mod exists (re-notes F13).
TEST_MODULE = os.environ.get("NWPAD_TEST_MODULE", "Contest Of Champions 0492")


def pytest_collection_modifyitems(config, items):
    if os.environ.get("NWPAD_LIVE") == "1":
        return
    skip = pytest.mark.skip(reason="live test: set NWPAD_LIVE=1 on the test box")
    for item in items:
        item.add_marker(skip)


class Ctl:
    """One persistent connection to the control socket (plan §8.3)."""

    def __init__(self, timeout=5.0):
        run = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(os.path.join(run, "nwpad.sock"))
        self.buf = b""

    def __call__(self, cmd, **args):
        self.sock.sendall((json.dumps({"cmd": cmd, **args}) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("control socket closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        reply = json.loads(line)
        assert reply.get("ok"), f"{cmd} failed: {reply}"
        return reply

    def close(self):
        self.sock.close()


# The library config the tests assume: explicit defaults (plan §6.3), so Robert's
# own ~/.config/nwpad/config.toml never affects a run. run_game.sh passes this file
# through NWPAD_CONFIG.
TEST_CONFIG = """# written by tests/live/conftest.py
camera_yaw_speed = 180
camera_pitch_speed = 90
run_threshold = 0.7875
run_hysteresis = 0.125
mouse_idle_ms = 300
strafe_window = 10
strafe_exit_ms = 150
cursor_rehide_ms = 1000
"""
TEST_CONFIG_KEYS = 8


@pytest.fixture(scope="session")
def game():
    """Launch the game into TEST_MODULE once per session and stop it afterwards."""
    state = os.environ.get("NWPAD_STATE", os.path.expanduser("~/.nwpad"))
    os.makedirs(state, exist_ok=True)
    with open(os.path.join(state, "config.toml"), "w") as f:
        f.write(TEST_CONFIG)
    proc = subprocess.run([os.path.join(ROOT, "tools", "run_game.sh"), "--", "+TestNewModule", TEST_MODULE],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        pytest.fail(f"run_game.sh failed ({proc.returncode}): {proc.stdout}{proc.stderr}")
    run_dir = proc.stdout.strip().splitlines()[-1]
    pid = int(open(os.path.join(run_dir, "pid")).read().strip())
    try:
        ctl = Ctl()
        deadline = time.monotonic() + 60
        while not ctl("state")["in_game"]:
            if time.monotonic() > deadline:
                pytest.fail(f"module {TEST_MODULE!r} not loaded after 60 s; see {run_dir}/game.log")
            time.sleep(0.5)
        ctl.close()
        yield run_dir
    finally:
        stop_game(pid)


@pytest.fixture
def ctl(game):
    c = Ctl()
    yield c
    c("release")
    c.close()


def game_window_center():
    """X-screen coordinates of the middle of the game window (xdotool)."""
    env = {**os.environ, "DISPLAY": os.environ.get("NWPAD_DISPLAY", ":0")}
    wid = subprocess.run(["xdotool", "search", "--name", "Neverwinter Nights"], env=env,
                         capture_output=True, text=True, check=True).stdout.split()[0]
    geo = dict(line.split("=", 1) for line in subprocess.run(
        ["xdotool", "getwindowgeometry", "--shell", wid], env=env,
        capture_output=True, text=True, check=True).stdout.split())
    return int(geo["X"]) + int(geo["WIDTH"]) // 2, int(geo["Y"]) + int(geo["HEIGHT"]) // 2


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
