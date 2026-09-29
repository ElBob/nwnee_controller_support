# Ghidra helpers

These are headless scripts for the outer (RE) tier. Everything they write goes to `re-work/`, which is gitignored. Summarize the conclusions in `docs/re-notes.md`.

- **`decompile_fn <binary> <names...>`** decompiles functions by demangled name. This is R1's main tool, run against `nwserver-linux`:
  ```
  tools/ghidra/decompile_fn ~/.nwpad/server/<pkg>/bin/linux-x86/nwserver-linux \
    CNWSMessage::HandlePlayerToServerInputDriveControl \
    CNWSMessage::HandlePlayerToServerInputAbortDriveControl \
    CNWSMessage::HandlePlayerToServerInputMessage \
    CNWSCreature::AddDriveAction CNWSCreature::AIActionDrive CNWSCreature::DriveUpdateLocation
  ```

Planned additions: version-tracking export (`nwserver-linux` → `nwmain-linux`, R3), xref search for message minors, and signature generation from an address.
