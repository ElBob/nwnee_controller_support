# nwpad — Runtime Analog Controller Support for Neverwinter Nights: EE (Linux)

Status: **v1.0**, approved for execution. Changes after this point go through the decision log (§13).

## 1. Goal and scope

Add true analog stick control of character movement and camera to the native Linux client of NWN:EE (`nwmain-linux`) at runtime, without modifying the game binary on disk. The mechanism is an `LD_PRELOAD` shared library, `libnwpad.so`.

**In scope:**

- **Movement from the left stick.** Full 360° direction relative to the camera, with free facing: the character can strafe and backpedal while facing forward. Stick magnitude selects walk or run.
- **Camera from the right stick.** Continuous, framerate-independent yaw and pitch.

**Out of scope:** zoom (the mouse wheel stays the zoom control), menu and UI navigation, and all button, trackpad, and gyro mapping. Steam Input handles those (§2).

**Targets:** desktop Linux with an Xbox controller, and the Steam Deck. Both are required for v1.

**Definition of "true analog":**

- Movement direction is continuous and never quantized to 8 directions.
- Camera rotation speed is linear in raw stick deflection. Response shaping is done in Steam Input.
- Movement speed is discrete (walk or run), because NWN animates at fixed movement rates. Continuous speed is out of scope for v1 (§12).

**Multiplayer:** single-player is the primary target. If movement ends up client-only (Path A, §5), it works on any server as a side effect. If it needs Path B, v1 is single-player and self-hosted only, and multiplayer is revisited after v1. The library is always active on any server. The README warns that some persistent worlds prohibit client modifications, and complying with a server's rules is the user's responsibility.

## 2. Role of Steam Input

Steam Input is the controller layer. The library does not replace it, and the project does not remap sticks to keystrokes.

- **Steam Input owns** buttons, trackpads, gyro, action sets, keystroke and mouse bindings, and all stick tuning: deadzones, response curves, inversion, and outer-ring behavior.
- **The Steam Input layout must output both sticks as gamepad sticks** ("Joystick Move" and "Joystick Camera"), not as mouse or keys. Otherwise the library sees no analog input.
- **The library reads raw stick values** through the game's SDL GameController API and applies only a small fixed safety deadzone (§6.3) to absorb drift.
- **The project ships recommended Steam Input layouts** for the Xbox controller and the Steam Deck, published as community layouts and documented in the README (M5).

## 3. Behavior specification

**Movement.** The left stick direction, rotated by the current camera yaw, gives the world-space movement direction. While the stick is deflected, the character faces the camera's forward direction and moves along the stick direction. Pushing the stick sideways strafes, and pulling it back backpedals. When the stick returns to the safety deadzone, the character stops and keeps its current facing. This follows the free-facing requirement. The facing rule ("face camera forward while moving") is the v1 default and may be revisited after feel testing.

**Walk and run.** With the game's Always Run setting off, magnitude below the run threshold walks and magnitude above it runs, with hysteresis. With Always Run on, any deflection outside the safety deadzone runs.

**Camera.** The right stick drives yaw and pitch at `speed × raw deflection × dt`. Pitch stays within the game's current camera limits. Script-imposed locks (`LockCameraDirection`, `LockCameraPitch`) and cutscene camera mode are honored: stick input has no effect on a locked axis.

**Device arbitration: last-used device wins.**

- Mouse camera input (including trackpad or gyro mouse output from Steam Input) suspends stick camera control until the mouse has been idle for a short timeout.
- Keyboard movement (WASD, QE) suspends stick movement until those keys are released.
- The stick takes over again as soon as it leaves the safety deadzone after the other device goes idle.

**Game gating.** Stick movement is blocked in every situation where keyboard movement is blocked: dialog, cutscenes, text input focus, and incapacitating effects. The library injects at a layer where the game's own checks apply (§5). Where it can't, it replicates those checks and tests them explicitly.

## 4. Reverse-engineering strategy and findings

### 4.1 Sources, in order of value

1. **The EE dedicated server binary `nwserver-linux`, which exports C++ symbols.** NWNX:EE resolves engine functions with `dlsym` on mangled names (`NWNXLib/API/FunctionsLinux.cpp`), so the full symbol table is present. It's a free download from the dedicated server packages at nwn.beamdog.net. This gives symbolized, current server code for everything movement-related.
2. **Server-to-client matching.** The client contains the same server code for single-player. Ghidra Version Tracking from `nwserver-linux` to `nwmain-linux` labels the server half of the client and the shared `CNWMessage` read/write functions. Client-only code (message senders and handlers, the camera) is then reached through xrefs.
3. **NWNX:EE source and generated headers** (github.com/nwnxee/unified): struct layouts, message enums, and working examples of hooking input messages.
4. **The 1.69 Linux `nwmain`, which has symbols**, as a fallback for client-only code if the EE client is stripped.
5. **Frida at runtime**, to confirm hypotheses.
6. **People.** The NWNX:EE team is reachable on Zulip and knows the client and wire protocol. A targeted question can shortcut R1 (task R8).

Every finding (function, global, struct offset, signature) is recorded in `docs/re-notes.md` with its evidence. No address enters `signatures/ee.yaml` without a matching note.

### 4.2 Known facts from public code (NWNX:EE source, read 2026-09-28)

Payload interpretations are inferences until R1 confirms them.

- **Input message minors** (`NWNXLib/API/Constants/Messages.hpp`): `WalkToWayPoint` 0x01, `AlwaysRun` 0x1a, `TurnOnSpot` 0x1c, `DriveControl` 0x1d, `AbortDriveControl` 0x21. None of them is controller-specific.
- **W, S, Q, E use `DriveControl`.** NWNX's keyboard event (`Plugins/Events/Events/InputEvents.cpp`) reads a `uint8` flags value at payload byte 14: 3→W, 2→S, 4→Q, 8→E. Bytes 0–13 are likely a position vector plus a 2-byte field.
- **A and D use `TurnOnSpot`,** whose payload starts with a continuous (x, y) float facing vector.
- **Drive movement carries a bearing.** `CNWSCreature::AddDriveAction(uint16_t nGroupId, const Vector& vPathStart, int32_t nBearing, int32_t nClientPathNumber, int32_t nDriveFlags, int32_t nNumWayPointsToGenerate)`, together with `AIActionDrive` and `DriveUpdateLocation(BOOL bRun)`. `CNWSMessage` has `HandlePlayerToServerInputDriveControl` and `...AbortDriveControl`.
- **Creature movement state:** `m_fMovementRateFactor`, `m_fDriveModeMoveFactor`, `m_bDriveMode`, `m_bCutsceneCameraMode`.
- **Camera messages (server to client), major `Camera` 0x10:** `ChangeLocation` 0x01 via `SendServerToPlayerCamera_ChangeLocation(pPlayer, nFlags, fCameraAngle, fCameraDistance, fCameraPitch, nSmooth)`, plus `SetMode`, `Store`, `Restore`, `SetHeight`, and `LockPitch`/`LockDist`/`LockYaw` (0x06–0x08). There is also `SendServerToPlayerCamera_SetLimits(pPlayer, fMinPitch, fMaxPitch, fMinDist, fMaxDist)`. The client's handlers for these messages write exactly the camera state we need. There is also `CNWSMessage::HandlePlayerToServerCameraMessage`, so the client reports some camera state back to the server.
- **Message peeking:** NWNX's `Utils::PeekMessage` reads the byte-aligned buffer. NWN messages also have a bit-packed section.

### 4.3 Evidence from the console ports

NWN:EE shipped on Switch, PS4, and Xbox One in December 2019, with crossplay against PC servers for Switch and Xbox. Since no controller-specific input minors exist, gamepad clients must move characters on PC-hosted servers using the messages above. This supports Path A, though consoles may quantize. The PC client also shares a codebase with builds that had full controller support, so it may contain dormant gamepad code (R6).

### 4.4 Research tasks

These can start immediately, in parallel with M0. Tasks marked "agent" are headless and text-producing.

| ID | Owner | Task | Answers |
|---|---|---|---|
| R1 | agent | Decompile from `nwserver-linux`: `HandlePlayerToServerInputDriveControl`, `...AbortDriveControl`, the `TurnOnSpot` and `AlwaysRun` branches of `HandlePlayerToServerInputMessage`, `AddDriveAction`, `AIActionDrive`, `DriveUpdateLocation`, and all writers of `m_fDriveModeMoveFactor`. | Full payload layout (byte and bit-packed parts), bearing units and whether the server honors or snaps it, how strafe and backpedal flags combine with the bearing, keepalive and timeout expectations. **This decides Path A vs. B.** |
| R2 | agent | Check `nwmain-linux` for exported symbols (`nm -D`, `readelf --dyn-syms`). | Whether client RE is nearly free. |
| R3 | agent | Ghidra Version Tracking `nwserver-linux` → `nwmain-linux`; export the matched names. Locate the client senders for minors 0x1c, 0x1d, 0x21, and 0x1a. | The functions Path A calls, plus a labeled client binary. |
| R4 | agent | Extract `CNWSCreature` offsets (position, orientation, drive mode, cutscene mode, movement factors) from the NWNX headers and confirm them against R1. | Ground truth for the socket's `state` command. |
| R5 | agent | Search public NWN:EE documentation for command-line options that load a module or save, and that override the user directory. | The approach for unattended loading and install isolation (§7.2). |
| R6 | agent | Run `strings` and `nm -D` on `nwmain-linux` for gamepad, joystick, controller, and SDL GameController references. | Whether dormant console controller code exists that could be reused. |
| R7 | agent | After R3, find the client's handler for major 0x10: `ChangeLocation`, `SetLimits`, and the locks. Also find where the client stores Always Run state. | The camera object fields and setter, pitch limits, lock flags, and Always Run state for M1 and M3. |
| R8 | Robert | Ask the NWNX:EE team on Zulip whether `DriveControl` honors an arbitrary bearing and what console clients send. | May settle R1 early. |

## 5. Movement design

### 5.1 Decision gate (M2)

- **Path A** is used if R1 plus a runtime check show that the server honors an arbitrary bearing *and* supports moving along that bearing while facing a different direction. The library calls the client-side `DriveControl` sender (from R3) with the world movement direction and keeps facing on the camera's forward direction, using `TurnOnSpot` if the drive message doesn't carry facing. This is client-only and works on any server.
- **Path B** is used otherwise. The library hooks the in-process server's `HandlePlayerToServerInputDriveControl` or `AddDriveAction` and substitutes the analog bearing and facing before they reach the creature. This is single-player and self-hosted only, and multiplayer is revisited after v1.

**Rejected for v1:** Path A2 (`TurnOnSpot` plus forward drive only) and click-to-move through `WalkToWayPoint`. Both force the character to face its movement direction, which violates the free-facing requirement. A2 may still serve as a diagnostic stepping stone during M2.

### 5.2 Common behavior

- **Injection layer.** Call the same client function the keyboard handler calls, so client-side gating (§3) applies. If the gating lives above that function, replicate the checks and test them.
- **Send rate.** Send on meaningful change: a heading delta above a threshold, or a walk/run/stop transition. Add a keepalive at the cadence observed in `msglog` captures of real keyboard movement, and respect a hard rate cap.
- **Stop.** Send `AbortDriveControl` (or the equivalent R1 identifies) exactly once when the stick enters the safety deadzone.
- **Always Run.** Read the client's Always Run state (R7) every frame.

## 6. Camera design

### 6.1 Locating the state

Use the client's handler for camera major 0x10, minor `ChangeLocation` (R7). It writes yaw, distance, and pitch into the camera object, which gives the fields and ideally a setter function. The `SetLimits` and `Lock*` handlers give the pitch limits and lock flags. If R7 stalls, the fallback is a float scan with the socket's `scan` command, then tracing back to a stable pointer in Ghidra.

### 6.2 Per-frame update

Compute `yaw += rx × yaw_speed × dt` and `pitch += ry × pitch_speed × dt`, clamped to the current limits and skipping locked axes. Prefer calling the game's own setter over writing fields directly. If the game overwrites direct writes each frame, apply the deltas inside a hook on the camera update function.

### 6.3 Library configuration

The library has few settings, because tuning lives in Steam Input. The file is `~/.config/nwpad/config.toml`, and every key is optional.

| Key | Default | Notes |
|---|---|---|
| `camera_yaw_speed` | 180 °/s | Speed at full deflection. Steam Input can only scale below this. |
| `camera_pitch_speed` | 90 °/s | Same, for pitch. |
| `run_threshold` | 0.6 | Raw magnitude where walk turns into run. |
| `run_hysteresis` | 0.05 | Band around the threshold. |
| `mouse_idle_ms` | 300 | Idle time before the stick regains the camera. |

The safety deadzone is fixed at 0.05 raw magnitude and isn't configurable. The values above are starting points, confirmed during feel sign-off (M4).

## 7. Architecture

`libnwpad.so` is a single C11 preload library.

- **Hook layer.** Interposes `SDL_GL_SwapWindow`, which provides the per-frame callback on the main thread, and `SDL_PollEvent`, which filters out `SDL_CONTROLLER*` events and observes mouse and keyboard activity for arbitration. It also hooks game functions identified in R-tasks as needed.
- **Signature resolver.** Resolves all game addresses at load time from `signatures/ee.yaml`. If a signature misses, the dependent feature is disabled and logged, and the game keeps running.
- **Input source.** Uses the game's SDL GameController API, initialized lazily on the first frame. It also accepts a virtual input override from the control socket for testing.
- **Arbitration, movement, and camera modules.** Pure functions of input plus game state that produce actions. This keeps them unit-testable.
- **Control socket and telemetry** (§8.3). Compiled out of release builds.

**Performance budget:** under 0.1 ms per frame for all library work on the reference test box, measured by telemetry.

**Build:** CMake, GCC or Clang, `-Wall -Wextra -Werror`. The library has no dependencies beyond libc, libdl, and the game's own SDL2.

## 8. Agentic development and testing loop

### 8.1 Topology

**Claude Code** runs on Robert's local machine. It drives a **dedicated Linux test box over SSH** and has full access to that box. The test box has a monitor, and Robert also plays and feel-tests on it.

The game runs in the box's real desktop session. It's windowed at a fixed resolution, and a Steam client is running. The agent launches the game on that display, so no headless GPU setup is needed.

**Session lock.** Before launching, the agent takes `~/.nwpad/session.lock` on the box, and releases it afterward. If Robert holds the lock (`tools/lock.sh hold` / `release`), the agent waits. The agent also refuses to launch if `nwmain-linux` is already running outside its own session.

**The Steam Deck is manual-testing only.** It isn't part of the automated loop.

### 8.2 Environment

**Game.** The Steam copy, tracking whatever Steam installs. M0 records the build number and the `nwmain-linux` hash in `docs/re-notes.md`. `sigcheck` runs before every live session. If the hash changes, the agent stops and reports rather than chasing signatures silently.

**Isolation.** Tests never touch Robert's own NWN user directory, settings, or saves. The approach, chosen in M0 by R5 findings in this order:

1. The game's own user-directory override, if one exists.
2. Launching with an overridden `HOME` or XDG path pointing at `~/.nwpad/userdir`.
3. A separate Linux user account on the box.

Each approach gets a half-day timebox.

**Test module.** `nwpad_test.mod` has a flat open area, a known start point and facing, a wall segment, and no NPCs or time-of-day effects. It also includes two script-driven features:
- An OnEnter script that calls `SetCameraFacing` to put the camera in a known state.
- A test hook that applies camera locks and starts or ends a cutscene on request.

The module is kept as neverwinter.nim JSON (`nwn_gff`, `nwn_erf`) and built by script. The area is authored once in the toolset under Wine.

**Unattended loading.** The approaches are tried in order, each with a half-day timebox:
1. A command-line option to load a save or module (R5).
2. A `load_save` socket command that calls the function the Load Game UI uses.
3. Scripted UI input with xdotool on the real display.

The test save holds a fixed test character. It's created locally and never committed.

### 8.3 Control socket

The socket is `$XDG_RUNTIME_DIR/nwpad.sock`, mode 0600, and exists only when `NWPAD_SOCKET=1` is set. It is compiled out of release builds. The protocol is JSON lines. From the agent's machine, it's reached through `ssh -L` socket forwarding, or `nwpadctl` runs on the box over SSH.

| Command | Purpose |
|---|---|
| `ping` | Liveness, version, and frame counter. |
| `status` | Readiness, enabled features, and signature resolution results. |
| `state` | Creature position, orientation, drive mode, and cutscene mode, read from the in-process server `CNWSCreature` (R4) as ground truth. Also camera yaw, pitch, and locks; Always Run; which device has control; text-input-active; and frame dt. |
| `stick` / `release` | Sets or clears virtual stick input, with optional `hold_ms`. |
| `wait_frames` / `wait_ms` | Waits inside the game loop, then returns state. |
| `screenshot` | PNG via `glReadPixels` in the swap hook. |
| `msglog` | Logs player-to-server input messages (minor, bytes, timestamp) from a hook on the in-process server's `HandlePlayerToServerInputMessage`. |
| `load_save` | Loads the test save (if approach 2 in §8.2 is used). |
| `script` | Runs a named test-module script (camera locks, cutscene toggle, dialog start). |
| `read` / `scan` | Memory read and scan, for RE sessions. |
| `log` | Sets the telemetry level and path. |

### 8.4 Tools

| Tool | Runs on | Purpose |
|---|---|---|
| `tools/remote.sh` | local | Syncs the repo to the box, builds there, and runs a command over SSH. It pulls artifacts back into `artifacts/<run-id>/`. |
| `tools/run_game.sh` | box | Takes the session lock, launches the game with the preload library and the isolated user directory, waits for `ready`, and enforces a timeout. |
| `tools/lock.sh` | box | Robert's manual hold and release of the session lock. |
| `tools/nwpadctl` | box or local | CLI over the control socket. |
| `tools/collect_crash.sh` | box | Core dump, `gdb -batch` backtrace, and the telemetry tail. |
| `tools/fetch_server.sh` | box | Downloads and unpacks the dedicated server package and records its hash. |
| `tools/ghidra/` | box | Headless Ghidra: `decompile_fn` by symbol name, version tracking and name export, xref search, and signature generation. Outputs go to `re-work/` (gitignored). |
| `tools/frida/` | box | Trace scripts, run with `frida -q` and a timeout. |
| `tools/sigcheck` | box | Offline signature resolution against the installed binary. |
| `tools/uinput_pad.py` | box | Virtual gamepad for end-to-end tests through SDL. |
| `tools/xinput.sh` | box | xdotool wrappers for mouse and keyboard, used in arbitration tests. |

### 8.5 The loop

The agent picks the cheapest tier that answers its current question.

- **Inner tier** (local, seconds): build and run unit tests.
- **Middle tier** (box, about 1–2 minutes): `sigcheck`, launch, pytest live scenarios, and artifact pull-back. On failure, the agent reads telemetry before changing code.
- **Outer tier** (box, RE sessions): hypothesis, then headless Ghidra, then runtime confirmation with Frida, `msglog`, or `read`, then a `re-notes.md` entry, and only then signature changes.

A milestone is complete when its acceptance tests pass, all earlier tests still pass, and `sigcheck` is clean.

### 8.6 Tests

**Unit tests (C, local and CI):**
- The safety deadzone gives zero output inside it and raw passthrough outside it.
- Camera-relative heading math works in all quadrants and at wraparound.
- Facing and movement decomposition handles strafe and backpedal cases.
- Run hysteresis doesn't flicker under noise.
- Always Run overrides the threshold.
- The send-rate limiter respects its thresholds, keepalive, and cap.
- The stop message is sent exactly once.
- Pitch clamping follows dynamic limits, and locked axes ignore input.
- Arbitration handles mouse idle timeout, keyboard takeover, and stick reclaim.

**Live scenarios (pytest, test box).** Tolerances are starting values, and only Robert may relax them.

| Scenario | Assertion |
|---|---|
| Camera linearity | Yaw rate at 0.5 deflection is 0.5× the full-deflection rate (±5%), independent of frame rate. |
| Camera limits and locks | Pitch stays within the game limits. With a script-applied yaw or pitch lock, the stick has no effect on that axis. |
| Heading accuracy | 16 stick angles × 3 camera yaws: displacement direction after 1 s is within 5° of expected. |
| Non-quantization | Headings at 22.5° offsets give distinct displacement directions. |
| Free facing | Strafe (stick right) and backpedal (stick down): displacement matches the stick direction, and facing stays within 5° of camera forward. |
| Stop | The character stops within 300 ms of release, and position is stable for 1 s. |
| Walk/run | Rates match the game's walk and run speeds on either side of the threshold. With Always Run on, it runs at 0.3 deflection. |
| Direction change | Stick rotation while moving updates the heading without a stop or stutter. |
| Collision | Pushing into the wall stops progress, and re-steering away works. |
| Gating | No movement during dialog, cutscene, or chat focus. |
| Arbitration | Mouse camera movement via xdotool suspends the stick camera until idle. WASD suspends stick movement until released. |
| Packet shape (Path A) | `msglog` shows our messages have the same layout as captured keyboard packets, apart from the heading fields. |
| Soak | 10 minutes of randomized input: no crash, no stuck movement, rate cap respected. |
| End-to-end SDL | Heading and stop tests driven through `uinput_pad.py`. |
| Overhead | Library time per frame at the 99th percentile is under 0.1 ms. |

### 8.7 Human checkpoints

The agent stops and asks Robert:
- At the M2 gate, before committing to Path A or Path B.
- Before changing the signature format or the resolver's failure behavior.
- Before relaxing any test tolerance.
- For feel tuning and sign-off, on desktop and on the Deck.
- If the game binary hash changes.

### 8.8 Failure handling

- **Crash:** `collect_crash.sh` runs automatically. After three crashes on the same change, the agent stops and reports.
- **Hang or never ready:** the launch is killed at the timeout and reported as an environment failure, separate from test failures.
- **Flaky test:** at most two reruns. Persistent flakiness is reported, never hidden.

## 9. Milestones

| Milestone | Deliverable | Acceptance |
|---|---|---|
| **R: Desk research** | R1–R8, in parallel with M0. | Payload layouts and camera handler findings in `re-notes.md` with evidence. |
| **M0: Environment** | Box setup, session lock, isolation, test module and save, unattended load, socket `ping`/`status`, build and hash recorded, `CLAUDE.md` in place. | 10 consecutive unattended launches reach `ready`. |
| **M1: Camera** | Camera via R7 handler findings; linear stick control; limits, locks, and cutscene honored; mouse arbitration. | Camera, limits, locks, and camera arbitration tests pass. |
| **M2: Movement gate** | R3 senders located; `msglog` captures of real keyboard packets; hand-built bearing experiments; Path A or B recommendation. | Human checkpoint. |
| **M3: Analog movement** | Movement on the chosen path, with free facing, stop, and send-rate limiting. | Heading, non-quantization, free facing, stop, and direction-change tests pass. |
| **M4: Complete behavior** | Walk/run and Always Run, gating, keyboard arbitration, config file. | Full live suite and soak pass; overhead within budget; desktop feel sign-off. |
| **M5: Release** | Release build; Steam Input layouts for Xbox and Deck; README (install, launch options, Steam Input setup, multiplayer note); Deck validation. | Game runs normally with all signatures deliberately broken (features off, logged). **v1 done:** Robert signs off after real play sessions on desktop and Deck. |

## 10. Deployment

**Install.** `libnwpad.so` goes in `~/.local/lib/nwpad/`.

**Steam launch options:** `LD_PRELOAD="$HOME/.local/lib/nwpad/libnwpad.so:$LD_PRELOAD" %command%`. This chains the preload so the overlay keeps working. The game must run as the native Linux build, not under Proton.

**Steam Input** stays enabled, with the project's recommended layout or any layout whose sticks output as gamepad joysticks.

**Verifying it loaded.** The library writes a startup line to the game's stdout log, recording its version and signature status.

**Uninstall.** Remove the launch option and delete the directory.

## 11. Repository and licensing

The repo is public, under the MIT license.

**Committed:** source, tests, tools, signatures, `re-notes.md` findings and offsets (in the same spirit as NWNX's published headers), the test module JSON, the Steam Input layout files, and docs.

**Never committed** (enforced by `.gitignore` and a pre-commit check): game binaries or assets, dedicated server files, raw decompiler output (`re-work/`), the test save, screenshots of game content in `artifacts/`, and anything under the isolated user directory.

**CI** (GitHub Actions) runs the build and unit tests. `sigcheck` and live tests need the game, so they run only on the test box.

## 12. Risk register

| Risk | Likelihood | Impact | Mitigation / trigger |
|---|---|---|---|
| Server ignores or snaps the bearing, or can't separate facing from movement | Medium | Loses multiplayer compatibility for v1 | Path B (accepted). R8 may answer early. |
| Unattended loading doesn't work cleanly | Medium | Slow middle loop | Three approaches, timeboxed; xdotool as the last resort. |
| EE client is stripped and version tracking matches poorly | Medium | More manual RE | 1.69 symbols; runtime tracing via `msglog` and Frida. |
| Direct camera writes get overwritten | Medium | Extra hook needed | Hook the camera update function (§6.2). |
| Client-side gating lives above the injection point | Medium | Movement in dialog or cutscenes | Replicate the checks; the gating tests catch it. |
| Steam Deck behaves differently (gamescope, SDL build, input path) | Low–Medium | Deck target slips | Manual Deck validation in M5; passthrough layout. |
| Game update lands mid-project | Low (no updates in years) | Signatures break | Hash check before every live session; the agent stops on change. |
| Session contention with Robert's use of the box | Medium | Interrupted tests or play | Session lock; the agent refuses to launch if the game is already running. |
| Continuous speed later wanted | Low | Out of scope for v1 | Investigate `m_fDriveModeMoveFactor` (R1) after v1. |

## 13. Decision log

| Date | Decision |
|---|---|
| 2026-09-28 | Free facing (strafe and backpedal) is required. Path A if the bearing is honored, otherwise Path B for single-player and self-hosted; multiplayer revisited later. A2 and click-to-move rejected. |
| 2026-09-28 | Claude Code runs locally, driving a dedicated Linux box over SSH with full access. The box has a monitor and is shared with Robert's play. |
| 2026-09-28 | Steam copy of the game, tracking whatever Steam installs; hash checked. |
| 2026-09-28 | Zoom is out of scope. Last-used device wins. Walk/run by stick magnitude, overridden by Always Run. |
| 2026-09-28 | Targets are the Xbox controller and Steam Deck. The Deck is tested manually only. |
| 2026-09-28 | Steam Input is the controller layer and owns all stick tuning. The library uses raw values with a fixed safety deadzone. |
| 2026-09-28 | The library is always active on any server; the README carries a persistent-world rules note. |
| 2026-09-28 | Public repo, MIT license. Agent rules live in `CLAUDE.md`. |
| 2026-09-28 | v1 is done when the automated suite passes and Robert signs off after play sessions on desktop and Deck. |
