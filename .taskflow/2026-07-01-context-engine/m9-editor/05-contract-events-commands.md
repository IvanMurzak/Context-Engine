# 05 — Contract work: fan-in, client SDK, session state, events, commands

Engine-side and client-side contract work M9 requires. All additions are **additive under
protocolMajor=1** (new operational verbs + new topics are capability-listed) with **one
deliberate behavioral tightening: attach-token enforcement (D20)** — not additive for a
tokenless client, and sequenced honestly (C-F1): e01 lands enforcement behind a daemon
compatibility flag, e02 migrates the CLI onto `context_client`, then enforcement defaults ON.
Everything else keeps the frozen-contract discipline.

> ⚠️ **FACTUAL CORRECTION — 2026-07-19 (learned the hard way in e02).** This section originally
> called the CLI *"the only existing client; no external releases exist"*, and that premise is what
> made the enforcement flip look safe. **It was wrong**: it omitted the **in-repo `RpcClient` test
> harness**, so flipping D20 ON reddened **all five `m1-exit-*` gates** until the harness was given
> a token. e02 shipped correctly once that was found, but the lesson generalizes and **e04/e05 flip
> more seams**: before any change to client-visible behaviour, enumerate **every in-repo consumer —
> test harnesses and fixtures included — not just shipped clients.** Ledger:
> [`ROADMAP.md`](ROADMAP.md), 2026-07-19 e02 entry.

## 1. Daemon multi-client fan-in (D19)

Today: serial single-connection (`transport.h:14-17`, `kernel_server.h:37`). M9:

- `TransportServer` accepts N concurrent connections; per-connection `Session` (scopes, ack
  cursors); requests funnel into the existing single-threaded dispatch/write queue (L-50
  serialization is preserved — concurrency is at the TRANSPORT, not the mutation model).
- Event fan-out: per-subscriber queues already exist (`event_stream.h:59-94`); wire them to N
  live connections (push frames) instead of poll-only.
- `clients` topic reports attach/detach (already registered; now it fires).
- Bounded: max connections + per-connection frame budgets; a stuck client hits the existing
  gap-marker path, never stalls the daemon.

## 2. Attach auth (D20)

- Handshake gains an OPTIONAL `token` field (additive): daemon verifies against
  `.editor/instance.json`'s token; wrong/missing token → `attach.denied` (uniform error catalog).
- Windows named pipe gets an owner-SID DACL (closing the documented gap); POSIX keeps 0600
  socket + instance file.
- The Shell reads instance.json (or spawns the daemon and receives the token on stdout pipe);
  editor-core and panels NEVER see the token (04 §1).

## 3. Client SDK

- **`context_client`** (C++, installed/exported — the first installed **client-SDK library**;
  the root `src/CMakeLists.txt:284-288` today installs only the `context-hello` demo +
  `context_kernel` for the R-VER-004 layout — A-F1):
  endpoint discovery, connect/attach (token, scopes, protocol negotiate), typed request helpers,
  and the **subscription consumer**: maintain N topic subscriptions, snapshot-then-delta
  application, ack cursor management, gap → automatic re-snapshot, reconnect-with-backoff +
  incarnation-epoch handling (fresh snapshot on daemon restart). This is the missing piece every
  live client needs (01 §3) — the CLI's `wire_client` plumbing is lifted into it and the CLI
  migrates onto it (one implementation).
- **JS client** (inside editor-core): thin typed wrapper over the Shell IPC bridge; its verb/
  event typings are **generated from `describe`** at build time (the registry is already the
  single source of truth — codegen is a projection, hand-written typings are prohibited by the
  same R-CLI-009 spirit).
- Out-of-tree story: `context_client` + generated schema = what an alternative-editor author
  uses; the boundary CI job (D10) builds our own editor exactly that way.

## 4. Selection / camera / play state → daemon session state (D7)

New daemon-side **editor session state** (in-memory, per L-20; NOT authored files):

- Verbs (operational, `session_control` scope), under the **`editor` namespace** — the
  existing deterministic `session *` family (a headless harness over a state FILE,
  `registry.cpp:401-477`) is untouched and stays distinct (C-F4): `editor select {ids[], mode}`
  (L-35 id-path keys, as the panels already use), `editor camera set {viewportId, transform,
  projection}`, `editor play|pause|stop|step` (real RPC play control — today playbar is
  in-process only), plus reads `editor selection get`, `editor cameras get`. Both families
  live in the one registry, so R-CLI-013 parity CI covers the additions automatically.
- Topic payload extensions on `session` (additive): `selection-changed {ids, origin}`,
  `camera-changed {viewportId, origin}`, `play-state {playing|paused|stopped, origin}` — with
  `origin` = client id, enabling echo suppression.
- **Effect**: scene tree, inspector, N viewports across N windows, the CLI, and AI agents all
  share one selection truth; an agent can answer "what is the human looking at" from the
  contract. Persisted into `.editor/session.json` on clean shutdown (restore convenience) — daemon-owned
  per the session-file split (03 §1); the editor's own UI state lives in
  `.editor/editor-state.json`.
- The GUI panels' existing local selection seams (`scene_tree_panel.h:62-68`) become
  subscribers/writers of this state rather than private owners.

## 5. `editor.ui` bus (editor-local, D7)

- Lives in editor-core (mirrored across windows via the Shell); envelope discipline mirrors the
  daemon stream (seq, topic, snapshot-on-subscribe) so consumers use ONE mental model.
- Topics: `editor.ui.focus` (active window/panel), `editor.ui.layout` (dock/layout changes),
  `editor.ui.drag`, `editor.ui.viewport` (hover/pick facts), `editor.ui.theme-changed`,
  `editor.ui.palette`. Package panels subscribe via the bridge **under the `ui_events`
  capability** (install-consent-listed — C-F18); they may register namespaced custom topics
  (`<pkg>.…`), declared in the manifest — same registry model as daemon topics.
- NOT forwarded to the daemon (D7): selection/camera/play — the semantic facts — already live
  daemon-side; chrome noise stays local. Facts only, never commands.

## 6. Commands, palette, keybindings (D8)

- **One command registry** in editor-core; sources: (a) contract verbs — auto-projected (the
  `help_model.h` pattern generalized; palette entries carry the same introspected docs), (b)
  editor commands (window/dock/theme/navigation), (c) panel-manifest commands. R-HUX-004's
  palette SHOULD lands here as MUST-in-practice for M9.
- **when-contexts**: `panelFocus`, `panelType`, `viewportMode`, `playState`, `textInputFocus`,
  `windowType` — evaluated from `editor.ui` + session state. Resolution order: text-input >
  focused panel > window > global (03 §6).
- **Keymap**: default map ships in editor-core; user overrides in a per-user canonical-JSON file
  (`~/.context/keybindings.json`), watched + hot-reloaded, schema-validated ($schema/version —
  same discipline as themes). Every binding targets a command id; no raw key handlers anywhere
  (enforced by review + a lint in T1).
- Session undo commands (`session.undo/redo` — `undo_journal.h:91-92`) finally get their real
  Ctrl+Z/Ctrl+Y bindings through this path.

## 7. Writes over the wire (D22)

- `OverrideWriteGateway` gets a wire implementation: gesture commit → RPC `edit`/`edit-batch`
  with `--if-match` raw-byte CAS; `cas.mismatch` → the existing rebase-or-drop engine
  (`commit_override_write`) reruns against fresh state. The in-process compose/filesync gateway
  remains ONLY for headless T1 tests (injected mock, as today).
- Result: the editor app performs authored mutations exactly like any agent — same queue, same
  CAS, same events; L-30 guarantees hold under concurrent human+AI editing by construction.
- Read path: panels hydrate from `query`/`snapshot` + subscriptions with a read-your-writes
  barrier where a gesture must observe its own commit.
  > ⚠ **CORRECTED 2026-07-25 (e09b-2, run `e59866f7011a`) — this clause named the WRONG flag.**
  > The barrier is **`--after-hash`** (`EditorKernel::query_after_hash`), which is **LIVE** and which
  > the daemon **already applies inside `edit`**, reporting the outcome as `reflected`.
  > **`--after-generation` is reserved-but-accepted and INERT in v1** — `registry.cpp`'s
  > `make_core_flags()` states it plainly: `--idempotency-key`, `--after-generation` and
  > `--atomic-plan` "stay reserved-but-accepted in v1 (their behavior activates with the replay
  > store / the batch backend); `--after-hash` is LIVE". An implementer following the original
  > wording would have passed a **silently no-op flag** and believed it had a barrier. e09b-2 used
  > `--after-hash` and **counted its verdict rather than assuming it**.
  > ⛔ The immutable spec `tasks/e09-wire-writes-undo.md:30` still carries the old wording — it is
  > origin-of-record and stays unedited; **this doc is the authority.**

## 8. Sequence: one inspector edit, end-to-end (the canonical flow)

```
DOM input (window 2) → hydration → command "inspector.edit"
  → panel model stage_edit (C++ via bridge)
  → gesture end → commit → WireOverrideWriteGateway
  → RPC edit {file, pointer, value, ifMatch: <raw-hash>}     [file_write scope]
  → daemon write queue → atomic write → watcher/hash → derivation (dirty subgraph)
  → events: files.changed → derivation.settled{gen} → diagnostics (stability)
  → all subscribed clients (window 1, window 2, CLI, agents) update
  → undo journal records the checkpoint (replay = same path)
CAS miss → re-read → field untouched? rebase+retry : drop LOUDLY + notification + editor.ui fact
```
