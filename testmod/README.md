# Test module

`nwpad_test.mod` is the module the live tests run in (plan §8.2):

- A flat open area with a known start point and facing.
- A wall segment for collision tests.
- No NPCs, encounters, or time-of-day effects.
- An OnEnter script that calls `SetCameraFacing` to set a known camera state.
- Test scripts for camera locks, cutscene mode, and starting a dialog, invoked through the control socket's `script` command.

The source lives here as neverwinter.nim JSON (`nwn_gff`, `nwn_erf`) and is built locally. The built `.mod` is never committed. The area is authored once in the toolset (under Wine), then converted to JSON. This happens in M0.
