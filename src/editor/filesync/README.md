# `src/editor/filesync/` — the file-sync layer (M1)

The heart of the file-authoritative moat: **files are the single source of truth**, and this layer
keeps a derived index consistent with disk under dropped watcher events, torn writes, and bulk git
operations. It is `EditorKernel`'s real atomic-IO + watcher implementation, built **on** the kernel
platform seams (`context::kernel::Clock` / `TaskRunner` / `EventBus`, consumed read-only) — exactly as
`src/kernel/include/context/kernel/platform.h` anticipates.

Library target: **`context_filesync`** (links `context_kernel` + `context_serializer` +
`context_warnings`). Namespace: `context::editor::filesync`.

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
- **Binary-sidecar authoring rules (L-33, M2 wave 3)** — `sidecar.h`. Heavy numeric payloads live in
  versioned binary sidecars beside their authored JSON owner, never inline base64. The pieces: a
  **12-byte header codec** (8-byte magic + little-endian format version; the magic starts with `C`
  and embeds a NUL, so a sidecar can NEVER parse as JSON — `serializer::canonicalize()` therefore
  passes it through raw and the **canonical hash EQUALS the raw-byte hash by construction**, the
  R-FILE-001 sidecar rule); the authored reference shape `{"$sidecar": "<relpath>", "hash":
  "<decimal raw-byte hash>"}` (read out of the parsed tree by `serializer/sidecar_ref.h` — the
  serializer integration — with owner-relative resolution + the **R-SEC-008 jail** on every sidecar
  path here); **verification diagnostics** (`sidecar.dangling_ref` / `sidecar.hash_mismatch` /
  header codes) and an **orphan sweep** (`sidecar.orphaned`), all R-CLI-008 catalog codes; a
  bidirectional **`SidecarIndex`** feeding the reconciler's **sidecar-aware reconcile** — a
  changed/removed sidecar emits a synthetic `via_sidecar` change dirtying its registered owner(s);
  registration comes from the parse layer via `Reconciler::set_sidecar_refs`, since filesync never
  parses on the reconcile hot path — and the two **intent-logged plans**:
  `plan_sidecar_family_write` (sidecar-FIRST order — every sidecar durable before the referencing
  JSON write; refuses to author a dangling or lying ref) and `plan_owner_move` (**owned
  satellites**: a move/rename carries each referenced sidecar at its owner-relative relpath,
  dest-writes-then-src-removes so no observable mid-state has a referencing JSON without its
  sidecar — enabled by the `PlannedWrite` `remove` kind, CAS-guarded like writes on resume).
  Sidecars are **NEVER content-merged** — a sidecar conflict is whole-file ours/theirs
  (R-FILE-012(d)); `is_sidecar_bytes` is the classifier that merge layer will consume.
- **Native OS watcher backends (R-FILE-002)** — `NativeWatcher` is the real hint source behind the
  `Watcher` seam: **ReadDirectoryChangesW** (Windows, native recursive subtree), **inotify** (Linux,
  per-directory watches recursively maintained as directories appear, with a scan that closes the
  watch-add race), and **FSEvents** (macOS, file-level events on a private dispatch queue). The
  correctness model is unchanged — every delivered path is a hint the reconciler re-hashes
  *unconditionally*, and the crawl stays the dropped-event safety net, so hint loss costs latency,
  never correctness. `degraded()` **latches** on any loss-of-coverage signal (registration failure,
  kernel queue overflow / RDCW buffer overflow / FSEvents coalescing drops, backend death, or the
  userspace pending-cap tripping) and the reconciler then emits the visible `watcher.degraded`
  diagnostic naming the subtree and the crawl fall-back cadence — silent degradation is forbidden.
  `context daemon` composes `NativeWatcher` over its authored root; `NullWatcher` remains for
  in-memory compositions and scan-bound measurement rigs.
- **Native on-disk `FileStore` (R-FILE-002/004)** — `NativeFileStore` is the real backend behind the
  `FileStore` seam: `std::filesystem` for read/write/stat/list, a genuine **atomic rename** (POSIX
  `rename(2)`; Windows `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` — used directly because MinGW's
  `std::filesystem::rename` won't replace an existing target), and a real **fsync durability barrier**
  (`fsync` + best-effort parent-dir fsync on POSIX; `FlushFileBuffers` on Windows). Bytes are read/written
  in binary so content hashes are byte-exact across platforms. `MemoryFileStore` stays selectable for
  deterministic, fault-injectable tests; the whole file-authoritative loop runs against real disk when
  `NativeFileStore` is injected. Native impls never throw `SimulatedCrash` — tests model a real-disk crash
  with a thin decorator around the native store.

## Known M1 simplifications (deliberate, documented)

- **Intent-log payloads are stored inline** in the entry so a resume can always complete. A production
  impl would stage payloads in the temp files (write-temp-then-rename) to avoid doubling write volume;
  the recover-vs-diagnose contract is unchanged.
- **Path jail is logical** (lexical normalization + prefix check) over the injectable seam. The fully
  TOCTOU-safe variant (`O_NOFOLLOW` / `openat` relative to a jail-root fd, re-`realpath` after open) is a
  documented hardening follow-up, still deferred: the now-landed `NativeFileStore` does **not** implement
  it (see `native_file_store.h`), so its jail stays the logical `normalize` + `is_inside_jail` applied by
  the reconciler / `WriteQueue` that call the store.
- **Watcher hints are best-effort by design.** The native backends (`NativeWatcher`) deliver hints
  for the overwhelmingly common cases, but some OS shapes intentionally lean on the crawl instead:
  a directory moved into the tree may produce no per-file events for its contents (covered by a
  one-shot scan on the new directory plus the crawl), and a storm that overflows the kernel or
  userspace queues drops hints and latches `degraded()`. Hashes are authoritative, so all of these
  are latency costs, never correctness bugs. `NullWatcher` (always degraded, zero hints) remains the
  in-memory/measurement composition.
- **Crashed-write temp residue is not garbage-collected.** A crash between `atomic_write`'s temp write
  and its rename (or during `IntentLog::begin`'s own atomic write) can leave a `.tmp.<unique>` file
  behind. Such residue is correctly *ignored* by reconciliation (`is_atomic_temp_name` /
  `is_control_path`), so it never corrupts state — but nothing sweeps it, so a repeatedly-crashing
  daemon accumulates orphaned temp files. A production impl reaps residue that has no matching pending
  intent entry during the recovery crawl; deferred because a correct sweep must not race an in-flight
  rename.

## Tests

`ctest --preset dev` runs each `filesync-*` executable (see `CMakeLists.txt`): content hash, SHA-256 /
HMAC known-answer vectors, atomic-IO torn-write proof, path jail, watcher fault primitives, reconcile
index round-trip, expected-writes TTL, the reconciler pipeline (dropped-event + same-mtime-edit safety
nets, self-echo, degraded event), the intent-log crash-recovery / integrity / jail / CAS paths, the
binary-sidecar rules (header versioning, raw≡canonical hash, ref resolution + jail, dangling / orphan /
hash-mismatch diagnostics, sidecar-first + owned-satellite plans with crash windows between every
durable step, sidecar-aware reconcile), and — for the native on-disk store — the `NativeFileStore`
real-FS contract (atomic-rename replace, fsync, stat/list/remove, binary round-trip, real-disk
torn-write proof) plus on-disk intent-log crash recovery.
The `filesync-test_native_watcher` suite exercises the CURRENT platform's real backend on real FS
events: create/modify/remove/new-directory smoke, hint-driven reconciler convergence, self-echo
suppression under real watcher timing, a branch-switch storm converged by the crawl safety net, and
the degraded-registration diagnostic (every hint wait is bounded; convergence asserts on the
crawl-converged end state so OS timing can never flake it).
