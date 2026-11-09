#!/usr/bin/env python3
"""A virtual Xbox 360 pad for tests, through uinput (runs on the test box).

  tools/uinput_pad.py [--hold SECONDS] [--settle SECONDS] [--steps SPEC]
  tools/uinput_pad.py --stdin           # keep the pad; play each line of stdin as a SPEC

Creates a pad SDL's game controller database knows (Microsoft Xbox 360 pad, vendor
045e product 028e, with the usual evdev button and axis codes), waits for the
compositor and the game to find it, then plays SPEC and keeps the device for
--hold seconds. SPEC is comma-separated steps: a button name (a b x y lb rb back
start ls rs) taps it, "+name" presses it, "-name" releases it, "wait:SECONDS" waits.
With --stdin the pad stays until stdin closes (a line "ready" is printed once it exists).
The device disappears when the script ends.
"""
import argparse
import sys
import time

try:
    from evdev import AbsInfo, UInput, ecodes as e
except ImportError:
    sys.exit("uinput_pad needs python-evdev")

BUTTONS = {
    "a": e.BTN_SOUTH, "b": e.BTN_EAST, "x": e.BTN_NORTH, "y": e.BTN_WEST,
    "lb": e.BTN_TL, "rb": e.BTN_TR, "back": e.BTN_SELECT, "start": e.BTN_START,
    "ls": e.BTN_THUMBL, "rs": e.BTN_THUMBR,
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--settle", type=float, default=2.0, help="seconds before the first step")
    ap.add_argument("--hold", type=float, default=0.5, help="seconds to keep the pad after the last step")
    ap.add_argument("--steps", default="")
    ap.add_argument("--stdin", action="store_true")
    a = ap.parse_args()
    stick = AbsInfo(0, -32768, 32767, 16, 128, 0)
    trigger = AbsInfo(0, 0, 255, 0, 0, 0)
    caps = {
        e.EV_KEY: list(BUTTONS.values()),
        e.EV_ABS: [(e.ABS_X, stick), (e.ABS_Y, stick), (e.ABS_Z, trigger),
                   (e.ABS_RX, stick), (e.ABS_RY, stick), (e.ABS_RZ, trigger),
                   (e.ABS_HAT0X, AbsInfo(0, -1, 1, 0, 0, 0)), (e.ABS_HAT0Y, AbsInfo(0, -1, 1, 0, 0, 0))],
    }
    with UInput(caps, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E, version=0x0110) as ui:
        def play(spec):
            for step in filter(None, (s.strip() for s in spec.split(","))):
                if step.startswith("wait:"):
                    time.sleep(float(step[5:]))
                    continue
                name = step.lstrip("+-")
                if name not in BUTTONS:
                    sys.exit(f"unknown button {name!r}")
                if step[0] != "-":
                    ui.write(e.EV_KEY, BUTTONS[name], 1)
                    ui.syn()
                if step[0] != "+":
                    time.sleep(0.06)
                    ui.write(e.EV_KEY, BUTTONS[name], 0)
                    ui.syn()
                time.sleep(0.12)

        time.sleep(a.settle)
        if a.stdin:
            print("ready", flush=True)
            for line in sys.stdin:
                play(line)
                print("done", flush=True)
            return 0
        play(a.steps)
        time.sleep(a.hold)
    return 0


if __name__ == "__main__":
    sys.exit(main())
