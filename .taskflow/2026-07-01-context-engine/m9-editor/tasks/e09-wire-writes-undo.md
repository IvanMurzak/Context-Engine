---
id: e09-wire-writes-undo
title: Writes over the daemon RPC (D22) — WireOverrideWriteGateway CAS path, undo journal wiring, session-file ownership split
group: A
sequence: 7
repo: "."
base_branch: "main"
depends_on: [e02-client-sdk-boundary]
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [05, 01, 03]
---

## Goal

Retire the editor's in-process write shortcut: implement `OverrideWriteGateway` over the live
daemon `edit`/`edit-batch` RPC with raw-byte CAS, keep L-30 rebase-or-drop guarantees under
concurrent human+AI editing by construction, wire the undo journal to real persistence, and
enforce the session-file ownership split (C-F3). This is the user-data-integrity task.

## Scope & seams

- **WireOverrideWriteGateway** (`context_client`-based; seam `inspector_panel.h:57-73`):
  gesture commit → RPC `edit`/`edit-batch` with `--if-match` raw-byte CAS (`file_write`
  scope); `cas.mismatch` → the EXISTING `commit_override_write` rebase-or-drop engine
  (`inspector_panel.h:187-193`) reruns against fresh state; drop is LOUD (notification +
  `editor.ui` fact + wait-hue surface — 10 invariants).
- The in-process compose/filesync gateway remains ONLY for headless T1 tests (injected mock,
  as today — `inspector_panel.h:10-13` trailing-M5 surface closed).
- **Read-your-writes**: `--after-generation` barriers where a gesture must observe its own
  commit (05 §7); panels hydrate from `query`/`snapshot` + subscriptions.
- **Undo journal wiring**: the host actually reads/writes the journal
  (`undo_journal.h:129-136` `to_json/load_json` — never called by a host today); journal
  lives in the editor-owned `.editor/editor-state.json`; replay routes through the SAME wire
  write path (R-HUX-001; canonical flow 05 §8).
- **Session-file split enforced** (03 §1 / C-F3): daemon = single writer of
  `.editor/session.json`; editor Shell = single writer of `.editor/editor-state.json`;
  no cross-process writes to the other's file (T1 assert); both disposable-by-contract
  (corrupt → aside + defaults, loudly — 07 §6).
- Editor performs NO direct project-file writes — all mutations via daemon verbs (08 threat
  table row; structural assert in the boundary/include-graph discipline).

## Definition of Done

- [ ] The canonical 05 §8 sequence T2-asserted against a LIVE daemon: DOM gesture → RPC edit
      → derivation → events fan out → second window updates
- [ ] Concurrent-CAS drill (scripted second `context_client` racing the same field): rebase
      path AND drop-loudly path both asserted (T2)
- [ ] Undo/redo replays over the wire; journal persists to editor-state.json and restores
      across restart
- [ ] Ownership split enforced by tests; corrupt session/editor-state recovery loud +
      non-blocking
- [ ] In-process gateway unreachable outside T1 mocks (structural assert)
- [ ] 3-OS CI green incl. sanitizer legs
