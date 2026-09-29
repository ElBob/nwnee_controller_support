# Reverse-engineering notes

Every function, global, offset, and signature the library uses must have an entry here with its evidence and the binary it applies to (CLAUDE.md rule 4). Raw decompiler output stays in `re-work/` (gitignored); summarize it here in your own words.

## Binaries

| Binary | Source | Build | sha256 | Recorded |
|---|---|---|---|---|
| `nwmain-linux` | Steam | TBD (M0) | TBD (M0) | |
| `nwserver-linux` | Dedicated server package | TBD (R1) | TBD (R1) | |

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

## Conventions to confirm

- **Core angle convention:** degrees, counter-clockwise from world +X, stick +y = forward (`src/core`). Confirm the game's yaw direction and zero point in M1, and adapt in the backend rather than in the core.
- **Right stick sign:** stick right currently decreases yaw (turns clockwise). Confirm against the game's camera in M1.

## Open questions

| ID | Question | Task |
|---|---|---|
| Q1 | Full `DriveControl` payload layout, including bit-packed fields | R1 |
| Q2 | Does the server honor an arbitrary bearing, and can facing differ from movement direction? | R1, R8, M2 |
| Q3 | Does `nwmain-linux` export symbols? | R2 |
| Q4 | Where does the client store camera yaw, pitch, limits, and locks? | R7 |
| Q5 | Where does the client store Always Run state? | R7 |
| Q6 | Is there dormant controller code from the console builds? | R6 |
| Q7 | Command-line options for loading a save or module and for overriding the user directory? | R5 |
| Q8 | What does `m_fDriveModeMoveFactor` control? | R1 |
