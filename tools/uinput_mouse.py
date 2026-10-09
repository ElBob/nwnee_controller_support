#!/usr/bin/env python3
"""Virtual absolute mouse for arbitration tests (plan §8.4). Runs on the test box.

XWayland ignores XTEST pointer motion under Plasma 6, so xdotool can't produce
mouse motion for the game (re-notes: test box environment). This creates a
uinput device shaped like a QEMU USB tablet (absolute X/Y plus buttons), which
KWin treats as a real pointer.

  tools/uinput_mouse.py --to X Y [--jiggle N] [--interval S]
  tools/uinput_mouse.py --to X Y --drag DX DY [--hold S]

X Y are X-screen pixel coordinates (as xdotool reports them). --jiggle moves
the pointer back and forth N times around the target, one event per interval.
--drag presses the left button at X Y, moves to X+DX Y+DY, holds for S
seconds, and releases (NWN's click-and-drag movement). --click clicks the left
button at X Y (driving menus in the settings checks); --wheel N turns the wheel.
"""
import argparse
import os
import sys
import time

try:
    from evdev import AbsInfo, UInput, ecodes as e
except ImportError:
    sys.exit("uinput_mouse needs python-evdev")

ABS_MAX = 32767


def screen_size():
    """Size of the X screen in pixels (xdotool getdisplaygeometry)."""
    import subprocess
    out = subprocess.run(["xdotool", "getdisplaygeometry"], capture_output=True, text=True,
                         env={**os.environ, "DISPLAY": os.environ.get("DISPLAY", ":0")})
    w, h = out.stdout.split()
    return int(w), int(h)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--to", nargs=2, type=int, metavar=("X", "Y"), required=True)
    ap.add_argument("--jiggle", type=int, default=0)
    ap.add_argument("--interval", type=float, default=0.02)
    ap.add_argument("--drag", nargs=2, type=int, metavar=("DX", "DY"))
    ap.add_argument("--hold", type=float, default=1.0, help="drag: seconds to hold at the end point")
    ap.add_argument("--click", action="store_true", help="left-click at X Y")
    ap.add_argument("--wheel", type=int, default=0, help="wheel notches at X Y (positive: up)")
    ap.add_argument("--settle", type=float, default=1.0, help="wait for the compositor to add the device")
    a = ap.parse_args()

    sw, sh = screen_size()
    caps = {
        e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE],
        e.EV_REL: [e.REL_WHEEL],
        e.EV_ABS: [(e.ABS_X, AbsInfo(0, 0, ABS_MAX, 0, 0, 0)),
                   (e.ABS_Y, AbsInfo(0, 0, ABS_MAX, 0, 0, 0))],
    }
    with UInput(caps, name="nwpad virtual tablet", vendor=0x0627, product=0x0001) as ui:
        time.sleep(a.settle)

        def move(x, y):
            ui.write(e.EV_ABS, e.ABS_X, max(0, min(ABS_MAX, round(x * ABS_MAX / (sw - 1)))))
            ui.write(e.EV_ABS, e.ABS_Y, max(0, min(ABS_MAX, round(y * ABS_MAX / (sh - 1)))))
            ui.syn()

        x, y = a.to
        move(x, y)
        for i in range(a.jiggle):
            time.sleep(a.interval)
            move(x + (8 if i % 2 == 0 else 0), y)
        if a.click:
            time.sleep(0.15)
            ui.write(e.EV_KEY, e.BTN_LEFT, 1)
            ui.syn()
            time.sleep(0.08)
            ui.write(e.EV_KEY, e.BTN_LEFT, 0)
            ui.syn()
        for _ in range(abs(a.wheel)):
            time.sleep(0.1)
            ui.write(e.EV_REL, e.REL_WHEEL, 1 if a.wheel > 0 else -1)
            ui.syn()
        if a.drag:
            time.sleep(0.1)
            ui.write(e.EV_KEY, e.BTN_LEFT, 1)
            ui.syn()
            steps = 10
            for i in range(1, steps + 1):
                time.sleep(a.interval)
                move(x + a.drag[0] * i / steps, y + a.drag[1] * i / steps)
            time.sleep(a.hold)
            ui.write(e.EV_KEY, e.BTN_LEFT, 0)
            ui.syn()
        time.sleep(0.1)  # let the last events drain before the device goes away
    return 0


if __name__ == "__main__":
    sys.exit(main())
