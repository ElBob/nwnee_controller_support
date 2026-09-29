# nwnee_controller_support (nwpad)

Analog controller support for the native Linux client of **Neverwinter Nights: Enhanced Edition**, injected at runtime.

The left stick moves your character in any direction relative to the camera, turning to face where it walks the way holding the mouse button on the ground does, or strafing and backpedaling when pushed straight sideways or back. The right stick turns and pitches the camera smoothly. Nothing is remapped to keystrokes. The library, `libnwpad.so`, loads into the game with `LD_PRELOAD` and drives the game's own movement and camera, and the game files on disk are never modified.

> **Status: beta.** Camera and movement work in the native Linux client (build 8193.37-17). Steam Input layouts and Steam Deck validation are still to come. See [`docs/plan.md`](docs/plan.md).

## How it fits with Steam Input

Steam Input stays on and is the controller layer. It handles buttons, trackpads, gyro, and all stick tuning (deadzones, curves, inversion). The only requirement is that your layout outputs both sticks as **gamepad joysticks**, not as mouse or keys. nwpad reads the raw stick values and does the rest. Recommended layouts for the Xbox controller and the Steam Deck will ship in [`steam-input/`](steam-input/).

## Install

1. Build and install the release library (needs CMake, a C compiler, and Python 3 with PyYAML):
   ```
   tools/install.sh
   ```
   This puts `libnwpad.so` in `~/.local/lib/nwpad/`. `tools/install.sh --uninstall` removes it.
2. In Steam, set NWN:EE's launch options (Properties → General → Launch Options) to:
   ```
   LD_PRELOAD="$HOME/.local/lib/nwpad/libnwpad.so:$LD_PRELOAD" %command%
   ```
   The game must run as the native Linux build, not under Proton.
3. Steam Input: use a layout that outputs both sticks as **gamepad joysticks**, not mouse or keys (see [`steam-input/`](steam-input/)).

The game log shows `[nwpad] version ... loaded` when the library is active. Set `NWPAD_DISABLE=1` to load it inert. To uninstall, clear the launch option and run `tools/install.sh --uninstall`.

## Controls

- **Left stick:** move. Most directions walk or run that way and turn to face it, like holding the mouse button on the ground. Within about 10° of directly right, back, or left of where the character faces, it strafes or backpedals instead, like E, S, or Q. Light pressure walks; past 0.85 it runs (back to walking below 0.725). With the game's Always Run on, any push runs.
- **Right stick:** turn and tilt the camera. Moving the mouse takes over the camera until it has been still for 300 ms.
- The mouse cursor hides while the sticks are in use and comes back on the next mouse movement.

## Settings

`~/.config/nwpad/config.toml` (or the file named by `NWPAD_CONFIG`); every key is optional:

| Key | Default | Meaning |
|---|---|---|
| `camera_yaw_speed` | 180 | Camera turn speed at full deflection, °/s |
| `camera_pitch_speed` | 90 | Camera tilt speed at full deflection, °/s |
| `run_threshold` | 0.7875 | Centre of the walk/run band |
| `run_hysteresis` | 0.125 | Width of the walk/run band (so: run above 0.85, walk below 0.725) |
| `mouse_idle_ms` | 300 | How long the mouse must be still before the stick gets the camera back |
| `strafe_window` | 10 | Half-width of the strafe/backpedal windows, ° |
| `hide_cursor` | 1 | Hide the cursor while the sticks are in use |

Deadzones and response curves belong in Steam Input. The library's own safety deadzone is fixed at 0.15.

## Multiplayer note

nwpad is always active, including on remote servers. Some persistent worlds prohibit client modifications. Check a server's rules before using it there; that responsibility is yours.

## Repository layout

```
src/core/            pure logic (deadzone, heading, walk/run, send-rate, camera, arbitration, config)
src/hook/            LD_PRELOAD entry point, SDL interposition, game backend
signatures/ee.yaml   byte signatures for game functions, each backed by docs/re-notes.md
tests/unit/          C unit tests and the preload smoke test (run anywhere)
tests/live/          pytest scenarios against the running game (test box only)
tools/               remote execution, launch/lock, sigcheck, Ghidra and Frida helpers
testmod/             test module source (built locally)
steam-input/         recommended Steam Input layouts
docs/plan.md         project plan
docs/re-notes.md     reverse-engineering findings with evidence
CLAUDE.md            rules for the Claude Code agent loop
```

## Development

Building needs CMake, a C11 compiler, and Python 3 with PyYAML (the signature table is generated from `signatures/ee.yaml`).

```
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
tools/install_hooks.sh   # pre-commit check that blocks game-derived files
```

## License

MIT. Neverwinter Nights is a trademark of its respective owners. This project ships no game code or assets.
