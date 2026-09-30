# nwnee_controller_support (nwpad)

Analog controller support for the native Linux client of **Neverwinter Nights: Enhanced Edition**, injected at runtime.

The left stick moves your character in any direction relative to the camera, turning to face where it walks the way holding the mouse button on the ground does, or strafing and backpedaling when pushed straight sideways or back. The right stick turns and pitches the camera smoothly. Nothing is remapped to keystrokes. The library, `libnwpad.so`, loads into the game with `LD_PRELOAD` and drives the game's own movement and camera, and the game files on disk are never modified.

> **Status: beta.** Camera and movement work in the native Linux client (build 8193.37-17). Steam Input layouts and Steam Deck validation are still to come. See [`docs/plan.md`](docs/plan.md).

## How it fits with Steam Input

Steam Input stays on and is the controller layer. It handles buttons, trackpads, gyro, and all stick tuning (deadzones, curves, inversion). The only requirement is that your layout outputs both sticks as **gamepad joysticks**, not as mouse or keys. nwpad reads the raw stick values and does the rest. Recommended layouts for the Xbox controller and the Steam Deck will ship in [`steam-input/`](steam-input/).

## Install

1. Get the release library into `~/.local/lib/nwpad/libnwpad.so`, either way:
   - **Build it** (needs CMake, a C compiler, and Python 3 with PyYAML): `tools/install.sh`. `tools/install.sh --uninstall` removes it.
   - **Download it** (Steam Deck, or anywhere without build tools): the `libnwpad-release` artifact of the latest CI run on GitHub, copied to that path. The library runs on any x86-64 Linux with glibc 2.17 or newer.
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

In the game: **Options > Game Options > Input**, in the Camera group:

- **Controller Support**: on/off. Off, nwpad stays loaded but idle (no stick input, no cursor hiding).
- **Controller Hides Cursor**
- **Controller Camera Turn Speed** / **Controller Camera Tilt Speed**: °/s at full deflection.
- **Controller Run Point**: where walking becomes running.

They work like the game's own settings (the Options search finds them with "controller") and apply as
you change them. Save keeps them, Cancel undoes them.

nwpad keeps its settings in the game's own `settings.tml`, in an `[nwpad]` section
(in your profile: `~/.local/share/Neverwinter Nights/settings.tml`, or the
`-userdirectory` you launch with). The section is added the first time the game
runs with nwpad; the game keeps it when it saves its own settings. The last three keys
below are only in the file; edit it while the game is closed.

```toml
[nwpad]
	enabled = true
	hide-cursor = true
	mouse-idle-ms = 300
	[nwpad.camera]
		tilt-speed = 90.0
		turn-speed = 180.0
	[nwpad.movement]
		run-point = 0.7875
		strafe-exit-ms = 150
		strafe-window = 10.0
```

| Key | Default | Range | Meaning |
|---|---|---|---|
| `enabled` | true | | Controller support on/off. Off, the library stays loaded but idle: no stick input, no cursor hiding |
| `hide-cursor` | true | | Hide the cursor while the sticks are in use |
| `mouse-idle-ms` | 300 | 100–1000 | How long the mouse must be still before the stick gets the camera back |
| `camera.turn-speed` | 180 | 60–360 | Camera turn speed at full deflection, °/s |
| `camera.tilt-speed` | 90 | 30–180 | Camera tilt speed at full deflection, °/s |
| `movement.run-point` | 0.7875 | 0.5–0.95 | Centre of the walk/run band; the band is 0.125 wide (run above 0.85, walk below 0.725 at the default) |
| `movement.strafe-window` | 10 | 0–30 | Half-width of the strafe/backpedal windows, ° |
| `movement.strafe-exit-ms` | 150 | 0–1000 | How long the stick must leave a strafe window before it becomes a drag |

Out-of-range values are clamped. The first time nwpad adds the section it backs
`settings.tml` up once, as `settings.tml.bak-nwpad`.

**Older `config.toml`.** If you used `~/.config/nwpad/config.toml`, its values are
imported into the new section on that first run; after that `settings.tml` wins and
`config.toml` is only used if `settings.tml` can't be read. `NWPAD_CONFIG=<file>`
makes nwpad use that `config.toml` only (for development; the keys are the old
names: `camera_yaw_speed`, `camera_pitch_speed`, `run_threshold`, `run_hysteresis`,
`mouse_idle_ms`, `strafe_window`, `strafe_exit_ms`, `hide_cursor`).

**Uninstalling.** Running the game without nwpad is safe with the `[nwpad]` section
present: the game ignores it and keeps it (re-notes F29). Delete it by hand if you like.

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

Build options: `NWPAD_DEBUG_SURFACES` (control socket and test commands; off for releases) and, on Linux, `NWPAD_XWAYLAND_EDGE_FIX` (default on: lets right-edge camera turning work under scaled XWayland desktops, see `docs/re-notes.md` F27).

```
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
tools/install_hooks.sh   # pre-commit check that blocks game-derived files
```

## License

MIT. Neverwinter Nights is a trademark of its respective owners. This project ships no game code or assets.
