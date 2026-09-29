# CLAUDE.md — working rules for nwpad

nwpad is an `LD_PRELOAD` library that adds analog stick movement and camera control to the native Linux client of Neverwinter Nights: EE. The full plan is in `docs/plan.md`. Read it before starting a new milestone. This file covers how to work, not what to build.

## Quick start

- **Local build and tests:** `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- **Test box:** `tools/remote.sh build` (sync, build, unit tests) or `tools/remote.sh test` (plus sigcheck and live tests, with artifacts pulled back)
- **Hooks:** run `tools/install_hooks.sh` once per clone.
- **Where things live:** the library code is split between `src/core/` (pure logic, unit-tested) and `src/hook/` (SDL interposition, plus `backend_stub.c`, which real backends replace). Signatures are in `signatures/ee.yaml`, findings in `docs/re-notes.md`, and the plan in `docs/plan.md`.

## Topology

You run on Robert's local machine. The game runs on a dedicated Linux test box reached over SSH (host alias `nwpad-box`). You have full access to that box, but Robert also plays on it. Everything that touches the game goes through the tools in `tools/`, which handle syncing, the session lock, and pulling artifacts back into `artifacts/<run-id>/`.

## Hard rules

1. **Respect the session lock.** Never launch the game except through `tools/run_game.sh`, which takes `~/.nwpad/session.lock`. If the lock is held, or `nwmain-linux` is already running outside your session, wait or report. Never kill a game process you didn't start.
2. **Never touch Robert's own NWN data.** That means his NWN user directory, settings, saves, and Steam configuration. Tests use only the isolated user directory described in `docs/plan.md` §8.2.
3. **Never commit Beamdog-derived material.** No game or server binaries or assets, nothing from `re-work/` (raw decompiler output), no test save, and no screenshots. The pre-commit check enforces this; don't bypass it.
4. **Evidence before signatures.** No address, offset, or signature goes into `signatures/ee.yaml` without a matching entry in `docs/re-notes.md` that records the evidence (decompile excerpt summary, trace output, or `msglog` capture) and the binary hash it applies to.
5. **No hardcoded addresses.** Resolve everything through the signature resolver. A missing signature must disable its feature and log, never crash the game.
6. **Never relax a test tolerance.** Only Robert may change tolerances. Propose the change and stop.
7. **Stop if the game binary changes.** If `sigcheck` reports a different `nwmain-linux` hash from the one recorded in `docs/re-notes.md`, stop and report. Don't chase signatures silently.

## Stop and ask Robert

- At the M2 movement gate, before committing to Path A or Path B.
- Before changing the signature format or the resolver's failure behavior.
- Before relaxing any tolerance (rule 6).
- When feel tuning or sign-off is needed. You can prepare parameter sweeps, but you can't judge feel.
- After three crashes caused by the same change.
- When a research task needs a human, such as R8 (asking the NWNX team).

## Choosing a tier

Use the cheapest tier that answers the question.

**Inner tier (local, seconds).** Build and run unit tests with `cmake --build build && ctest --test-dir build`. Most logic changes should be validated here first. Keep game-independent logic in pure functions so it can be tested this way.

**Middle tier (test box, about 1–2 minutes).** Run `tools/remote.sh test <pytest-selector>`. This syncs, builds, runs `sigcheck`, launches the game, runs the live scenarios, and pulls artifacts back. On failure, read the telemetry in the pulled artifacts around the failing step before changing code. Don't guess from the assertion message alone.

**Outer tier (RE sessions).**
1. State the hypothesis in `docs/re-notes.md` under an open question.
2. Gather static evidence with headless Ghidra (`tools/ghidra/decompile_fn <symbol>` and related scripts). Output stays in `re-work/`.
3. Confirm at runtime with Frida scripts, socket `msglog`, or `read`/`scan`.
4. Record the conclusion and evidence summary in `re-notes.md`.
5. Only then add or change signatures.

## Failure handling

- **Crash:** `tools/collect_crash.sh` runs automatically. Read the backtrace and telemetry before retrying.
- **Launch timeout or never ready:** treat it as an environment failure and report it separately from test failures.
- **Flaky test:** at most two reruns. Report persistent flakiness; don't hide it with longer timeouts.

## Code conventions

- C11. Build with `-Wall -Wextra -Werror`. The library depends only on libc, libdl, and the game's SDL2.
- Don't allocate memory, block, or do I/O in the per-frame path, apart from telemetry buffered to a background writer. The budget is under 0.1 ms per frame.
- All debug-only surfaces (control socket, `read`, `scan`, `msglog`) sit behind a build flag and are absent from release builds.
- Stick tuning belongs to Steam Input. Don't add deadzone or curve options to the library beyond the fixed safety deadzone (plan §6.3).
- Keep commits small and focused, and reference the milestone or research task ID in the message (for example `M1: resolve camera ChangeLocation handler`).

## Useful references

- The plan: `docs/plan.md`. The decision log (§13) records what is settled; don't reopen those decisions without asking.
- RE findings: `docs/re-notes.md`.
- NWNX:EE source (struct layouts, message enums): github.com/nwnxee/unified.
