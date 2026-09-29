# nwnee_controller_support (nwpad)

Analog controller support for the native Linux client of **Neverwinter Nights: Enhanced Edition**, injected at runtime.

The left stick moves your character in any direction relative to the camera, turning to face where it walks, the same way holding the mouse button on the ground does. The right stick turns and pitches the camera smoothly. Nothing is remapped to keystrokes. The library, `libnwpad.so`, loads into the game with `LD_PRELOAD` and drives the game's own movement and camera, and the game files on disk are never modified.

> **Status: pre-alpha.** The build, the core logic, and the SDL hook layer are in place. The game backend (camera and movement) is still a stub, so the library currently loads and reads your controller but doesn't control anything yet. See [`docs/plan.md`](docs/plan.md) for the roadmap.

## How it fits with Steam Input

Steam Input stays on and is the controller layer. It handles buttons, trackpads, gyro, and all stick tuning (deadzones, curves, inversion). The only requirement is that your layout outputs both sticks as **gamepad joysticks**, not as mouse or keys. nwpad reads the raw stick values and does the rest. Recommended layouts for the Xbox controller and the Steam Deck will ship in [`steam-input/`](steam-input/).

## Install (once functional)

1. Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNWPAD_DEBUG_SURFACES=OFF && cmake --build build`
2. Copy `build/libnwpad.so` to `~/.local/lib/nwpad/`.
3. Set the Steam launch options for NWN:EE to:
   ```
   LD_PRELOAD="$HOME/.local/lib/nwpad/libnwpad.so:$LD_PRELOAD" %command%
   ```
   Make sure the game runs as the native Linux build, not under Proton.
4. Optional: tweak `~/.config/nwpad/config.toml` (or point `NWPAD_CONFIG` at another file) (`camera_yaw_speed`, `camera_pitch_speed`, `run_threshold`, `run_hysteresis`, `mouse_idle_ms`, `strafe_window`, `hide_cursor`).

The game log shows a `[nwpad] version ... loaded` line when the library is active. Set `NWPAD_DISABLE=1` to load it inert. To uninstall, remove the launch option.

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
