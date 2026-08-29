# Daemon editor session state (M9 e08a — D7 tier 1)

The **semantic human state** — *what is selected, where the cameras are, whether the game is
playing* — lives in the **daemon**, not in a GUI panel's private members. Every client (a second
editor window, the CLI, a scripted AI agent) reads and drives the *same* state over the *same*
contract surface, so "what is the human looking at" is a question the contract can answer.

This is **tier 1** of the two-tier event model. Tier 2 — UI chrome (focus, dock layout, drag,
hover) — is the editor-local `editor.ui` bus and is deliberately **never** forwarded to the daemon.

Implementation: `src/editor/editorkernel/` (`editor_session_state.{h,cpp}` + the `editor.*` verbs in
`kernel_server.cpp`). Contract surface: `src/editor/contract/src/registry.cpp`.

## The verbs

All **operational** (a live daemon serves them; a one-shot CLI invocation is refused
`contract.operational_only`, exactly like `snapshot`/`query`) and all scoped **`session_control`** —
the reads included, because what they read is the live human session, not authored data. They are
registered in the ONE registry, so CLI ≡ RPC ≡ MCP ≡ `describe` parity is covered automatically
(R-CLI-013) and the client schema + editor typings regenerate from them.

| verb | rpc method | what it does |
|---|---|---|
| `editor select {ids[], mode}` | `editor.select` | set the selection (L-35 id-paths); `mode` = `replace` (default) / `add` / `toggle` / `remove` |
| `editor selection-get` | `editor.selection-get` | read the selection |
| `editor camera-set {viewportId, transform, projection}` | `editor.camera-set` | set one viewport's camera (payloads carried **opaquely**) |
| `editor cameras-get` | `editor.cameras-get` | read every viewport camera |
| `editor play` / `pause` / `stop` / `step --ticks N` | `editor.play` / … | drive the L-51 play state over RPC |

The deterministic `session *` family (`session new` / `step` / `seed` / `inject` / `hash` /
`record`) is a **different thing and stays untouched** (C-F4): that one drives a *headless
simulation over a state file*; this one drives the *live human session*.

Naming note: the registry's identity is a two-level `(noun, verb)` pair, so design 05's prose
`editor selection get` is registered as the hyphenated `editor selection-get` — the shape the
sibling `editor scene-tree` already established.

## `origin` — the echo-suppression contract

Every real change publishes a fact on the **`session`** topic (additive payload members; the topic
name is unchanged, so the contract freeze is satisfied):

```
selection-changed { ids, mode, origin }
camera-changed    { viewportId, origin }
play-state        { state, simTick, origin }
```

`origin` is the **client id of whoever caused the change**, and it is what makes a shared stream
safe to consume:

* `attach` returns that id back to each client as **`clientId`** (`client::Client::client_id()` in
  the SDK). Wire clients get `1, 2, 3…`; **`0` means the daemon itself**.
* The daemon fans every fact out to **every** subscriber, with no per-client filtering. A consumer
  **applies** a fact whose `origin` differs from its own id and **drops** one that matches. That
  single rule *is* the contract — there is no side channel and no server-side suppression to get
  subtly wrong.
* **Ids are minted per WIRE CONNECTION.** The in-process attach path (`gui/contract`'s shim, which
  calls `Dispatcher::attach` directly rather than going through a transport) gets no connection and
  therefore `origin == 0`. That is correct today — nothing in-process drives the `editor.*` verbs —
  but it means **echo suppression cannot distinguish two in-process consumers from each other**. A
  consumer that needs its own identity must be a real wire client (the Shell already is, via
  `context_client`); e08b/e08c should not route panel writes through the in-process shim expecting
  per-panel `origin`. e08b took exactly that route: the Shell's `SessionFeed` writes over the Shell's
  own wire connection, so its `origin` is real.
* **`origin == 0` is ALSO "not attached", so an unattached consumer must not treat a 0/0 match as its
  own echo** — it would silently swallow every daemon-originated fact. Guard the comparison on
  "I have an id at all" (both e08b consumers do, and both have a test for it).

### The writer sees its own change through the REPLY, never the fact

The corollary of the drop rule, and the one that is easy to get backwards: the fact a write publishes
carries the WRITER's `origin`, so the writer is precisely the client that will not apply it. Every
mutating verb therefore answers with the resulting state (`editor.select` -> `ids`, the play verbs ->
`state` / `simTick`), and **that reply is how the acting client learns its own outcome**. A consumer
that renders only from facts shows every other client's changes and none of its own.

Two supporting invariants make it trustworthy, and both are tested:

1. **A no-op publishes nothing.** Re-selecting the same ids, re-writing the same camera, or
   `stop` in `edit` state changes nothing and emits no event — otherwise the daemon itself would be
   an echo generator.
2. **Ids are never reused within a daemon lifetime** — the daemon mints them from a monotonic
   counter, so a client that reconnects after another dropped never inherits the departed client's
   id and a fact still in flight can never be mis-attributed. (`editorkernel-test_kernel_server`
   asserts both halves: two live clients differ, *and* a reconnect's id is beyond every id issued.)

## Play state (L-51)

`edit` → authored truth, no live session. `playing` / `paused` → a live session whose runtime state
is **discarded** on `stop`, never written to authored files.

The state machine mirrors `gui::playbar::PlaybarModel` exactly and reuses its reserved `play.*`
codes, so the panel can be rewired onto the daemon with no semantic translation. `pause`/`step` in
`edit` are refused `play.not_running`; `play` when already playing, `pause` when already paused, and
`stop` in `edit` are benign no-ops. `step` leaves `playing`/`paused` alone (you may step from
either).

The one deliberate refinement is at the reply boundary: the playbar signals a benign no-op as
`ok=false` with **no** error code, which an R-CLI-008 envelope cannot express (a failure must carry
a catalog code). So a benign no-op answers `ok=true, changed=false`, and the playbar's `ok` maps
losslessly onto `changed`.

The `state` tokens (`edit` / `playing` / `paused`) are **byte-identical** to
`gui::playbar::state_token()` — the L-51 indicator is fed straight off the topic. The
`editor-session-multiclient-t2` drill links the real playbar and asserts this, so the two cannot
drift.

## `.editor/session.json` — the daemon is the single writer

The daemon writes the file on **clean shutdown** and restores it **before accepting the first
client** on the next boot. The Shell owns `config.json` / the dock layout and **never** this file
(the 03 §1 split).

```json
{
  "version": 1,
  "selection": { "ids": ["root/child", "root/other"] },
  "cameras": [ { "viewportId": "main", "transform": {…}, "projection": {…} } ]
}
```

* Cameras are an **array of objects carrying their key**, never a map-keyed object — the same
  encoding discipline the authored-data conventions mandate, so the file stays diffable.
* **Play state is deliberately not persisted.** A restarted daemon holds no live session, so
  restoring `playing` would be a lie about L-51 provenance. Boot is always `edit`.
* Writes go through a temp file + rename, so a crash mid-write leaves the previous good file intact.

### Corrupt-file recovery is loud and non-blocking (07 §6)

A file that is unparseable, structurally wrong, or from a **future** `version` is:

1. renamed aside to `.editor/session.corrupt.json` (`-1`, `-2`, … if that name is taken — evidence
   is never clobbered),
2. replaced by defaults, and
3. announced **loudly**: an `editor.session_state_invalid` diagnostic on the `diagnostics` topic
   (published before any client attaches, so a client subscribing with `sinceSeq: 0` replays it out
   of the R-CLI-015 ring) *and* a line on the daemon's stderr.

The daemon **still boots and serves**. Refusing to start over a convenience file would be strictly
worse than forgetting a selection. `editor.session_state_invalid` is its own catalog code, never the
R-QA-005 `session.state_invalid` of the `session *` file-harness family (C-F4).

### The OTHER half of the split recovers identically (M9 e09d)

`.editor/editor-state.json` — the **Shell's** file (dock layout, window placement, panel state blobs,
the e09c session undo journal, the e14b presence marker) — is disposable by the same contract, and
since e09d it recovers the same way: quarantined to `.editor/editor-state.corrupt[-N].json`, replaced
by defaults, announced under its OWN catalog code `editor.editor_state_invalid` on **stderr** and as a
`diagnostics` payload in the **Problems panel** (see `shell.md` § 14). It never blocks the boot
either.

The two announcements are **not** equivalent in durability, and the difference is worth knowing: the
daemon's diagnostic is replayed out of the R-CLI-015 ring to any client subscribing with `sinceSeq:
0`, whereas the Shell's lives only in the Problems panel's in-memory model — a daemon snapshot
carrying a `diagnostics` container would replace the whole set and take it with it. That does not fire
today (the real snapshot is a bare cursor), which is precisely why it is recorded as a caveat rather
than left reading as a guarantee.

And since e09d, preservation is a **precondition rather than a best effort**: if the unusable document
can be neither moved aside nor copied, the Shell refuses to write session state at all rather than let
the next flush destroy the only copy. Forgetting this session's layout is strictly better than
destroying the previous session's.

⚠ **"The same way" describes the shape, not yet the hardening.** e09d's review round fixed several
things on the **Shell's** half that the daemon's still carries: an `exists()` probe whose *error* is
honoured rather than read as "absent"; the same on the quarantine-slot probe (an errored probe there
reads as "free", and the rename then replaces — destroying an earlier salvage); the exhausted-slot
replacement announced instead of silent; a copy fallback when the rename is refused; the refuse-to-
write precondition above; a range-guarded `version` read (the daemon's `as_int()` on an unguarded
double is the same `float-cast-overflow` UB, latent only because no test feeds it one); and a
size-capped read. The daemon's exposure is *deferred* rather than absent — `persist_session_state`
runs at clean shutdown — but the parity is a filed follow-up, not a fact.

Two codes, not one, deliberately: a recovery diagnostic that cannot say WHICH session file was reset
has discarded the only distinction the ownership split created. And the messages differ too — the
Shell's names the window layout and undo history, because that is what the user actually lost.

**The split itself is mechanised, not documented**: `tools/check_session_ownership.py` (ctest
`editor-shell-session-ownership`) fails a tree where either file gains a second writer, where an
owner moves out of its process's subtree, or where the in-process override-write gateway is named
from product code. See `shell.md` § 14.

## Driving it from the CLI

`context attach` is a second client like any other:

```sh
context attach --project <dir> --editor-select "root/child,root/other" [--editor-select-mode add]
context attach --project <dir> --editor-play step --editor-ticks 2
context attach --project <dir> --editor-session          # read selection + cameras
```

Any `--editor-*` flag switches the drive from the edit/query file pair to the session state. The
reply reports this connection's `clientId`, so a script can apply the same echo-suppression rule the
SDK does.

## Who consumes it (M9 e08b — the GUI is rewired onto this; e08d — editor-core)

The panels no longer own selection or play state; they are subscribers and writers of the state above.

| consumer | writes | subscribes | where |
|---|---|---|---|
| Scene tree panel | `editor.select` via `scenetree::SelectionGateway` | `selection-changed` | `src/editor/gui/panels/scenetree/` |
| Playbar | `editor.play\|pause\|stop\|step` via `playbar::PlayControlGateway` | `play-state` | `src/editor/gui/playbar/` |
| editor-core `when` contexts | — (read-only) | `play-state`, via the Shell's `session.state` relay | `src/editor/webui/core/src/when.ts` (`DaemonSessionState`) + `session.ts` (the feed) + `boot.ts` (`startSession`) |
| editor-core play-bar strip (editor-window-chrome d1) | `session.control {verb}`, relayed by the Shell through the SAME `SessionFeed` writer | `play-state` (+ `simTick`), via the same `session.state` relay | `src/editor/webui/core/src/playbar.ts` + `commands.ts` (`play.*`) + `session_bridge.{h,cpp}` (the relay's write half) |

All three rows are wired. The editor-core row was **half-delivered by e08b** — `DaemonSessionState`
landed complete, but `boot.ts` still resolved its when-context from a frozen `edit` stub, so the
browser-side `playState` never moved — and **e08d closed it**: `boot.ts` builds the ONE when-context
provider over a live `DaemonSessionState`, and the stub constant was DELETED rather than left in
place. `boot.test.ts` fails if that source is reverted to a stub (verified by planting one).

**editor-core reaches this state through the Shell, not the daemon.** It is a pure wire-client of the
Shell (no daemon socket, no attach token), and the e05c bridge accepts no persistent queries — there
is no push channel to the renderer at all. So the Shell RELAYS its own `SessionFeed`'s last-known
state over the privileged bridge method **`session.state`**
(`src/editor/shell/session_bridge.{h,cpp}`), answering with the daemon's own `play-state` fact shape
plus `attached` / `generation` / `simTick` (the tick is additive, editor-window-chrome d1 — the
strip's `t+` timer renders it); editor-core hands that reply VERBATIM to `applyFact` and re-reads it
on a cheap generation-compare poll, exactly as `themes.get` / `keybindings.get` are read. Echo
suppression is not repeated there: it already happened Shell-side, in `SessionFeed`.

Since editor-window-chrome d1 the same bridge also carries the WRITE half, **`session.control
{verb: play|pause|stop|step}`**: the Shell relays each verb to the SAME `SessionFeed` writer the
docked playbar drives (`SessionFeed::control` -> `PlaybarModel` -> `editor.play|pause|stop|step`),
so the strip's buttons, the `play.*` palette commands and the dock panel are one implementation with
one echo-suppression story, and the reply (`{changed, state, simTick, errorCode}`) is the daemon's
own transport answer relayed through `PlaybarModel::adopt`. Because the daemon's echo of the Shell's
OWN write is dropped by `SessionFeed`, `session.state`'s `generation` **sums the applied-fact count
and the playbar model's control generation** (`editor_main.cpp`) — a locally driven transition must
move the generation, or every `session.state` poller in the process would freeze across exactly the
transitions the strip drives.

⚠ **Known staleness (CE #356).** Play state is published as a FACT and there is no `play-state` GET
verb, so after a daemon RESTART the Shell's — and therefore editor-core's — last-known state can be
stale with no honest repair. Resetting to `edit` on re-attach was rejected: a dropped wire to a
SURVIVING daemon would then falsely assert "no live session". `attached` is relayed so a consumer can
at least distinguish "no link" from "a daemon in edit"; the real fix is a daemon-side read verb.

Both C++ seams are declared in the boundary-clean panel libraries and implemented ONCE, wire-side, by
`shell::panels::SessionFeed` (`src/editor/shell/panels/session_feed.{h,cpp}`) — which is also where
the `origin` drop rule is applied, so no individual panel has to get it right. The Shell subscribes to
the `session` topic alongside `diagnostics` / `derivation` (`editor_main.cpp`).

The feed's `client::Client*` is a **non-owning view of a client the daemon lifecycle owns and
destroys** — on a lost daemon (`tear_down_link`) and at exit (`shutdown_at_exit`) — and a panel write
is renderer-driven, so it can land in the window between those and a reattach, or during the exit
pump. The Shell therefore re-derives the binding through the one `panels::bind_session_client(feed,
client)` seam immediately after each `lifecycle.pump()` (the only call that can change the client),
and clears it with `nullptr` **before** `shutdown_at_exit()` frees one; a cleared feed refuses every
write honestly. The per-connection echo-suppression id is derived from the client at that seam rather
than carried alongside it, so a stale id — which would silently suppress another client's facts while
applying our own — has nowhere to come from.

The in-process `SessionControl*` path the playbar used to drive is **removed**, not shadowed; the
runtime-session adapter that produces play frames lives on in `context_gui_playbar_session` (see
`src/editor/gui/playbar/README.md`).

## Tests

* `editorkernel-test_editor_session_state` — the state machine, the persisted projection, and the
  corrupt-recovery paths (T1).
* `editorkernel-test_kernel_server` — the RPC surface over the real transport: two clients, `origin`
  stamping, the scope refusals, and the persist/restore round trip.
* `editor-session-multiclient-t2` (`src/tests/integration/`) — the T2 drill against a REAL
  `context daemon`: the CLI **and** a scripted SDK agent as the two clients, play control, restart
  persistence, and corrupt recovery.
* `editor-shell-test_session_feed` — the consumer side over a SCRIPTED wire (a real `client::Client`
  on a mock channel): the fact dispatch, echo suppression, and the write seams.
* `editor-shell-test_session_bridge` — the e08d relay: the wire shape editor-core parses, the honest
  unbound / throwing-provider degradations, and the binding over a real `BridgeRouter`.
* `webui-ts-unit` (`core/src/test/session.test.ts` + `boot.test.ts` + `playbar.test.ts`) — the e08d
  browser half: the feed's reply -> sink path, and the BOOT WIRING (the live when-context resolves
  from the daemon's play state; the case fails if that source is reverted to a stub). Since d1 also
  the strip: the `session.control` client, the honest 3->5 `data-play-state` mapping with
  `compiling`/`error` pinned unreachable, and the full-boot press round trip
  (strip -> registry -> `session.control` -> reply adopted -> strip re-rendered).
* `editor-session-panels-t2` (`src/tests/integration/`) — the e08b T2 drill: the REAL Shell panel
  composition against a REAL daemon, with the real `context` binary as the second client. Both
  directions and the no-echo-loop property, which no scripted wire can check.

## Not in scope here

The `editor.ui` bus (e08c), writes/undo over the wire (e09b-2 / e09c — LANDED; see `docs/shell.md` § Session undo), and the **second-window** propagation
drill (e10 — it needs the multi-window subsystem, which is unbuilt). editor-core's `editor.ui` half of
the when-context is still the "nothing focused" baseline for the same reason: the bus is local to a
window, and mirroring it is e10's seam.
