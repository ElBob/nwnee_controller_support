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
- Drag lookahead: with the target 2 m ahead of the (lagging) client position, full stick ran at 2.9–3.0 m/s against 4.0 m/s for a far WalkPlayerToPoint target; the server eases off near its target. With 5 m ahead, the stick runs at the game's full speed. The stop tap makes the lookahead irrelevant on release.
- Confidence: confirmed.

### F23: Always Run
- Binary / hash: nwmain-linux 6d19c39b
- What: `CClientOptions::SetAlwaysRun(int)` (Ghidra) stores the value at `CClientOptions+0x4`, but only after `SendPlayerToServerInput_AlwaysRun` succeeds. `CClientExoApp::GetClientOptions()` returns the options object. The console command `setalwaysrun <n>` calls the same setter. `UpdateDriveMode` reads `[*internal]+4` to choose the W drive flags (F20), which suggests the internal app's first field is the same `CClientOptions*`.
- Use: the backend reads `+0x4` for Always Run (plan §3). The walk/run choice for drag is ours: WalkPlayerToPoint mode 1 walks, mode 2 runs (F21). Strafe and backpedal speed stays whatever the game's keys do.
- Runtime: toggling it with `SetAlwaysRun` through the test socket changes `+0x4`, and with it on, a 0.3 deflection runs at the game's run speed (live `test_walk_run`).
- Confidence: confirmed.

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
