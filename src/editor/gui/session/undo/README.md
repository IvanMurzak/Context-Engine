# `src/editor/gui/session/undo/` — M5-F7: GUI session undo/redo

The observer editor's **session undo/redo** (`context_gui_undo`, **R-HUX-001** / **R-FILE-006** /
**R-FILE-007** / **L-20** / **L-21** / **L-30**, issue #162): familiar **Ctrl+Z / Ctrl+Y** session
undo with **gesture-batch auto-checkpointing** (one undo step per gesture, not per keystroke), scoped
to the M5 shipped editing surface — the **F3 inspector override writes** (`../../panels/inspector/`).

This is a **short-horizon session convenience layered on the write path**, NOT an engine undo
subsystem: durable/long-range history stays **git** (R-FILE-007 / L-21). It reuses the existing
`cas.mismatch` error-catalog code — **no new error-catalog codes**.

## The load-bearing safety property (R-HUX-001)

Undo/redo is **replayed through the SAME write path as any other mutation** — the serialized write
queue, `--if-match` CAS, and the **L-30 rebase-or-drop** policy — so an undo can **never clobber a
concurrent writer** (human or AI). A naive "restore the previous bytes" undo is exactly what
R-HUX-001 forbids. Each replay:

1. **reads the field's CURRENT value** (the up-front no-clobber guard). If it no longer holds the value
   we last wrote, a concurrent writer touched it → the undo/redo is **DROPPED LOUDLY** (a `cas.mismatch`
   diagnostic + event), the stale value is never restored over the co-writer's change;
2. otherwise routes the revert (or re-apply) through **`inspector::commit_override_write`** — the ONE
   L-20/L-30 commit engine shared with the inspector's gesture commit — CAS-guarded on the just-read
   hash: if a writer races between the read and the write, it **rebases** onto the new state when the
   field path is untouched, or **drops** if the same field path collided. Collision is decided at
   **field-path granularity** (R-FILE-006 / L-30), not file granularity.

Only a **cleanly-reverted** checkpoint (every field applied/rebased) moves onto the redo stack; a
dropped undo is a loud, terminal no-progress event — the user re-applies.

## Pieces

- **`undo_journal.h` / `undo_journal.cpp` — `UndoJournal`.** The undo + redo stacks over the inspector
  override-write surface:
  - **`FieldEdit`** — one reversible edit (root scene + L-35 id-path + entity-relative pointer +
    `before`/`after` values). Self-contained, so a checkpoint survives selection changes and JSON
    round-trips.
  - **`Checkpoint`** — a gesture (L-20): usually one field edit, occasionally a batch (`begin_gesture`
    / `capture` / `end_gesture`) reverted in reverse order with independent per-field L-30 handling.
  - **`record` / `capture`** — a lone `capture` auto-checkpoints one gesture; recording any new gesture
    invalidates the redo future.
  - **`undo()` / `redo()`** — the CAS-guarded, no-clobber replay above, returning a `ReplayResult`
    whose per-field outcomes reuse `inspector::CommitResult`.
  - **`to_json()` / `load_json()`** — canonical-serializable persistence for the gitignored
    **`.editor/session.json`** (R-FILE-006 ephemeral session state). Total + robust: a malformed
    session file leaves an empty journal, never throws.
  - **`build_panel()`** — a headless `uitree::Panel` exposing undo/redo availability + one
    keyboard-reachable command per available action, **a11y-conformant by construction**
    (`uitree::audit_a11y` returns no violations) and deterministic. Commands appear only when
    reachable → never an unreachable-command violation.

The journal owns **NO disk / no filesync dependency** (mirroring the inspector panel): it commits
through the `inspector::OverrideWriteGateway` seam and (de)serializes to a JSON tree. Wiring the
gateway to the live daemon write path, binding Ctrl+Z/Y, and persisting `.editor/session.json` is the
**CEF-host integration path** (a trailing M5 surface), out of this headless module's scope. The
mechanism — write-path replay, CAS, rebase-or-drop — **extends to viewport transforms once F1 lands**
(R-HUX-001).

## Building + testing (default `dev` gate — no CEF)

```bash
cmake -S src --preset dev && cd src && cmake --build --preset dev
ctest --preset dev -R "^gui-session-undo-"   # journal replay / L-30 / a11y
```

- `gui-session-undo-test_undo_journal` — gesture-batch checkpointing, undo/redo replay through an
  in-memory gateway, the **L-30 rebase** (unrelated field moved) and **loud DROP** (this field
  collided — the **R-HUX-001 no-blind-clobber** assertion: `attempts == 0`, the co-writer's value
  untouched), redo, redo-future invalidation, and `.editor/session.json` JSON round-tripping.
- `gui-session-undo-test_undo_a11y` — the per-surface a11y scan + keyboard-only reachability
  (R-A11Y-001) across the empty / undoable / undo+redo-available states.

Both run inside the default `build` job's `ctest --preset dev` on ubuntu/macOS/windows (no CEF).
