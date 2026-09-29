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
  - `CNWCModule::TurnCamera(float delta, int direct)` and `TiltCamera(float delta, int direct)` return immediately when bit 0x10 (yaw) or 0x20 (pitch) of the dword at `module+0x2bc` is set. These are the script camera locks.
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

## Conventions to confirm

- **Core angle convention:** degrees, counter-clockwise from world +X, stick +y = forward (`src/core`). Confirm the game's yaw direction and zero point in M1, and adapt in the backend rather than in the core.
- **Right stick sign:** stick right currently decreases yaw (turns clockwise). The game's mouse path also decreases yaw when moving right (F14), so the signs agree; confirm the feel in M1.

## Open questions

| ID | Question | Task |
|---|---|---|
| Q1 | ~~Full `DriveControl` payload layout~~ F16 | R1 |
| Q2 | Does the server honor an arbitrary bearing, and can facing differ from movement direction? | R1, R8, M2 |
| Q3 | ~~Does `nwmain-linux` export symbols?~~ Yes (F9) | R2 |
| Q4 | Where does the client store camera yaw, pitch, limits, and locks? (Static answer in F14; runtime confirmation pending) | R7 |
| Q5 | Where does the client store Always Run state? | R7 |
| Q6 | ~~Is there dormant controller code from the console builds?~~ No (F11) | R6 |
| Q7 | ~~Command-line options for loading a save or module?~~ `+TestNewModule` (F13); user directory: `-userdirectory` (F10) | R5 |
| Q8 | What does `m_fDriveModeMoveFactor` control? | R1 |
