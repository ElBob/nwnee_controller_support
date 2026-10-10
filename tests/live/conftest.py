"""Live scenario tests run against the real game on the test box (plan §8.6).

They are skipped unless NWPAD_LIVE=1, so `pytest` is safe to run anywhere.
tools/remote.sh test sets NWPAD_LIVE=1 on the box.
"""
import json
import os
import re
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


# The library config the tests assume: explicit values (the defaults of plan §6.3,
# but a shorter cursor_rehide_ms), so Robert's own config.toml never affects a run. run_game.sh passes this file
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


X_ENV = {**os.environ, "DISPLAY": os.environ.get("NWPAD_DISPLAY", ":0")}


def _game_window_id():
    return subprocess.run(["xdotool", "search", "--name", "Neverwinter Nights: Enhanced"], env=X_ENV,
                          capture_output=True, text=True, check=True).stdout.split()[0]


def game_window():
    """The game window's X-screen position and size: (x, y, w, h) (xdotool)."""
    geo = dict(line.split("=", 1) for line in subprocess.run(
        ["xdotool", "getwindowgeometry", "--shell", _game_window_id()], env=X_ENV,
        capture_output=True, text=True, check=True).stdout.split())
    return int(geo["X"]), int(geo["Y"]), int(geo["WIDTH"]), int(geo["HEIGHT"])


def game_window_center():
    """X-screen coordinates of the middle of the game window."""
    x, y, w, h = game_window()
    return x + w // 2, y + h // 2


def xkey(key, action="key"):
    """Tap (action "key"), press ("keydown") or release ("keyup") a key through X
    (XTEST keyboard events reach the game), with the game window focused."""
    subprocess.run(["xdotool", "windowactivate", "--sync", _game_window_id()], env=X_ENV, capture_output=True)
    subprocess.run(["xdotool", action, key], env=X_ENV, check=True)


def mouse(x, y, *args):
    """tools/uinput_mouse.py at (x, y) in the game window, then a moment to settle.
    args as the tool takes them (--click, --wheel N, --jiggle N ...)."""
    wx, wy, _, _ = game_window()
    subprocess.run([os.path.join(ROOT, "tools", "uinput_mouse.py"), "--to", str(wx + x), str(wy + y),
                    "--settle", "0.4", *args], env=X_ENV, check=True, capture_output=True)
    time.sleep(0.3)


def server_heard(game):
    """The NUI window tokens the local server got a message for but doesn't know (it
    logs each), which is what a remote server would see."""
    log = open(os.path.join(game, "game.log"), errors="replace").read()
    return [int(t) for t in re.findall(r"window does not exist: (\d+)", log)]


def server_heard_nwpad(game):
    """As server_heard, nwpad's own tokens only (0x6e77xxxx): never expected."""
    return [t for t in server_heard(game) if t >> 16 == 0x6E77]


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
