# Quickbar picker plan

Status: **Q0–Q2 done** (2026-09-30). Robert at the Q0 checkpoint: D1 hold a back grip + right trackpad, release to use (since changed by Robert: a press of the picker key opens it, the confirm key uses the highlighted button, the cancel key or another press closes it; README "Quickbar picker"); D2 a client-side NUI window (F32); D3 the visible bank (12); layout: radial ring; D4 the game's own button function. Q1 data (F31), client-side NUI (F32) and the picker prototype (F33) work on the test box. Next: Q3, the Deck bindings and playtest. Item icons done (F34 composite, F35 PLT: armor, cloaks, helmets). Open: a remote-server check.

## 1. Goal

A Deck player can see what is on the quickbar and use it without a keyboard or a precise mouse.

- The picker shows each slot's **icon**, and the **name** of the slot currently selected (for example "Empowered Fireball", "Longsword", "Power Attack").
- Picking a slot uses it exactly as clicking that quickbar button would, including the game's targeting cursor for spells that need a target.
- It reflects the live quickbar, including bank switches and changes made in-game.

The interface (radial or grid, which trackpad, how selection and confirmation feel) is expected to change through playtesting. The hard part, and the first milestone, is getting each slot's **name and icon out of the game**. The interface is built on top of that.

**Scope change.** `plan.md` §1–2 leaves trackpads to Steam Input, and the settings plan rules out drawing our own UI. This feature needs both: Steam Input can't show live quickbar contents, which is the gap. The drawing rule is kept in spirit: nothing nwpad draws should look foreign; see D2.

## 2. What we know

- The client exports its C++ symbols (re-notes F9). Game functions can be called by name, and classes found by their vtables.
- The in-game GUI is a mix of the old `CGui*` panels and the newer Nuklear-based `Nui` windows (F30). We can already find and modify live `Nui` windows, and swap a virtual slot on a window class without patching code (F30 `OnOpened`).
- **NUI** (the game's "new UI", EE 8193.34+) builds windows from JSON: layouts, labels, images by resref, draw lists (images, text, lines and circles at arbitrary positions, so a radial is possible), and binds that update values live. Normally a server script creates them (`NuiCreate`), sends the JSON to the client, and receives the client's events. The client side is in this binary: `Nui::JSON::JsonWindow` with a static event queue and bind-update queue (F30 symbol survey). The settings plan set NUI aside because server-side creation isn't available on servers we don't control; the open question was whether **the client can create one locally** (it can: F32), which makes NUI the most native and most iterable way to draw the picker (D2 c).
- Steam Input's radial menus show fixed labels and a fixed icon set. They can't show what is currently on a quickbar slot. Robert's Deck layout already has a static quickbar radial (`steam-input/README.md`).
- Under Steam Input the game sees an Xbox 360-style pad through its static SDL 2.0.8. There's no touchpad API in that SDL, and the virtual pad doesn't carry trackpad data anyway. For nwpad to use a trackpad, Steam Input has to translate it into something the pad does carry; see §6.
- NWN's quickbar is 3 banks of 12 buttons (plain, Shift, Ctrl). Each button has a type (empty, item, spell, feat, skill, mode, emote, associate command, DM tools, ...) plus type-specific ids: for example spell id, class, metamagic, domain level, and item object id. That layout is known from the server side (NWNX `CNWSQuickbarButton`); the client's copy is still to be found.

## 3. Decisions for Robert

| ID | Question | Options |
|---|---|---|
| D1 | Which input opens the picker, and how is a slot picked? | Hold a back grip (L4/R4) and point with the right trackpad, release to cast / point and click the trackpad / other. Iterate in playtesting; the first build takes one mapping (§6). |
| D2 | How is the picker drawn? | (a) Raw Nuklear calls with the game's context / (b) nwpad's own OpenGL overlay, styled after the game / (c) a client-side NUI window built from JSON. §7 compares them; recommendation after Q0. |
| D3 | Which slots? | All 36 (3 banks) / the current bank's 12 / a curated subset (spells only, ...). Affects layout, not the data work. |
| D4 | How does a pick fire? | Call the game's own quickbar-button function (preferred, like movement) / inject the F-key into the game's event loop (fallback). No real keystrokes either way. |

## 4. Research (Q0, test box)

Each finding goes into `re-notes.md` as usual. Targets, in order:

1. **The client's quickbar store.** Candidates: symbols containing `Quickbar` or `QuickBar` in `nm -DC`; the message handler that applies the server's quickbar updates (a `CNWCMessage::HandleServerToPlayer...Quickbar...` style function), whose writes show the struct; and the in-game quickbar panel class (`CGuiInGame`, a quickbar panel, or a `Nui` equivalent). Deliverable: how to reach the 36 buttons from `g_pAppManager` and each button's layout.
2. **Names.** Best case: the function the game uses for the quickbar tooltip, which already produces "Empowered Fireball" and handles every button type. Fallback: build the name ourselves per type, using the 2DA name column (`spells.2da`, `feat.2da`, `skills.2da`) plus the talk table lookup (`CTlkTable` / `CTlkFile` string by strref), and the item's name for items.
3. **Icons.** Best case: the texture the quickbar button is already drawn with, which also covers composed item icons (weapon icons are layered from model parts). Fallback: the icon resref from the 2DA (`IconResRef`, `ICON`, `Icon`) loaded through the game's resource manager.
4. **Using a slot.** The function a quickbar button click calls (so D4 can use the game's own path, including the targeting cursor).
5. **NUI, created on the client** (D2 c; first among the drawing options). Find the path from the server's NUI messages to a live `JsonWindow`: the client's message handler for "create window" (and "set bind", "destroy"), the `JsonWindow` constructor and what it needs (JSON payload, window id or token, owner), and where it's registered (`Nui::Window::Track` / `s_independent`, F30). Then answer:
   - Can nwpad call that path locally, without a server message, in single-player **and** on a remote server?
   - Events: how does the client drain `JsonWindow::s_event_queue` to the server? nwpad must take the events for its own window (clicks, mouse-over) and make sure they're never sent to a server, and it must not collide with a server's own window ids or tokens.
   - Binds: can nwpad set bind values locally, through the same path `s_bindupdate_queue` feeds, to update the icons and name as the selection moves?
   - Capabilities for the picker: `NuiImage` by resref (spell and feat icons; composed item icons may need a different route, §4.3), draw lists for a radial, a transparent, borderless, non-modal window that doesn't take the game's input focus, and positioning at the screen centre.
   - What the game's own NUI documentation (the base-game script include that defines the JSON builders) says about the schema. We read it for reference only and never commit it.
6. **A hook point for raw drawing** (only if D2 = a): a place in the game's frame where its Nuklear context is open (a `Nui` window's `Render` slot, or the Nui manager's per-frame pass), and how a GL texture becomes an `nk_image`.

**Test data.** The test module's pregenerated character needs known quickbar contents: a spell, a feat, an item, and an empty slot. Options, from Q0: set slots through a debug-only game-function call; a test `.bic` we build ourselves (our own data, so it can be committed if nothing in it is Beamdog-derived); or the module's `OnClientEnter` via our testmod. Tests never touch Robert's own characters.

## 5. Data milestone first: `quickbar` on the control socket

Q1 ends with a debug-only `quickbar` command that returns all 36 slots as JSON: bank, index, type, ids, **name**, **icon resref**, and whether the icon texture was found. Plus a debug `quickbar_icon` command that writes one icon's pixels to a PNG in the run's artifacts (never committed; icons are Beamdog art).

Live tests: with the test character's known quickbar, every slot's type and name match; the icon PNGs are non-empty and the right size; changing a slot in-game (via the same debug path) shows up in the next read; switching bank doesn't change the data (all banks are read).

This milestone is useful whatever D1–D3 turn out to be, and it's where most of the risk is.

## 6. Input (for D1)

The trackpad reaches nwpad only through what Steam Input sends the virtual pad. Workable mappings, to try in order:

1. **Action layer on a back grip.** Holding the grip enables a layer where the right trackpad outputs the right joystick (absolute "joystick" mode), and the grip also presses a button nwpad treats as "picker open". Direction on the trackpad → slot; releasing the grip fires the slot, or cancels if the pad is untouched. Needs one pad button that nothing else uses; we'd pick it from the Console Port layout's free buttons.
2. **Trackpad touch as the signal.** Steam Input can bind "touch" on a trackpad to a button, so resting a thumb on the pad opens the picker. It's more discoverable, but easy to trigger by accident.
3. **Steam Input API (ISteamInput)**, in-process through the game's `libsteam_api.so`. This gives real trackpad positions and custom actions, but needs an in-game-actions manifest for app 704450. It's more capable but far more setup, so we'd only look at it if 1–2 feel bad.

While the picker is open, nwpad stops using the right stick for the camera. Movement on the left stick keeps working, so you can walk while choosing.

`steam-input/make_layouts.py` gains the picker bindings; Robert's Deck layout change is his, as before.

## 7. Drawing (for D2)

| | (c) Client-side NUI window | (a) Raw Nuklear | (b) Own OpenGL overlay |
|---|---|---|---|
| Look | Native: it *is* a game window | Native fonts and style | Ours; text needs our own font |
| Icons | `NuiImage` by resref; item icons TBD (§4.3) | `nk_image` from the button's texture | Draw the same GL texture |
| Layout changes | Edit JSON, even at runtime from a file, which is ideal for playtest iteration | Nuklear calls in C | Our own code |
| Hook | Call the client's window-creation path (Q0.5); take our window's events | A point inside the game's Nuklear frame (Q0.6) | Our existing `SwapWindow` hook |
| Risk | Must never leak events to a server or clash with server windows | Tied to Nui internals mid-frame | GL state save/restore; otherwise isolated |

Preference: (c) if Q0.5 finds a local creation path that works on remote servers too, then (a), then (b). (b) stays the fallback, and a quick way to prototype the interaction if Q0 runs long.

## 8. Milestones

| Milestone | Deliverable | Acceptance |
|---|---|---|
| **Q0** | Research findings (§4, NUI first among the drawing options) in `re-notes.md`, test-character approach, D2 recommendation | **Checkpoint:** Robert picks D1–D4 |
| **Q1** | `quickbar` / `quickbar_icon` debug commands (§5) | Live tests pass; names and icons correct for spell, feat, item, empty |
| **Q2** | Picker prototype: opens from the chosen input, shows icons + the selected name, fires the slot | Works on the box with the Xbox pad (layer emulated); frame budget met |
| **Q3** | Deck bindings and layout | **Robert playtests on the Deck**; iterate on interface |
| **Q4** | Release: release-build surfaces only (debug commands stay debug), README, settings (maybe a toggle in Options) | Full suite and settings checks pass; Robert signs off |

## 9. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| The client keeps the quickbar only as GUI state, not a tidy struct | Harder Q0 | Read it from the quickbar panel's buttons instead; the tooltip/icon come from there anyway |
| Item icons are composed and have no single resref | No icon for weapons | Use the panel's rendered texture (§4.3), not the 2DA |
| NUI windows can't be created client-side, or only in single-player | D2 (c) unavailable or SP-only | (a) or (b); an SP-only NUI picker is still worth a look if it's much nicer |
| A client-side NUI window's events reach a server | A server sees unknown events or tokens | Consume our window's events before they're queued for sending; a live test on a local multiplayer server checks nothing is sent |
| No clean point inside the game's Nuklear frame | D2 (a) unavailable | (c) or (b) |
| Steam Input can't give a clean "picker open" signal alongside the existing layout | Awkward input | Try §6 options in order; ISteamInput last |
| Drawing breaks the game's GL state | Visual glitches | Save/restore state; live test compares a screenshot region with the picker closed before and after |
| Icons or names are Beamdog assets | Must not be committed | Read at runtime only; artifacts stay local (precommit check blocks image types) |
