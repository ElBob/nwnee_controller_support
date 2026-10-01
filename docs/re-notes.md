# Reverse-engineering notes

Every function, global, offset, and signature the library uses must have an entry here with its evidence and the binary it applies to (CLAUDE.md rule 4). Raw decompiler output stays in `re-work/` (gitignored); summarize it here in your own words.

## Binaries

| Binary | Source | Build | sha256 | Recorded |
|---|---|---|---|---|
| `nwmain-linux` | Steam | buildid 20277208 (`build.txt` 26c6e573, 2025-10-06) | `6d19c39bc646af5ddbc333ff31797a4acd9019506bcb45ef1b93b1bd5925e700` | 2026-09-28 |
| `nwserver-linux` | Steam (ships next to the client) | same | `d6d952826b671fc62c376dca912e87f1aa45d4aa81c620e912ceb4d8e051111d` | 2026-09-28 |

## Test box environment

- The game needs a connected monitor. With both DP connectors disconnected (the box's KVM switched away), KWin never maps the window: SDL 2.0.8's `SDL_CreateWindow` blocks in `XIfEvent`, or the game dies on an `XF86VidModeGetModeLine` BadValue X error. `tools/run_game.sh` refuses to launch in that state (exit 13). With the monitor connected, no SDL hints are needed.
- The same hang happens when the monitor is connected but KDE has powered it down (DRM `dpms` = Off) after idle time. `run_game.sh` wakes it with `kscreen-doctor --dpms on` and holds a `kde-inhibit --power --screenSaver` for as long as the game runs.
- `run_game.sh` launches the binary directly (not through Steam) with `SteamAppId=704450`, so the Steam API doesn't relaunch the game through Steam.
- Synthetic input under Plasma 6 XWayland: `xdotool key` reaches the game (the PollEvent hook counts the key events), but XTEST pointer motion doesn't (0 motion events). `tools/uinput_mouse.py` creates a uinput absolute tablet instead, and KWin delivers its motion to the game. It moves the real pointer on the box's desktop.

## Entry template

```
### F<n>: <short name>
- Binary / hash: nwmain-linux <sha256 prefix>
- What: function / global / struct offset
- Evidence: Ghidra reasoning summary, trace output summary, msglog capture, etc.
- Signature: signatures/ee.yaml key, if any
- Confidence: confirmed / likely / hypothesis
```

## Findings

### F1: Player-to-server input minors
- Binary / hash: n/a (public source)
- What: `NWNXLib/API/Constants/Messages.hpp` in nwnxee/unified defines `WalkToWayPoint` 0x01, `AlwaysRun` 0x1a, `TurnOnSpot` 0x1c, `DriveControl` 0x1d, `AbortDriveControl` 0x21. There are no controller-specific minors.
- Confidence: confirmed for the NWNX-supported server build.

### F2: Keyboard movement uses DriveControl with a flags byte
- Binary / hash: n/a (public source)
- What: NWNX's keyboard event (`Plugins/Events/Events/InputEvents.cpp`) peeks a `uint8` at payload offset 14 of `DriveControl`: 3→W, 2→S, 4→Q, 8→E.
- Open: bytes 0–13 are probably a position vector plus a 2-byte field; any bit-packed fields are unknown. To be settled by R1.
- Confidence: flags confirmed by NWNX usage; the rest of the layout is a hypothesis.

### F3: A/D use TurnOnSpot with a float facing vector
- Binary / hash: n/a (public source)
- What: the same hook reads two floats (x, y) as the new orientation from a `TurnOnSpot` payload.
- Confidence: likely.

### F4: Drive actions carry a bearing
- Binary / hash: n/a (public source)
- What: `CNWSCreature::AddDriveAction(uint16_t nGroupId, const Vector& vPathStart, int32_t nBearing, int32_t nClientPathNumber, int32_t nDriveFlags, int32_t nNumWayPointsToGenerate)`, plus `AIActionDrive` and `DriveUpdateLocation(BOOL bRun)`. `CNWSMessage` has `HandlePlayerToServerInputDriveControl` and `HandlePlayerToServerInputAbortDriveControl`.
- Open: bearing units and range; whether an arbitrary bearing is honored; how facing relates to it. These decide Path A vs. B.
- Confidence: signatures confirmed; semantics unknown.

### F5: Creature movement state fields
- What: `CNWSCreature` has `m_fMovementRateFactor`, `m_fDriveModeMoveFactor`, `m_bDriveMode`, `m_bCutsceneCameraMode`.
- Open: offsets to be extracted in R4.

### F6: Server-to-client camera messages
- What: major `Camera` 0x10. `SendServerToPlayerCamera_ChangeLocation(pPlayer, nFlags, fCameraAngle, fCameraDistance, fCameraPitch, nSmooth)` (minor 0x01), `SendServerToPlayerCamera_SetLimits(pPlayer, fMinPitch, fMaxPitch, fMinDist, fMaxDist)`, and `LockPitch`/`LockDist`/`LockYaw` (0x06–0x08). There is also `CNWSMessage::HandlePlayerToServerCameraMessage`.
- Use: the client-side handlers for these messages write the camera fields we need (R7).

### F7: Console ports used the same protocol
- What: NWN:EE on Switch and Xbox crossplays with PC servers, and there are no controller-specific input minors (F1). Gamepad clients therefore move characters using the messages above.
- Use: supporting evidence for Path A, not proof, since consoles may quantize.

### F8: The client links SDL2 statically, with the dynamic API
- Binary / hash: nwmain-linux 6d19c39b
- What: SDL 2.0.8 is linked into the executable (no SDL in `DT_NEEDED`), and the game calls it directly: one `call SDL_PollEvent` in `Update(void*)` and one `call SDL_GL_SwapWindow` in `GLRender::SwapBuffers(SDL_Window*)`, with no PLT. `LD_PRELOAD` symbol interposition therefore can't reach SDL. The build has SDL_DYNAPI, so every exported `SDL_Foo` is the stub `push %rbp; mov %rsp,%rbp; pop %rbp; jmp *slot(%rip)`. The slots (for example `SDL_PollEvent` at 0x18af388 and `SDL_GL_SwapWindow` at 0x18b0150) are in `.data`, which is writable and outside `GNU_RELRO`.
- Use: the hook layer calls `SDL_GetVersion` to force the jump-table fill, then swaps its wrappers into those two slots (`src/hook/nwpad.c`). The stub is decoded at runtime, so no address or signature is needed.
- Evidence: `readelf -d`, `objdump -d` of the stubs and call sites, `readelf -S -l` for section and RELRO bounds. At runtime the library logged `game SDL 2.0.8` and `first frame` from the hooked swap (run 20260928-212606).
- Confidence: confirmed.

### F9: The client exports its C++ symbols
- Binary / hash: nwmain-linux 6d19c39b
- What: `nwmain-linux` is not stripped and exports 40,051 defined dynamic symbols. They include `CNWCMessage::SendPlayerToServerInput_DriveControl(Vector const&, unsigned int, unsigned short, unsigned char, unsigned char)`, `SendPlayerToServerInput_TurnOnSpot(Vector const&)`, `SendPlayerToServerInput_AlwaysRun(int)`, `SendPlayerToServerInput_WalkToWayPoint(...)`, `CNWCModule::TurnCamera(float, int)`, `TiltCamera(float, int)`, `ZoomCamera(float)`, `SetCameraLimits(float, float, float, float)`, and the global `CurrentCamera`.
- Use: answers Q3 (R2). Client functions can be resolved by mangled name with `dlsym`, as NWNX does on the server, so R3 (version tracking) is mostly unnecessary.
- Evidence: `nm -D --defined-only -C`.
- Confidence: confirmed.

### F10: `-userdirectory` isolates the game's user data
- Binary / hash: nwmain-linux 6d19c39b
- What: the client parses a `-userdirectory <path>` option (`SetUpUserDirectory(int, char**)`, string `-userdirectory`).
- Evidence: launched with `-userdirectory ~/.nwpad/userdir`; the game created `nwn.ini`, `logs/`, `modules/`, and the rest there, and `find` found nothing modified in Robert's `~/.local/share/Neverwinter Nights`.
- Use: isolation approach 1 (plan §8.2); the `run_game.sh` default. Answers the user-directory half of Q7; loading a module or save is still open.
- Confidence: confirmed.

### F11: No dormant controller code in the client
- Binary / hash: nwmain-linux 6d19c39b
- What: the game makes no direct calls to any `SDL_GameController*` function or `SDL_NumJoysticks`. The gamepad strings in the binary are SDL's built-in controller mapping database. No game symbols relate to gamepads.
- Use: answers Q6 (R6): nothing to reuse. The `SDL_GameController*` functions are exported, so the library can call them.
- Evidence: `objdump -d` call-site counts, `strings`, `nm -C`.
- Confidence: likely (indirect calls through the jump table aren't ruled out, but no call site exists).

### F12: The symbolized server ships with the Steam install
- Binary / hash: nwserver-linux d6d95282
- What: `bin/linux-x86/nwserver-linux` is installed alongside the client and exports 18,862 dynamic symbols, including `CNWSMessage::HandlePlayerToServerInputDriveControl(CNWSPlayer*)`, `HandlePlayerToServerInputAbortDriveControl`, `CNWSCreature::AddDriveAction`, `AIActionDrive`, and `DriveUpdateLocation`.
- Use: R1 can start without `tools/fetch_server.sh`, and it's the same build as the client.
- Confidence: confirmed.

### F13: `+TestNewModule` loads a module unattended
- Binary / hash: nwmain-linux 6d19c39b
- What: `nwmain-linux +TestNewModule "<module name>"` starts the in-process server, loads the module, and drops a default character into it with no UI interaction. Other options present as strings: `+LoadNewModule`, `+connect`, `+password`, `+connect_lobby`, `-dmc`, `-noaliases`.
- Evidence: run 20260928-213602 with `"Contest Of Champions 0492"` (a stock module in `data/mod`): the log shows `Loading Module`, and a screenshot (local `artifacts/` only) shows the character in the arena. Build v89.8193.37-17.
- Use: unattended loading, approach 1 (plan §8.2). Answers the rest of Q7. A stock module is enough for camera work until `nwpad_test.mod` exists.
- Note: once in a module, the game ignores SIGTERM (still running after 25 s), so the test fixture falls back to SIGKILL.
- Confidence: confirmed.

### F14: Client camera control (static analysis)
- Binary / hash: nwmain-linux 6d19c39b
- What:
  - `CNWCModule::TurnCamera(float delta, int direct)` and `TiltCamera(float delta, int direct)` return immediately when bit 0x10 (yaw) or 0x20 (pitch) of the dword at `module+0x2bc` is set. (These are not the script camera locks; see F18.)
  - With `direct` = 1 the delta is added to the camera object's yaw at `+0x88` or pitch at `+0x8c`. This is the mouse path: `CClientExoAppInternal::PerformXMouseMoveAction` / `PerformYMouseMoveAction`. With `direct` = 0 it's added at `+0x58` / `+0x60`, which is the keyboard path from `CClientExoAppInternal::UpdateCamera`, driven by a rate the caller decays each frame. A nonzero delta sets bit 0 of `module+0x134` and `module+0x280` = 1.
  - The camera object comes from `module+0xe8`, then virtual call `[0xf0](-1)`, then virtual call `[0x48]`.
  - Units are degrees. The mouse turn delta is `dx × -75 × 0.2 / (screen_width / 1920 / 0.01)`, i.e. -0.15° per pixel at 1920 wide, so moving right decreases yaw.
  - `CNWCModule::GetCameraMinPitch()` / `GetCameraMaxPitch()` read `module+0x2ac` / `+0x2b0`, with a fallback when the value is negative.
- Module access: `g_pAppManager` (exported) points to a `CAppManager` whose first field is the `CClientExoApp*`. All 365 call sites of `CClientExoApp::GetModule()` load it as `mov rdi,[g_pAppManager]; mov rdi,[rdi]`. `CClientExoApp::GetModule()` and `GetModuleCamera()` forward to the internal app at `+0x8`.
- Use: M1 can call `TurnCamera(delta, 1)` / `TiltCamera(delta, 1)` exactly as the mouse does. That uses the game's own setter, and the locks are honored for free.
- Open: does the game clamp direct pitch to the limits? What distinguishes `+0x58` from `+0x88` (current vs. target)? Runtime confirmation is still pending (outer tier step 3).
- Evidence: `objdump -d` of the functions named above; float constants read from `.rodata`.
- Confidence: confirmed at runtime (F15).

### F15: Camera control confirmed at runtime
- Binary / hash: nwmain-linux 6d19c39b
- What (all degrees; offsets on this build):
  - The access path from F14 works in a module: `*g_pAppManager` then `CClientExoApp::GetModule()` gives the module, and `module+0xe8` then vcall `[0xf0](-1)` then vcall `[0x48]` gives the camera object.
  - `cam+0x88` / `+0x8c` (and `+0x58` / `+0x60`) are pending deltas: `TurnCamera(45, 1)` left 45 at `+0x88` while the game was stopped, and the next frame consumed it (back to 0).
  - Absolute yaw is at `cam+0x54` (mirrored at `module+0x12c`), and absolute pitch at `cam+0x5c` (mirrored at `module+0x130`). A +30 turn moved yaw 90 → 120.
  - Pitch runs from 1 (top-down) to 89 (head-on). With the limit fields unset (`module+0x2ac` / `+0x2b0` = -1), `GetCameraMinPitch()` / `GetCameraMaxPitch()` return 1 / 89.
  - The game clamps direct tilts to those limits (11 + 100 → 89; 50 − 200 → 1). One oddity: +200 from 50 left pitch at 50. That's irrelevant to us, because the core clamps before sending.
  - The lock dword `module+0x2bc` was 0 with no script locks.
- Evidence: gdb attach (batch) in `Contest Of Champions 0492`, calling the functions above and diffing 1 KiB dumps of the camera and module objects across each call. Robert watched the view jump 45°, then 30°, then go head-on and top-down.
- Use: the M1 backend reads yaw and pitch from `cam+0x54` / `+0x5c`, the limits from the getters, and the locks from `module+0x2bc`, and applies per-frame deltas through `TurnCamera` / `TiltCamera` with `direct` = 1.
- Confidence: confirmed.

### F16: What the client sends for each kind of movement
- Binary / hash: nwmain-linux 6d19c39b, nwserver-linux d6d95282
- Server handler (disassembly of `CNWSMessage::HandlePlayerToServerInputDriveControl`): it reads `float x, float y` (32 bits each), `ReadOBJECTIDServer`, `ReadWORD(12 bits)`, `ReadBYTE`, `ReadBYTE`. The object id must equal the creature's current area id. It then clears actions and calls `AddDriveAction(0xffff, {x, y, 0}, word, byte1, byte2, 2)`. The client sender `SendPlayerToServerInput_DriveControl(Vector const&, uint, ushort, uchar, uchar)` writes the same fields in the same order (major 6, minor 0x1d). Its only caller is `CClientExoAppInternal::UpdateDriveMode`.
- Runtime capture (gdb breakpoint on the client sender, keys held with xdotool, in `Contest Of Champions 0492`):
  - The word is the bearing in tenths of a degree (900 = 90.0°). It follows the camera: turning the camera changed the next W packets' bearing (900 → 1416). But it isn't equal to the F15 yaw field: at spawn (run 20260928-232916) the bearing was 900 while `cam+0x54` read 0.0, which suggests bearing = camera yaw + 90°. That still needs checking at other yaws. It doesn't change while strafing or backpedaling.
  - byte1 is a per-packet sequence number. byte2 is the drive flags: W = 3, S = 2, Q = 4, E = 8, and W+Q = 7. The packet `x, y` is the creature's current position.
  - Packets repeat about every 0.1 s while a key is held. With bearing 90: W moved +Y, S −Y, Q −X, E +X.
  - So keyboard driving means "face the camera yaw; move forward, back, or strafe relative to it". Free facing exists in the protocol, but relative movement directions come only from the flag combinations.
- Mouse: holding the left button on the ground (Robert: click-and-drag moves in any direction) sends `SendPlayerToServerInput_WalkToWayPoint` about every 0.14 s, not DriveControl. That's pathfinding to a point, with the character facing its path. In this capture all 24 packets had the same target, so whether the synthetic pointer moved during the drag isn't settled. A/D send `SendPlayerToServerInput_TurnOnSpot` with a continuous facing vector.
- Bearing override (gdb rewriting the bearing and flags of keyboard packets): inconclusive. The packet positions are the client's own prediction, which uses its real bearing, so client and server disagree. Bearing 0 + W moved +X, and bearing 0 + S moved −X, but 90 / 180 / 45 gave inconsistent directions. Settling this needs the server-side creature position (R4, the socket `state`) and a clear area.
- Camera coupling: with W held and every packet's bearing rewritten to the keyboard bearing − 45° (run 20260928-232916), the camera yaw stayed at 0.0 for the whole 2 s. In the default camera mode, driving doesn't rotate the camera. Chase-cam mode (`CNWCModule::UpdateCameraModeChaseCam`) is untested. The character moved at about 64°, between the client's 90° and the rewritten 45°, which is the same client-prediction conflict as above.
- Confidence: layout and keyboard semantics confirmed; arbitrary-bearing honoring open.

### F17: Script chunks from the client (cheat message 0x1d)
- Binary / hash: nwserver-linux d6d95282, nwmain-linux 6d19c39b
- What:
  - Server: `CNWSMessage::HandlePlayerToServerCheatMessage` (Ghidra, `re-work/`) refuses cheat minors with "Cheat/debug command not allowed: Not a DM, or not in DebugMode" unless `CServerExoApp::GetDebugMode()` is on, or the player is a DM (net-layer player info +0x24), or `g_pAppManager+0x28` is 0 and the minor isn't one of two special-cased minors (table at 0xc4f06d, not yet read).
  - Minor 0x1d reads a `CExoString` chunk, an object id, and a BOOL, then calls `CVirtualMachine::RunScriptChunk(chunk, oid, valid, wrap)`.
  - Client: `CNWCMessage::SendPlayerToServerCheat_RunScriptChunk(CExoString const&, unsigned int oid, int wrap)` builds that message. `CClientExoApp::GetNWCMessage()` returns `internal->[+0x208]`. `CExoString` is `{char *str; uint32 len}` (the sender reads `[+0]` and `[+8]`).
- Use: a debug-only `script_chunk` socket command, so tests can apply camera locks and cutscenes without a custom module. Whether single-player sessions pass the gate is being checked at runtime.
- Evidence: Ghidra decompile of the server handler; disassembly of the client sender and getter.
- Confidence: confirmed (the single-player gate passes; F18).

### F18: Camera locks and effective limits live in the camera object
- Binary / hash: nwmain-linux 6d19c39b
- What: script locks collapse ranges in the camera object (floats, degrees):
  - Yaw: `cam+0x68` / `+0x6c`, 0 / 360 unlocked. `LockCameraDirection(TRUE)` sets both to about 2.5e-6, and `cam+0x64` goes 0 → 1.0 while locked.
  - Pitch: `cam+0x74` / `+0x78`, 1 / 89 unlocked. `LockCameraPitch(TRUE)` sets both to the current pitch (50).
  - Distance: `cam+0x80` / `+0x84`, 1 / 25 unlocked. `LockCameraDistance(TRUE)` sets both to 20.
  - Unlocking restores the defaults.
- Corrects F14: bits 0x10 / 0x20 of `module+0x2bc` didn't change under any of these locks, so they're something else (open). `GetCameraMinPitch()` / `GetCameraMaxPitch()` aren't needed: the backend reads the effective pitch range from the camera and treats a collapsed range as locked.
- Also: script chunks run in a single-player `+TestNewModule` session (F17's gate passes). `SetCameraFacing(45.0, 10.0, 30.0, SNAP)` moved the yaw field to -45.0 and pitch to 30.0. With the spawn data (yaw field 0, drive bearing 90°, W moving +Y, creature facing 90°), this gives camera forward (degrees counter-clockwise from +X, the NWScript convention) = yaw field + 90°. The DriveControl bearing (F16) is that same forward direction.
- Evidence: socket `read` dumps (1 KiB of module and camera) diffed across script lock toggles, run 20260928-234244.
- Confidence: confirmed.

### F19: Server-side player creature
- Binary / hash: nwmain-linux 6d19c39b
- What: `g_pAppManager` is `{CClientExoApp*, CServerExoApp*, ...}` (the server app at +0x8 matches NWNX's layout and the server code's `g_pAppManager + 8` in F17). `CServerExoApp::GetFirstPCObject()` returns the first player's object id, or 0x7f000000 if there is none (disassembly: it walks the player list and reads each player's `+0x64`). `CServerExoApp::GetCreatureByGameObjectID(oid)` returns the `CNWSCreature*`. Both forward to `CServerExoAppInternal` through `+0x8`.
- Offsets on `CNWSCreature` (from `read server_pc` dumps across movement, run 20260928-234835): position x/y at `+0xa4` / `+0xa8` (z presumably `+0xac`), unit facing vector at `+0xb0` / `+0xb4`. Spawn was (20, 20) facing (0, 1). Holding W for 1 s moved y to 25.24; E for 1 s moved x to 22.27 with facing unchanged; A for 0.5 s turned facing to (-1, 0).
- Use: ground truth for movement tests, exposed as `state.creature` (facing in degrees counter-clockwise from +X).
- Confidence: confirmed.

### F20: Client walk and drive entry points (Ghidra, client)
- Binary / hash: nwmain-linux 6d19c39b
- `CClientExoAppInternal::UpdateDriveMode()` (the per-frame keyboard drive):
  - Key state is kept in the internal app: `+0x1b4` / `+0x1b8` / `+0x1bc` (forward and back variants), `+0x1c0` / `+0x1c4` (A / D, which send `TurnOnSpot` with the facing rotated), and `+0x1c8` / `+0x1cc` (strafe; they add flag 4 / 8).
  - The DriveControl bearing is the client creature's facing, `atan2(creature+0x48, creature+0x44)` in tenths of a degree. The position is `creature+0x38/+0x3c/+0x40`, and the per-packet sequence is `creature+0x2f2`. The camera doesn't enter directly, so something else turns the creature to the camera while driving (F16's observation).
  - A separate branch sends `WalkToWayPoint` one unit sideways or behind when `creature[0x39e] == 2` (an alternate control scheme), not for mouse drag.
- `CClientExoAppInternal::WalkPlayerToPoint(x, y, z, int mode, uint target_oid, int ring)` is the mouse's walk entry, called from `PerformLButtonDownAction` with `mode = (arg != 0) * 2` and `target_oid = 0x7f000000` for ground. It:
  - returns early (throttled) if called within 1000 ms of the last walk (`+0x5b4`) with a nearby target (`+0x5a8` / `+0x5ac`);
  - tests line of sight and inter-tile path depth (plays `gui_nowalk` on failure);
  - runs `CNWCCreature::ClientSideWalkCommand` (the client's prediction) and sends `WalkToWayPoint`.
  - `mode` 0 follows Always Run and Shift; 2 forces the run/walk flag passed to `ClientSideWalkCommand`. Which way it forces is to be checked.
  - `ShowWalkToRingVFX(x, y, z, ring)` returns immediately when `ring` = 0, so passing 0 shows no ring.
- `CClientExoApp::GetPlayerCreature()` forwards to the internal app (`+0x8`).
- Use: drag mode calls `WalkPlayerToPoint(point ahead along the stick, mode, 0x7f000000, 0)`. Strafe and backpedal set the drive key fields.
- Confidence: likely (decompile); runtime checks follow.

### F21: The keyboard movement handler
- Binary / hash: nwmain-linux 6d19c39b
- What: `CClientExoAppInternal::HandleInputEvent(int action, int pressed, int, int)` (Ghidra) handles the movement actions: 0x57 Q → `+0x1c8`, 0x58 E → `+0x1cc`, 0x59 → `+0x1b4`, 0x5a W → `+0x1b8`, 0x5b S → `+0x1bc`, 0x5c A → `+0x1c0`, 0x5d D → `+0x1c4`. Each press stores `pressed`, stamps timers (`+0x198`, `+0x1a8`), and sets `+0x1b0` = 1 so the next `UpdateDriveMode` sends at once. Releasing Q / E / S / W sends `SendPlayerToServerInput_AbortDriveControl`. The preamble drops events while a UI text field has focus, but only when the 4th argument is nonzero.
- Runtime (gdb breakpoint, xdotool S held): real key events arrive as `(0x5b, 1, 0, 0)` repeated by autorepeat (11 in 1.2 s), then `(0x5b, 0, 0, 0)`.
- Facing: neither a real S nor setting the fields turns the character toward the camera. Q / E / S move relative to the creature's own facing (camera forward 80.5°, facing 89°: S moved at facing + 180°). This is what the M2 option (b) builds on.
- Setting the fields directly (socket `drive_keys`) moves the character exactly like the keys (S: 3.6 m at facing + 180°; Q / E: 3.2 m at facing ± 90°; facing unchanged), but skips the release's AbortDriveControl, so the library calls `HandleInputEvent` instead.
- Also measured: `WalkPlayerToPoint` mode 1 walks (1.85 m/s steady) and mode 2 runs (3.85 m/s); mode 0 follows the game default. Where the game keeps Always Run is still open (M4); `client_internal+0x184` read 0 here.
- Confidence: confirmed.

### F22: Mouse drag mode and how it ends
- Binary / hash: nwmain-linux 6d19c39b
- What: the byte `CClientExoAppInternal+0x140` is an input mode. It's 1 while the mouse drags, and `UpdateDriveMode` sets it to 2 temporarily. `CClientExoAppInternal::PerformLButtonUpAction()` (Ghidra) sends `CNWCMessage::SendPlayerToServerInput_StopDragMode()` when the mode is 1, then sets the mode to 0. `WalkPlayerToPoint` skips its line-of-sight test and passes the mode on in drag mode (F20).
- Why it matters: stopping a drag by re-targeting the client's position walks the character back, because the client's position trails the server's by about 0.5 m while running (run 20260929-070241: client y 21.83 vs server 22.38; the server then slid back to 21.90 over 0.7 s). The library drags in mode 1 and stops with StopDragMode, as the mouse does.
- Also measured there: releasing the game's own S key stops in about 0.37 s (0.07 m of travel after 300 ms), which is native key behavior.
- Stop measurements (run 20260929-075304, server and client positions):
  - Re-targeting the client position stops the rendered character at once, but the server's copy (about 0.5 m ahead while running) slides back about 0.5 m over 0.7 s.
  - StopDragMode alone doesn't stop the walk: the character still goes to the last target (1.3–2.4 m with a 2 m lookahead).
  - Robert's method, a forward key tap after ending the drag (`HandleInputEvent(0x5a, 1)`, then `(0x5a, 0)` 60 ms later, which sends AbortDriveControl): server and client stop together (final gap 0.01 m, no slide-back) at 0.43 s, after 1.1–1.75 m of running momentum. That's about the same as releasing the real keys (0.37 s).
- Client vs. server facing: after moves, the client's facing can settle several degrees off the server's (83.7 vs. 90, 96.6 vs. 90). The drive uses the client's copy.
- Walk stops (Xbox pad feel test): the forward tap's drive runs, so from a walk it surged about 1.7 m after release, whatever the stick deflection (0.07 and 0.3 both). The backend now stops walks by ending the drag and re-targeting the client's position. The on-screen character stops at once (0.03–0.07 m). The server, which trails the client at walking speed, catches up forward to the same spot (final gap 0.00–0.01 m). Runs keep the forward tap.
- Stop-tap race: the tap's drive packet carries the client's position, and the server's drive starts from it. A server-side teleport (script `JumpToLocation`) landing within about 60 ms of a stick release gets undone. The soak's scripted reset hit this 2 times in 3 runs until it waited 0.5 s after release; with the wait, a 10-minute soak passed (3839 moves, 122 stops). A real key held through a teleport has the same exposure, since it's the game's own drive protocol. One earlier failure (server jumped, client stayed about 12 m away) is probably the same race.
- Drag lookahead: with the target 2 m ahead of the (lagging) client position, full stick ran at 2.9–3.0 m/s against 4.0 m/s for a far WalkPlayerToPoint target; the server eases off near its target. With 5 m ahead, the stick runs at the game's full speed. The stop tap makes the lookahead irrelevant on release.
- Confidence: confirmed.

### F23: Always Run
- Binary / hash: nwmain-linux 6d19c39b
- What: `CClientOptions::SetAlwaysRun(int)` (Ghidra) stores the value at `CClientOptions+0x4`, but only after `SendPlayerToServerInput_AlwaysRun` succeeds. `CClientExoApp::GetClientOptions()` returns the options object. The console command `setalwaysrun <n>` calls the same setter. `UpdateDriveMode` reads `[*internal]+4` to choose the W drive flags (F20), which suggests the internal app's first field is the same `CClientOptions*`.
- Use: the backend reads `+0x4` for Always Run (plan §3). The walk/run choice for drag is ours: WalkPlayerToPoint mode 1 walks, mode 2 runs (F21). Strafe and backpedal speed stays whatever the game's keys do.
- Runtime: toggling it with `SetAlwaysRun` through the test socket changes `+0x4`, and with it on, a 0.3 deflection runs at the game's run speed (live `test_walk_run`).
- Confidence: confirmed.

### F24: The game's mouse cursor
- Binary / hash: nwmain-linux 6d19c39b
- What: the game uses SDL's hardware cursor: one `SDL_CreateColorCursor` and one `SDL_SetCursor` call site. `SDL_ShowCursor` has four direct call sites: `main` (hide at startup), `CGuiMan::ShowMouse()` (1), `CGuiMan::HideMouse()` (0), and `CCachedMouseCursorManager::SetMouseCursor(...)` (1). The last shows the cursor again every time its shape changes, for example when hovering a door or NPC.
- Use: the library hooks `SDL_ShowCursor` through the dynamic-API jump table (like F8) and records what the game asks for. While a stick is in use it keeps the cursor hidden and holds back the game's show requests. The first real mouse motion restores the game's last request. `hide_cursor = 0` in the config turns this off.
- Evidence: `objdump` call-site survey. At runtime (run 20260929-112305), a virtual stick hid the cursor with `game_wants` still true, it stayed hidden after release, and uinput mouse motion restored it. Robert confirmed it visually.
- Confidence: confirmed.

### F25: Launching through Steam
- Binary / hash: nwmain-linux 6d19c39b (Steam buildid 20277208)
- What: with the launch option `LD_PRELOAD="$HOME/.local/lib/nwpad/libnwpad.so:$LD_PRELOAD" %command%`, Steam runs the native client inside Steam Linux Runtime **soldier** (pressure-vessel: `reaper` → `srt-bwrap` → `pv-adverb` → `nwmain-linux`). The preload reaches the game ahead of Steam's own `gameoverlayrenderer.so`, and the library loads, hooks SDL 2.0.8, and resolves 10/10 signatures. Its log lines go to `~/.local/share/Steam/logs/console-linux.txt`. The container's helper processes inherit the preload too; they log "game SDL2 not found; hooks not installed" and stay inert.
- Portability: built on glibc 2.44, the library first required GLIBC_2.43 (`atan2f`), 2.38 (`fmodf`), and 2.34 (`dlsym`, `dladdr`). pressure-vessel uses the newer of the host's and the runtime's glibc, so on an older host such as the Steam Deck it would fail to load. `src/glibc_compat.h` pins those symbols to the x86-64 baseline (GLIBC_2.2.5), leaving GLIBC_2.17 as the newest requirement; CI checks this.
- Evidence: run through `steam -applaunch 704450` on the box (2026-09-29): `/proc/<pid>/maps`, environ, process ancestry, and the Steam console log.
- Confidence: confirmed.

### F26: Edge turning and the game's recorded pointer
- Binary / hash: nwmain-linux 6d19c39b
- What: with `camera.edge-turning = true` (settings.tml; the INI calls it "ScreenEdgeCameraTurn") in fullscreen, the camera turns at about 75°/s while the pointer is on the outermost pixel column. At x = 0 it turns; from x = 1 on it doesn't (measured one pixel at a time at 3840×2160). Windowed mode doesn't edge-turn.
- The check uses the game's own record of the pointer: `CClientExoAppInternal+0x120` / `+0x124` (int x, y). `PerformXMouseMoveAction` / `PerformYMouseMoveAction` store them from mouse input, clamped to the GUI size at `g_pGuiMan+0xb8` / `+0xbc`. They were the only fields of the internal object that changed between centre and edge.
- `SDL_WarpMouseInWindow` doesn't help under KWin's XWayland: SDL records the new position, but the real pointer stays put, the camera keeps turning, and SDL's position then sticks.
- Use: while a stick is in use, if the recorded pointer is on the leftmost or rightmost column, the library moves the record one pixel in (only left and right: edge turning does nothing at the top or bottom, per Robert). The camera stops, the OS pointer isn't touched, and the next real mouse motion overwrites the record (Robert's request).
- Evidence: runs 20260929-120909 to -121641. Before the fix, the left edge kept turning after a stick release; after it, the record read x = 1 and the turn rate was 0.
- Confidence: confirmed for both edges (the right edge after F27).

### F27: Right-edge turning is unreachable under 2x desktop scaling
- What: on the box (TV output at scale 2, `kwinrc [Xwayland] Scale=2`, X screen 3840×2160) the real pointer never gets past X = 3838. A relative mouse pushed right, even one unit at a time, stops there, and the game records x = 3838. The game's right-edge turning needs x = 3839 (width − 1), so it can never trigger; the left edge (0) works. KWin keeps the pointer in whole logical pixels (0–1919), and XWayland doubles them, so X coordinates are even. Robert saw the same with his own mouse.
- Fix (library): in the PollEvent hook, a mouse-motion event at x = width − 2 moving right (xrel > 0) is reported to the game as x = width − 1. `state.events.right_edge_fixes` counts them. Where the last pixel is reachable, the only effect is a 2-px right edge zone. The alternative, the TV at 100% scale, would shrink the whole desktop.
- Build gate: this correction exists only in builds with `NWPAD_XWAYLAND_EDGE_FIX` (a CMake option, on by default on Linux, absent on other platforms).
- Symmetry with the left edge (Robert): after a stick nudge the pin stays set, so the next mouse motion, even purely vertical, reports the edge again and turning resumes, as it does on the left. Moving vertically at the edge, sub-pixel rounding lands the pointer on either 3838 or 3839 (seen: x = 3839 xrel +1, then 3838 xrel −1), so the pin only clears when the pointer moves left of both columns.
- Sticky (after Robert's test, where the right edge only caught fast flicks): a real mouse pressed against the edge keeps sending motion events at x = 3838 with no horizontal movement, and each one reset the game's record to 3838. The correction now pins the record at width − 1 from the first rightward arrival until the pointer leaves that column. (Superseded: a stick nudge no longer clears the pin; see Symmetry above.)
- Evidence: run 20260929-124722 and after: right edge turns at −76°/s (game pointer 3839), steady under simulated hand jitter (2 s), and a stick turn plus release stops it (record nudged to 3838).
- Confidence: confirmed.

### F28: Steam Deck bring-up (SteamOS 3.8.28)
- Binary / hash: nwmain-linux 6d19c39b on the Deck too (buildid 20277208), native, no Proton mapping.
- glibc 2.41 on the Deck: the unpinned build (GLIBC_2.43) wouldn't have loaded; the pinned release (F25) does.
- Layout: Robert's Deck layout for NWN:EE is the workshop "Neverwinter Nights Console Port" (3095778009), cloud-synced (a copy shows up on the box). `steam-input/deck_from_console_port.py` switched its left stick from arrow keys (group 1) to its own `joystick_move` group 30, and its right stick from the companion radial menu (group 51) to a new `joystick_move` group 52. The original is kept as `controller_neptune.vdf.bak-nwpad-20260929173317`.
- A first load of the quicksave "XP1-Chapter 1" crashed on the Deck with nwpad active (the log stopped after the module load and NUI font bake; there was no core dump and no crash report, because the game installs its own crash handlers). It loaded with `NWPAD_DISABLE=1`, loaded on the box (debug build under gdb, same save copied into the test profile), and then loaded on the Deck with nwpad active and the crash-trace build. It hasn't reproduced since. The release build now includes a chained crash handler (`NWPAD_CRASH_TRACE`, `src/hook/crashtrace.c`) that logs the signal, address, pc, the library's current step, and a backtrace to the Steam log before handing on to the game's handler.
- Steam on the Deck, restarted over SSH: it needs the Plasma session's `DISPLAY` / `XAUTHORITY` / D-Bus environment (without `XAUTHORITY`, `steam -applaunch` fails with "Unable to open X11 display"), and the first start right after a shutdown sometimes exits on the single-instance lock, so retry once.

### F29: The settings registry and the Options window (settings plan S1, S3, S4, S5)
- Binary / hash: nwmain-linux 6d19c39b
- **Registry (S1):** `CExoConfig` (164 exported methods) is a central registry: a `std::map` from key path to `CExoConfig::BindConfigBase*` (tree header at `+0x60`), plus the parsed `settings.tml` as a `cpptoml::table` at `+0x38`. `CExoConfig::Initialize()` (Ghidra) walks every binding, resolves its key (and each alias in the binding's vector at `+0x108`/`+0x110`) in the TOML table, and applies the value through the binding's virtual methods. Bindings are the templates `CExoConfig::BindConfig<bool|double|long|std::string>`, whose methods are exported (`GetDefault`, `IsDefault`, `IsModified`, `ResetToDefault`, `Constrain`, `ApplyConstraints`, `StepSize`, `MakeSchema`, `ReadFromIni`/`WriteToIni`, `OverrideForSingleplayer`, `RequestTemporaryOverride`, `Rollback`, ...). No constructor or registration function is exported: bindings are built by inlined template code in static initializers.
- **Options window (S3):** `Nui::ConfigWindow` (constructor at 0x6d47b0, 51,739 bytes) builds its tabs from a hand-authored key list, about 200 string literals: graphics 83, server 36, ui 23, ruleset 19, game 9, camera 9 (`camera.mode`, `camera.edge-turning`, `camera.turn-speed-multiplier`, ...), input 3, sound 5, and a few debug and tooling groups. Group names include "Frame Limiter", "Tweaks", "Mouse Cursor Scale Override", "LOD", and "SQLite Tracing". Its filtering (`std::remove_if` with predicates built from `(SelectedTab, unsigned int, bool)`) runs over that list. A registered key that isn't in the list doesn't appear in the window.
- **Labels (S4):** `Nui::ConfigWindow::PreprocessOpt(tab, opt, prefix)` derives the displayed label from the key. It strips the tab prefix, replaces `.` and `-` with spaces, and drops a trailing `.enabled`. `settings.tml`'s embedded schema also carries per-key `label` and `translations` entries. No talk-table strrefs are needed for labels.
- **Unknown keys (S5):** with an extra `[nwpad]` section (`enabled = true`, `[nwpad.camera] turn-speed = 200.0`) in the test profile's `settings.tml` and the library inert, the game rewrote the file in its own normalized form and kept the section: sorted into place before `[nwscript]`, with the float written as `200.00000000000000`.
- **Open:** S2 (load order relative to the library's first frame) and S6 (change notification; `std::function` managers exist in `CExoConfig`, not yet traced).
- Confidence: registry and Options window structure confirmed (disassembly and Ghidra); S5 confirmed at runtime.

### F30: Registering a setting and adding it to the Options window (settings plan N2 spike)
- Binary / hash: nwmain-linux 6d19c39b
- **Correction to F29:** registration *is* exported: weak `CExoConfig::Bind<T>(const std::string& key, const T& default, std::function<void(T)> on_change)` for `bool` (0x4f53b0), `double` (0x4f8920), `long` (0x4f4c70) and `std::string` (0x4f43d0), returning `BindConfig<T>*` (600-byte object, vtable `_ZTVN10CExoConfig10BindConfigIdEE`). `Bind` returns NULL if the key is already bound; otherwise it `new`s the binding, inserts it in the map at `+0x58` (header `+0x60`), stores the callback (S6: change notification exists), and writes the key into the live TOML table at `+0x38`, keeping a value already there and otherwise using the default (Ghidra).
- **Instance and order (S2):** `CExoBase::CExoBase` allocates the `CExoConfig` (0xd8 bytes) and stores it at `CExoBase+0`, so the instance is `*(CExoConfig**)g_pExoBase` (also used that way by `ConfigWindow::RenderContent`). `main` calls `DefineClientConfiguration()` (34 `Bind<double>` calls among others), then `DefineServerConfiguration()`, which ends with `CExoConfig::Initialize()`: apply TOML values to every binding, snapshot the table to `+0x28`, `Commit`. The library's first frame is after all of this. Because `Initialize` leaves unknown keys in the `+0x38` table (F29 S5), a `Bind` on the first frame should find nwpad's persisted value already in the table (to confirm at runtime).
- **Options window structure:** the constructor builds, on the stack, `ConfigTab` (0x58 bytes; groups vector at `+0x20`), `ConfigGroup` (0x58; title from `Translate(strref)`, e.g. 0x1b61f/0x1b620 around the camera group; opt vectors at `+0x20` and `+0x38`), `ConfigOpt` (0x148; key string at `+0`, a second string at `+0x20`, flags at `+0x30`/`+0x31`/`+0x131`/`+0x141`), then assigns the tabs to `this+0x1d8`, filters with `remove_if`, and runs `PreprocessOpt` (0x6beaa0) on each option.
- **Rendering is immediate-mode:** `ConfigWindow::RenderContent` (0x6d2380) walks `this+0x1d8` every frame (Nuklear). An option appended to a group's vector after construction should therefore draw on the next frame with no layout rebuild.
- **Finding the window:** `Nui::Window::s_by_identifier` (0x1a00340) is the static map of open windows. Superseded by the `OnOpened` slot (below).
- **Labels:** `PreprocessOpt` (0x6beaa0) sets the label at `ConfigOpt+0x20` to `key.substr(prefix.size())` without checking the prefix, drops a trailing `.enabled`, turns `.`/`-` into spaces and Title-Cases it. `RenderBind<double>` (0x6cf010) shows `TranslateUTF8(binding+0x60)` if that strref is non-zero, else the plain `ConfigOpt+0x20` label. A late option therefore needs its label written directly (plain text works; `Bind` leaves `+0x60` = 0).
- **Descriptions are strref-only:** `ConfigDescriptionWindow` (0x6c7ce0) shows `Translate(binding+0x64)`; the game's own bindings set `+0x60` (e.g. `camera.turn-speed-multiplier` = 0x10779) after `Bind`/`Constrain`/`MapIni` in `DefineClientConfiguration`. Plain-text descriptions would need a talk-table hook.
- **Rendering a binding:** `RenderBind` finds the binding by the option's key and dispatches on its type (`dynamic_cast` to `BindConfig<long|bool|double|string>`); ranges come from `Constrain` (`+0x140`...), Reset compares `CExoConfig::Get<double>` (0x4f9120) with the default at `+0x138`, and `+0xa0` = 0 shows "Value will not persist over restarts." (`Bind` sets it to 1). `Get<T>`/`Set<T>` are exported for bool, double, long and string.
- **Options search exists** (correction to the line this replaces): the window has a search box above the tabs ("Type and press enter to search."). It filters the same row vectors, so appended rows are found by their labels (runtime: "controller" lists all five nwpad rows).
- **Working and committed tables:** `+0x28` is the working table and `+0x38` the committed one. `Set<T>(key, v, commit)` (0x5089c0 for double) writes the working table (after `ApplyConstraintsToValue`) and commits only if `commit`; `Get<T>(key, committed)` reads either. `Commit(keys)` copies working → committed; `BindConfig<T>::Rollback` does `Set(key, Get(key, true), false)`. The Options window's rows always `Set(..., false)`.
- **Save and Cancel:** `ConfigWindow+0x200` is a `vector<std::string>` of every key the window shows, built in the constructor after filtering. Save (GetLayout lambda #4) sets `+0x1f0`; `OnClosed` (0x6be720) then commits those keys, otherwise rolls them back. Appended rows must also append their keys there, or Cancel leaves their edits in place.
- **Constraints on rollback:** Rollback re-applies the constraints, so a `StepSize` whose grid misses the default moves it (run point 0.7875 with step 0.025 became 0.775); steps must hold the defaults.
- **Runtime (2026-09-29, test box):** a first-frame `Bind` picks up the persisted `[nwpad]` values; five rows appended to Input > Camera render and behave natively (checkbox/slider, orange while pending, blue when non-default, per-row reset, Save writes `settings.tml`, Cancel rolls back, values survive a relaunch), from both the main-menu and in-game Options; no crashes across repeated open/close. Cost: reading the values with `Get<T>` every frame took the library's own p50 from 10 to 60 µs; they're now delivered by change callbacks instead (below), and the rows are added from the window's `OnOpened`.
- **Change notification (S6):** `CExoConfig::Update()` (0x4df990) runs every frame from `CClientExoAppInternal::MainLoop` and returns at once unless `CExoConfig+2` (dirty, set by `Set`) is on. When dirty it walks the bindings and, for each with `+0x50` (changed; set by `Set` and by `Bind`), resolves the working value and calls the binding's wrapper at `+0x30`, which calls the `std::function<void(T)>` given to `Bind` with the value (`_M_invoke(const _Any_data&, T&&)`), then clears `+0x50`. So the callback fires on every row edit, reset and Cancel rollback, and once on the first `Update` after `Bind`. nwpad passes a hand-built `std::function` (functor = entry index, stored in place; manager handles get-pointer/clone/destroy).
- **Options window opening:** the `enable_make<Nui::ConfigWindow>` vtable has 15 slots (dtors, `OnOpened`, `OnClosed` (ConfigWindow's), `OnCollapsed`, `OnUncollapsed`, `OnFocusGained`, `OnFocusLost`, `OnGeometryChanged`, `Initialize`, `Restore`, `Persist`, `Update`, `Render`, `HandleModalEscKey`). `OnOpened` (slot 2) is the empty `Nui::Window::OnOpened` (0x6bbf30, exported), called once per open after the constructor; `GetLayout` isn't virtual (called directly from the constructor). nwpad swaps that one slot (in RELRO: `mprotect` around the write) for a wrapper that adds the rows and calls the original, so only the Options window class is affected and no per-frame work remains.
- **Open:** the `remove_if` predicate and the `ConfigOpt` flags beyond those set (`+0x40` = 1, `+0x41` edit buffer, `+0x141` = 1, as the constructor sets them); the game's schema labels (`"~~schema".binds.<key>.label`) aren't used by this window.
- Confidence: exports and structure confirmed (nm, objdump, Ghidra); runtime behaviour confirmed on the test box (`tools/settings_checks.sh` check 5).

### F31: The quickbar (quickbar plan Q0/Q1)
- Binary / hash: nwmain-linux 6d19c39b
- **Reaching it:** `*g_pAppManager` (CClientExoApp*) → `CClientExoApp::GetInGameGui()` (0x637c70: `*(this+8)+0x90`) → `CGuiInGame` → `+0x60` `CPanelQuickBar*` (0x72a8 bytes, built in `CGuiInGame::Initialize`).
- **Buttons:** `CPanelQuickBar+0xf8` holds 36 `CGuiQuickButton`s inline, 0x328 bytes apart: bank × 12 + slot (banks: plain, Shift, Ctrl). `+0x7298` points at the first button of the visible bank (`ActivateButtonSet(0-2)` swaps which 12 are on the panel). `ClickButton(n)` is `HandleLeftButton()` on visible button n (Ghidra).
- **Button fields:** type byte `+0x160` (values from the `CGuiQuickButton::OBJECTTYPE_*` constants, low byte: 0 empty, 1 item, 2 spell, 3 skill, 4 feat, 6 dialog, 7 attack, 8 emote, 10 mode toggle, 18 command line, 39 associate command, 40 examine, 41 barter, 43 cancel polymorph, 44 spell-like ability, DM types); id word `+0x130` (spell: id in the low 16 bits, metamagic `(w>>31)&0x3f`, class `(w>>28)&3 | ((w>>38)<<2)&4`, per `SetButton_Spell`/`HandleTriggerTooltip`); item object id `+0x10c`; tooltip strref `+0x70` (int, -1 none) used when `+0x128` is 0; command label `CExoString` `+0x188`; icon resref `CResRef` `+0x13c` (spells copy `CNWSpell+0x18`, the icon; `CSpellIcon` adds the metamagic overlay); icon model `CAurObject*` `+0x150`.
- **Names, as `HandleTriggerTooltip` (0x6a6c70) builds them:** spells: metamagic prefix strref (0x104ca–0x104cf for metamagic 1/2/4/8/0x10/0x20) + `CNWSpell::GetSpellNameText()` from `CNWSpellArray::GetSpell(id)` (`*(g_pRules+0xd0)`), plus " (Class)" in the tooltip; items: `CNWCItem::GetName(1)` of `CClientExoApp::GetItemByGameObjectID(+0x10c)`; command line: the label; everything else: `CTlkTable::GetSimpleString(+0x70)`. The tooltip text goes to `CGuiMan::ShowTooltipText` → a `CGuiTextBubble` (`g_pGuiMan+0x178`) → a `CAurString` whose accessors aren't exported, so nwpad builds names itself. Calling conventions: `CExoString`-returning members take the return slot in `rdi`, `this` in `rsi` (`GetSimpleString(ret, *g_pTlkTable, strref)`); `CExoString` is `{char*, u32}`, freed by `CExoString::~CExoString`.
- **Runtime (test character, Contest Of Champions 0492):** bank 0 = Attack (`IR_ATTACK`), Talk To (`IR_DIALOG`), Examine (`IR_EXAMINE`), Stealth Mode (`isk_movsilent`), Light Crossbow, Dagger, Potion of Cure Light Wounds (items: no icon resref), Summon Familiar (`ife_familiar`), Mage Armor (`is_MageArm`), Summon Creature I, Daze, Light; bank 1 slot 0 Ray of Frost; 23 empty.
- **Open:** item icons (composed from the item's appearance; the button's `+0x150` model holds the result); `HandleLeftButton` on a button of a bank that isn't visible.
- Confidence: layout confirmed by disassembly/Ghidra and at runtime (`quickbar` control command).

### F32: Client-side NUI windows (quickbar plan Q0.5)
- Binary / hash: nwmain-linux 6d19c39b
- **Server messages:** `CNWCMessage::HandleServerToPlayerNuiEvent(subtype)` (0x7b09d0; dispatched from `HandleServerToPlayerMessage`, which strips the 3-byte packet header and calls `SetReadMessage(data+3, size-3, 0xffffffff, 1)`). Subtypes: **1 create**: INT token, CExoString window id, INT kind (0: definition from a resource by CResRef; 1: inline JSON), JSON; **2 destroy**: INT token; **4 binds**: INT count, then per bind INT token, CExoString name, JSON value; 5: INT token + CExoString (not traced). Returns true if the message read cleanly. Windows are kept in `CGuiInGame+0x890` (`unordered_map<int token, shared_ptr<JsonWindow>>`) and `Nui::Window::s_by_identifier`.
- **JSON on the wire:** `CNWMessage::ReadJSON` = `ExpectType(0x10)`, DWORD length, that many bytes of **UBJSON** (nlohmann `from_ubjson`); `WriteJSON` mirrors it with `WriteType(0x10)`.
- **Building a message locally:** a private `CNWMessage` (constructor exported; under 0x80 bytes), `CreateWriteMessage(size, 0xffffffff, 1)`, `WriteINT`/`WriteCExoString`/`WriteType`+`WriteDWORD`+`WriteVOIDPtr`, `GetWriteMessage` (the buffer starts with 3 bytes reserved for the packet header), then `SetReadMessage(data+3, size-3, 0xffffffff, 1)` and the handler with the subtype. The handler only uses the base `CNWMessage` read methods.
- **Schema** (from the base game's `nw_inc_nui.nss`, read for reference only): window `{version:1, title, root, geometry{x,y,w,h}, resizable, collapsed, closable, transparent, border, accepts_input, size_constraint, edge_constraint, font}`; elements `{type, label, value, ...}` (`col`/`row` with `children`, `label` with `text_halign`/`text_valign`, `image` with value = resref and `image_aspect`/`image_halign`/`image_valign`), common `id`, `width`, `height`, `draw_list` + `draw_list_scissor` (items: `type`, `enabled`, `color`, `fill`, `line_thickness`, `order`, `render`, `arrayBinds`, plus `rect`/`image`/`text` by kind); values may be `{bind, number_flags, number_precision, text_flags}`.
- **Events to the server:** `JsonWindow::FlushQueues` sends `s_event_queue` and `s_bindupdate_queue` (global, all windows) every frame via `SendPlayerToServerNui_Events/_Binds`. `JsonWindow::OnOpened`/`OnClosed` (vtable `enable_make<Nui::JSON::JsonWindow>`) queue `_window_` "open"/"close" events with the token at `JsonWindow+0x1e0`. With `accepts_input: false`, hovering and clicking queue nothing. nwpad swaps the two slots for wrappers that drop the event for its own tokens (`0x6e77xxxx`) and call the original otherwise.
- **Runtime:** a window created this way renders natively (icon by resref and label by bind); before the filter, the local server logged `HandlePlayerToServerNuiEvent: Update invalid: window does not exist: <token>` once per create; with it, nothing, while a window with token 5 still reaches the server (live test `test_nui.py`).
- **Unused binds leak:** setting a bind that no element of the window uses makes the client send that bind back to the server (`Nui::JSON::DynamicBinding::NotifyParent` → `s_bindupdate_queue`); one server log line per such bind. Binds the window uses don't. nwpad only sets bind names that appear in the window's definition.
- Confidence: confirmed by disassembly/Ghidra and at runtime in single-player. Not yet tried against a remote server.

### F33: The quickbar picker (quickbar plan Q2)
- Binary / hash: nwmain-linux 6d19c39b
- **Centring:** NUI geometry is in GUI units: screen size (`g_pGuiMan+0xb8/+0xbc`, pixels) over `CAurora::GetGUIScale()` (static, returns float).
- **Using a button:** `CGuiQuickButton::HandleLeftButton` on any of the 36 buttons acts as clicking it: modes toggle at once (Stealth Mode, runtime), targeted spells enter the game's targeting mode as a click would.
- **Runtime:** the ring (12 icons in a client-side NUI window, draw-list images, circles and text, `accepts_input: false`, transparent, no title) renders natively and updates through binds; no NUI message for nwpad's tokens reaches the local server across open, selection changes and close (live test `test_picker.py`).
- Confidence: runtime on the test box (Xbox-pad path emulated by the control socket's virtual stick).

## Conventions to confirm

- **Core angle convention:** degrees, counter-clockwise from world +X, stick +y = forward (`src/core`). The game's camera yaw field (F15) is camera forward − 90° (F18), so the backend must add 90° before core bearing math (M3). Creature facing (F19) already uses the core convention.
- **Right stick sign:** stick right currently decreases yaw (turns clockwise). The game's mouse path also decreases yaw when moving right (F14), so the signs agree; confirm the feel in M1.

## Open questions

| ID | Question | Task |
|---|---|---|
| Q1 | ~~Full `DriveControl` payload layout~~ F16 | R1 |
| Q2 | ~~Does the server honor an arbitrary bearing, and can facing differ from movement direction?~~ Settled by the M2 decision (plan decision log); F16 | R1, R8, M2 |
| Q3 | ~~Does `nwmain-linux` export symbols?~~ Yes (F9) | R2 |
| Q4 | Where does the client store camera yaw, pitch, limits, and locks? (Static answer in F14; runtime confirmation pending) | R7 |
| Q5 | Where does the client store Always Run state? | R7 |
| Q6 | ~~Is there dormant controller code from the console builds?~~ No (F11) | R6 |
| Q7 | ~~Command-line options for loading a save or module?~~ `+TestNewModule` (F13); user directory: `-userdirectory` (F10) | R5 |
| Q8 | What does `m_fDriveModeMoveFactor` control? | R1 |
