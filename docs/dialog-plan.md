# Dialog plan

Status: **D0 research mostly done** (2026-10-08; re-notes F36): replies and the selection path confirmed at runtime; the NPC's line is captured by wrapping `StringGob::SetText` (one vtable slot), which is also the trigger. Robert's decisions: a **vertical list**; our window **replicates the game's dialog window and covers it**, with the NPC's text in a **larger font**; the highlight moves with the **D-pad, either stick, or the arrow keys**; the whole feature can be **turned on or off in the config** (`nwpad.dialog`, default on). §2 marks what's confirmed and what's still a lead.

## 1. Goal

While a conversation with an NPC is open, a controller player can:

- read the NPC's line,
- see the replies and move a highlight between them with the stick,
- pick one with a button,

without a mouse. As with the picker, nwpad shows this in its own client-side NUI window (re-notes F32), styled like the game, and uses the game's own path to send the reply, so it's exactly what clicking or a number key does.

## 2. What we need, and where it is (first pass)

| Need | Where | Confidence |
|---|---|---|
| The dialog window exists | `CGuiInGame+0x70` → `CGuiInGameChatDialog*` (0xd10 bytes); null when no conversation | confirmed (Ghidra: `ShowDialogEntry` creates it, `CloseDialog` frees it and nulls the field) |
| A new NPC line | `CNWCMessage::HandleServerToPlayerDialog(1)` → `CGuiInGame::ShowDialogEntry(text, speaker oid, object)` → `CGuiInGameChatDialog::SetDialogMessage(text)` | confirmed call chain |
| The replies arrive | `HandleServerToPlayerDialog(2)` → `CGuiInGame::ShowDialogReplies(count, CExoString* texts, unsigned* ids, …, conversation, end_flag)` → `CGuiInGameChatDialog::SetReplies` | confirmed call chain |
| Reply count | `CGuiInGameChatDialog+0x130` (int) | strong (`SelectReply` bounds-checks against it) |
| Reply texts | `+0x128`: a `new[]`'d array of `CExoString` (16 bytes each), copied in `SetReplies` | strong; layout to confirm at runtime |
| Reply ids / flags | `+0x110` reply ids (sent to the server); `+0x138` per-reply flags, bit 0 = not selectable | strong |
| Speaker, conversation | `+0x140` speaker object id, `+0x144` conversation id; `+0xd08` = 1 for a lone "Continue/End" reply; `+0xd04` busy | strong |
| The NPC's text | passed to `SetDialogMessage`, which hands it to a render string (not readable back, like the tooltip, F31). The dialog object may keep a copy (an edit-scroll buffer near `+0x118` / `+0x1e8`) | **lead**; Q1 |
| Selecting a reply | `CGuiInGame::HandleDialogNumKey(n)` → `CGuiInGameChatDialog::SelectReply(n-1)`, the game's own number-key path (honours disabled replies and the busy flag) → `HandleDialogSelection` → `CNWCMessage::SendPlayerToServerDialog_Reply` | confirmed |
| Ending | `HandleDialogSelection(-3)` (end) / `(-2)` (close); the server's subtype 5 calls `CloseDialog` | confirmed |

The game already maps the number keys 1–9 to replies, so Steam Input alone could send them, but the player couldn't see which number is which reply from the couch, and there's no "move the highlight" feedback. That's the gap nwpad fills.

## 3. How it works (proposed)

**Trigger, without patching game code.** All of `ShowDialogEntry`, `ShowDialogReplies` and `SetReplies` are plain (non-virtual) calls from the network handler, so there's no vtable slot to swap like the Options window's `OnOpened` (F30). Candidates, in order:

1. **A virtual slot on `CGuiInGameChatDialog`.** It's a `CGuiPanel` with its own vtable; if it overrides something called on each update (activation, a per-frame `Update`, or `FixDialogSize`'s relayout), swap that slot. Q2.
2. **A cheap check per frame.** `CGuiInGame+0x70` plus a change marker (reply array pointer `+0x128`, which `SetReplies` reallocates on every new set of replies). Two pointer compares per frame; it costs nothing measurable, but it's the polling Robert asked us to avoid for settings, so it's the fallback.
3. Not planned: hooking the network handler by patching code.

**Reading.** On a change: reply count, ids, flags and texts from the dialog object; the NPC's line from wherever Q1 finds it. Text needs cleaning: the game's colour tags (`<cRGB>…</c>`), `<StartAction>` / `<StartCheck>` / `<StartHighlight>` markup (the game colours those parts), and newlines. The speaker's name comes from the object id at `+0x140`.

**Showing.** A client-side NUI window (F32): the NPC's line at the top (scrolling for long text), the replies as a list below, the highlighted one marked; disabled replies dimmed. The game's own dialog window stays as it is at first (it still works with the mouse); hiding or replacing it is a later choice (D2).

**Choosing.** While the dialog is open, nwpad takes the stick (up/down, or the right stick like the picker) and a confirm key, the same mechanism as the picker's keys (only nwpad's while the window is up; Enter and Escape are the game's otherwise). Confirm calls `HandleDialogNumKey(highlight + 1)`. A lone "Continue" / "End" reply (`+0xd08`) is just one reply; the cancel key ends the conversation with `HandleDialogSelection(-3)` (to confirm that's what Escape does in the game).

**Picker interplay.** The picker shouldn't open during a conversation (or the confirm key would be ambiguous); nwpad's movement already stops in dialog because the game takes the character.

## 4. Research (Q, test box)

1. **The NPC's text.** Find a readable copy in `CGuiInGameChatDialog` (the edit scroll at `+0x1e8`, the buffer at `+0x118`), or read the `CExoString` that `ShowDialogEntry` receives (it's a local in the handler, so only reachable through a hook). Fallback: rebuild it from the conversation resource (`.dlg`) by node, which is a lot more work.
2. **A trigger slot.** List `CGuiInGameChatDialog`'s vtable (as F30 did for the Options window) and find a slot that's called when entries or replies change.
3. **Reply layout** at runtime: confirm `+0x128` / `+0x130` / `+0x110` / `+0x138` against a live conversation, including disabled replies and the "Continue" case.
4. **Text markup**: collect what real conversations contain (colours, action / check tags, tokens like `<FirstName>` are resolved by the time the client gets them — `ParseStr` runs in the handler).
5. **Test conversation.** The test module needs an NPC with a known conversation (two levels, a disabled reply, a "Continue" node). Either one in the Contest Of Champions module, or our own `testmod` with a `.dlg` we author ourselves (our data, committable).
6. **Edge cases** to look at, not necessarily solve now: barks (floating text, no window), cutscene conversations, a conversation opening a store or barter, conversations that start while the picker is open, and multiplayer (more than one player in a conversation).

## 5. Milestones

| Milestone | Deliverable | Acceptance |
|---|---|---|
| **D0** | Q1–Q6 findings in `re-notes.md`; the trigger approach | **Checkpoint:** Robert picks D1–D3 |
| **D1** | Debug `dialog` command (NPC line, replies with ids/flags) and `dialog_select` | Live tests against the test conversation: text and replies right at each node, selecting walks the tree, nothing extra sent to the server |
| **D2** | The NUI dialog window with stick highlight, confirm and cancel | Works on the box with the Xbox pad; frame budget met |
| **D3** | Layout bindings and polish (long text, many replies, markup) | **Robert playtests** |

## 6. Decisions for Robert (at the D0 checkpoint)

| ID | Question | Options |
|---|---|---|
| D1 (decided: list) | How are replies chosen? | A vertical list moved with the left stick / D-pad up-down (like most console RPGs) / a wheel like the picker (fast for ≤ 8 replies, awkward for long lines) |
| D2 (decided: ours covers it, same layout, larger NPC text) | What happens to the game's own dialog window? | Keep it as it is, ours alongside / hide it while ours is up |
| D3 (decided: D-pad, either stick, arrow keys; the layout's D-pad already sends the arrow keys) | Which keys? | Confirm = Enter (as the picker), cancel = Escape; the highlight on D-pad or stick. The layout's D-pad is zoom/journal/map today, so moving it needs the same "only while open" treatment |

## 7. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| The NPC's text isn't readable back from the dialog object | No line in our window | Hook-free fallback: show only the replies first (the game's window still shows the line), or rebuild from the `.dlg` |
| No suitable virtual slot | Need the per-frame pointer check | Two compares per frame; measure it |
| Long lines / many replies | Hard to read on a TV/Deck | Scrolling; wrap and truncate in the list, full text for the highlighted reply |
| Server-side conversation scripts (multiplayer) behave differently | Edge cases | We only send what a number key sends; the server sees nothing new |
