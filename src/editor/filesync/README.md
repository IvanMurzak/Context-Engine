# `src/editor/filesync/` — the file-sync layer (M1)

The heart of the file-authoritative moat: **files are the single source of truth**, and this layer
keeps a derived index consistent with disk under dropped watcher events, torn writes, and bulk git
operations. It is `EditorKernel`'s real atomic-IO + watcher implementation, built **on** the kernel
platform seams (`context::kernel::Clock` / `TaskRunner` / `EventBus`, consumed read-only) — exactly as
`src/kernel/include/context/kernel/platform.h` anticipates.

Library target: **`context_filesync`** (links `context_kernel` + `context_warnings`). Namespace:
`context::editor::filesync`.

## What it implements

- **Atomic IO (R-FILE-004)** — `atomic_write()` stages into a sibling temp file, fsyncs, then renames
  over the target. A reader observes fully-old or fully-new content, never a torn write. Proven by a
  test that injects a crash between the temp write and the rename.
- **Watch–hash–reconcile pipeline (R-FILE-002)** — `Reconciler`. OS watchers are **hints only**; the
  **content hash is authoritative**. A watcher-hinted path is re-hashed *unconditionally* (mtime
  granularity is untrusted); the cold-scan crawl is mtime+size-gated; a **low-frequency full re-hash
  crawl** is the dropped-event safety net (convergence even if every watcher event is lost). The
  **expected-writes table** self-echo-suppresses the daemon's own writes on a short TTL (which can
  never mask a genuine later external edit). The **persisted reconcile index** (`.editor/index`,
  gitignored, rebuildable) makes warm attach mtime/size-gated. A degraded watcher emits a visible
  `watcher.degraded` log event — never a silent fall-back to crawl latency.
- **Crash-recovery intent log (R-FILE-004)** — `IntentLog` + `WriteQueue`. Per in-flight op it records
  `(opId, planned writes, target content-hashes)`, fsync'd **before** the first write and cleared
  **after** the last durable rename. On restart the recovery pass **resumes** incomplete ops (or emits
  a machine-readable diagnostic). Every entry is **HMAC-integrity-checked** (dependency-free SHA-256 /
  HMAC-SHA256), and every resumed write is re-**jailed** (R-SEC-008) and re-**CAS'd** against its
  planning-time hash, so a forged or moved-on entry can neither escape the project root nor clobber
  changed state. All multi-file verbs are idempotent under partial apply.
- **Injectable seams from day one (R-QA-010)** — a virtual `FileStore` (with a fault-injectable
  `MemoryFileStore`), a virtual `Watcher` (`FakeWatcher` can drop/duplicate/reorder events), and the
  kernel `Clock`. The seams are M1 architecture and are **not retrofittable**; the deterministic
  fault-injection harness (a later task) drives them.

## Known M1 simplifications (deliberate, documented)

- **Intent-log payloads are stored inline** in the entry so a resume can always complete. A production
  impl would stage payloads in the temp files (write-temp-then-rename) to avoid doubling write volume;
  the recover-vs-diagnose contract is unchanged.
- **Path jail is logical** (lexical normalization + prefix check) over the injectable seam. The fully
  TOCTOU-safe variant (`O_NOFOLLOW` / `openat` relative to a jail-root fd, re-`realpath` after open) is
  the native `FileStore` impl's responsibility.
- **`NullWatcher`** is the portable default (always degraded → correctness from the crawl). A real
  inotify / ReadDirectoryChangesW / FSEvents watcher is a later platform-specific drop-in behind the
  same seam; because hashes are authoritative, correctness never depends on it.

## Tests

`ctest --preset dev` runs each `filesync-*` executable (see `CMakeLists.txt`): content hash, SHA-256 /
HMAC known-answer vectors, atomic-IO torn-write proof, path jail, watcher fault primitives, reconcile
index round-trip, expected-writes TTL, the reconciler pipeline (dropped-event + same-mtime-edit safety
nets, self-echo, degraded event), and the intent-log crash-recovery / integrity / jail / CAS paths.
