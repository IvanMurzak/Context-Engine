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
  per-panel `origin`.

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

## Tests

* `editorkernel-test_editor_session_state` — the state machine, the persisted projection, and the
  corrupt-recovery paths (T1).
* `editorkernel-test_kernel_server` — the RPC surface over the real transport: two clients, `origin`
  stamping, the scope refusals, and the persist/restore round trip.
* `editor-session-multiclient-t2` (`src/tests/integration/`) — the T2 drill against a REAL
  `context daemon`: the CLI **and** a scripted SDK agent as the two clients, play control, restart
  persistence, and corrupt recovery.

## Not in scope here

Panel rewiring (e08b), the `editor.ui` bus (e08c), writes/undo over the wire (e09), and the
**second-window** propagation drill (e10 — it needs the multi-window subsystem, which is unbuilt).
