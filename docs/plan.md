# nwpad — Runtime Analog Controller Support for Neverwinter Nights: EE (Linux)

Status: **v1.0**, approved for execution. Changes after this point go through the decision log (§13).

## 1. Goal and scope

Add true analog stick control of character movement and camera to the native Linux client of NWN:EE (`nwmain-linux`) at runtime, without modifying the game binary on disk. The mechanism is an `LD_PRELOAD` shared library, `libnwpad.so`.

**In scope:**

- **Movement from the left stick.** Full 360° direction relative to the camera. The character faces where it walks, as with the game's own click-and-drag movement (decision log, M2). Stick magnitude selects walk or run.
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

**Movement.** The left stick direction, rotated by the current camera yaw, gives the world-space movement direction. There are two movement modes (Robert, M2 gate):

- **Drag:** the character walks in the stick direction and turns to face it, exactly like holding the left mouse button on the ground.
- **Strafe/backpedal:** when the stick's world direction is within a small window around 90°, 180°, or 270° from the character's current facing (right, back, left), the character keeps its facing and strafes or backpedals, exactly like holding E, S, or Q. The windows follow the character, not the camera (Robert, option (b)): after a drag, pulling the stick straight back backpedals without turning.

Mode selection is asymmetric:
- From rest, the stick's first direction outside the deadzone picks the mode: inside a window means strafe/backpedal; anything else, including forward, means drag.
- Strafe/backpedal turns into drag once the stick has stayed outside its window for `strafe_exit_ms` (150 ms), so a released stick springing back through other angles doesn't turn the character around.
- Drag stays drag: moving into a window doesn't switch it.
- Returning to the deadzone goes back to rest.

The window half-width starts at 10° and is tuned in feel testing. With only one sticky transition, no extra hysteresis is needed. When the stick returns to the safety deadzone, the character stops and keeps its current facing. (Before the M2 gate the spec required free facing, with strafe and backpedal while facing the camera; Robert replaced it at the gate, see the decision log.)

**Walk and run.** With the game's Always Run setting off, magnitude below the run threshold walks and magnitude above it runs, with hysteresis. With Always Run on, any deflection outside the safety deadzone runs.

**Camera.** The right stick drives yaw and pitch at `speed × raw deflection × dt`. Pitch stays within the game's current camera limits. Script-imposed locks (`LockCameraDirection`, `LockCameraPitch`) and cutscene camera mode are honored: stick input has no effect on a locked axis.

**Device arbitration: last-used device wins.**

- Mouse camera input (including trackpad or gyro mouse output from Steam Input) suspends stick camera control until the mouse has been idle for a short timeout.
- Keyboard vs stick movement isn't arbitrated by the library (Robert, 2026-09-29). The engine gets both through its own entry points and handles them; this gets verified in play testing.
- The stick takes over again as soon as it leaves the safety deadzone after the other device goes idle.

**Game gating: struck (Robert, 2026-09-29).** The library doesn't block stick movement in dialogs or cutscenes. In the game, walking away from a conversation ends it once you get far enough, which can have story consequences, so the stick should behave like the player's own movement. Whatever the game's own movement entry points refuse (F20, F21), they still refuse.

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

**Decided at the gate (2026-09-28): Path D, drag emulation plus key-equivalent strafe/backpedal.**
- Drag mode (§3) mirrors the game's click-and-drag movement, which resends `WalkToWayPoint` toward the cursor while the button is held (re-notes F16). The library produces the same stream toward a point ahead of the character along the stick direction, entering through the client function the mouse-drag path uses.
- Strafe/backpedal mode presses and releases S, Q, or E through the client's own key handler (`HandleInputEvent`, re-notes F21), so the client's prediction and the server agree (unlike rewriting packets, F16), and releasing sends the game's own AbortDriveControl.
- Both modes are client-only and work on any server. Paths A and B above are kept for reference. Free facing in arbitrary directions is out of scope for v1.

### 5.2 Common behavior

- **Injection layer.** Call the same client functions the mouse and keyboard use (F20, F21), so the game's own checks apply. The library adds no gating of its own (§3).
- **Send rate.** Send on meaningful change: a heading delta above a threshold, or a walk/run/stop transition. Add a keepalive at the cadence observed in `msglog` captures of real keyboard movement, and respect a hard rate cap.
- **Stop.** Send `AbortDriveControl` (or the equivalent R1 identifies) exactly once when the stick enters the safety deadzone.
- **Always Run.** Read the client's Always Run state (R7) every frame.

## 6. Camera design

### 6.1 Locating the state

Use the client's handler for camera major 0x10, minor `ChangeLocation` (R7). It writes yaw, distance, and pitch into the camera object, which gives the fields and ideally a setter function. The `SetLimits` and `Lock*` handlers give the pitch limits and lock flags. If R7 stalls, the fallback is a float scan with the socket's `scan` command, then tracing back to a stable pointer in Ghidra.

### 6.2 Per-frame update

Compute `yaw += rx × yaw_speed × dt` and `pitch += ry × pitch_speed × dt`, clamped to the current limits and skipping locked axes. Prefer calling the game's own setter over writing fields directly. If the game overwrites direct writes each frame, apply the deltas inside a hook on the camera update function.

### 6.3 Library configuration

The library has few settings, because tuning lives in Steam Input. Since the settings work (docs/settings-plan.md) they live in a `[nwpad]` section of the game's `settings.tml`; `~/.config/nwpad/config.toml` is imported once and then only a fallback (README "Settings"). The original `config.toml` keys, every one optional:

| Key | Default | Notes |
|---|---|---|
| `camera_yaw_speed` | 180 °/s | Speed at full deflection. Steam Input can only scale below this. |
| `camera_pitch_speed` | 90 °/s | Same, for pitch. |
| `run_threshold` | 0.7875 | Centre of the walk/run band (Robert's feel test: run above 0.85, walk again below 0.725). |
| `run_hysteresis` | 0.125 | Width of the band around the threshold. |
| `mouse_idle_ms` | 300 | Idle time before the stick regains the camera. |
| `strafe_window` | 10° | Half-width of the strafe/backpedal windows around 90/180/270° (§3). |
| `strafe_exit_ms` | 150 | How long the stick must stay outside a strafe/backpedal window before it becomes a drag, so a released stick springing back doesn't turn the character around. |
| `hide_cursor` | 1 | Hide the mouse cursor while the sticks are in use; the first mouse motion brings it back (re-notes F24). |
| `cursor_rehide_ms` | 2000 | After mouse motion, how long the mouse must be still and a stick held before the cursor hides again. |

The safety deadzone is fixed at 0.15 raw magnitude and isn't configurable. It was 0.05 until Robert's Xbox Series pad test: after a small push the left stick can settle at 0.084 and keep the character walking until the pad is bumped. The values above are starting points, confirmed during feel sign-off (M4).

## 7. Architecture

`libnwpad.so` is a single C11 preload library.

- **Hook layer.** Hooks `SDL_GL_SwapWindow`, which provides the per-frame callback on the main thread, and `SDL_PollEvent`, which filters out `SDL_CONTROLLER*` events and observes mouse and keyboard activity for arbitration. The game links SDL2 statically, so these hooks swap entries in SDL's dynamic API jump table rather than interposing symbols (re-notes F8). It also hooks game functions identified in R-tasks as needed.
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

The commands as built (the handler in `src/hook/nwpad.c` documents each one's arguments):

| Command | Purpose |
|---|---|
| `ping` | Liveness, version, and frame counter. |
| `status` | Readiness, enabled features, config, settings, and signature resolution results. |
| `state` | Creature position and facing from the in-process server `CNWSCreature` (R4, ground truth) and the client; camera yaw, pitch, and locks; drive style and mode; which device has the camera; the cursor; event counters; the picker; per-frame cost (`reset_cost` clears it). |
| `stick` / `release` | Sets or clears virtual stick input, with optional `hold_ms`. |
| `script_chunk` | Runs NWScript on the local server, as the cheat console does (F17): test setup such as starting a conversation. |
| `read` | Memory read, for RE sessions. |
| `walk_to`, `always_run` | The game's own walk and Always Run option, for reference measurements. |
| `quickbar`, `quickbar_bank`, `quickbar_use`, `equipped_icon` | Quickbar contents, bank, use, and an equipped item's icon (F31, F34). |
| `picker`, `picker_shift` | Open, select in, and close the quickbar picker; change its bank. |
| `nui_create` / `nui_bind` / `nui_destroy` | Client-side NUI windows (F32). |
| `resource_publish` | Serve a file as a game resource (F35), e.g. the test conversation. |
| `dialog`, `dialog_select` | The open conversation, and answering or ending it (F36). |
| `dialog_ui`, `dialog_preview` | nwpad's conversation window: keys, a dry-run mouse, its state (F37); a made-up conversation to look at. |

### 8.4 Tools

| Tool | Runs on | Purpose |
|---|---|---|
| `tools/remote.sh` | local | Syncs the repo to the box, builds there, and runs a command over SSH. It pulls artifacts back into `artifacts/<run-id>/`. |
| `tools/run_game.sh` | box | Takes the session lock, launches the game with the preload library and the isolated user directory, waits for `ready`, and enforces a timeout. |
| `tools/lock.sh` | box | Robert's manual hold and release of the session lock. |
| `tools/nwpadctl` | box or local | CLI over the control socket. |
| `tools/collect_crash.sh` | box | Core dump, `gdb -batch` backtrace, and the telemetry tail, for a run directory. |
| `tools/fetch_server.sh` | box | Downloads and unpacks the dedicated server package and records its hash. |
| `tools/ghidra/` | box | Headless Ghidra: `decompile_fn` by symbol name, version tracking and name export, xref search, and signature generation. Outputs go to `re-work/` (gitignored). |
| `tools/frida/` | box | Trace scripts, run with `frida -q` and a timeout. |
| `tools/sigcheck` | box | Offline signature resolution against the installed binary. |
| `tools/uinput_mouse.py` | box | Virtual absolute mouse for arbitration and conversation-window tests (XTEST pointer motion doesn't reach XWayland clients). Keys go through `xdotool` (`conftest.xkey`). |
| `tools/gen_signatures.py` | box | Compiles `signatures/ee.yaml` into the library's header (CMake runs it). |
| `tools/m5_checks.sh` | box | The release build in the game with every signature resolved, and a build with every signature broken (features off, nothing crashes). |
| `tools/settings_checks.sh` | box | The native Options entries and `settings.tml`, end to end (settings plan). |
| `tools/make_test_dlg.py` | box | Writes nwpad's own test conversation (dialog plan). |
| `tools/install.sh`, `tools/install_hooks.sh` | box / local | Release install; the repo's git hooks. |

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
| Facing follows movement | From rest, stick at 45° (drag): displacement matches the stick direction, and facing ends within 5° of the displacement direction. |
| Strafe and backpedal | From rest, with the stick right, back, and left of the character's facing (inside the windows): displacement matches the stick direction, and facing stays within 5° of its starting value. |
| Mode transitions | Strafe then rotate the stick out of its window: switches to drag. Drag then rotate into a window: stays drag. |
| Stop | From a run: the character (server) stops within 450 ms of release, and position is stable for 1 s. From a walk: the on-screen character stops within 450 ms, and the server reaches the same spot within 1 s. |
| Walk/run | Rates match the game's walk and run speeds on either side of the threshold. With Always Run on, it runs at 0.3 deflection. |
| Direction change | Stick rotation while moving updates the heading without a stop or stutter. |
| Collision | Pushing into the wall stops progress, and re-steering away works. |
| Arbitration | Mouse movement suspends the stick camera until idle. |
| Packet shape (Path D) | Our `WalkToWayPoint` messages have the same layout and cadence as captured mouse-drag packets, apart from the target point. |
| Soak | 10 minutes of randomized input: no crash, no stuck movement, rate cap respected. |
| End-to-end SDL | Heading and stop tests driven through `uinput_pad.py`. |
| Overhead | The library's own time per frame at the 99th percentile is under 0.1 ms. Time inside the game functions it calls is reported separately (provisional, decision log). |

### 8.7 Human checkpoints

The agent stops and asks Robert:
- At the M2 gate, before committing to Path A or Path B.
- Before changing the signature format or the resolver's failure behavior.
- Before relaxing any test tolerance.
- For feel tuning and sign-off, on desktop and on the Deck.
- If the game binary hash changes.

### 8.8 Failure handling

- **Crash:** the library's crash handler writes a backtrace into the run's `game.log`; `collect_crash.sh <run-dir>` adds the core dump and a `gdb` backtrace. After three crashes on the same change, the agent stops and reports.
- **Hang or never ready:** the launch is killed at the timeout and reported as an environment failure, separate from test failures.
- **Flaky test:** at most two reruns. Persistent flakiness is reported, never hidden.

## 9. Milestones

| Milestone | Deliverable | Acceptance |
|---|---|---|
| **R: Desk research** | R1–R8, in parallel with M0. | Payload layouts and camera handler findings in `re-notes.md` with evidence. |
| **M0: Environment** | Box setup, session lock, isolation, test module and save, unattended load, socket `ping`/`status`, build and hash recorded, `CLAUDE.md` in place. | 10 consecutive unattended launches reach `ready`. |
| **M1: Camera** | Camera via R7 handler findings; linear stick control; limits, locks, and cutscene honored; mouse arbitration. | Camera, limits, locks, and camera arbitration tests pass. |
| **M2: Movement gate** | R3 senders located; `msglog` captures of real keyboard packets; hand-built bearing experiments; Path A or B recommendation. | Human checkpoint. |
| **M3: Analog movement** | Drag-emulation movement (Path D), with stop and send-rate limiting. | Heading, non-quantization, facing-follows-movement, stop, and direction-change tests pass. |
| **M4: Complete behavior** | Walk/run and Always Run, config file (approved). (Gating struck; keyboard arbitration left to the engine, §3.) | Full live suite and soak pass; overhead within budget; desktop feel sign-off. |
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
| Steam Deck behaves differently (gamescope, SDL build, input path) | Low–Medium | Deck target slips | Manual Deck validation in M5; passthrough layout. |
| Game update lands mid-project | Low (no updates in years) | Signatures break | Hash check before every live session; the agent stops on change. |
| Session contention with Robert's use of the box | Medium | Interrupted tests or play | Session lock; the agent refuses to launch if the game is already running. |
| Continuous speed later wanted | Low | Out of scope for v1 | Investigate `m_fDriveModeMoveFactor` (R1) after v1. |

## 13. Decision log

| Date | Decision |
|---|---|
| 2026-09-28 | ~~Free facing (strafe and backpedal) is required.~~ Superseded at the M2 gate, below. Path A if the bearing is honored, otherwise Path B for single-player and self-hosted; multiplayer revisited later. A2 and click-to-move rejected. |
| 2026-09-28 | Claude Code runs locally, driving a dedicated Linux box over SSH with full access. The box has a monitor and is shared with Robert's play. |
| 2026-09-28 | Steam copy of the game, tracking whatever Steam installs; hash checked. |
| 2026-09-28 | Zoom is out of scope. Last-used device wins. Walk/run by stick magnitude, overridden by Always Run. |
| 2026-09-28 | Targets are the Xbox controller and Steam Deck. The Deck is tested manually only. |
| 2026-09-28 | Steam Input is the controller layer and owns all stick tuning. The library uses raw values with a fixed safety deadzone. |
| 2026-09-28 | The library is always active on any server; the README carries a persistent-world rules note. |
| 2026-09-28 | Public repo, MIT license. Agent rules live in `CLAUDE.md`. |
| 2026-09-28 | SDL hooks go through SDL's dynamic API jump table, because the game links SDL 2.0.8 statically and symbol interposition can't reach it (re-notes F8). |
| 2026-09-28 | Test isolation uses the game's `-userdirectory` option (re-notes F10). |
| 2026-09-28 | Signature entries may name an exported symbol instead of a byte pattern (approved by Robert); patterns remain for code without symbols. Failure behavior is unchanged. |
| 2026-09-28 | v1 is done when the automated suite passes and Robert signs off after play sessions on desktop and Deck. |
| 2026-09-28 | M2 gate (Robert): the character faces where it walks, like the game's click-and-drag. Free facing is dropped for v1. Movement uses Path D, drag emulation through `WalkToWayPoint` (re-notes F16). |
| 2026-09-28 | M2 gate (Robert), refined: stick directions within a window around 90/180/270° from camera forward strafe or backpedal as the E/S/Q keys do, and everything else drags. From rest the first direction picks the mode, strafe/backpedal becomes drag when the stick leaves its window, and drag never switches back. |
| 2026-09-29 | M3 (Robert, option (b)): the strafe/backpedal windows are measured from the character's facing, not the camera's; strafe and backpedal keep the character's facing, as the keys do. |
| 2026-09-29 | Stop tolerance raised from 300 ms (an initial guess) to 450 ms to match the game (Robert). The stop measures 0.43 s; releasing the game's own movement keys takes about 0.37 s (re-notes F22). |
| 2026-09-29 | Gating struck (Robert): stick movement isn't blocked in dialogs or cutscenes, because walking away from a conversation is a legitimate player action with story consequences. Keyboard arbitration (WASD suspends the stick) is on hold, not committed for v1. |
| 2026-09-29 | Keyboard arbitration dropped from the library (Robert): the engine handles keyboard and stick input through its own entry points; verified in play testing. The config file (`~/.config/nwpad/config.toml`) is approved. |
| 2026-09-29 | Overhead budget, provisionally (Robert): p99 under 0.1 ms applies to the library's own logic (measured ≤ 20 µs in the soak). Time inside the game functions it calls (≤ 110 µs p99 in total) is reported and revisited later for further improvement. |
| 2026-09-29 | Feel testing on an Xbox Series pad (Robert): safety deadzone 0.05 → 0.15 (the stick can rest at 0.084 off-centre); run band 0.575–0.625 → 0.725–0.85 (start running above 0.85, back to walking below 0.725). Stopping from a walk re-targets the current position, because the forward tap runs and surged about 1.7 m. |
| 2026-09-29 | Stop test, walk case (Robert): measure the on-screen stop (450 ms) plus the server reaching the same spot within 1 s. At walking speed the server trails the client and catches up forward, with no surge and no slide-back. |
| 2026-09-29 | Cursor hiding (Robert): the mouse cursor hides while the sticks are in use and returns on mouse motion, honoring the game's own show/hide requests (config `hide_cursor`, default on). |
| 2026-09-29 | Robert's feel test: releasing a backpedal could turn the character around, because the stick springing back crosses angles outside the window. Leaving a strafe/backpedal window now has to last `strafe_exit_ms` (150 ms) before it becomes a drag. Right-edge turning made sticky while the mouse stays on the last reachable column (re-notes F27). |
| 2026-09-29 | Native settings (docs/settings-plan.md), Robert at the S checkpoint: tier "lite first, then a panel spike": settings move into a `[nwpad]` section of `settings.tml` (the game preserves it, re-notes F29), then a timeboxed spike for Options-window entries. D1: the Camera group. D2: `config.toml` imported once, then fallback only. D3: `strafe_window` and `mouse_idle_ms` stay file-only. |
| 2026-09-29 | Settings panel checkpoint, Robert: build the Options entries (rows appended to Input > Camera, no game-function hooks), labels only (descriptions need talk-table strrefs), "Controller"-prefixed labels. Built and checked the same day (re-notes F30). |
| 2026-09-29 | Cursor, Robert after playing: moving the mouse while a stick is held shows the cursor; it hides again after N s of continuous stick use with the mouse still. N = `nwpad.cursor-rehide-ms`, default 2000 (file-only); walking off a still mouse still hides at once. |
| 2026-09-30 | Quickbar picker trigger, Robert: a configurable keyboard key (`nwpad.picker-key`, default ScrollLock), so Steam Input can drive it from any button or grip; nwpad swallows the key. Replaces the pad Back button. |
| 2026-10-01 | Picker works like Baldur's Gate 3's radial, Robert: a press opens it and it stays open (the hold mode was then removed, Robert), Enter uses the pick, Escape closes; both only nwpad's while open. Xbox layout: RT opens, RS click = Enter, View = Esc; left click moves to hold LB + RS click. |
