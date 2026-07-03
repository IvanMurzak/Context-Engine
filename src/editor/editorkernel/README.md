# `src/editor/editorkernel/` — the EditorKernel composition (M1)

`context_editorkernel` wires the merged M1 file-authoritative libraries into ONE runnable, headless
**attach path** (R-ARCH-005 / R-BRIDGE-008). It does not re-implement any of them — it composes them
through their public headers:

```
file
  │  context_filesync   watch-hash-reconcile (R-FILE-002), atomic-IO writes (R-FILE-004)
  ▼
reconciled change
  │  context_derivation  incremental graph (L-19/L-22): canonical parse → derived World,
  │                      + the monotonic generation counter (R-CLI-006 read barrier)
  ▼
derived context::kernel::World  (context_kernel)
  │  context_bridge      daemon-per-worktree single-instance lock (R-BRIDGE-001),
  │                      client event stream (R-BRIDGE-008), R-SEC-007 scope dispatch
  ▼
attached client (capability handshake)
```

Everything runs on the injectable platform seams (`FileStore` / `Watcher` / `Clock` / `TaskRunner`),
so the whole loop is deterministic and headless (R-HEAD-001 / R-QA-010) — no GPU, display, or real
wall-clock.

Two write paths reach the derived World:

- **`edit_file()`** — the daemon-initiated *CLI-verb* write: scope-checked (`file_write`, R-SEC-007),
  routed **through filesync atomic-IO**, then ingested into derivation. Returns a `WriteTicket` whose
  `canonical_hash` is the own-write read barrier key (`--after-hash`, R-CLI-006).
- **`ingest_external()`** — folds a **raw** out-of-band edit (a hand edit / another tool) that the
  reconcile crawl detected into derivation (content hash is authoritative over watchers, R-FILE-002).

## M1 seam notes

- The daemon's single-instance lock lives on the **real** filesystem (`project_root`), while the
  reconcile pipeline operates over the **injectable** `FileStore` seam (`filesync_root`). With the
  portable in-memory `FileStore` the two roots are distinct; when the native `FileStore` lands they
  coincide on one on-disk directory.
- The composition forwards the **derived-world generation** onto the client event stream as
  `derivation.settled{generation}`. Cross-process transport, a native `FileStore`, and an OS watcher
  are follow-ups (the merged components ship `MemoryFileStore` / `NullWatcher` seams at M1).

Tests: `tests/test_editor_kernel.cpp` — the composed-loop integration smoke (boot + attach handshake,
CLI-verb edit with the read-your-writes barrier, raw edit, single-instance attach signal, and the
R-SEC-007 scope-denial exit-class-6 path).
