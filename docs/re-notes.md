# Reverse-engineering notes

Every function, global, offset, and signature the library uses must have an entry here with its evidence and the binary it applies to (CLAUDE.md rule 4). Raw decompiler output stays in `re-work/` (gitignored); summarize it here in your own words.

## Binaries

| Binary | Source | Build | sha256 | Recorded |
|---|---|---|---|---|
| `nwmain-linux` | Steam | buildid 20277208 (`build.txt` 26c6e573, 2025-10-06) | `6d19c39bc646af5ddbc333ff31797a4acd9019506bcb45ef1b93b1bd5925e700` | 2026-09-28 |
| `nwserver-linux` | Steam (ships next to the client) | same | `d6d952826b671fc62c376dca912e87f1aa45d4aa81c620e912ceb4d8e051111d` | 2026-09-28 |

## Test box environment

- The game needs a connected monitor. With both DP connectors disconnected (the box's KVM switched away), KWin never maps the window: SDL 2.0.8's `SDL_CreateWindow` blocks in `XIfEvent`, or the game dies on an `XF86VidModeGetModeLine` BadValue X error. `tools/run_game.sh` refuses to launch in that state (exit 13). With the monitor connected, no SDL hints are needed.
- Steam's launch option for NWN:EE on the box is not usable for tests (it points at an unrelated wrapper), so `run_game.sh` launches the binary directly with `SteamAppId=704450` to stop a relaunch through Steam.

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

## Conventions to confirm

- **Core angle convention:** degrees, counter-clockwise from world +X, stick +y = forward (`src/core`). Confirm the game's yaw direction and zero point in M1, and adapt in the backend rather than in the core.
- **Right stick sign:** stick right currently decreases yaw (turns clockwise). Confirm against the game's camera in M1.

## Open questions

| ID | Question | Task |
|---|---|---|
| Q1 | Full `DriveControl` payload layout, including bit-packed fields | R1 |
| Q2 | Does the server honor an arbitrary bearing, and can facing differ from movement direction? | R1, R8, M2 |
| Q3 | ~~Does `nwmain-linux` export symbols?~~ Yes (F9) | R2 |
| Q4 | Where does the client store camera yaw, pitch, limits, and locks? | R7 |
| Q5 | Where does the client store Always Run state? | R7 |
| Q6 | ~~Is there dormant controller code from the console builds?~~ No (F11) | R6 |
| Q7 | Command-line options for loading a save or module? (User directory: `-userdirectory`, F10) | R5 |
| Q8 | What does `m_fDriveModeMoveFactor` control? | R1 |
