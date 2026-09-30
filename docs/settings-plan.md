# Native settings integration plan

Status: **S done; N1-lite done** (2026-09-29): `src/hook/settings.c` seeds and reads `[nwpad]` in `settings.tml`, imports `config.toml` once, falls back to it; `nwpad.enabled` idles the library; checks in `tools/settings_checks.sh`. Next: the panel spike (N2, timeboxed). Robert chose: lite first (a `[nwpad]` section in `settings.tml`, no UI), then a timeboxed panel spike; D1 Camera group; D2 import once, then fallback only; D3 `strafe_window` and `mouse_idle_ms` file-only. Findings: `docs/re-notes.md` F29.

## 1. Goal

nwpad's settings should look and behave like part of the game:

- They appear in the game's own **Options menu**, where the Options search box finds them.
- They persist in **`settings.tml`** alongside the game's settings.
- **Reset to default** works, and changes **apply live**.
- Nothing is drawn by nwpad itself. A custom overlay would look foreign and defeat the purpose.

Behavior stays as it is today: this plan changes only where settings live and how they're edited.

## 2. Why this looks feasible

- Since patch 1.79, NWN:EE keeps most client settings in `settings.tml`, a TOML file with an embedded schema intended for introspection and tool-based editing (see the nwn.fandom.com `Settings.tml` page).
- Every setting can be changed in-game through the debug menu (Ctrl+Shift+F12, then Config), which implies a generic, schema-driven editor.
- The modern Options menu has a search box that finds any setting (see the nwn.wiki Game Options page).
- The client exports its C++ symbols (`re-notes` F9), so any settings registry should be callable by name, the same way the camera and movement functions are.

Together these point to a **central settings registry** that the Options UI, the debug editor, and `settings.tml` all draw from. If nwpad registers into it, the native UI, search, persistence, and reset may all come for free. Sections 4.1 and 4.3 test this assumption first.

## 3. Decisions for Robert

| ID | Question | Options |
|---|---|---|
| D1 | Where do the settings appear in Options? | A new "Controller" category / inside the existing Camera or Controls category / whatever the registry allows without extra UI work |
| D2 | What happens to `~/.config/nwpad/config.toml`? | Imported once, then ignored / kept as an override that wins over `settings.tml` / kept only as the fallback when the registry is unavailable |
| D3 | Which settings are exposed in the UI? | See §5; the open item is whether `strafe_window` and `mouse_idle_ms` are user-facing |

## 4. Research tasks (S1–S6)

Each finding goes into `docs/re-notes.md` with evidence, as usual (CLAUDE.md rule 4).

### 4.1 S1: Find the settings registry

Search the exported symbols (`nm -DC nwmain-linux`) for setting, config, schema, option, and TOML-related classes. Identify:
- The registry object and how to reach it (a global like `g_pAppManager`, or through the app).
- The registration call: key path, type (bool/int/float/string/enum), default, range, description, and any UI category or ordering fields.
- How values are read and written, and whether there's a change-notification mechanism.

**This is the go/no-go for tier 1 (§6).**

### 4.2 S2: Load order and seeding

Find when `settings.tml` is loaded relative to the library's first frame. The library currently only *calls* game functions (plus the SDL jump-table hooks), and this plan should not require hooking game functions. Expected approach:
- Register nwpad's settings on the first frame, after the registry exists.
- Seed their values by reading the `[nwpad]` section of `settings.tml` with the core's TOML-subset parser.
- Let the game handle saving from then on.

Confirm that registering after the load neither loses values nor produces duplicate or conflicting entries.

### 4.3 S3: How the Options menu is populated

Register a single dummy setting, then:
1. Check whether it appears in the debug Config menu. This is expected if the registry exists.
2. Check whether Options search finds it, and whether it appears in any Options category.

If it appears in neither, find how Options panels are built: generated from the registry, or hand-authored panels that reference specific keys.

### 4.4 S4: Labels and descriptions

Determine whether the registry and the Options UI take plain-text labels and descriptions, or only talk-table string references (strrefs).
- **Plain text:** use it directly (English only for v1).
- **Strrefs only:** evaluate (a) a lookup hook serving a reserved strref range, versus (b) whatever literal-string path the debug editor uses. Record the cost; this may push D1 toward whatever needs no labels.

### 4.5 S5: Leftover keys after uninstall

Start the game *without* the library while `settings.tml` contains `[nwpad]` keys. Record whether the game keeps them, drops them on save, or warns. It must not crash. Document the behavior in the README either way.

### 4.6 S6: Change delivery

Determine whether the registry offers change callbacks. If not, read nwpad's handful of values from the registry each frame; the frame budget (p99 of the library's own logic under 0.1 ms) has room for that. Measure it with the existing frame-cost telemetry.

## 5. Settings

Stick tuning stays in Steam Input (plan §2). Only settings a player would reasonably change are exposed.

| Current key | Proposed `settings.tml` key | UI | Type / range | Default |
|---|---|---|---|---|
| (new) | `nwpad.enabled` | "Controller support" | bool | on |
| `camera_yaw_speed` | `nwpad.camera.turn-speed` | "Camera turn speed (stick)" | float, 60–360 °/s | 180 |
| `camera_pitch_speed` | `nwpad.camera.tilt-speed` | "Camera tilt speed (stick)" | float, 30–180 °/s | 90 |
| `run_threshold` + `run_hysteresis` | `nwpad.movement.run-point` | "Run point" | float, 0.5–0.95 | 0.7875 |
| `hide_cursor` | `nwpad.hide-cursor` | "Hide cursor while using sticks" | bool | on |
| `strafe_window` | `nwpad.movement.strafe-window` | D3 | float, 0–30 ° | 10 |
| `mouse_idle_ms` | `nwpad.mouse-idle-ms` | D3 | int, 100–1000 ms | 300 |

Notes:
- **Run point** is a single slider for the centre of the band. The band width (0.125, giving run above 0.85 and walk below 0.725 at the default) stays internal and moves with it.
- **Key names** follow the game's lowercase, hyphenated style. The final spelling follows S1, since the registry may impose its own conventions.
- **Hidden entries.** If a D3 item isn't exposed in the UI, it can still be registered without a UI entry (if the registry supports that), or left in `config.toml`.
- **Disabling** with `nwpad.enabled` stops all stick input and cursor hiding, but the library stays loaded, so it can be turned back on live. This is separate from `NWPAD_DISABLE=1`, which keeps the library inert from launch.

## 6. Integration tiers

Take the highest tier the research supports.

**Tier 1: registry with automatic Options entries. This is fully native.**
Registering is enough: the settings appear in Options, Options search finds them, they persist in `settings.tml`, reset works, and changes apply live (S6).

**Tier 2: registry exists, but Options panels are hand-authored.**
Settings persist in `settings.tml` and are editable in the debug Config menu. A visible Options entry then requires building a panel through the game's GUI classes. That's the highest-risk work in this plan, so it gets a timebox of two agent sessions and a human checkpoint before any code is written. If it doesn't fit the timebox, ship tier 2 without the panel and document the debug menu path.

**Tier 3: no usable registry.**
Keep `config.toml` as it is and improve the README. No custom overlay.

**Also rejected: NUI.** The game's scriptable JSON windows are created by server-side NWScript. That's awkward for a client mod and unavailable on servers we don't control, so NUI is only worth a second look if a client-side creation path turns up during S1.

## 7. Behavior requirements

- **Search.** Settings are findable in Options search with the words "controller", "gamepad", and "stick"; the descriptions carry those words if the labels don't.
- **Descriptions.** Each setting has a one-line description.
- **Reset.** Reset-to-default restores the §5 defaults.
- **Live apply.** Changes take effect on the next frame, with no restart.
- **Persistence.** Values persist in `settings.tml` across restarts.
- **Migration.** On the first run with registry support, values present in `config.toml` are imported into the registry, then handled according to D2.
- **Degradation.** If any registry signature is missing, the library falls back to `config.toml` silently and all features keep working (the existing resolver rule).
- **Scope.** Client-only: nothing about this touches servers.

## 8. Signatures and code

- New registry entry points go into `signatures/ee.yaml` as symbol entries with `note:` references, like the camera and movement functions.
- Registry access lives in a new `src/hook/settings.c` behind a small interface: register, read, write, and "is available". The backend and core never talk to the registry directly.
- The core `nwpad_config` struct stays the single in-memory source of values. `settings.c` fills it from the registry each frame (or on change notification), so the core and its unit tests are untouched.
- Unit tests cover the mapping from the §5 keys to `nwpad_config` (including run point to threshold plus hysteresis) and the migration logic, all without the game.

## 9. Tests

**Control socket:** a `settings` command that lists, reads, and writes nwpad's registry entries (debug builds only).

**Live scenarios:**

| Scenario | Assertion |
|---|---|
| Registration | All §5 keys exist in the registry after the first frame, with the correct defaults. |
| Live apply | Writing `nwpad.camera.turn-speed` through the registry changes the measured yaw rate within one frame. |
| Enable toggle | With `nwpad.enabled` off, the stick produces no movement or camera change, and the cursor isn't hidden. |
| Persistence | Set values, quit, relaunch: the values are retained and `settings.tml` contains the `[nwpad]` section. |
| Migration | A `config.toml` with non-default values is imported on first run, then handled per D2. |
| Uninstall | Launch without the library while `[nwpad]` keys are present: no crash; key handling matches the S5 findings. |
| Degradation | With the registry signatures deliberately broken, everything works from `config.toml`. |

**Evidence for sign-off:** screenshots (kept local, never committed) of the debug Config menu and of Options search results for "controller".

**Manual:** on the Deck, the entries are reachable and readable with trackpad navigation in Game Mode.

## 10. Milestones

| Milestone | Deliverable | Acceptance |
|---|---|---|
| **S** | S1–S6 findings in `re-notes.md`, and a tier recommendation. | **Human checkpoint:** Robert confirms the tier and answers D1–D3. |
| **N1** | `settings.c`: registration, seeding from `settings.tml`, migration from `config.toml`, fallback. | Registration, persistence, migration, uninstall, and degradation tests pass; entries visible in the debug Config menu. |
| **N2** | Options menu presence: automatic (tier 1) or a built panel (tier 2, timeboxed). | Options search finds the entries; **Robert signs off on placement and wording**. |
| **N3** | Live apply via callback or per-frame read, the `nwpad.enabled` toggle, descriptions, README update. | Live-apply and enable-toggle tests pass; the full existing suite still passes; frame budget met. |
| **N4** | Deck validation. | Robert signs off in Game Mode. |

## 11. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| No central registry, so every option is hand-wired | Low–medium | Falls to tier 3 | S1 decides early; `config.toml` keeps working. |
| Registry exists but Options panels are hand-authored | Medium | Tier 2 panel work, highest crash risk | Timebox plus human checkpoint; ship the debug-menu path if the panel doesn't fit. |
| Labels require strrefs | Medium | Extra hook or awkward labels | S4 evaluates the options before committing to D1. |
| Registering after `settings.tml` loads loses or duplicates values | Medium | Values reset each launch | Seed from `settings.tml` ourselves (S2); covered by the persistence test. |
| The game drops unknown `[nwpad]` keys when run without the library | Medium | Settings lost after running without the library | Document it; `config.toml` fallback per D2 can preserve them. |
| A game update changes the settings subsystem | Low | Integration breaks | Symbol resolution plus fallback to `config.toml`; the hash check already stops the agent on any binary change. |
