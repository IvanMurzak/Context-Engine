# Context Game Engine — Requirements

> **Engine name:** Context Game Engine ("Context").
> **Status:** design phase complete (2026-07-01/02). **Implementation status (2026-07-15):** M0–M7
> COMPLETE in the engine repo (`IvanMurzak/Context-Engine`); M8 (build pipeline) next. This folder
> remains the design authority; per-milestone as-built deltas live in the engine repo.
> **Purpose:** the authoritative, reviewable list of what the engine must do. Every requirement
> has a stable ID, a priority, a description, and a rationale so it can be discussed, accepted,
> deferred, or rejected individually.
> **Provenance:** this document is current-state text. Full per-round review provenance — the
> 2026-07-01 design-review remediation and review rounds 1–5 (2026-07-01 → 2026-07-02), including
> every amendment, supersession, and owner ruling — lives in `REVIEW-2026-07-01.md` and the
> `REVIEW-R1…R5` trackers in this folder.

**Priority key (MoSCoW):**
`MUST` = non-negotiable for v1 · `SHOULD` = strongly wanted, may slip · `COULD` = desirable, opportunistic · `WON'T (v1)` = explicitly out of scope for the first version · `REMOVED` = retired by owner ruling, entry kept for the record (see R-DATA-003).

**How to review:** read each requirement, and for any you disagree with, reference it by ID
(e.g. "R-LANG-003 should be MUST, not SHOULD"). Gaps you spot should become new `R-*` entries.

---

## 1. Terminology

- **Editor** — the desktop application (Windows/macOS/Linux) that lets a human author a game: place and edit 3D objects, tweak properties, play the game instantly without a full build, hot-reload on file change, debug, and profile.
- **EditorKernel** — the headless authoring engine: all editor capabilities that do **not** require a display, runnable on a monitorless Linux VPS and controllable entirely via CLI. The GUI is a client layered on top of EditorKernel.
- **RuntimeKernel** — the game execution engine that ships inside every Game Build and is embedded inside EditorKernel for play-in-editor.
- **Build / Game Build** — a fully packaged game for a specific target Platform.
- **Platform** — a target OS/runtime: Windows, macOS, Linux, Android, iOS, Web (browser; WebGPU-only in v1 — L-56). The v1 platform set is Windows/macOS/Linux desktop + Linux server/headless + Web (MUST); Android is a trailing SHOULD; iOS is v2-first — see R-BUILD-001.
- **Project** — a folder of files describing one game that the Editor operates on.
- **Package** — a modular, independently-versioned unit of functionality added to a Project or to the engine (engine-system package or gameplay/content package).
- **Authored state** — the project truth: everything that lives in Project files and would be committed to version control.
- **Session state** — ephemeral per-session data (selection, camera, in-flight gestures, play-mode runtime state); lives in EditorKernel memory, never in authored files.
- **Derivation graph** — EditorKernel's incremental, content-hash-memoized computation from files to derived state (parsed scenes, composed worlds, imported assets, diagnostics).
- **Event bus** — the kernel-internal, in-process pub/sub used by RuntimeKernel systems (R-KERNEL-001); never exposed to clients.
- **Event stream** — the client-facing, sequenced notification stream on the bridge (R-BRIDGE-008); carries post-derivation facts, not kernel internals. Distinct from the event bus above.
- **Authored code files** — TS/shader source files: authored **text** with toolchain-defined formatting, exempt from the canonical-JSON rules (L-32 carve-out); their compile/typecheck diagnostics flow through R-FILE-003.
- **Generation (derived-world generation)** — the monotonic counter the derived world carries; every query result, snapshot, and event batch is stamped with the generation it reflects, and a snapshot is internally consistent to a single generation (R-BRIDGE-008).
- **Settled / quiescence** — the state in which no dirty subgraph and no in-flight async import remain; signalled by the `derivation.settled{generation}` event — the machine-readable "the daemon is done reacting to my writes" answer (R-BRIDGE-008).
- **Incarnation epoch** — one run of the daemon; `seq` is a single global monotonic counter within an incarnation (assigned at post-derivation emission, totally ordered across topics), and every snapshot/batch carries an `incarnationId` so a restart forces a fresh snapshot rather than trusting a stale "since seq N" cursor (R-BRIDGE-008).
- **Diagnostic `stability`** — the field on every diagnostic (and other derived fact): `provisional` while the generation is still churning, promoted to `stable` once that generation settles; `context validate` reports against the last stable generation (R-BRIDGE-008 / R-FILE-003).
- **EditorKernel as project language server ≠ text-editor LSP** — "the LSP model" (L-19, §2a) names an *architecture*: EditorKernel is the **project-level language server** — a live derived index over project files, exposed via **CLI/RPC/MCP**. The engine ships **no custom text-editor LSP implementation** (**L-59** [owner-ruled 2026-07-01]): editor intelligence over authored JSON (autocomplete, validation, hover) comes from the published per-kind **JSON Schemas** (L-32) consumed by existing schema-aware editors; cross-file semantic queries go to EditorKernel's own query surface (R-CLI-006/013). An optional thin LSP shim forwarding to EditorKernel's query API is post-v1 sugar.
- **Id-path** — the nested-instance addressing form `[instanceId, …, entityId]` (L-35): override entries, gesture/merge conflict detection, and the `--at-instance` write flag address an entity anywhere in a nested-instance tree by the path of instance ids from the addressing scene down to the entity.
- **Composed identity (composed-entity identity)** — the deterministic identity of an entity in the composed/derived world: the full id-path from the root scene, `[rootScene, instanceId, …, entityId]`, or its stable hash (L-37). Stable across re-derivation and engine upgrade; migrations must not alter it. The ONE identity shared by player saves (R-DATA-005), network ids (R-NET-001), and query results.
- **Scene-root entity** — the well-known root entity every scene carries; scene-level state (settings) lives as singleton components on it, so overrides, queries, provenance, merge, and migration apply to scene settings for free (L-35). Its scene-settings components are inert by default when the scene is instanced as a sub-scene; a component type may declare `composable` to opt in.
- **`componentVersions` map** — the authored-file header map `{"<ns>:<type>": <schemaVersion>}` stamping each component payload's schema version: schema versioning is per-COMPONENT-PAYLOAD, not only per file kind (L-32/L-37); parse-time migration selects migrations per payload by stamp.
- **Schema vocabulary** — the shared engine vocabulary every per-kind JSON Schema uses (R-DATA-006): `x-ctx-type` (engine semantic types), `x-ctx-storage` (numeric width/layout), `x-ctx-ref` (typed reference targets), the pinned tagged-union convention, and the SI-units + radians law with per-field `x-ctx-units` in introspection.
- **Observer-grade (editor)** — the v1 scope of the M5 editor: the viewport (3D + 2D) + play controls + scene tree + an inspector whose edits are override writes + the Problems panel; richer authoring surfaces trail post-M5 within v1 or move to v2 (§14c; the trailing bucket is homed in `ROADMAP.md` §1, M8.5).

---

## 2. Core architecture & process model

### R-ARCH-001 — Headless-core, thin-clients architecture `MUST`
**Requirement.** All authoring capability lives in a headless **EditorKernel**. The GUI, the CLI, and any AI/MCP agent are **clients** with an equal capability *surface* via the same public contract — file writes for mutations (R-ARCH-002) plus one RPC surface; no client class has a privileged private API. Per-client **authorization** (R-SEC-007 scopes) MAY restrict what a given client is allowed to do.
**Rationale.** Guarantees CLI-completeness by construction and enables headless/automated/AI-driven use — while permitting least-privilege for unrecognized clients.

### R-ARCH-002 — Files are the single source of truth; the file-write law `MUST`
**Requirement.** All authored project state lives in files, and every mutation of authored state — from GUI, CLI, or AI — is performed as a file write (L-19). EditorKernel exposes an RPC surface only for non-authored operations: queries, session actions (play/pause/step), and derived outputs (build, import, screenshot). The law: **if it changes what would be committed to version control, it happens as a file write; everything else is an RPC.**
**Rationale.** One truth, no dual-path reconciliation; AI text edits are first-class by construction; git-native collaboration and crash safety.

### R-ARCH-003 — Two distinct cores `MUST`
**Requirement.** EditorKernel (authoring) and RuntimeKernel (game execution) are separate components. RuntimeKernel is embedded inside EditorKernel for play-in-editor and ships standalone in builds.
**Rationale.** One runtime codebase in both editor and build eliminates the "works in editor, breaks in build" gap.

### R-ARCH-004 — Durable state on disk `MUST`
**Requirement.** Project state (scenes, assets, settings, dependencies) is persisted as files on disk; in-memory session state is reconstructable from those files.
**Rationale.** Crash resilience, version control, collaboration, and AI-agent operation.

### R-ARCH-005 — Deterministic, documented startup/attach algorithm `MUST`
**Requirement.** Any client's connection to a Project follows a documented sequence: discover a running EditorKernel for that Project → attach if alive → otherwise operate **in-process as a library** (R-FILE-008) or spawn a daemon and attach. The in-process library mode is part of the documented algorithm, not an afterthought.
The **first action** of any **write-capable** instantiation is an **atomic try-lock** of the exclusive `.editor/lock` (R-BRIDGE-001). **Try-lock failure *is* the "an instance is live → attach" signal** — there is a single gate, with no separate liveness probe that could race the lock. In-process library mode (R-FILE-008) is never a gatekeeper: it either **publishes a minimal attach endpoint** for the duration of its run (so a concurrently-arriving client attaches instead of contending) **or** holds the lock as a **shared/read lock for read-only work and escalates to the exclusive lock per write**. This keeps "the daemon is an optimization, never a gatekeeper" true even when the current holder is a transient CLI process rather than a long-lived daemon.
**Rationale.** Foundation of the single-instance and bridge requirements below; the daemon is an optimization, never a gatekeeper.

---

## 2a. File-authoritative EditorKernel (LSP model)

> Locked 2026-07-01 (L-19…L-31). EditorKernel follows the language-server architecture: files are
> the only truth; EditorKernel is a live derived index over them. **RuntimeKernel is exempt** — it
> ships inside Game Builds and consumes only compiled derivation output (R-FILE-009).
> Terminology note (L-59): "language-server architecture" means EditorKernel is the project-level
> language server exposed via CLI/RPC/MCP; the engine ships no text-editor LSP — see §1.

### R-FILE-001 — Canonical, deterministic serialization `MUST`
**Requirement.** Every authored file kind has one canonical serialization: stable key order, stable formatting, stable array ordering. The canonical form additionally pins **number formatting** — shortest-round-trip float formatting per ECMAScript `Number::toString`; `NaN`/`Infinity` are banned in schemas; `-0` is defined (serialized as `0`) — and **NFC Unicode normalization** for strings. Two hashes are defined per file: the **raw-byte hash** (used by watch/reconcile and CAS `--if-match`) and the **canonical-content hash** (used for derivation/cache keys), so cosmetic byte differences never poison derivation keys. Tool saves always canonicalize the whole file they write; external non-canonical formatting is normalized on the first tool save (which also gives L-34 ref-hint healing a consistent story). New-Project templates ship a `.gitattributes` pinning authored JSON to LF/text; CRLF or a BOM in an authored file surfaces as a machine-readable diagnostic (R-FILE-003).
For **binary sidecar files** (L-33) the **raw-byte hash ≡ the canonical hash** — binaries have no canonicalization pass, so one raw-byte digest serves both the watch/reconcile role and the derivation/cache-key role. **Id-keyed child collections (L-33) are realized as arrays of objects carrying a stable `id` member** — the map-keyed (object-keyed-by-`id`) encoding is **forbidden**: JSON objects are unordered, so canonical key-sorting would destroy authored order; ordering is the array's, and merge identity comes from the `id` member (R-FILE-012(b)) [spike-ratified 2026-07-02, owner]. Canonical serialization additionally ships a **cross-implementation test-vector corpus** — float edge cases (shortest-round-trip boundaries, `-0`, subnormals), NFC normalization cases, and key/array-ordering cases — that every writer implementation (the C++ and TS writers) must reproduce **byte-exactly**; property tests assert `serialize ∘ parse` idempotence and the **re-canonicalization fixpoint** (canonicalizing canonical output is a no-op). The corpus is a versioned deliverable per R-QA-011 and lands with the M1 serializer (R-QA-008 minimal v1). The corpus is **load-bearing, not a formality** [spike-ratified 2026-07-02, owner]: the M0 parse-bench spike proved the ECMAScript `Number::toString` rules cross-language byte-stable only via fixpoint testing (Python vs C++ writers, 3,810 files, 0 mismatches) — the notation subtleties (fixed vs exponent notation boundaries, `-0` → `0`) demand that every writer consume it. **NFC normalization cost is unmeasured as of M0** — the M1 serializer MUST measure it and implement an ASCII quick-check fast path (expected to cover the overwhelming majority of authored content) [spike-ratified 2026-07-02, owner].
**Rationale.** Diffs must be semantic; a single canonical form (rather than format preservation) is the only way multiple writers — human, AI, tool — converge instead of fighting; the two-hash split keeps watching cheap and caching correct.

### R-FILE-002 — Watch–hash–reconcile pipeline `MUST`
**Requirement.** EditorKernel detects external file changes via OS watchers treated as *hints*, reconciled by debounced content-hash scans (mtime+size gated). Bulk changes (e.g. a git branch switch) converge to a correct derived state without restart. Daemon-initiated writes register `(path, resulting content-hash)` in an **expected-writes table** consumed by the watcher, so the daemon's own writes are self-echo-suppressed rather than re-processed as external changes; expected-writes-table entries **expire on a short TTL / one debounce cycle**, so a self-echo suppression can never mask a genuine later external edit of the same path. EditorKernel maintains a **persisted reconcile index** (gitignored `.editor/index`, rebuildable from scratch at any time) so warm attach across daemon restarts is mtime/size-gated rather than a full re-hash (feeds R-FILE-011(a)). **Measured basis** [spike-ratified 2026-07-02, owner]: file-open syscalls — not parsing — dominate a 100k-file fresh attach (~50 µs/open on Windows ≈ 5 s/core), which is exactly the cost this persisted index removes from warm attach; the M1 fresh-attach path SHOULD consider open-batching/overlapped I/O (parse-bench spike).
The mtime+size gate applies **only to the cold full scan**; any watcher-**hinted** path is re-hashed **unconditionally** (mtime granularity is documented as untrusted for content equality, so a same-second in-place edit is never missed). A **low-frequency background full re-hash crawl** (or an fsevents/USN since-token replay where the OS provides one) runs as the dropped-event safety net, guaranteeing eventual convergence even if every watcher event for a path is lost.
When OS watch registration **fails or truncates** — per-user inotify/watch/fd limits exhausted, network filesystems, watch-hostile mounts — the daemon emits a **`watcher.degraded` diagnostic/event** (through the R-CLI-008 catalog and the R-BRIDGE-008 event stream) naming the affected subtree and the effective fallback cadence (the background crawl). A **silent fall-back to crawl latency is forbidden**: clients and agents must be able to see that change-detection latency has degraded from watcher-speed to crawl-speed. `context doctor` (R-BUILD-008) checks the per-user watch/fd limits against the project's size and worktree-daemon count up front, before the limit is hit mid-session.
**Rationale.** OS watchers drop events; hashes are the truth (the Watchman/LSP lesson); without self-echo suppression the daemon chases its own tail, and without a persisted index every restart pays a cold scan.

### R-FILE-003 — Eventually-valid disk: last-good state + diagnostics `MUST`
**Requirement.** Invalid or mid-edit files never crash EditorKernel and are never auto-"fixed" unasked. The last-good derived state is retained and machine-readable diagnostics (JSON-pointer + line/column) are surfaced via CLI status and the event stream. Explicit `context validate --fix` may repair. Machine-readable diagnostics extend to **TS compile/typecheck/bundle failures** (file/line/column/error code, published on the diagnostics topic and via CLI). The ONLY daemon-initiated writes to authored files are: meta-file creation (L-36), GUID move-healing (L-36), explicit `context validate --fix`, and ref-hint healing on tool save (L-34) — each required to be **idempotent** (healed output re-heals to itself).
Every diagnostic carries a **`stability` field** — `provisional` while the derived-world generation is still churning (dirty subgraph or in-flight async import), promoted to `stable` once the `derivation.settled{generation}` event fires (R-BRIDGE-008). `context validate` reports against the **last stable generation** so an agent never acts on a mid-derivation diagnostic that a settling pass will clear; a diagnostic's generation stamp lets a client discard stale problem-markers on the next `settled`.
**Rationale.** The red-squiggle model, not the rejection model; an AI self-corrects in one loop — including on code errors; enumerating the daemon's write surface resolves the tension between "never auto-fix" and the healing behaviors L-34/L-36 require.

### R-FILE-004 — Atomic, serialized writes; per-file atomicity is the unit `MUST`
**Requirement.** All writers use atomic write-temp-then-rename; EditorKernel serializes its own writes through one queue; a compare-and-swap option (`--if-match <content-hash>`) exists for agents needing CAS semantics. **There is no cross-file transaction machinery** (owner-confirmed, L-25): multi-file operations (e.g. an asset rename touching its meta and every referencing scene) apply per-file-atomically in dependency-safe order; any observed mid-state resolves via eventual validity + diagnostics (R-FILE-003), never corruption. Durable multi-file checkpoints are git commits (R-FILE-007).
**Dependency-safe order is defined normatively**: move/rename operations write the file + its meta first (GUID identity survives any observed mid-state); path hints in referencing files heal lazily on later tool saves. The rename verb does **not** rewrite referencing scenes; an explicit `context rename --heal-refs` pass exists for eager healing. All daemon multi-file verbs CAS each write against the content-hash read at planning time and requeue the operation on mismatch. The daemon write queue applies per-client fairness/rate bounds so no attached client can starve the others.
**Crash recovery.** A bounded **crash-recovery intent log** under `.editor/` (gitignored) records, per in-flight operation, `(opId, planned writes, target content-hashes)`; the entry is **fsync'd before the first write** of the operation and **cleared after the last durable rename**. On daemon restart the recovery pass either **resumes** an incomplete operation to completion or, if it cannot, emits a machine-readable diagnostic (R-FILE-003) **naming the incomplete op** and its remaining writes. To make resume safe, **all multi-file verbs MUST be idempotent and re-runnable under partial apply** (re-running a half-applied op converges to the same result) — the idempotence guarantee applies to the whole multi-file verb surface, not only the four heal-writes of R-FILE-003. **Write-acknowledgement to a client occurs only after the durable rename**, never at enqueue. This is explicitly **not** a transaction or undo system — there is still no cross-file atomicity and no rollback (**L-25 stands**); the intent log only guarantees forward progress and honest post-crash reporting, and durable multi-file checkpoints remain git commits (R-FILE-007).
**Intent-log integrity + jail/CAS on resume.** Each intent-log entry carries an **HMAC**; the HMAC key is a **per-project persisted secret** — stored `0600` under `.editor/`, the same trust class as the attach token (R-BRIDGE-007) — and each entry additionally **records the writing `incarnationId`** (provenance, not the key). Stated honestly, the HMAC protects against **corruption and cross-project / foreign-log replay — NOT same-user tampering** (an attacker with the user's filesystem rights can read the key; that boundary is the OS user, per R-SEC-010). A **resumed write is not blind-replayed**: it passes the **same path jail (R-SEC-008, TOCTOU-safe) and the same CAS against the planning-time content-hash** as a fresh write, so resume cannot escape the project root or clobber a file that changed since the crash — a forged entry cannot escape the project root or clobber moved-on state regardless of its HMAC. Any entry that fails integrity, jail, or CAS surfaces as a machine-readable diagnostic (R-FILE-003 / R-CLI-008) naming the incomplete op rather than forcing a state.
**Rationale.** No torn files; safe concurrent writers (human GUI + AI editor + CLI); cross-file transactions would be version-control creep inside the engine, the same trap as undo/redo.

### R-FILE-005 — One incremental derivation graph `MUST`
**Requirement.** All derived state (parsed scenes, composed worlds, imported assets, diagnostics) is produced by a single content-hash-memoized incremental derivation graph; only dirty subgraphs recompute. Derived state is a **pure function of (files, engine version)**, with the registered schema + migration set a **named derived stratum** inside that function — the graph is **stratified** to avoid a bootstrap circularity (the registered set is an input to parsing, but the set itself comes from parsed package manifests / definition-kind files): **pass 0** parses the manifest/definition kinds under **engine-shipped schemas only** (cache keys computed WITHOUT the registered set) and produces the registered schema + migration set; **pass 1** parses content kinds with cache keys that include the **derived hash of that set** (the R-FILE-010 key component, well-founded by the stratification). A change to a definition file re-derives pass 0 and only then invalidates the dependent pass-1 subgraphs. Package-supplied schemas and their migration functions (L-37) are thereby derivation inputs — two projects with different package versions never serve each other stale derived data. The sandboxed-tier execution environment that runs package migrations is an **EditorKernel component booted before pass-1 parsing** — cold-start order: lock → index → watcher → VM → registration (pass 0) → content parse (pass 1) (`ARCHITECTURE.md` §3.1).
**Rationale.** Scale to 10k+ files; "same repo → same world" determinism; unifies the import cache, scene composition, and hot reload into one machinery.

### R-FILE-006 — Authored vs session state; gesture-granular commits `MUST`
**Requirement.** Authored state exists only in files. Session state (selection, camera, in-flight gestures, play-mode runtime state) is ephemeral EditorKernel memory (optionally persisted gitignored). Interactive manipulations commit to file at gesture end; there is no Save action. **Gesture-conflict policy** (owner-confirmed, L-30): if the underlying file changes during a gesture, the commit rebases onto the new state when the gesture's field paths are untouched, and is otherwise dropped with a visible notification, diagnostic, and event — never a silent overwrite in either direction. Collision is decided at field-path granularity, not file granularity.
**Rationale.** 60 Hz edits can't hit disk; honest truth boundary; GUI and AI mutate the project identically; a session-state gesture has no right to overwrite truth it didn't see.

### R-FILE-007 — Version control is the history system (no engine undo/redo) `MUST`
**Requirement.** The engine implements no undo/redo machinery. History, checkpoints, and rollback are provided by version control: git commits, branches, and worktrees. New Projects initialize a git repository by default; EditorKernel stays correct under any git operation (branch switch, checkout, revert, stash) via the watch–hash–reconcile pipeline (R-FILE-002). Editor clients MAY offer thin convenience sugar over git (checkpoint/restore), but that is client sugar, never an engine subsystem.
**Rationale.** Owner ruling: a parallel undo system over file-authoritative state duplicates what git already does better; AI/CLI workflows already live in commits, branches, and worktrees. **Git is a convention, not a runtime dependency** (owner-confirmed, L-27): EditorKernel never invokes git (no shell-out, no embedded git library in core) — it only observes files changing; without a repository everything works, there is simply no history.

### R-FILE-008 — EditorKernel as an embeddable library `MUST`
**Requirement.** EditorKernel is a library; the daemon is a long-lived shared instance of it. The CLI attaches to a live daemon when present (R-ARCH-005) and otherwise operates in-process on the files with identical semantics.
**Rationale.** Offline capability; the daemon is an optimization, never a gatekeeper.

### R-FILE-009 — RuntimeKernel consumes derived artifacts only `MUST`
**Requirement.** RuntimeKernel never reads or parses authored project files and contains no watcher or derivation machinery. It consumes derivation-graph output through one loading seam: fed live in-memory by EditorKernel (play-in-editor; hot reload via handle invalidation), and baked into packed binaries by the build pipeline (shipped games). Player save-games are RuntimeKernel's own serialization (see R-DATA-005), distinct from authored files.
**Rationale.** RuntimeKernel ships inside builds and must stay minimal and fast (R-KERNEL-*); one data format with two feeds preserves editor==build fidelity.

### R-FILE-010 — Hybrid derivation cache `MUST`
**Requirement.** Derived artifacts are cached in a machine-level, per-user, content-addressed shared cache by default (configurable location), with a per-project override for CI/hermetic builds. The cache key is the hash of **every input that affects the output — enumerated exhaustively**: source asset bytes **+ sidecar meta / import settings** (the same texture with different compression settings yields a different key and its own entry) **+ importer version + target platform profile** (per-platform variants coexist as separate entries, making platform switches instant after first import) **+ the importer build hash and the CPU ISA** (cross-machine strict-FP import determinism is explicitly deferred, and remote-cache trust is scoped accordingly) **+ the content-hash of the registered schema + migration set** (the R-FILE-005 derived stratum: a package upgrade that changes a migration yields new keys rather than serving artifacts derived under the old migration). Cache keys are **fine-grained**: importer version plus a **per-artifact-kind derived-format version**; a blanket engine-version component is retained **only for the compiled-code cache** — so an engine patch that changes no importer or derived format leaves existing entries valid (no fleet-wide cold start per engine release). Entries are write-once/immutable, created via atomic temp+rename, and **self-verifying**: entries carry a content hash validated on read; corrupt entries are rejected and re-derived. Nothing is ever "invalidated" — changed inputs simply produce a new key, and stale entries age out via LRU. When the cache directory is machine-shared, entries are isolated per user via ACLs. The cache supports an LRU size cap, pinning, and a `cache` CLI (stats / verify / gc / pin); `cache verify` re-derives and compares — doubling as a derivation-purity test. A team-level remote cache is a natural later extension over the same keys.
**Cache pressure.** The default LRU size cap is **auto-sized to the observed working set × active-worktree count** (not a fixed constant), because L-26 daemon-per-worktree parallelism multiplies cache residency. The cache **auto-gc's under disk/size pressure** and **pins each active worktree's live derived set** so an eviction never evicts an artifact a running daemon still needs. `cache stats` reports a **hit-rate metric** (and residency/eviction counters) so contention is observable. A **multi-worktree contention scenario** joins the R-FILE-011 CI benchmark, so the "warm attach ≤ 5 s" target is validated under concurrent multi-worktree cache load, not only in isolation.
**Code-artifact trust.** **Code/executable artifacts** in the shared cache — compiled WASM, compiled C++, compiled shaders — are a distinct trust class from inert derived data. In v1 the shared cache serves **one trust domain per machine** (one user), so no cross-domain machinery is needed; when third-party native packages and the remote cache arrive (v2 — R-SEC-001), a code artifact is trusted from the shared cache ONLY if it is **signed by the trust root (R-SEC-009)** or was **produced by this trust domain**, with foreign-domain code entries either **partitioned per trust domain** or **verified by re-derivation on first cross-domain read**. The **R-BUILD-006 remote warm cache MUST be signed + verified (R-SEC-009)** — not merely "trusted" because it is remote. Inert derived data (textures, meshes, baked curves) keeps the content-hash self-verification above; the stricter rule applies specifically to artifacts that execute.
**Rationale.** Owner-confirmed (Option 3 hybrid): fresh worktrees must derive near-instantly or daemon-per-worktree (L-26) loses its value; derivation purity (R-FILE-005) is what makes shared caching sound. Keying on source bytes alone would serve one project's artifacts to another with different import settings — the enumerated key makes that impossible by construction, and the code-artifact rule closes the same-user-untrusted-code path (a poisoned cache entry cannot inject executable code into a build via the shared cache).

### R-FILE-011 — Scale & responsiveness envelope `MUST`
**Requirement.** (Owner requirement.) EditorKernel is designed for projects of **100,000+ files** (assets, 3D models, animations, scenes, scripts) — the stated "tens of thousands" envelope with headroom. Concrete targets, enforced by a synthetic large-project benchmark in CI:
(a) **index-warm attach** (persisted reconcile index valid — R-FILE-002 — and shared cache hot): live derived world in ≤ 5 s at 100k files — the hash scan is mtime/size-gated and fully parallel. A **fresh-worktree first attach** is bounded by **parse + canonicalize + hash** throughput — **not raw-byte hashing** (the derivation/cache key is the *canonical-content* hash, R-FILE-001, which requires parsing and canonicalizing each authored file) — with progress events, not by the 5 s target; the CI benchmark measures that parse+canonicalize+hash path (raw-byte hashing alone would report a misleadingly optimistic number).
(b) **incremental edit**: a single authored-file change → updated derived state + events in ≤ 100 ms (heavyweight asset imports run async and stream in); template-instance fan-out and schema validation run **async-streamed** (like imports), keeping the ≤ 100 ms bound on the direct derived-state update.
(c) **bulk change** (branch switch touching thousands of files): converges without restart, daemon responsive throughout;
(d) **cold import**: bounded by importer throughput, saturates all cores, progress observable via the event stream;
(e) **memory**: the daemon holds an index proportional to project size with lazy payload loading — never all asset payloads in RAM. The index memory is enumerated and bounded per component — **raw-byte hashes, canonical-content hashes, dependency edges, the GUID index, and cached schemas**. Dependency **edges are O(refs), which may exceed O(files)** — a dense-reference project has more edges than files, so memory is bounded against ref count, not file count.
An **M2 schema invariant caps composition nesting depth** and emits a **fan-in diagnostic** when a single file is referenced past a threshold; a **dense-reference synthetic** (high ref-fan-in/fan-out) joins the CI benchmark so the edge-heavy case is exercised, not just the file-heavy one. **Upgrade-cost target**: an engine patch with unchanged importers/derived formats leaves warm attach unaffected (fine-grained cache keys, R-FILE-010). A **per-stage latency budget table** (watch → hash → parse → validate → compose → instantiate → fan-out) is an M1 exit criterion.
**Composed resource budgets.** The CI benchmark adds an **N-daemons-on-one-machine scenario** — multiple worktree daemons (the L-26 workflow) on one host — validating that watch handles, fds, index memory, and cache residency **compose** within per-user OS limits rather than being budgeted per-daemon in isolation; the scenario is a named **R-QA-012 fleet-manifest row**, and `context doctor` checks the same limits up front (R-BUILD-008; `watcher.degraded` — R-FILE-002).
**Sim-throughput / orchestration-density targets.** A committed **ticks/sec/instance** target and an **instances-per-box orchestration-density** target join the R-FILE-011-adjacent benchmark set for the wedge's RL/server-sim pillar — the concrete numbers are **committed no later than the M8.5 exit** (re-anchored 2026-07-15 — the original "Context Sim" pre-release anchor was superseded when the track did not run; earlier if the owner ships a Context Sim launch — `ROADMAP.md` §2). Stated honestly: vs GPU-vectorized simulators (Isaac/Brax/Madrona-class) Context does **not** compete on raw samples/sec — the pitch is **agent-authored environments + CPU-parallel commodity orchestration** (README acknowledged-gaps row).
**Rationale.** Real game projects are huge; every §2a architectural choice (incremental derivation, hash gating, shared cache, coalesced events) exists to make this envelope reachable — the CI benchmark keeps it true as the engine grows.

### R-FILE-012 — Schema-aware structural merge `MUST`
**Requirement.** The engine provides `context merge-file` — a schema-aware, structural three-way merge over authored JSON at **field-path granularity**, installable as a git merge driver. After any merge, `context validate` (duplicate-GUID, dangling-`$ref`, orphan-override diagnostics) is the documented convergence gate for the L-26 worktree workflow. **`context new` auto-installs the merge driver** into the new project's git config (`.gitattributes` maps authored JSON kinds to `context merge-file`) so parallel worktree merges are structural by default with no manual setup. The post-merge `context validate` convergence gate reports against the **last stable derived-world generation** (R-BRIDGE-008 / R-FILE-003 `stability`), so convergence is asserted on a settled world, not mid-derivation. The human-facing visual counterpart is R-HUX-008 (3-way scene-merge presentation).
**Conflict envelope + resolution verb.** When a three-way merge cannot auto-resolve, `context merge-file --json` returns a **machine-readable conflict envelope** — `conflicts: [{ path, base, ours, theirs }]` at field-path granularity, inside the R-CLI-008 result envelope. It **NEVER resolves silently last-writer-wins and NEVER writes text conflict markers into authored JSON** (markers make the file invalid JSON, breaking every downstream tool — the two failure modes an agent cannot detect or cannot parse). A **`context resolve-conflict --path <field-path> --take ours|theirs | --value <json>`** verb applies a resolution per conflict entry, so an agent resolves conflicts loop-wise without hand-editing merge output. R-HUX-008's human merge UI renders **this same structured envelope** — one conflict representation for humans and agents.
**Data-model seams in the merge contract.** **(a) Override id-paths:** conflict detection and the conflict envelope extend to the nested-instance **id-path form** (`[instanceId, …, entityId]` — L-35): field-path granularity means id-path granularity for override entries. **(b) Id-based merge identity (L-33):** the same intra-file id with differing fields is the **same entity — field-merged**; the same id **ADDED on both sides** relative to base is a **structural conflict**, never silently unified. **(c) Post-merge validation:** `context validate` gains a **duplicate-intra-file-id diagnostic**, and a **re-key verb** mints a fresh id and rewrites in-file references to it. **(d) Binary sidecars are NEVER content-merged** — a sidecar conflict is whole-file **ours/theirs** in the machine-readable conflict envelope (L-33); likewise a **meta conflict where both sides minted a GUID for the same asset path** resolves whole-asset ours/theirs — GUID identity is never field-blended. **(e) Migrated overrides:** orphan-override diagnostics carry the **pre-migration path** (L-37) so a merge across a schema bump stays diagnosable.
**Mixed-version worktrees.** Two rules close the mixed-version × migration-stamping × merge composition break. **(a) Newer-stamped payloads are a named whole-file conflict class:** any merge input file containing payloads stamped NEWER than the running engine/package set's registered schemas (per the L-32/L-37 `componentVersions` stamps) is a **whole-file ours/theirs conflict class in the machine-readable conflict envelope** — parallel to the binary-sidecar rule in (d) — **never a parse error and never undefined behavior**: the merge driver cannot field-merge data it cannot parse under its installed schemas, so it refuses honestly at file granularity, and `context resolve-conflict` applies the resolution. **(b) Engine/package upgrades are worktree-atomic:** an upgrade is performed on a **shared base** — run `context migrate` + commit on the base branch **before mixed-version work resumes across worktrees** — a documented convergence-gate step of the L-26 worktree workflow (annotated on L-26/L-37), so parallel worktrees never durably diverge in schema generation mid-flight. **(c) Semantic conflicts named** (team-scale honesty): the post-merge convergence-gate documentation also names **semantic conflicts** — both sides individually valid, the merged world behaviorally broken — and their mitigation: the **R-QA-005 sim-level test pass runs as part of the merge convergence gate**; `context validate` proves structural health, the sim-test pass proves behavioral health.
**Rationale.** Cross-agent parallelism is worktree-per-agent + git merge (L-26/L-50) — the structural merge driver is the load-bearing convergence primitive for that model, not a nicety. Line-based merging of JSON scenes produces spurious conflicts and silent structural corruption that a schema-aware driver avoids; the stable intra-file ids (L-33/L-35) are what make field-path merging well-defined.

### R-FILE-013 — Derivation-side backpressure `MUST`
**Requirement.** Under sustained external write load (a bulk git operation, a runaway agent, a code-generation loop), the derivation engine applies **backpressure** rather than unbounded work amplification: the whole dirty set **coalesces into batched derivation passes** (not one pass per file event); a **bounded-lag / queue-depth signal** is published on the event stream (R-BRIDGE-008) so cooperative clients and agents can **self-throttle**; and a **load-shed policy** prioritizes queried/visible subgraphs (what a client is actually looking at) and defers the rest. A **documented maximum dirty-set latency under sustained write load** is defined and **benchmarked** in the R-FILE-011 CI harness.
**Rationale.** The incremental derivation graph (R-FILE-005) and the coalesced event stream (R-BRIDGE-008) keep single edits cheap, but nothing else bounds behavior when writes arrive faster than derivation completes; without published lag + load-shedding, a fast writer (an agent is a plausible one) drives the daemon into unbounded catch-up. Pairs with the queue-depth signal that lets an agent pace itself and with the `settled` quiescence event (R-BRIDGE-008) that tells it when catch-up is done.

---

## 3. CLI & automation

### R-CLI-001 — Full CLI parity `MUST`
**Requirement.** Every action performable in the Editor GUI is performable via the CLI, with equivalent parameters and results.
**Rationale.** Core product requirement; enables CI, scripting, and AI agents.

### R-CLI-002 — Machine-readable I/O `MUST`
**Requirement.** CLI commands support structured (e.g. JSON) input and output in addition to human-readable output.
**Rationale.** Reliable programmatic and agent consumption.

### R-CLI-003 — Scriptable batch & non-interactive mode `MUST`
**Requirement.** The CLI runs non-interactively (no prompts) given complete arguments, and supports batched/sequenced commands for CI pipelines. (MUST because R-BUILD-002, itself MUST, depends on it.)
**Rationale.** Automation and reproducible builds.

### R-CLI-004 — Stable, versioned RPC schema `MUST`
**Requirement.** The RPC schema (including the CLI verb surface) is versioned; breaking changes follow a documented deprecation policy. Version **compatibility is negotiated**, not equality-checked — see **R-CLI-010** (capability-negotiation handshake + written deprecation lifecycle + stable method-ids). The **subscription protocol** (subscribe/unsubscribe/ack) is **pinned as part of the versioned contract by R-CLI-015**. The whole surface is generated from a single registry (**R-CLI-009**) and self-described by **R-CLI-013**.
**Staged stability: `protocolMajor=0` until the M3 freeze.** M1 ships the public contract **explicitly UNSTABLE**: the handshake carries **`protocolMajor=0`** (the R-CLI-010 wire shape), and while it is 0 the contract MAY break without deprecation cycles — stated loudly in `context describe` output so no client mistakes the M1 surface for a frozen one. The contract **freezes at M3** — after the agent corpus (R-QA-006) has exercised it in anger — when `protocolMajor` bumps to 1 and the R-CLI-010 deprecation lifecycle activates. The M1↔M3 staging of the contract cluster is pinned in `ROADMAP.md` (M1): grammar/envelope/registry/describe (R-CLI-007/008/009/013) land **at M1**; the full query-language EBNF (R-CLI-012) and the subscription/topic-introspection detail (R-CLI-014/015) complete **by M3**.
**Rationale.** Scripts and agents must not silently break across engine updates.

### R-CLI-005 — Schema introspection for all registered kinds & component types `MUST`
**Requirement.** The CLI/RPC surface enumerates **all registered file kinds AND component types** — engine-, project-, and package-contributed — each with its versioned JSON Schema, **and additionally the verbs, their parameters, and their flags**. File validation (R-FILE-003) validates component payloads against these schemas; the introspectable set updates live as packages are added or removed. The complete, versioned self-description (verbs + params + flags + RPC methods + event topics + error-code catalog + protocol/capability version) is emitted by **R-CLI-013** (`context describe --json`), of which this file-kind/component-type enumeration is one section — which is what makes the R-HUX-004 claim true that the command palette is generated from live introspection.
**Rationale.** An agent cannot author what it cannot discover; the schema-validated-JSON decisions (L-32) assume this surface exists. Package-contributed component types (R-LANG-010) make a static schema set impossible — introspection must be a first-class, live capability.

### R-CLI-006 — Query surface over derived world and live sim `MUST`
**Requirement.** A versioned query capability runs over (a) the **derived world** (composed scenes) and (b) **live sim state** during play. It supports component-presence and field-value predicates, **spatial predicates** (radius/AABB) over derived transforms, and field projection. JSON results carry entity IDs plus **file/JSON-pointer provenance** so any result can be traced to its authored source. Queries compose with mutation verbs (`context query … | context set …`). The query grammar itself is specified by R-CLI-012 (this requirement pins the surface's existence and provenance/generation semantics; R-CLI-012 pins the language).
**Generation + read-your-writes.** Every query result and snapshot is stamped with the **derived-world generation** it was computed against, and every mutation verb returns **both** resulting hashes of the file it wrote, **labelled**: the **raw-byte hash** and the **canonical-content hash** (R-FILE-001). `--if-match` CAS uses the **raw-byte hash** (it guards the exact bytes on disk). Queries accept a **read-your-writes barrier** — `--after-hash H` / `--after-generation G` — which **bounded-blocks** until the derived world reflects that hash/generation (or times out with an explicit error), so an agent can write then immediately query the consequence without racing derivation. Barrier usage: **`--after-hash H` (canonical-content hash) is the own-write barrier** — an agent barriers on the hash its own mutation returned; **`--after-generation G` is for observed foreign generations** — a generation stamp seen on another client's event, snapshot, or result. **`generationAfter`** (returned in every result envelope, R-CLI-008) is defined as **the derived-world generation the write will be incorporated into** — a barrier-resolvable value: `--after-generation <generationAfter>` is guaranteed to resolve once derivation incorporates that write. A composed `context set … | context query …` pipe **implicitly barriers** on the write's resulting hash.
**Spatial predicates** (radius/AABB) resolve against the R-SIM-007 spatial acceleration structure, so they are **O(result + log N)** rather than O(N) — this is what makes spatial queries viable within the R-BRIDGE-008 session-query latency budget.
**Provenance is a CHAIN; the composed write path is contract.** Provenance on composed entities is a chain, not a single pointer: results carry `provenance: [{ source: schemaDefault|template|override, file, pointer, level }]` ordered **winning-value-first**, so an agent sees every contributor to a composed value — which template supplied it and which instancing level overrode it. The composed **write path** is pinned as public contract (it joins the M1 contract-freeze list; it cannot slip past M2): `context set` on a composed entity writes an **override entry in the OUTERMOST (root) instancing scene by default**; **`--edit-template`** targets the defining template file instead; **`--at-instance <idPath>`** targets a mid-level instancing scene; the result envelope (R-CLI-008) reports the **file + JSON-pointer actually written**. Override addressing uses the L-35 **id-path form**. Advisory override-staleness tooling rides this surface: **`context query --overrides diverged`** lists overrides whose recorded `base` no longer matches the current template value (advisory only, never auto-pruned unasked).
**Live-sim queries vs the unthrottled loop.** Live-sim queries against an **unthrottled headless session** (R-HEAD-003) are serviced at the **bounded per-tick service point** — tick-boundary sampling, never mid-tick — and the session-query latency budget (R-BRIDGE-008) is measured against that service point.
**Rationale.** L-35's bulk selective updates and L-52's state-based verification both *depend* on this surface; provenance is what turns a query result into an actionable file edit for an agent.

### R-CLI-007 — CLI verb grammar & package namespace `MUST`
**Requirement.** The CLI/RPC surface has **one grammar**: `context [<ns>:]<noun> <verb> [--flags]`. A fixed **core-flag set** is honoured by every verb: `--json`, `--project`, `--if-match`, `--after-generation`, `--dry-run`, `--idempotency-key`. **Engine verbs use the bare namespace**; **package-contributed verbs MUST carry the package's reserved namespace** (`<ns>:`), collision-checked at package-add time (adding a package whose namespace already exists fails with a machine-readable diagnostic per R-CLI-008). The grammar — nouns, verbs, namespaces, and the core-flag set — is part of the **versioned public contract** (R-CLI-004) and is enumerated by R-CLI-013.
**v1 scope.** v1 ships the **grammar reservation**: the `<ns>:` namespace syntax, its introspection surface, and the rule that package verbs carry their namespace are contract from day one (retrofitting grammar is a breaking change). The **collision checker at package-add time activates when a second package source exists** — until an ecosystem of verb-contributing packages is live there is nothing to collide — and its error shape stays reserved in the R-CLI-008 catalog now so activation is non-breaking.
**Rationale.** A uniform, predictable shape is what lets an agent (and a human via the R-HUX-004 palette) compose verbs without special-casing each; reserved package namespaces prevent an ecosystem of packages from colliding on verb names; putting the grammar in the versioned contract stops it drifting per release.

### R-CLI-008 — Uniform result envelope + versioned error-code catalog `MUST`
**Requirement.** Every CLI/RPC result is a **uniform envelope**: `{ ok, data | error, generationAfter, warnings[] }`, where `error = { code, message, retriable, pointer?, data? }`. `code` is drawn from a **stable, versioned, introspectable error-code catalog** (enumerated by R-CLI-013); the catalog is **additive-only** across a major version (CI-enforced). A **fixed exit-code table** maps envelope outcomes to process exit codes for the CLI. Over JSON-RPC, the transport-level error `data` carries the **same `code`** so CLI and RPC diagnostics are one schema. **This envelope is the single machine-readable-diagnostic schema**: every diagnostic producer — R-FILE-003 (file/TS diagnostics), R-BRIDGE-006 (version-mismatch error), R-SEC-008 (path-jail violations), R-PKG-005 (engine-compat mismatch) — emits through **this** `error`/catalog schema rather than ad-hoc shapes. `generationAfter` is defined by R-CLI-006 (the generation the write will be incorporated into).
**Build-pipeline failures join the catalog** rather than surfacing as opaque tool output: at minimum **`build.aot_failed`**, **`build.transcode_failed`**, **`build.template_unverified`**, **`build.toolchain_fetch_failed`**, and **`build.link_failed`** — each carrying `retriable` and a `pointer` to the failing artifact/log, so an agent can branch (retry, re-fetch via R-BUILD-008, or park for approval per R-SEC-011). The headless smoke-run result (R-BUILD-009) returns through this same envelope.
**Rationale.** Agents branch on machine outcomes; a single envelope + a stable code catalog + a fixed exit-code table makes "did it work, why not, and can I retry?" answerable uniformly across every surface, and folding all diagnostic producers into one schema removes drift between them.

### R-CLI-009 — Single registry; generated surface; structural parity `MUST`
**Requirement.** **One registry is the single source of truth** for the entire public surface. The file-rewriter CLI verbs, the RPC methods, the built-in MCP tools (R-BRIDGE-007), and the GUI command palette (R-HUX-004) are **all generated from it** — hand-maintained parity between these surfaces is **prohibited**. **CI proves CLI ≡ RPC ≡ MCP ≡ introspection** (R-CLI-013): a conformance check fails the build if any surface diverges from the registry.
**Rationale.** Four hand-kept surfaces drift the moment anyone forgets one; generating them from one registry — and gating that in CI — is the only way "full CLI parity" (R-CLI-001) and the MCP/RPC equivalence stay true as the surface grows.

### R-CLI-010 — Capability-negotiation handshake + deprecation lifecycle `MUST`
**Requirement.** The attach handshake is a **capability negotiation**, replacing a strict version-equality check (it supersedes R-BRIDGE-006's original equality predicate): the client and daemon exchange `{ protocolMajor, supportedCapabilities[], minClientProtocol }`, **succeed within a compatibility window** by degrading to the **negotiated capability subset**, and **hard-fail (with the R-CLI-008 error schema) only outside the window**. A **written deprecation policy** governs change: a method/flag/capability is marked `deprecated:true` with a `removedIn` version in introspection (R-CLI-013), stays for **≥ N minors**, then is removed; **method-ids are stable** across the lifecycle. The Project-pinned engine version (R-VER-003) remains the arbiter of which version *should* be running.
**v1 scope: carry the fields, defer the negotiation behavior.** From day one the handshake **CARRIES `{ protocolMajor, capabilities[] }`** — the wire shape is contract and cannot be retrofitted — but v1 **behavior is hard-fail on mismatch**: with only one released protocol version there is nothing to negotiate with. The **degradation/negotiation behavior activates at the second released protocol version**, with the deprecation lifecycle exactly as written. This trims v1 implementation, not the contract shape.
**Rationale.** Hard version-equality makes every mixed-version attach fail even when the surfaces are compatible; negotiation lets a slightly-older client keep working against the subset it understands while still failing loudly on a genuine break — the LSP/DAP-proven model — and a written deprecation lifecycle is what lets the contract evolve without silently breaking agents.

### R-CLI-011 — Batch result contract + plan-level atomicity `MUST` (batch envelope) · `SHOULD` (`--atomic-plan`)
**Requirement.** A batch (or a composed `query | set`) returns a **structured per-target result**: `{ perTarget: [{ path, ok, hashAfter, error? }], generationAfter, applied, failed }` — this envelope is MUST (agents need machine-readable per-target outcomes from day one). An optional **`--atomic-plan`** flag (SHOULD for v1; the flag stays **reserved in the grammar, R-CLI-007**, so adding the behavior later is non-breaking) makes the batch **plan-level all-or-nothing**: every target is CAS'd against its **planning-time content-hash** and the **whole batch is refused** (nothing applied) if any target mismatches. This is plan-level atomicity **without runtime transactions** — **L-25 stands** (still no cross-file transaction machinery, still no rollback); `--atomic-plan` simply declines to start when the world moved under the plan. One `settled` barrier (R-BRIDGE-008) resolves for the batch as a whole.
**Rationale.** Bulk edits (L-35 selective updates) need a machine-readable per-target outcome, and agents need an all-or-nothing option that fails cleanly on a stale plan rather than half-applying — but delivering that as CAS-refuse-to-start keeps the per-file-atomicity model (L-25) intact instead of smuggling in transactions. The all-or-nothing option is not load-bearing for the wedge (an agent can CAS per target and retry), hence its SHOULD.

### R-CLI-012 — Query-language specification `MUST`
**Requirement.** The query surface (R-CLI-006) has **one specified language**, part of the versioned contract (R-CLI-004): an **EBNF grammar**; an **enumerated operator set** (equality, range, boolean, existence, string-match); **mandatory total result ordering** (a stable sort key, defaulting to entity-id, so paginated results never reorder); a **cursor-pagination contract unified with R-BRIDGE-008** (one cursor model across event catch-up and query paging); and **defined string semantics** (NFC normalization + explicit case handling). The **same predicate/projection language** is used across the **derived world**, **live-sim state**, and **schema introspection** — not three dialects.
**Rationale.** L-35 bulk updates and L-52 state-based verification depend on a query surface, but an under-specified language yields non-deterministic ordering, incompatible pagination, and per-surface dialects that agents cannot reuse; one specified grammar with total ordering and a unified cursor makes queries reproducible and composable everywhere.

### R-CLI-013 — Whole-contract self-description `MUST`
**Requirement.** One artifact/endpoint — **`context describe --json`**, mirrored over RPC — emits the **complete versioned machine description** of the public surface: every **verb + its params + flags** (R-CLI-007), every **RPC method + schema**, every **event topic + payload schema** (R-CLI-014), every **file kind + component-type schema** (R-CLI-005), the full **error-code catalog** (R-CLI-008), and the **protocol/capability version** (R-CLI-010). It is generated from the single registry (R-CLI-009).
**Rationale.** An agent (or the human palette, or a codegen client) needs one authoritative, versioned description of the entire contract; scattering it across per-kind introspection would leave verbs/params undiscoverable and make R-HUX-004's "palette generated from introspection" untrue.

### R-CLI-014 — Event/topic & payload-schema introspection `SHOULD`
**Requirement.** Event **topics and their event-type payload schemas** are **runtime-discoverable** (an extension of R-CLI-005/013): a client can enumerate the live topic set and each topic's payload schema. **Packages may register namespaced topics + event schemas** (mirroring the R-CLI-007 package-namespace rule), which then appear in introspection automatically. A slippable SHOULD: the core topic set is already described statically by `context describe` (R-CLI-013); live package-registered-topic introspection follows the package-event ecosystem it exists to serve.
**Rationale.** The event stream (R-BRIDGE-008) is only programmable if a client can discover which topics exist and what each event carries; package-registered topics make the event surface extensible the same way component types and verbs are.

### R-CLI-015 — Subscription protocol `MUST`
**Requirement.** The event stream (R-BRIDGE-008) has a **concrete subscription protocol**, pinned in the versioned contract (R-CLI-004) rather than deferred wholesale to M1: `subscribe { topics, pathScope, sinceSeq? } → { snapshot, subId }`, `unsubscribe { subId }`, and `ack { subId, seq }`. **Ring-buffer retention is defined relative to the slowest acked cursor** — the daemon retains catch-up history until the slowest live subscriber has acked past it, then ages it out (over-slow subscribers still get the R-BRIDGE-008 gap-marker + re-snapshot, so the daemon never blocks). This requirement is the **single normative home of the ring-retention rule**; R-BRIDGE-008's "ring-buffer catch-up" wording is descriptive and points here rather than restating it.
**Rationale.** R-BRIDGE-008 describes snapshot-then-delta semantics; agents attach/detach constantly, so the concrete methods and an ack-driven retention rule must be in the contract now, and defining retention against the slowest acked cursor bounds daemon memory without ever stalling on a slow client.

### R-CLI-016 — Client-supplied idempotency keys `SHOULD` (replay store; the `--idempotency-key` core flag is reserved and accepted from day one)
**Requirement.** Mutating/side-effecting RPC/CLI operations accept a **client-supplied `--idempotency-key`** (a core flag, R-CLI-007). The daemon records `(key → { resultEnvelope, generationAfter })` for a **bounded window** and, on a retry with the same key, **replays the stored result** instead of re-executing — covering the dropped-acknowledgement case (client didn't see the response and retries). **Note:** tick stepping (R-QA-005) is inherently non-idempotent (each call advances the sim), so idempotency keys guard the write/derive surface, not sim advancement.
**v1 scope.** The **replay-store implementation is `SHOULD`** for v1: the dropped-ack window it closes is narrow on a local-socket transport, and the R-QA-005 `simTick` counter already handles the stepping case. The **`--idempotency-key` core flag remains reserved in the grammar (R-CLI-007) and accepted from day one**, so clients can send keys now and gain replay semantics when the store lands — a non-breaking activation. R-SEC-011's park-and-resume references this key; its resume flow is likewise `SHOULD` in v1.
**Rationale.** Agents and CI retry on transport hiccups; without idempotency keys a retried "create/apply" double-applies, and the R-FILE-004 durable-rename ack already occurs after the effect — so a dropped ack is exactly the case a replayed stored result fixes.

### R-CLI-017 — Transport-portable large-result handle `MUST`
**Requirement.** The oversized-response handle (R-BRIDGE-007) is a **transport-portable opaque resource URI**, fetched over the **same channel** via `resource.read { handle, range }` (CLI: `context fetch`). The **local temp-file path is an optimization** available **only when client and daemon share a filesystem** — never the sole mechanism, so a remote or cross-container client can still retrieve a large result. Handle and cursor (R-CLI-012 / R-BRIDGE-008) reconcile as **one large-result contract** (a handle for one oversized payload; a cursor for a paged/streamed sequence).
**v1 scope.** The **opaque-URI handle FORMAT and the `resource.read { handle, range }` verb are contract from day one** (the wire shape cannot be retrofitted); v1 **implements same-filesystem fetch only** — the URI resolves against the local daemon. The cross-machine fetch path lands with the remote-exposure door (R-BRIDGE-007); clients written against the portable form need no change when it arrives.
**Rationale.** A temp-file-path handle silently assumes a shared filesystem; the MCP/remote-exposure doors (R-BRIDGE-007) break under that assumption, so the portable-URI-fetched-over-the-same-channel form is the only one that works for every client, with the shared-FS temp file kept purely as a fast path.

---

## 4. Single instance & communication bridge

### R-BRIDGE-001 — Single Editor instance per Project `MUST`
**Requirement.** At most one EditorKernel instance operates on a given Project at a time, enforced via an OS-level advisory lock (not merely a lockfile's presence). Mutual exclusion is an OS byte-range lock on a file INSIDE the project — `.editor/lock` — so exclusion holds regardless of how the path is spelled; the canonical project path is a **discovery hint only**, because Windows junctions, `subst`, and UNC spellings can alias one directory to many paths. Instance identity is keyed by canonical project path: each git worktree is therefore its own Project instance with its own EditorKernel — the intended workflow for parallel human+AI development, converging via git merges (owner-confirmed, L-26).
The `.editor/lock` acquisition is the **atomic first action** of any write-capable instantiation, and a **try-lock failure is itself the "instance already live → attach" signal** — a single gate with no separate liveness check to race (see R-ARCH-005). Read-only in-process use may take the lock in a **shared mode** and escalate to exclusive per write; a lock holder that is a transient CLI (not a daemon) still publishes an attach endpoint for its lifetime, so the lock never degrades into a gatekeeper (R-FILE-008). Stale-lock recovery for a crashed holder is R-BRIDGE-004.
**Rationale.** Prevents concurrent corruption of Project files while enabling contention-free parallelism across worktrees; locking the project's own inode/file makes path aliasing irrelevant.

### R-BRIDGE-002 — Attach-not-spawn `MUST`
**Requirement.** A CLI command targeting a Project that already has a running EditorKernel connects to and drives the existing instance rather than starting a new one.
**Rationale.** Explicit product requirement; avoids duplicate/competing instances.

### R-BRIDGE-003 — Local IPC transport `MUST`
**Requirement.** Clients communicate with EditorKernel over a fast local transport (Unix domain socket / named pipe, or localhost socket) supporting request/response and event streaming.
**Rationale.** Low-latency control and live event mirroring (the "dev control bridge").

### R-BRIDGE-004 — Stale-lock recovery & heartbeat `MUST`
**Requirement.** The system detects and recovers from stale locks left by crashed instances, using liveness checks/heartbeats.
**Rationale.** Robustness against crashes.

### R-BRIDGE-005 — Multi-client concurrent attachment `MUST`
**Requirement.** Multiple clients (GUI + CLI + AI agent) may attach to one EditorKernel simultaneously and observe a consistent, live-updating view. (MUST because the locked architecture — L-50, R-COLLAB-001 — already depends on it at MUST level.)
**Rationale.** Human + AI collaboration is a core use case (see R-COLLAB-*).

### R-BRIDGE-006 — Version-checked attach handshake `MUST`
**Requirement.** Attaching to a running EditorKernel begins with a version/capability handshake — the **R-CLI-010 capability negotiation** (which superseded this requirement's original version-EQUALITY predicate): the client (CLI/GUI/MCP) and daemon exchange engine + protocol/capability versions, succeed within the compatibility window by degrading to the negotiated subset, and outside the window the attach **fails with an explicit, machine-readable error** (emitted through the uniform R-CLI-008 envelope/catalog) stating both versions and the remediation (use the Project-pinned engine, upgrade the CLI, or restart the daemon). The client never silently degrades below the negotiated subset and never spawns a second daemon on a version conflict. The Project's pinned engine version (R-VER-003) is the arbiter of which version *should* be running.
**Rationale.** Owner ruling: a CLI embedding EditorKernel vX attaching to a daemon running vY on a genuinely incompatible surface must fail loudly — that beats corrupted state or a confused agent; negotiation (R-CLI-010) keeps compatible mixed-version attaches from failing spuriously.

### R-BRIDGE-007 — JSON-RPC 2.0 wire format; built-in MCP adapter `MUST`
**Requirement.** The R-BRIDGE-003 transport carries JSON-RPC 2.0: request/response for queries, session actions, and derived outputs; notifications for the event stream; the version handshake (R-BRIDGE-006) is the first exchange. Responses exceeding a size threshold return a **transport-portable opaque resource URI handle** per R-CLI-017 (the local temp-file path is a same-filesystem optimization only). EditorKernel ships a built-in MCP adapter as a first-class client door, with tool schemas generated from the same versioned RPC schema (R-CLI-004). The endpoint is local-only by default — user-only socket/pipe permissions plus a per-instance auth token in `instance.json` (0600) — and any remote exposure is a separate, explicitly-gated feature, off by default.
**Per-OS access control:** POSIX = socket in a `0700` directory + `0600` token file; Windows = named pipe with an **owner-SID-only DACL** + equivalent NTFS ACLs on the instance/token files; any localhost-TCP fallback requires the token on every request, compared in **constant time**; large-response temp files (the same-FS handle fast path) carry owner-only ACLs.
**Mutual authentication rides the remote door.** The **mutual-authentication / integrity-MAC'd `instance.json` machinery ships with — and gates — any remote-exposure mode**: the daemon authenticates itself to the client (not only client→server), and `instance.json` is integrity-protected (a MAC/signature the client verifies before trusting `pid`/`endpoint`/versions — trust anchored in R-SEC-009), so a client cannot be tricked into attaching to a rogue daemon and a tampered instance file cannot redirect a client to an attacker-controlled socket. **The remote door never opens with one-way or no authentication.** The v1 local-only endpoint relies on the per-OS ambient protections above (owner-SID DACL / `0700` socket dir, `0600` token, constant-time compare); local mutual auth arrives with the remote feature.
**Rationale.** Owner-confirmed: zero-dependency clients and human debuggability, LSP-proven at this workload; MCP is itself JSON-RPC 2.0, making the AI door structurally cheap.

### R-BRIDGE-008 — Event stream: topics, sequencing, backpressure `MUST`
**Requirement.** The event stream (R-BRIDGE-003 notifications) uses topic subscriptions (`files`, `derivation`, `diagnostics`, `session`, `clients`, `log`; optionally path-scoped). The `log` topic carries structured entries (severity, source [`ts`|`native`|`module`], tick, session), and `context logs --follow --json` works against live sessions **including headless runs**. Events carry monotonic sequence numbers; subscribing or reconnecting yields a current-state snapshot then deltas, with ring-buffer catch-up ("since seq N" — retention semantics normatively owned by R-CLI-015: relative to the slowest acked cursor) and fresh-snapshot fallback. Bulk changes coalesce into batch events; slow subscribers get bounded queues and, on overflow, an explicit gap marker instructing re-snapshot — the daemon never blocks on a slow client. Events describe post-derivation facts, never raw filesystem noise. Derivation events carry the **content-hash of the input file version** they derived from — an agent's write-acknowledgement ("my write produced this derivation"). Concrete vocabulary and schemas are M1 design work, published inside the versioned public contract (R-CLI-004). M1 exit criteria include: a session-query latency budget (≤ 5 ms p99 local), the cursor/paged query contract, and inspector push-subscriptions.
**Concurrency primitives:**
- **Generation counter.** The derived world carries a **monotonic generation counter**; every query result, snapshot, and event batch is stamped with the generation it reflects. A **snapshot is internally consistent to a single generation** (never a torn mix of two).
- **`settled` quiescence event.** A `derivation.settled{generation}` event fires **only when no dirty subgraph and no in-flight async import remain** — the signal that the world has quiesced at that generation. It is the machine-readable answer to "is the daemon done reacting to my writes yet?" and the completion signal for the R-FILE-013 backpressure catch-up.
- **`stability` field.** Diagnostics (and other derived facts) carry a **`stability` field** — `provisional` until the generation settles, then `stable`; `context validate` reports against the **last stable generation** (mirrored in R-FILE-003).
- **Read-your-writes barrier.** Mutation verbs return the **resulting input content-hash**; queries accept **`--after-hash H` / `--after-generation G`** and bounded-block until the derived world reflects it (mirrored in R-CLI-006); a composed `set | query` pipe implicitly barriers.
- **Incarnation epoch.** `seq` is **one global monotonic counter within a single daemon incarnation**, assigned at **post-derivation emission** and **totally ordered across topics** (not per-topic). Every snapshot and event batch is stamped with an **`incarnationId`**; on any `incarnationId` change (the daemon restarted) a client **forces a fresh snapshot** rather than trusting a "since seq N" cursor from the previous incarnation — so `seq` reuse across restarts can never be mistaken for continuity.
**Slow pagers.** Generation-consistent snapshots for slow pagers are bounded via the **R-CLI-017 materialize-to-handle path** — a client paging too slowly to consume a consistent generation gets the snapshot materialized to a large-result handle it fetches at its own pace; the daemon never holds MVCC history to keep old generations queryable.
**Rationale.** Owner-confirmed: AI clients attach, detach, and return constantly — snapshot-then-delta makes reconnection correct by construction; coalescing and bounded queues keep the daemon healthy under bulk change.

---

## 5. Headless operation

### R-HEAD-001 — GPU-independent headless mode `MUST`
**Requirement.** EditorKernel and RuntimeKernel run fully headless with **no GPU and no display required**. The same simulation code and mechanics execute identically headless, on cheap commodity machines.
**Rationale.** Enables inexpensive AI-agent-driven development and CI on ordinary VPS hardware.

### R-HEAD-002 — Rendering is strictly optional and detachable `MUST`
**Requirement.** Rendering is a separable subsystem. When headless, no GPU device is created and no rendering work is performed; when non-headless, a GPU is required and graphics render normally.
**Rationale.** Clean separation is what makes R-HEAD-001 possible.

### R-HEAD-003 — Headless is faster than rendered `MUST`
**Requirement.** Removing rendering measurably increases simulation throughput (no draw calls, GPU sync, or present waits); the fixed-timestep loop may run unthrottled headless. Even unthrottled, the loop provides a **bounded per-tick service point** at tick boundaries: session queries (R-CLI-006 / R-BRIDGE-008) and the inter-tick GC window (R-SIM-008) sample there, and the **≤ 5 ms p99 session-query budget is measured against that service point**. "Unthrottled" means **no render/present waits — not no service points**: a headless sim that never yielded would starve the query surface the wedge's RL/orchestration loops depend on.
**Rationale.** Fast headless iteration and large-scale automated testing/training.

### R-HEAD-004 — Optional offscreen rendering `SHOULD`
**Requirement.** When a GPU is available, an explicit opt-in offscreen render can produce images (thumbnails, screenshots, AI "vision") without a window; this never runs on the default headless critical path.
**Rationale.** Visual verification when needed, without compromising R-HEAD-001.

---

## 6. RuntimeKernel microkernel & modularity

### R-KERNEL-001 — Minimal microkernel `MUST`
**Requirement.** RuntimeKernel is a minimal kernel containing only: the World (component storage), the fixed-timestep Scheduler, the module registry, the event bus, the resource-handle registry, and the platform seam. Nothing with game-feature semantics lives in the kernel.
**Rationale.** Enables ultra-lightweight builds by adding only what a game needs.

### R-KERNEL-002 — Everything else is a package `MUST`
**Requirement.** All engine features — rendering, physics, animation, spline, particles, audio, input, networking, UI, and even the scripting VM — are packages composed onto the kernel.
**Rationale.** Modularity and minimal builds.

### R-KERNEL-003 — Dead-code-eliminated minimal builds `MUST`
**Requirement.** A build includes only referenced packages; unused code is not compiled/linked into the final artifact. The mechanism is a **build-time GENERATED registration translation unit**: the build enumerates the packages a Project actually references and generates the registration TU that registers exactly those. **Static-initializer self-registration is prohibited in shipped builds** — every self-registering object is reachable from its own initializer, which structurally defeats `--gc-sections`/DCE. Combined with **uniform LTO across the from-source vcpkg ports (L-42)** and linker garbage collection, an unreferenced package leaves zero footprint. Proving the generated-registration + LTO + linker-GC pipeline (via a measured size/compile-action delta) is an **M1 de-risk item**; the LTO final link is a per-build, cache-exempt cost budgeted in R-BUILD-006.
**Rationale.** "Super lightweight build when needed" is an explicit goal.

### R-KERNEL-004 — Uniform package contract `MUST`
**Requirement.** Every package implements a common module/plugin contract (registration, lifecycle, declared dependencies, declared component access).
**Rationale.** Consistent composition and safe scheduling.

### R-KERNEL-005 — Packages declare their target core(s) `SHOULD`
**Requirement.** A package declares whether it extends RuntimeKernel, EditorKernel, or both; editor-only parts are never compiled into a Game Build.
**Rationale.** Keeps tooling weight out of shipped binaries.

### R-KERNEL-006 — Static and dynamic composition `SHOULD`
**Requirement.** Packages compose statically for shipping builds (minimal, fast) and can load dynamically in the editor/dev for hot-reload. Scoped by L-43: dynamic composition = load-at-start in dev; no runtime hot-swap in v1.
**Rationale.** Balance shipping size against iteration speed.

---

## 7. Languages, scripting & logic portability

### R-LANG-001 — C++ engine/systems tier `MUST`
**Requirement.** The engine core, kernel, and performance-critical systems are implemented in modern C++.
**Rationale.** Deepest graphics/vendor-SDK ecosystem, mature engine libraries, low-level control.

### R-LANG-002 — TypeScript as the default gameplay language `MUST`
**Requirement.** Game logic is authored by default in TypeScript, executed by an embedded JS engine in-process, sharing the World with the engine.
**Rationale.** TypeScript is chosen first because it is **the language LLMs write most reliably** — the largest training corpus of any typed mainstream language — so the gameplay language itself is part of the AI-first moat: an agent's author→typecheck→fix loop converges fastest in TS. Second, it targets the engine's actual audience: **web developers and AI-builders**, not migrating incumbent-engine users — the design deliberately does not chase C#/GDScript/Blueprint familiarity, and there is **no engine-to-engine project importer in v1** (an acknowledged competitive gap; a post-v1 growth lever TBD — see the README "Acknowledged competitive gaps at v1"). The npm-ecosystem fit is a supporting reason, scoped honestly in R-PKG-001 (code reuse under a constrained ABI, not an asset ecosystem).

### R-LANG-003 — WASM as the native-tier logic & third-party module format `MUST`
**Requirement.** Portable logic and third-party engine modules compile to WebAssembly; RuntimeKernel embeds a per-platform WASM execution backend. Scope (L-40): WASM is the format for the native/perf tier and sandboxed third-party modules; the TS tier runs on a JS VM and does not compile to WASM in v1.
**Rationale.** Native-tier and third-party modules run identically in editor and every build; language-agnostic; sandboxable.

### R-LANG-004 — Write-once, run-every-platform logic `MUST`
**Requirement.** A developer writes game logic once (default TypeScript) and the system compiles/interprets it to the correct per-platform format automatically, regardless of code complexity or dependency count. The developer never manually targets a platform. Per L-40: for the TS tier this means bundle/transpile to JS executed by the per-platform JS VM — JIT where legal, and the browser's engine on the web target (WebGPU-only in v1 per L-56). The "interpreter on iOS" leg of L-40 follows iOS to v2 [owner-ruled 2026-07-02]: no v1 platform requires the interpreter configuration; v1 ships one embedded JIT-capable backend plus the browser's engine on web.
**Rationale.** Explicit product requirement; the system, not the user, handles platform translation.

### R-LANG-005 — Per-platform execution strategy incl. no-JIT platforms `MUST`
**Requirement.** The logic runtime selects the fastest legal execution mode per platform: JIT where allowed (desktop/Android), AOT where JIT is banned. The web target is NOT an AOT target: **browsers compile WASM themselves** — the shipped web build carries the WASM module and the browser's own engine compiles it; the engine performs **no** ahead-of-time native compilation for the web target. The true **native-AOT targets are iOS and consoles**, where a **WASM→native AOT toolchain** (wamrc, wasm2c+clang, or a Cranelift-based compile) must produce platform binaries; that toolchain is **not assumed to exist off the shelf** — it is a spiked deliverable with a **committed size + throughput acceptance bar** (the spike AOT-compiles a representative module and must land within stated binary-size and sim-throughput bounds, or the tool choice is revisited).
**v1 scope.** With iOS moved to v2-first [owner-ruled 2026-07-02] and consoles already WON'T (v1), **no v1 target requires WASM→native AOT** — the **AOT toolchain spike + its acceptance bar land as part of v2's first (iOS) deliverable**. The TS tier's **JS-VM bytecode precompile** (hermesc/qjsc-class, where the v2 constrained-target VM — deferred by L-61 — supports it) likewise follows iOS to v2 (its R-BUILD-006 budget line moves with it). The cheap-later invariants stay in v1: the **WASM module format** and the per-platform execution-strategy **seam** are unchanged, so the v2 AOT leg plugs into an existing seam.
**Rationale.** Correctness on platforms that forbid runtime code generation.

### R-LANG-006 — Optional native/high-performance logic tier `SHOULD`
**Requirement.** Performance-critical or determinism-critical game logic may be authored in the C++/WASM systems tier (data-oriented, manual memory, no GC), as an opt-in per subsystem.
**Rationale.** Supports Factorio-class titles without forcing complexity on typical games.

### R-LANG-007 — One-language default, no forced polyglot `MUST`
**Requirement.** The common case is a game authored entirely in TypeScript on a C++ engine; the second language appears only as opt-in packages at the package boundary, never as forced per-line mixing.
**Rationale.** Keeps approachability while allowing a performance ceiling.

### R-LANG-008 — Shared World across languages `MUST`
**Requirement.** All languages are clients of a single World; authoritative state lives in engine-owned component storage; cross-language data is shared by zero-copy views, and language boundaries are crossed at most once per system per frame (coarse-grained). **Storage-allocator clause [owner-ruled 2026-07-05, per the `spikes/js-engine` §2 loud finding]:** "engine-owned" means **lifetime/authority, not allocator identity** — authoritative component storage **MAY be VM-sandbox-interior** where the chosen embedded VM requires it. Stock V8 (`V8_ENABLE_SANDBOX`, the default on every prebuilt) *fatally aborts* on host-owned external backing stores, so the **VM-allocated stable-buffer shape** (the host holds the `shared_ptr`; per-system ArrayBuffers attach/detach over the stable sandbox-interior pointer) is the zero-copy protocol shape on V8; it also matches the web target's WASM-heap ArrayBuffer, unifying desktop + web on **one** R-LANG-009 view protocol. Zero-copy is fully preserved; only the allocator changes. Backends that can wrap host memory (e.g. QuickJS) keep the original shape — the embedding seam supports both.
**Allocator-identity clause** [spike-ratified 2026-07-02, owner]. **Zero-copy means NO PER-FRAME COPIES, and "engine-owned" means lifetime/authority — NOT allocator identity.** TS/WASM-visible hot component storage is **allocated through the sandboxed tier's interior allocator** — a V8-sandbox `BackingStore` for the JS tier, WASM linear memory for the WASM tier; both are allocate-interior **by construction** (sandbox-enabled V8 fatally aborts on wrapping exterior host memory in an ArrayBuffer, and WASM linear memory cannot wrap host memory at all) — and is **host-mapped** into the engine: the engine holds the stable buffer (e.g. the `BackingStore` shared_ptr), reads and writes the stable pointer directly, and per-system views attach/detach over it per R-LANG-009. **M1 World storage therefore defaults to sandbox-interior, host-mapped archetypes** for TS/WASM-visible hot components (the custom World's storage-allocator seam is pluggable — L-60). This shape mirrors the web target exactly, so one view protocol serves every VM environment.
**Rationale.** Avoids duplicate state, per-entity FFI cost, and GC pollution of simulation state. The allocator-identity clause exists because the literal pre-amendment wording ("engine-owned storage" wrapped by VM views) is unimplementable on every sandboxed tier: stock sandbox-enabled V8 and WASM linear memory both refuse to alias exterior host memory — inverting the allocation direction preserves the zero-copy property (no per-frame copies) while keeping the engine authoritative over lifetime and layout.

### R-LANG-009 — Cross-language view lifetime & invalidation protocol `MUST`
**Requirement.** Zero-copy views (R-LANG-008) are valid **only within one system invocation**. Structural changes (add/remove component, entity create/destroy) are deferred to end-of-system **command buffers** — memory never moves under a live view. At system exit the engine **detaches/neuters** any outstanding ArrayBuffers handed to JS. Write-set enforcement: debug builds wrap views in Proxies that throw on undeclared access; release builds hand out only declared-component views (no per-access checks).
Cheap **and reliable** ArrayBuffer detach/neuter is a **hard gating criterion** for the embedded-JS-VM choice (§2d, DESIGN-DECISIONS) — the M0 JS spike must prove it per candidate VM: a candidate that cannot detach outstanding views cheaply and reliably at end-of-system is **disqualified**, not merely scored down. If **no** candidate can detach reliably, the mandated fallback is **copy-in / copy-out views** — the engine hands JS a copy at system entry and writes declared outputs back at exit, **forfeiting zero-copy (R-LANG-008) but never shipping retained-view use-after-free**. Shipping a retained live view that outlives its system invocation is prohibited outright. **Gate outcome** [spike-ratified 2026-07-02, owner]: **PASS** on both measured engines (V8 ~55 ns / QuickJS 46–80 ns, exact detach semantics) — the copy-in/copy-out fallback was not needed; V8 selected (L-61); the fallback mandate stands for any future backend. **WASM analog** [spike-ratified 2026-07-02, owner]: `memory.grow` **contract-invalidates the host-mapped linear-memory base pointer** — the host MUST re-fetch the base at every system entry; a stale pointer is **host UB, not a trap** (the sandbox protects the host from the module, not the host from itself).
**Rationale.** Archetype migration moves memory; without a lifetime protocol, JS views dangle — the most dangerous undesigned seam found in review. "Undeclared access throws" (L-38) is unimplementable on raw TypedArrays; debug Proxies + declared-view handout in release is the honest enforcement story.

### R-LANG-010 — Declarative component-type authoring `MUST`
**Requirement.** Projects and npm packages define new component types **declaratively** (schema-first). One definition derives: the ECS storage layout, TS accessors, C++/WASM layout, canonical JSON serialization + a published schema (feeding R-CLI-005), and migration hooks (R-DATA-004). The authoring path is file/CLI-driven with no GUI dependency.
**Runtime registration; NO native rebuild in v1.** Project- and package-authored component types are **runtime-registered**: the declarative definition feeds **data-driven archetype storage** (layout interpreted from the schema at load time), so defining or changing a component type requires **NO native engine rebuild in v1**. This keeps the hot autonomous loop — an agent defines a type, authors data against it, and runs the sim — entirely inside the sandboxed tier with no human-gated native step (R-SEC-001 autonomy envelope). A native-codegen fast path (generating C++ structs for hot component types) MAY exist as an **optimization**, never as the only path. **M3 pins this mechanism.**
**Live shape changes are restart-class; the layout hash.** Changing a registered component type's **shape or `x-ctx-storage` layout** while a session is live is **restart-class hot reload** (mirrored on R-PLAY-003): the session **discards runtime state and re-instantiates from the new derivation**, announced by a **loud session event** — in-place migration of live archetype storage under running systems is explicitly NOT attempted in v1. Registered layouts additionally carry a **layout hash**; each WASM module's **declared component access is checked against the layout hash at module load**, and a mismatch is a **machine-readable load error** (a reserved R-CLI-008 catalog code) — a stale module can never read or write storage under a layout it was not built against.
**Rationale.** Without this path, packages cannot contribute data types — the ecosystem's most basic act — and agents cannot define gameplay data headless.

### R-LANG-011 — TS-tier performance floor `MUST`
**Requirement.** The engine publishes a per-platform **logic-throughput matrix** for the TS tier. In v1 the M3 exit measures and publishes the throughput matrix on the **wedge platforms' shipped VM configuration**; the interpreter-mode (no-JIT) measurement bar and its CI budget move to v2 with iOS [owner-ruled 2026-07-02] — no v1 platform legally forbids JIT. The L-47 profiler attributes per-system TS cost so promote-to-WASM decisions are data-driven. **Scheduling truth:** TS systems occupy a **single scheduler lane** (one JS VM = one thread); declared-access parallelism (R-SIM-006/L-38) applies to native/WASM/engine systems.
**Rationale.** "TS for 90% of gameplay" is only honest if the floor is measured and published; pretending the auto-parallel scheduler helps the TS tier would be a false promise.

### R-LANG-012 — TS-tier frame budget `MUST`
**Requirement.** The engine publishes a concrete **entities × systems × Hz floor** — a sustained per-frame budget the TS gameplay tier holds on the **reference min-spec device** (R-QA-007) under the **wedge platforms' shipped VM configuration** — and a **CI benchmark enforces it**. (The "in interpreter mode" clause and its CI budget follow iOS to v2 [owner-ruled 2026-07-02], per R-LANG-011.) To make that floor reachable, the engine provides **batched view handout**: one view-set is materialized **per archetype** and **reused across all systems within a tick** (rather than re-handing views per system), and a **per-frame ArrayBuffer create/detach cap** bounds the view-lifetime churn from R-LANG-009. The requirement states explicitly that **TS gameplay throughput is single-core-bound and does not scale with core count** (one JS VM = one thread — R-LANG-011); scaling comes from promoting hot systems to the native/WASM tier, not from adding cores.
**Rationale.** R-LANG-011 publishes *where the floor is* (the throughput matrix); this pins a *committed, CI-enforced frame budget* so "TS is fast enough for real gameplay" is a measured promise, not an aspiration — and it names the two mechanisms (batched views, ArrayBuffer cap) that keep the R-LANG-009 view protocol from eating the budget. Stating the single-core bound up front prevents the false expectation that the auto-parallel scheduler (R-SIM-006) speeds up TS.

---

## 8. Packages & dependency management

### R-PKG-001 — npm for gameplay/content packages `MUST`
**Requirement.** Gameplay and content packages are distributed via npm; a Project declares a dependency with a single line in a JSON manifest.
**Rationale.** Explicit product requirement; leverages the largest package ecosystem. Stated honestly: npm distribution buys **CODE reuse** — pure-computation TypeScript/JS packages running under the **constrained host ABI** (R-SEC-001: no Node built-ins, no native addons, no threads/workers — a large fraction of npm therefore does NOT run in the engine's VM). It does **NOT** solve the **ASSET ecosystem** (models, materials, animations, audio, VFX — where incumbent engines' asset stores live). The **asset cold-start is an acknowledged competitive risk requiring deliberate seeding**: first-party starter packs plus the R-QA-006 maintained samples kept as real, playable templates (see the README "Acknowledged competitive gaps at v1"; marketplace hosting stays post-v1 per O-5). If npm-reuse is ever marketed as a headline feature, a **compatibility corpus** — N popular npm packages proven to run under the constrained ABI — becomes a requirement of that claim.

### R-PKG-002 — Native engine-package manager `MUST`
**Requirement.** Engine-system (C++) packages are managed by a C++ package manager and composed at build time. Mechanism locked (L-42): **vcpkg** manifest mode + custom engine registry; from-source builds; compiled artifacts cached in the shared content-addressed cache (L-28); no cross-version binary ABI promise in v1.
**Per-target toolchain manifest.** Each engine version pins a **per-target toolchain manifest**: compiler distribution + exact version + sysroot/SDK + target ABI, plus whether each component is **engine-fetched** (signed, R-SEC-009) or a **dev-preinstalled prerequisite** (enumerated by R-BUILD-008). Concretely: **mainline clang** (Linux — fetchable); **clang-cl + the MSVC STL + Windows SDK** (Windows — the STL/SDK are **non-fetchable**, licensed with Visual Studio/Build Tools); **Apple clang + Xcode SDKs** (macOS/iOS — **non-fetchable**, and Apple targets additionally require a macOS build agent, R-BUILD-007); **Android NDK clang** (fetchable); **Emscripten's forked LLVM** (web — its own toolchain, not mainline clang). **vcpkg community-triplet reality:** upstream vcpkg CI curates the desktop triplets; for non-desktop targets (Android/iOS/Emscripten) the **custom engine registry carries per-triplet port patches** — an **ongoing maintenance cost owned by the engine team** (a `ROADMAP.md` §5 risk-table row), not a solved upstream given.
**Rationale.** Distribution and versioning of engine systems without ABI hell.

### R-PKG-003 — Unified "add a package" experience `SHOULD`
**Requirement.** EditorKernel presents a single dependency-management surface; the developer adds a package with one action, and the system routes it to the correct underlying manager (npm vs native).
**Rationale.** Hides two-package-manager complexity from the user.

### R-PKG-004 — Reproducible dependency resolution `MUST`
**Requirement.** Dependency versions are lockable/pinned so a Project builds identically across machines and time. Reproducibility scope, stated honestly: **identical given the same machine/toolchain profile** — the same per-target toolchain manifest (R-PKG-002), the same platform, with content-addressed cache reuse (L-28) — **NOT bit-identical binaries across different machines** (the docs already concede cross-machine import nondeterminism — R-FILE-010's importer-build-hash + CPU-ISA cache keys). If bit-identical cross-machine output is ever wanted, that is a **new requirement** (SOURCE_DATE_EPOCH, `-ffile-prefix-map`, deterministic archiver), deliberately not claimed in v1. Mirrored in R-VER-003.
**Rationale.** Team and CI reproducibility.

### R-PKG-005 — Engine-compatibility declaration `MUST`
**Requirement.** Every gameplay/content package declares an engine-compatibility range (an `engines`-style manifest field). `context add`, attach, and build validate the declaration and fail with machine-readable diagnostics on mismatch. Required from day one — the first published package carries it.
**Data-version compatibility.** The engine-compatibility declaration extends to **data-version compatibility**: a package's component-schema versions are part of its compatibility surface. The **downgrade rule** is pinned (pairs with the L-37 migration contract): when an authored payload's stamped schemaVersion is **newer than the installed package's schema** (a project file written by a newer package version), parsing emits the **`schema.newer_than_package`** diagnostic (R-CLI-008 catalog) and retains **last-good derived state** (R-FILE-003) — **never a best-effort parse** of data the installed schema does not understand. Remediation is machine-readable: upgrade the package, or `context migrate` under the newer package. The downgrade rule has an **engine-kind sibling**: when an authored payload of an **engine-shipped kind** is stamped newer than the running engine's schema (a file written by a newer engine version), parsing emits the **`schema.newer_than_engine`** diagnostic (R-CLI-008 catalog) with the same semantics — last-good retained, never a best-effort parse; remediation = run the Project-pinned engine (R-VER-003/004) or upgrade. In a merge, files carrying newer-stamped payloads of either kind are the R-FILE-012 whole-file conflict class.
**Rationale.** Retrofitting compatibility metadata onto an existing ecosystem is impossible (npm's own lesson); agents need a machine answer to "can I use this package with this engine version?"

### R-PKG-006 — Package conformance kit `SHOULD` (v1) · `MUST` (before any third-party marketplace opens)
**Requirement.** The engine ships **`context package verify`** — a conformance kit a package author (or a package repo's CI) runs against a package: **contract conformance** (R-KERNEL-004 registration/lifecycle/declarations well-formed), **declared-access honesty** (under an instrumented build, the package touches only the component access it declares — R-SIM-006), **schema validation** of every contributed kind/component type against the published vocabulary (R-DATA-006, R-CLI-005), **namespace + engine-compat checks** (R-CLI-007, R-PKG-005 — including data versions), and the **determinism-attestation harness** for sim-tier modules (R-SIM-005). A **reference CI template** lets a package repository run the kit per commit.
**Rationale.** The package contract is otherwise enforced only at integration time inside the engine's own CI; an ecosystem needs the check to run where packages are BUILT. `SHOULD` in v1 (first-party packages dogfood it); it becomes the gate of record — **MUST — before any third-party marketplace tier opens** (O-5; pairs with the R-SEC-001 no-third-party-native-in-v1 statement).

---

## 9. Build & platforms

### R-BUILD-001 — Target platforms: v1 set `MUST` · Android `SHOULD` (trailing) · iOS (v2-first) [owner-ruled 2026-07-02]
**Requirement.** The engine can produce Game Builds for Windows, macOS, Linux, Android, iOS, and the Web (WebGPU-only in v1 — L-56). The platform claim is scoped: **Windows + macOS + Linux desktop, Linux server/headless, and Web are MUST in v1; Android is a trailing SHOULD; iOS is v2-first.**
**Android (trailing SHOULD).** Android ships when the wedge is served and **never blocks a wedge milestone**; its mobile-specific gates (ASTC transcode legs, the mobile min-spec bench, APK/AAB signing, the mobile streaming conditional-MUSTs) **activate when Android lands**. Two external realities recorded 2026-07-15 for when the leg lands: **Google developer verification** — from 2026-09-30 (initial countries; global on certified devices in 2027) apps installed on certified Android devices, **including sideloaded/third-party-store APKs**, must be registered (package name + signing keys) by an identity-verified developer, so signing alone no longer makes an artifact end-user-installable; and the **Play target-API ratchet** (annually moving; API 36 for new apps/updates from 2026-08-31) is a perpetual maintenance cost owned by the Android export template.
**iOS (v2-first).** iOS is out of v1 entirely: it is **v2's FIRST deliverable** — and with it move the iOS-only costs: the WASM→native-AOT toolchain spike + acceptance bar (R-LANG-005), the second embedded JS-VM backend (the constrained-target pick deferred by L-61), the iOS provisioning/entitlements MUST-half of R-BUILD-005, the JS bytecode precompile (R-BUILD-006), the interpreter-mode CI budget (R-LANG-011/012), and the iOS min-spec/streaming floors (R-QA-007, R-ASSET-003/005). The **cheap-later invariants stay in v1** so v2-iOS plugs into existing seams: the WASM module format, the chunked pack format, the multi-backend VM seam, and the platform seam. **macOS DESKTOP remains a v1 target** (it is not iOS; R-BUILD-007's macOS build agent stays for it).
**Rationale.** Explicit product requirement, weighed through the beachhead lens (`ARCHITECTURE.md` §1.1): the wedge platforms serve the RL/server-sim + desktop + web pillars; mobile follows the wedge rather than gating it.

### R-BUILD-002 — CLI-driven, headless builds `MUST`
**Requirement.** Any build can be produced entirely from the CLI on a headless machine (CI-friendly). The headless-CLI guarantee is **per build agent**: any single target's build runs entirely from the CLI, headless, on an agent that satisfies that target's toolchain manifest (R-PKG-002 / R-BUILD-008). It is **not** a claim that every host builds every target — Apple targets require a macOS build agent with Xcode (R-BUILD-007) — so "all platforms from one command" means **orchestration over a build-agent pool** (including ≥ 1 macOS host), each pool member itself CLI-driven and headless; the M8 exit criterion is stated in these terms. The v1 orchestration target is the **v1 platform set** (Windows/macOS/Linux desktop + Linux server/headless + Web), with the Android leg joining when trailing-SHOULD Android lands and the iOS leg in v2 (R-BUILD-001).
**Rationale.** Automation and reproducibility.

### R-BUILD-003 — Per-platform asset optimization `MUST`
**Requirement.** The build pipeline transcodes assets to each target's optimal format (e.g. ASTC/ETC2/BCn textures, platform audio codecs) from one source asset.
**Rationale.** Performance and size on each platform without per-platform source assets.

### R-BUILD-004 — Modular, downloadable toolchains `SHOULD` · `MUST` (fetch-and-verify for engine-fetched toolchains)
**Requirement.** Heavy per-platform build toolchains/export templates are modular components fetched on demand rather than bundled. Every on-demand fetch of a toolchain or export template **MUST be signed + verified against the trust root (R-SEC-009) + TLS/cert-pinned, verify-before-execute, fail closed** — a backdoored export template compromises every shipped game built with it (the worst-case supply-chain artifact), so a fetched template is verified before it is used to package a build.
**Priority split.** For every toolchain component the R-PKG-002 per-target manifest declares **engine-fetched**, the fetch-and-verify mechanism is **MUST** (R-BUILD-008 depends on it). The modular/on-demand *packaging* of heavy toolchains stays `SHOULD`. Non-fetchable components (Xcode, MSVC STL/Windows SDK) are outside this mechanism by definition — they are dev-preinstalled prerequisites validated by `context doctor` (R-BUILD-008).
**Rationale.** Keeps the base install small.

### R-BUILD-005 — Code signing & store packaging `MUST` (signing + minimal packaging) · `COULD` (store-submission/age-rating hooks)
**Requirement.** **Code signing and minimal store packaging are MUST** on the platforms the engine ships to — an unsigned mobile "build" does not install on a device, so without these the platform claims are empty. **Store-submission automation and age-rating/privacy-manifest hooks remain `COULD`** (submission pipelines, ratings metadata). The signing MUST-half follows its platform (R-BUILD-001): **desktop signing/packaging is v1-MUST** — notably macOS code-signing/notarization on the macOS build agent (R-BUILD-007); the **Android APK/AAB signing leg activates when trailing-SHOULD Android lands**; the **iOS provisioning-profile + entitlements leg is v2 with iOS**. The M8 de-risk list (`ROADMAP.md`) names the two heavy legs by name: **Android SDK/Gradle/JDK** provisioning and **Xcode signing/provisioning**. Mechanisms (recorded 2026-07-15): **macOS** = Developer ID certificate + hardened-runtime signing + headless `notarytool` (App-Store-Connect API key) + stapling — macOS 15+ Gatekeeper removed the control-click bypass, making notarization an effective distribution requirement; **Windows** = an Authenticode signing hook in the export adapter (signtool-compatible; **Azure Artifact Signing** — the GA rename of Azure Trusted Signing — or a developer-supplied cert), with a `context doctor` prereq check and the honest note that signing does not grant instant SmartScreen reputation; **Android** additionally requires Google developer-verification registration when the leg lands (R-BUILD-001).
**Rationale.** Shipping to app stores/consoles; the split separates "the artifact is installable at all" (MUST — part of what "build" means on every platform the engine ships to) from "the store pipeline is automated" (COULD).

### R-BUILD-006 — Build-time budget + benchmark `MUST`
**Requirement.** The engine defines **committed build-time budgets** — **cold**, **incremental**, and **clean-CI** — and a **CI build-time benchmark** enforces them. Hermetic CI **may seed a read-only warm artifact cache** (a trusted remote content-addressed cache over the L-28/L-42 keys — signed + verified per R-SEC-009) so that **cold-CI is not the default** measured path — the common CI build is warm-cache-assisted, and the fully-cold build is a separately-tracked worst case. Costs that are **per-build, not cached-once** are **budgeted separately** from the from-source C++ compile, because they recur on every build regardless of cache warmth: the **per-platform asset transcode** (R-BUILD-003 — the v1 budget covers the v1 platform set, with the Android/ASTC legs activating when Android lands) and the **LTO/DCE final-link steps**, which are per-build-per-target and cache-EXEMPT — sccache-class caches memoize per-TU compiles, not monolithic LTO links, and the R-KERNEL-003 generated-registration + uniform-LTO pipeline makes the final link the recurring hot cost of every minimal build (mirrored on L-28). The **WASM-AOT compilation** and **JS-VM bytecode precompile** budget lines move to v2 with iOS (R-LANG-005 — no v1 target requires either).
**Rationale.** The L-28/L-42 shared cache makes the C++ from-source compile "roughly one build per machine ever," but that claim over-reaches alone: per-platform transcode and the LTO final link are re-done per build. Without an enforced build-time budget, from-source C++ + multi-platform packaging silently regresses into multi-hour builds; the trusted warm remote cache is what keeps CI honest without pretending every build is cold. Pairs with the `ROADMAP.md` §5 risk table (C++ build times).

### R-BUILD-007 — Apple-target builds require a macOS build agent `MUST`
**Requirement.** Apple-target Game Builds require a **macOS build agent with Xcode**: Apple code signing, the macOS/iOS SDKs, and (for iOS) provisioning are **non-fetchable** — Apple's licensing and tooling reality means they cannot be downloaded onto a Linux/Windows host by the R-BUILD-004 mechanism. In v1 this requirement's subject is the **macOS desktop build**: a macOS build agent with Xcode is required for macOS code-signing/notarization and SDK linkage; the **iOS-specific halves (provisioning profiles, device builds) are v2 with iOS** (R-BUILD-001). The headless-CLI build guarantee (R-BUILD-002) is therefore **PER-AGENT**, and the full platform set means **orchestration across a build-agent pool that includes at least one macOS host** — never a claim that one Linux box produces an Apple build. The M8 exit criterion and CI topology are stated in these terms; in neither version do Apple targets build on non-Apple hosts.
**Rationale.** Pretending Apple targets build anywhere would be the build story's least-honest claim; naming the macOS-agent constraint makes R-BUILD-002 and the M8 exit truthful and lets build-farm topology be designed for it up front rather than discovered at M8.

### R-BUILD-008 — Developer bootstrap & environment doctor `MUST`
**Requirement.** For every target platform, the docs and tooling **enumerate which toolchain components are engine-fetched** (downloaded on demand, signed + verified per R-SEC-009 / R-BUILD-004 — MUST for these) **and which are dev-preinstalled prerequisites** (non-fetchable: Xcode on macOS, the MSVC STL/Windows SDK on Windows, Node.js for TS-tier authoring per R-VER-003 — see the R-PKG-002 per-target toolchain manifest). A **`context doctor`** verb validates the presence and versions of every component required for the requested target(s) and reports **machine-readable diagnostics** through the R-CLI-008 envelope: what is missing, which version is wrong, whether it is fetchable (and can be fetched now) or must be preinstalled, and a remediation pointer.
**File-sync resource budgets.** `context doctor` additionally validates **OS resource budgets for the file-sync layer**: per-user watcher/fd limits (inotify instances + watches on Linux, open-fd caps, platform equivalents) checked **against the project's file count and the number of concurrent worktree daemons** (the L-26 workflow), with machine-readable remediation — raise the limit, or expect degraded change-detection latency (the `watcher.degraded` path, R-FILE-002). Pairs with the R-FILE-011 N-daemons-on-one-box benchmark scenario.
**Rationale.** An agent — or a new human — cannot fix an environment it cannot diagnose; "set up the toolchain" is only automatable if the fetchable-vs-preinstalled split is explicit and the doctor's output is machine-branchable rather than prose.

### R-BUILD-009 — Headless build smoke-run `MUST`
**Requirement.** For **headless-capable targets**, the build CLI can **launch the packed artifact it just produced**, step **N fixed ticks against the SHIPPED RuntimeKernel** (the binary in the artifact — not the editor-embedded RuntimeKernel), assert a **boot/state signal** (e.g. a named scene reached plus `simTick` progress or a state hash — R-QA-005), and return the result **inside the R-CLI-008 envelope**. Targets that cannot be smoke-run headless (e.g. an iOS device build) **declare that machine-readably** in the same envelope rather than silently skipping.
**Rationale.** Exit 0 from packaging means "the build pipeline succeeded," not "the game boots"; an autonomous agent shipping multiple platforms needs a machine answer to "does the artifact actually run?", and stepping the shipped RuntimeKernel is the only honest place to get it (editor-embedded play proves nothing about the packed binary).

---

## 10. Rendering

### R-REND-001 — WebGPU baseline, everywhere `MUST`
**Requirement.** The default rendering path targets WebGPU semantics across all platforms (native Vulkan/Metal/DX12 and web). Per L-56 [owner-ruled 2026-07-01]: v1 web builds require WebGPU; the former WebGL2 fallback tier is removed from v1 and may return post-v1 as an optional constrained package.
**Rationale.** One portable renderer including the web target; identical editor/web-build visuals — without a second renderer doubling M4.

### R-REND-002 — Tiered Render Hardware Interface (RHI) `MUST`
**Requirement.** Rendering runs on an RHI with capability tiers: **T1 (WebGPU baseline, all platforms including web)** and **T2 (native Vulkan/DX12/Metal for advanced features)**, with capability detection and graceful fallback. Per L-56 the former T0 WebGL2 tier is removed from v1; tiers collapse to T1/T2.
**"Graceful fallback" made honest.** With the sub-WebGPU (T0/WebGL2) tier removed by L-56, "graceful fallback" **no longer means falling back below T1** — there is no lower renderer to fall back to. T1 WebGPU **is the floor**, and R-QA-007 defines the committed per-platform min-spec device + target frame-rate on that floor. "Graceful fallback" scopes to **feature degradation within T1/T2** (advanced T2 effects degrade to their T1 equivalents on lesser hardware), not tier fallback. Devices that cannot run WebGPU are therefore **not MUST targets**: either a constrained path is committed for them as an explicit post-v1 package, or they are **scoped out of R-BUILD-001 honestly** — the design does not promise a silent sub-WebGPU renderer. See R-QA-007.
**Rationale.** Scale one Project from mobile to high-end PC without content forks.

### R-REND-003 — Sim/render one-way data flow `MUST`
**Requirement.** Rendering is a read-only observer of simulation state; it never mutates game state and never blocks the simulation.
**Rationale.** Enables headless operation and deterministic simulation.

### R-REND-004 — Modern PBR & real-time lighting `MUST`
**Requirement.** The renderer supports physically-based materials and real-time dynamic lighting/shadows as standard.
**Rationale.** Baseline for modern 3D graphics.

### R-REND-005 — Material/shader authoring & cross-compilation `MUST`
**Requirement.** A material/shader system authors once and compiles to all backends (SPIR-V/HLSL/MSL/WGSL/GLSL), managing shader variants. The cross-compilation chain is named rather than implied: source shaders compile via **glslang/DXC → SPIR-V**, then **SPIRV-Cross** to per-backend HLSL/MSL/GLSL. The **SPIR-V→WGSL leg** (via **Tint or Naga**) is the **sole path to the web target post-L-56** (no WebGL2 fallback exists) and carries a **flagged maturity risk** — a `ROADMAP.md` §5 risk-table row; the concrete WGSL tool choice is an **M4 deliverable**, made on the engine's real shader corpus with the M0 spike harness as the fixture (the M0 spike measured **byte-identical images** through Naga (native) and Tint (Chrome) — divergence risk low [spike-ratified 2026-07-02, owner]). **Shader compilation and variant generation are derivation-graph nodes** (R-FILE-005) — cached and keyed per R-FILE-010 like any other derived artifact, not an unbudgeted per-build side pipeline.
**Rationale.** Portable, artist- and engineer-usable materials.

### R-REND-006 — Baked-lighting format hooks `SHOULD` *(hooks)* / `COULD` *(baker)*
**Requirement.** Format hooks for baked lighting are `SHOULD` and land early: M2 mesh import reserves a **UV2 channel**; the M4 material contract and the build pack format carry **lightmap inputs**. The lightmap **baker** itself is `COULD` — a derivation-graph node, acceptable post-v1.
**Rationale.** Shipping on mobile and the WebGPU low tier realistically requires baked lighting; the hooks are cheap now and brutal to retrofit into a frozen pack format, while the baker can follow later.

---

## 10a. 2D support

> 2D is **first-class in v1** by owner ruling (**L-55** [owner-ruled 2026-07-01]), overriding the
> design review's WON'T recommendation. The ECS transform component must not hard-require 3D-only
> semantics (L-55).

### R-2D-001 — Sprite/2D rendering path `MUST`
**Requirement.** The M4 renderer includes a first-class 2D path: orthographic projection, sprite batching, sorting layers, and texture atlases — in the same renderer, not a fork.
**Rationale.** Huge 2D market share; AI-authored games skew 2D; a retrofit would repeat the documented "2D bolted onto a 3D engine" failure (L-55).

### R-2D-002 — 2D physics package `MUST`
**Requirement.** A 2D physics package (Box2D-class) ships in v1 (M6), under the same package contract (R-KERNEL-004) as 3D physics.
**Rationale.** 2D games need real 2D physics; emulating it with constrained 3D physics is the classic retrofit failure (L-55).

### R-2D-003 — Tilemap asset kind + 2D authoring `MUST`
**Requirement.** A tilemap asset kind (canonical JSON per L-32/L-33) plus tilemap authoring and a **2D editor viewport mode** (orthographic camera, snapping, tile painting) ship in v1 — the asset kind in M2's set, authoring/viewport trailing post-M5. The **tilemap ASSET KIND stays in M2's set** (retrofit-critical); the **tile-painting GUI and the 2D viewport-authoring mode trail the observer-grade M5 editor within v1** [owner-ruled 2026-07-02] — a sequencing change inside v1, not a scope cut, homed in the **`ROADMAP.md` §1 M8.5 trailing-v1 GUI bucket** (joined there by the R-HUX-006 in-context composition editing and R-HUX-010 contextual help). CLI/file-first tilemap authoring works from M2 (canonical JSON — L-32), so agents author tilemaps before the painting GUI lands. L-55's substance (sprites, Box2D-class physics, tilemap data) is untouched.
**Rationale.** Tilemaps are the backbone of 2D content; the editor must serve 2D as a real mode, not a 3D camera pointed at a plane (L-55).

---

## 11. Advanced graphics (optional, modular)

> **v2 — deferred** [owner-ruled 2026-07-01]: R-GFX-001…004 (the former M9 advanced-graphics set)
> are **explicit v2 — post-wedge** (see `ROADMAP.md` §3); v1 rendering = 2D + baseline-3D PBR
> (R-REND-004, R-2D-*). Their priority letters below are v2 priorities, not v1 commitments.
> R-GFX-005 (modularity invariant) stays in force; R-GFX-006 was already WON'T (v1).

### R-GFX-001 — Real-time ray tracing (RTX-class) `SHOULD` (v2)
**Requirement.** An optional render feature exposes hardware ray tracing (Vulkan RT / DXR / Metal RT) for reflections, shadows, AO, and path tracing on capable hardware, with raster/screen-space fallback.
**Rationale.** High-end visual option; not every game needs it.

### R-GFX-002 — Temporal upscaling / super-resolution `SHOULD` (v2)
**Requirement.** An optional upscaler integrates open (FSR) and vendor (DLSS/XeSS) super-resolution; the renderer produces the required motion vectors + depth.
**Rationale.** Performance headroom on high-resolution/high-fidelity targets.

### R-GFX-003 — Dynamic global illumination (Lumen-class) `COULD` (v2)
**Requirement.** An optional GI feature provides real-time global illumination via a tiered strategy (screen-space → probe/DDGI → SDF → hardware-RT).
**Rationale.** High-end lighting realism as an option.

### R-GFX-004 — Virtualized geometry (Nanite-class) `COULD` (v2)
**Requirement.** An optional GPU-driven virtualized-geometry pipeline (mesh shaders / compute rasterization, cluster LOD, visibility buffer) with traditional mesh+LOD fallback.
**Rationale.** Extreme geometric detail as an option.

### R-GFX-005 — Advanced features are modular packages `MUST`
**Requirement.** Every advanced graphics feature is a removable package; a Project that does not reference it incurs zero footprint. With R-GFX-001…004 in v2, this invariant is **subsumed by R-KERNEL-003's generated-registration DCE mechanism** for v1 — zero-footprint-when-unreferenced is the general package property, proven with a measured delta at M1; **no separate v1 work item exists under this ID**. The ID stays in force as the v2 acceptance bar each advanced-graphics package must meet.
**Rationale.** Consistent with the microkernel/minimal-build philosophy.

### R-GFX-006 — Digital-human pipeline (MetaHuman-class) `WON'T (v1)`
**Requirement.** A high-fidelity digital-human content/tooling pipeline is explicitly deferred to a package built on the finished animation/skinning/shading systems.
**Rationale.** Orthogonal, large, and dependent on core systems being complete.

---

## 12. Simulation, performance & determinism

### R-SIM-001 — Authoritative simulation `MUST`
**Requirement.** Simulation owns all game state; presentation subsystems (render, audio) are downstream observers.
**Rationale.** The load-bearing principle behind headless, determinism, and optimization.

### R-SIM-002 — Fixed-timestep loop `MUST`
**Requirement.** Simulation advances on a fixed timestep decoupled from render frame rate. The decoupling's presentation half is **specified in M1 kernel design** (with L-39): the **tick-rate policy** (how a Project selects — and whether it may change — the fixed rate), **render-side interpolation/extrapolation between fixed ticks** (the extracted render world carries the previous + current tick snapshots and an interpolation alpha — the reason the L-39 extract is double-buffered), and the **high-refresh presentation rule** (a 144 Hz display over a 60 Hz tick presents interpolated frames — never re-simulation, never tick-rate coupling to refresh). The fixed-timestep claim is only complete with the presentation contract stated.
**Rationale.** Stable physics, determinism, replay, headless throughput.

### R-SIM-003 — Data-oriented ECS `MUST`
**Requirement.** The World uses a data-oriented (archetype/SoA) ECS with cache-friendly layout as the default.
**Rationale.** High performance; supports Factorio-class scale.

### R-SIM-004 — Opt-in low-level memory control `SHOULD`
**Requirement.** The engine exposes arena/pool allocators and a no-GC path for the optimization tier.
**Rationale.** Extreme optimization when a game needs it.

### R-SIM-005 — Opt-in deterministic mode `MUST` (on the wedge platforms) · `SHOULD` (beyond the wedge)
**Requirement.** A Project may enable a deterministic simulation mode (for lockstep netcode, replays, reproducible tests), restricted to the native/WASM tier with controlled floating-point (or fixed-point) math. The JS gameplay tier is explicitly non-deterministic. Details locked (L-54): in deterministic mode, sim-critical systems must be native/WASM (JS excluded from simulation); strict IEEE FP on the sim path; CI gate: same inputs → identical state hash across platforms/runs. Deterministic mode requires the physics package's cross-platform-deterministic build configuration (e.g. Jolt's) or a fixed-point alternative; the L-54 CI state-hash gate includes a physics-active scene from day one.
**Determinism is a whole-build property, not a per-system flag**, enforced structurally: **strict-FP flags are set engine-wide on the sim path** (no fast-math anywhere the sim can reach); **FMA / floating-point contraction is pinned** (either uniformly on or uniformly off, never compiler-discretionary); and the sim path uses a **single shipped deterministic transcendental math library** — **no platform `libm`** on the sim path, since `sin`/`cos`/`exp` differ across libms. **Every sim-tier module — engine, native gameplay, and any third-party WASM system — MUST carry a `deterministic:true` build attestation, or the deterministic build fails** (a non-attesting module cannot silently taint a deterministic build). The **CI determinism gate exercises transcendentals** in addition to the physics-active scene (extending to **a representative third-party WASM system** when third-party sim modules ship in v2), and a **per-project conformance harness** hashes the game's *own* systems so a project proves its determinism, not just the engine's.
**Attestation is PRODUCED/VERIFIED, not a trusted manifest bit.** `deterministic:true` is **not a forgeable self-declared flag**. For engine and native-gameplay modules the **engine's deterministic toolchain PRODUCES the attestation** from the **actually-applied, verified build flags** (strict-FP engine-wide, FMA/contraction pinned, deterministic transcendental lib, no platform `libm`) — the attestation is emitted by the build that enforced the flags, so it cannot claim determinism a build did not deliver. Attestation signing/trust is anchored in **R-SEC-009** (a supplied attestation is trusted only if signed by the trust root or reproduced by the harness). **Verification of THIRD-PARTY determinism attestations is v2** (arriving with the marketplace-grade trust machinery — R-SEC-001/R-SEC-009 — when third-party sim modules can ship at all): the re-derive-and-compare harness reproduces/verifies a third-party attestation before acceptance, failing the deterministic build closed on an unverifiable one. v1 keeps the **engine-produced attestations** and the **wedge-platform CI gate**.
**Wedge scope.** Deterministic mode is **MUST on the wedge platforms** — beachhead pillar 2 (deterministic server-authoritative sim/multiplayer) rests on it — shipping **in v1** with the **L-54 CI state-hash gate in v1 (M6)**, hierarchical-hash auto-triage (R-QA-005) on the named platform matrix (min Linux-x64, Win-x64, macOS-ARM64 — R-QA-012), and the **L-48/R-NET-001 hook validation in M6/M8**; **parallel headless instance orchestration for RL is the wedge demo scenario** (`ARCHITECTURE.md` §1.1). It remains `SHOULD` beyond the wedge platforms.
**Rationale.** Determinism is achievable only in the controlled tier; the constraint must be honest — and continuously proven by CI.

### R-SIM-006 — Safe parallel scheduling `MUST`
**Requirement.** Systems declare their component read/write sets; the scheduler parallelizes non-conflicting systems safely. CI enforces correctness with thread/undefined-behavior sanitizers. **The TS single-lane truth:** one JS VM = one thread, so declared-access parallelism applies to native/WASM/engine systems; TS systems occupy a single scheduler lane (R-LANG-011). TS-tier enforcement is declared-view handout + debug-mode Proxies (R-LANG-009), not release-mode throws. Isolate-pool parallel TS is a post-v1 investigation against the chosen backend (V8 — L-61).
**Schedule is computed once, not per frame.** The parallel **schedule DAG is derived once** from the systems' **static** declared access sets and **cached**; it is **invalidated only on a composition change** (a package added/removed, or systems re-registered), **never rebuilt per frame**. A **small-system batching policy** coalesces many tiny systems into a batch so scheduling overhead cannot dominate their run cost. **Single-lane TS systems are excluded from DAG construction** entirely — they run sequentially on the one JS VM lane (R-LANG-011/R-LANG-012), so including them in the parallel DAG would be pointless work. Mirrored in L-38.
**Rationale.** Concurrency safety without a borrow checker (C++); the parallelism claim must be honest about where it applies. Rebuilding the conflict graph every frame would add per-frame cost that scales with system count; since access sets are static (declared, not dynamic), the DAG is a compile-once artifact of composition.

### R-SIM-007 — Spatial acceleration structure `MUST`
**Requirement.** The engine provides an **incrementally-updated broad-phase spatial index** over transform-bearing entities (e.g. a BVH / grid / loose octree kept current as transforms change, not rebuilt per frame). One index is **shared by three consumers**: **render culling** (L-39 — this is what makes "extract scales with the visible set, not world size" actually true), **spatial queries** (the R-CLI-006 radius/AABB predicates), and **asset streaming** (R-ASSET-003 proximity-driven load/unload). With the index, the extract step and spatial queries are **O(result + log N)**, not O(N). **R-KERNEL-001 stays minimal:** the spatial index is a **standard package that the render and query paths depend on — it is NOT kernel core.** The kernel keeps only component storage, the scheduler, the module registry, the event bus, the resource-handle registry, and the platform seam; the spatial index composes on top like any other package.
**Rationale.** Three separate subsystems (culling, spatial query, streaming) each independently needed "find things near here fast," and each was implicitly assuming an O(N) scan; a single shared broad-phase index resolves that and closes the L-39-vs-R-KERNEL-001 "scales with visible set" tension and the R-CLI-006-vs-5 ms-budget (R-BRIDGE-008) tension at once. Keeping it a package (not kernel) preserves the microkernel invariant while still guaranteeing the render/query paths have it.

### R-SIM-008 — JS-tier GC discipline `MUST`
**Requirement.** The TS gameplay tier holds a **per-frame GC-pause target defined relative to the fixed timestep** (a GC pause must not blow the frame budget of R-LANG-012). To reach it: the engine provides **pooled / no-allocation math and query APIs** for hot systems (so steady-state gameplay allocates little or nothing per frame); the embedded JS VM is configured for **incremental / generational GC** with a **scheduled inter-tick GC window** (collect in the gap between fixed ticks, not mid-tick); and the L-47 profiler gains a **GC-pause channel** so pauses are measurable and attributable. **Cheap, well-behaved GC was an explicit §2d VM-selection scoring criterion** (alongside interpreter throughput, ArrayBuffer detach cost, and debugger availability) — scored in the M0 spike and resolved by L-61 (V8: best GC p99 by ~40×) [spike-ratified 2026-07-02, owner].
**Rationale.** A fixed-timestep sim (R-SIM-002) is only smooth if GC pauses stay inside the inter-tick budget; an unmanaged JS heap will stall a frame unpredictably. Pooled APIs + a scheduled GC window + a profiler channel turn GC from an invisible hazard into a budgeted, observable cost — and make GC behavior part of the JS-engine decision instead of a post-hoc surprise.

---

## 13. Engine systems (as packages)

### R-SYS-001 — Highly optimized real-time physics `MUST`
**Requirement.** A physics package provides real-time rigid-body (and optionally soft-body) simulation, decoupled from rendering.
**Rationale.** Core modern engine feature.

### R-SYS-002 — Animation & skeletal (bone) animation `MUST`
**Requirement.** Animation and bone/skeletal animation with blending are provided as packages.
**Rationale.** Explicit product requirement.

### R-SYS-003 — Particle system `MUST`
**Requirement.** A particle system package supports authored and simulated effects.
**Rationale.** Explicit product requirement.

### R-SYS-004 — Spline system `SHOULD`
**Requirement.** A spline package supports paths/curves for movement, geometry, and tooling.
**Rationale.** Named as an example package; common need.

### R-SYS-005 — Navigation/AI support `COULD`
**Requirement.** Pathfinding/navigation is available as a package.
**Rationale.** Common gameplay need; not core.

### R-SYS-006 — Audio subsystem `MUST`
**Requirement.** A real-time audio package (own low-latency thread, spatialization, mixing) independent of frame rate; optional middleware integrations (FMOD/Wwise/Steam Audio). Default locked (L-46): miniaudio backbone; middleware as optional packages. Authored audio data — mixer/bus graphs, audio event definitions, spatialization settings — are canonical-JSON authored kinds per L-32.
**Rationale.** Core engine feature with real-time constraints; audio authoring must be file-first like everything else.

### R-SYS-007 — Input system `MUST`
**Requirement.** An input package supporting keyboard/mouse/gamepad/touch/VR controllers, action mapping, rebinding, and contexts; input routing/focus arbitration between UI and gameplay is well-defined. Model locked (L-45): action maps + input contexts with a layered UI-capture stack; bindings are canonical-JSON authored data.
**Rationale.** Core; UI-vs-gameplay routing is a known hazard to design early.

### R-SYS-008 — Animation-graph asset kind `SHOULD`
**Requirement.** An animation-graph asset kind (state machines, blend trees, transitions) is authored as canonical JSON (L-32) and evaluated by the animation package. Clip **authoring** is DCC-import-only in v1 (via R-ASSET-001); a timeline/sequencer/cinematics system is explicitly `WON'T (v1)`.
**Rationale.** Every real game needs animation control flow, and as canonical JSON it is AI-authorable like everything else; timeline/cinematics is a large orthogonal subsystem deferred deliberately.

---

## 14. UI system

### R-UI-001 — Web UI technologies `MUST`
**Requirement.** UI is authored in web technologies (HTML/CSS + the project's TypeScript).
**v1 scope [owner-ruled 2026-07-13, M7 checkpoint].** The v1 authoring form is a **TS
retained-tree API with CSS-like style properties**; HTML/CSS-*file* fidelity arrives with the
optional CEF runtime backend (trailing-v1/v1.x) over the same R-UI-002 provider contract — the
MUST is satisfied in v1 through the TS half plus CSS-semantics styling, not an in-engine
HTML/CSS parser.
**Rationale.** Explicit product requirement; familiar, powerful, and shares the gameplay language.

### R-UI-002 — Pluggable UI backends `MUST`
**Requirement.** The UI renderer is a swappable backend behind a stable UI-Provider contract; the engine is coupled to the contract, never to a specific UI engine. The developer (and the editor) can choose which backend to use.
**Rationale.** Explicit product requirement ("let the user choose; don't hard-integrate").

### R-UI-003 — Screen-space and world-space UI `MUST`
**Requirement.** UI renders as a screen-space overlay and, via render-to-texture, onto flat or curved surfaces in 3D space — scalable, rotatable, and positionable anywhere. The v1 scope of this MUST is the **screen-space overlay + basic render-to-texture world-space UI**. The **XR-grade application** of world-space UI — the VR/AR/XR surfaces this requirement's rationale cites, together with the R-UI-004 stack (raycast input, stereo, OpenXR layers) and L-16 — is **v2** [owner-ruled 2026-07-01] (see `ROADMAP.md` §3). The render-to-texture mechanism shipped in v1 is the same seam the v2 XR stack builds on.
**Rationale.** Explicit product requirement, essential for VR/AR/XR.

### R-UI-004 — XR-grade world-space UI `SHOULD` (v2) [owner-ruled 2026-07-01]
**Requirement.** World-space UI supports raycast-based input (controller/gaze/hand → UV → synthetic pointer events), stereo rendering, and OpenXR compositor (quad/cylinder) layers for crisp text. The whole XR-grade stack (raycast input, stereo rendering, OpenXR compositor layers — with L-16) is **explicit v2 — post-wedge** (`ROADMAP.md` §3); the priority letter is a v2 priority. v1 keeps R-UI-003's screen-space + basic render-to-texture world-space UI.
**Rationale.** VR/AR/XR usability and legibility.

### R-UI-005 — UI performance & text capabilities `SHOULD`
**Requirement.** The UI-Provider contract exposes capabilities — GPU-driver integration (no CPU readback), damage-based repaint (dirty regions only), GPU-composited transforms/opacity (no relayout) — that backends may implement; the engine negotiates and falls back. Per the O-2 owner ruling, the capability set also includes **text shaping, bidi, and IME support** as declared UI-Provider capabilities — guaranteed on web-tech backends, deferred-and-documented on the minimal backend, per the L-53 capability matrix.
**Rationale.** High rendering performance; capabilities vary per backend — and complex-text support is a capability, not an assumption.

### R-UI-006 — Headless UI logic `MUST`
**Requirement.** UI state/logic (retained tree + event handlers) runs headless with zero rendering cost; UI can be driven and asserted via CLI/AI without a GPU.
**Rationale.** CLI-completeness and automated UI testing.

### R-UI-007 — Editor UI uses CEF `MUST`
**Requirement.** The Editor application's GUI uses CEF (Chromium) embedded in the C++ host for maximum web compatibility; it is a detachable client, so headless mode simply omits it.
**Rationale.** Decided; desktop editor benefits from full web fidelity without harming headless.

### R-UI-008 — Full-fidelity and minimal UI options `SHOULD`
**Requirement.** Alongside the default engine-integrated backend, an optional full-web backend (CEF, desktop) and a minimal/all-platform backend are available on the same contract. Platform policy locked (L-53): per-platform best-available backend + published capability matrix; iOS = system WebView or minimal backend; consoles = minimal backend. Per owner ruling Q1 [owner-ruled 2026-07-02], the **iOS backend entry is v2 with iOS** (consoles were already WON'T (v1)); the v1 rows of the published capability matrix are **desktop + web**. The per-platform-best-available model is unchanged.
**Rationale.** Cover the compatibility↔footprint spectrum without hard-coupling.

---

## 14a. Editor extensibility

### R-EDIT-001 — Editor UI extension contract `MUST`
**Requirement.** Packages targeting EditorKernel may contribute **editor UI**: component inspectors, viewport gizmos, panels, and asset-kind editors — through a **versioned, sandbox-respecting extension contract** (web-component/iframe or declarative-schema based). The contract is designed in M5 alongside the first built-in inspectors; the L-49 trust model covers editor extensions (sandboxed by default).
**Concrete CEF extension sandbox (distinct from the TS L-49 tier).** Because the editor GUI is CEF (R-UI-007), the "sandbox-respecting" contract is concrete and is **its own trust boundary, distinct from the L-49 TS/WASM tier**: **Node integration and native-addon access are OFF** in extension contexts; each extension runs in a **per-extension isolated renderer with site isolation**; a **strict CSP** applies; untrusted panels load in **`sandbox`-attributed iframes**; extension contexts get **no direct access to the daemon socket or the attach token**. The bridge exposed to an extension is **capability-scoped** — **default read/query only**, never ambient file-write/build/install (those require an explicitly granted scope per R-SEC-007). An editor extension is therefore constrained the same way a scoped remote client is, not implicitly trusted because it renders in the editor.
**Testable-by-construction editor (with §14c).** Editor panels are built over a **headless-instantiable UI-logic tree** — R-UI-006's headless-UI guarantee extends to the **editor's own UI** — so palette/undo/Problems/inspector logic is CI-assertable **without booting CEF**; a **per-OS CEF boot smoke** (editor boots, renders a panel, executes one command) **gates M5** on each desktop OS; **R-A11Y-001 is enforced by an automated accessibility scan + a keyboard-only navigation test per new panel** (not code review alone); and the **R-HUX-011 latency budgets are measured from instrumented timestamps** in the real event path.
**v1 hardening scope.** With no third-party extensions shipping in v1 (the R-SEC-001 posture), the hostile-extension hardening above (per-extension isolated renderers, strict CSP, sandboxed iframes, capability-scoped bridge) is **design-time contract in v1** — specified and shaped into the architecture, not fully enforced-and-red-teamed; the retrofit-critical half **stays MUST: every built-in panel/inspector is built ON the extension contract from day one** (the Unity lesson), so opening the contract to third parties in v2 hardens an existing boundary instead of retrofitting one.
**Rationale.** Ecosystem make-or-break: without it, every package-contributed asset kind is second-class in the GUI. Unity spent a decade retrofitting editor extensibility; building the built-in editor *on* the contract from the first component avoids that.

---

## 14b. Accessibility

> O-3 resolved by owner ruling [owner-ruled 2026-07-01]: editor discipline REQUIRED from the first
> M5 component; runtime primitives SHOULD post-core.

### R-A11Y-001 — Editor UI accessibility discipline `MUST`
**Requirement.** From the **first M5 editor component**: semantic HTML, ARIA roles/labels, and complete keyboard navigation are required in all editor UI. CLI-completeness (R-CLI-001) is additionally noted as a structural accessibility property — every GUI action has a non-pointer path. Enforced mechanically, not by review alone: an **automated accessibility scan + a keyboard-only navigation test** run in CI **per new editor panel** (see R-EDIT-001). This requirement keeps its MUST under the v1-lean scope, applied to the observer-grade M5 surface.
**Rationale.** Accessibility retrofits onto a large UI codebase never happen; the editor is web tech, so the standard toolkit (semantic HTML + ARIA) is available from day one at near-zero cost — if enforced from the start.

### R-A11Y-002 — Runtime accessibility-primitives package `SHOULD`
**Requirement.** A runtime accessibility-primitives package ships post-core: subtitles, UI scaling, color-blind-safe options, and screen-reader hooks exposed via the web UI tree (R-UI-001/006). **Note:** CVAA obligations apply if chat/voice communication packages ever ship — flagged for that future package's design.
**Rationale.** Games increasingly face accessibility expectations and (for communication features) legal requirements; primitives-as-a-package fits the everything-is-a-package model.

---

## 14c. Human editor experience

> The AI/agent surface (files + RPC + query + MCP) is deeply specified; the requirements below make
> the **human presentation layer** a first-class deliverable of the M5 editor milestone, built on
> the exact same write path, diagnostics, composition, and git sugar the rest of the design locks.
> None of them add a new authoring mechanism; they render the existing one for humans.
>
> **M5 v1 scope — the observer-grade editor:** the viewport (3D + 2D) + play controls + scene tree
> + an inspector whose edits are override writes + the Problems panel (see the `ROADMAP.md` M5
> re-scope). Within this cluster: R-HUX-001 undo is **scoped to that shipped surface**;
> **R-HUX-002 and R-HUX-003 are v2**; R-HUX-004's MUST is per-verb `--help` (palette SHOULD);
> R-HUX-007 is SHOULD (v1.x); R-HUX-008 slips as SHOULD; **R-HUX-005, R-HUX-009, R-A11Y-001, and
> the `notes` affordance keep their place unchanged**. The **R-HUX-006 in-context
> composition/override viewport-editing MUST trails post-M5 within v1**, joining the tilemap-GUI
> trailing bucket [owner-ruled 2026-07-02, Q3] homed in `ROADMAP.md` §1, M8.5; **R-HUX-010 slips
> with the same bucket**; **R-HUX-011 keeps** its place on the shipped M5 surface.

### R-HUX-001 — GUI session undo/redo over the file-write journal `MUST`
**Requirement.** The GUI provides familiar **session undo/redo** (Ctrl+Z / Ctrl+Y or platform equivalent) with **gesture-batch auto-checkpointing** (one undo step per gesture, not per keystroke). Undo/redo is **replayed through the same write path as any other mutation** — the serialized write queue, `--if-match` CAS, and the L-30 gesture rebase-or-drop policy — so an undo **can never clobber a concurrent writer** (human or AI): if the file moved under the undo, it rebases onto the new state or drops loudly, exactly like a live gesture. This is a **session** convenience over recent edits, **independent of any git knowledge** — the user need not know git exists to use it. It does not reintroduce an engine undo subsystem: durable/long-range history is still git (R-FILE-007, L-21); this is a short-horizon session affordance layered on the write path. Undo/redo is **scoped to the editing surface the M5 observer-grade editor actually ships** (inspector override edits, viewport transforms); the mechanism — write-path replay, CAS, rebase-or-drop — extends to each editing surface as it lands.
**Rationale.** Humans expect Ctrl+Z; "git is the undo system" (L-21) is true for durable history but is not what a human reaches for mid-gesture. Routing undo through the CAS/rebase write path is what keeps it safe under concurrent AI editing — a naive "restore previous bytes" undo would silently clobber a co-writer.

### R-HUX-002 — Git-optional human history abstraction `MUST` (v2)
**Requirement.** The GUI presents history in **non-git vocabulary** a non-technical human understands: a **visual checkpoint timeline**, **named restore points**, and **background auto-commit** — all built on the **permitted client-side git sugar** (R-FILE-007), never on new engine machinery (**engine core stays git-free per L-27**). A user can checkpoint, name, browse, and restore without ever seeing the words "commit," "branch," or "HEAD."
**v2 deferral.** Deferred to **v2**: the non-git history UX serves the non-technical-human cohort, not the wedge (agents and technical users live in git natively — L-21). The client-git-sugar carve-out (R-FILE-007) it builds on is unchanged; v1 humans use git directly or R-HUX-001 session undo. The priority letter is a v2 priority.
**Rationale.** Git-as-history (L-21) is right for the engine but wrong as a *human* interface for most authors; the client-git-sugar carve-out already permits this, and it must be a real, designed surface — not left implicit — or the human path is worse than commercial engines' history UIs.

### R-HUX-003 — GUI launcher / project manager `MUST` (v2)
**Requirement.** A **GUI launcher / project manager** is the **default human entry path**: create-a-project **from template**, **open-recent**, and **engine-version selection** per project (R-VER-004, side-by-side installs). It resolves and can fetch the pinned engine version on open.
**v2 deferral.** Deferred to **v2**: `context new` (CLI) + the **runnable default template (R-QA-006 — its MUST half stands)** are the v1 entry path. The R-VER-004 install-layout convention keeps the door open — the v2 launcher resolves engine versions against a layout that exists from the first release. The priority letter is a v2 priority; R-QA-006's template promotion (originally driven by this requirement) stands on its own merits for the agent/CLI path.
**Rationale.** Humans do not start at a CLI `context new`; a launcher is the front door, and it is where per-project engine-version pinning (R-VER-003/004) becomes a human-usable choice rather than a config-file edit.

### R-HUX-004 — Human command discoverability `MUST` (per-verb `--help`) · `SHOULD` (in-GUI command palette, shell completion)
**Requirement.** Human discoverability of the command surface is built on the **R-CLI-005 schema introspection** (which already enumerates every verb and kind live): **per-verb `--help`** (MUST — it serves every human AND every agent on the primary v1 surface, the CLI, and is generated from the same R-CLI-013 introspection), **shell completion** (SHOULD), and an **in-GUI command palette** (SHOULD — it presupposes the fuller GUI; the observer-grade M5 editor ships fewer commands to discover). All three are generated from live introspection, never hand-maintained (R-CLI-009); because the palette is generated from the same live introspection, package-contributed verbs appear in it automatically.
**Rationale.** The agent surface is discoverable by machines (R-CLI-005); humans need the same discoverability rendered as help text, completion, and a palette — and generating it from introspection means it never drifts from the real verb set.

### R-HUX-005 — Human diagnostics presentation `MUST`
**Requirement.** The editor renders the **R-FILE-003 structured diagnostics** (JSON-pointer + line/column, including TS compile/typecheck errors) as a human surface: an **in-editor Problems panel** and **inline markers** in the relevant editors, both **click-to-navigate** to the offending file/location. Markers respect the R-BRIDGE-008 `stability` field (provisional diagnostics are visually distinguished from stable ones).
**Rationale.** The red-squiggle model (R-FILE-003) is designed for machine consumption; a human needs it drawn as squiggles and a problems list, or the one-loop self-correction story has no human equivalent.

### R-HUX-006 — Viewport quality bar `MUST` (in-context instance/override editing — trails post-M5 within v1) · `SHOULD` (rest)
**Requirement.** The 3D/2D viewport meets a **manipulation quality bar**: **multi-select**, a **3D grid with snapping**, **transform-space and pivot modes** (world/local, pivot/center), and **alignment** tools (SHOULD). The **MUST** core is **in-context scene-instance and override editing** in the viewport — the **GUI face of scene composition (L-35)** — so composition and per-instance overrides are **not CLI-only**: a human can enter an instanced scene, edit an override, and see the L-35 override entry written, all in the viewport.
**Sequencing.** The in-context scene-instance/override viewport-editing MUST **trails post-M5 within v1**, joining the same trailing-v1 bucket as the Q3 tilemap painting GUI / 2D viewport-authoring mode (named home: `ROADMAP.md` §1, M8.5). The **observer-grade M5 editor ships inspector-override edits only** (§14c) — a human edits an override through the inspector at M5; the in-viewport composition-editing face follows. This is a **sequencing change inside v1, not a scope cut**: the MUST stands, its slot moves; the SHOULD affordances (multi-select, snapping, pivots, alignment) trail with it.
**Rationale.** Scene composition (L-35) is the data model's most powerful feature; if it is only authorable via `context` verbs, the GUI is second-class for the one thing the engine does best. The general manipulation affordances (snapping, pivots, alignment) are the table stakes any 3D editor needs, hence SHOULD; the composition-editing face is the load-bearing MUST.

### R-HUX-007 — GUI asset browser `SHOULD` (v1.x)
**Requirement.** A **GUI asset browser** provides: **drag-and-drop import** (invoking the R-ASSET-001 importer framework), **thumbnails / previews** (**best-effort where a GPU is present**, reusing the R-HEAD-004 offscreen render — on a GPU-less host the browser falls back to a kind/type icon; thumbnails are never a hard requirement), **drag-to-place** an asset into the viewport, and **inspector-driven material assignment**. All of it resolves assets by GUID (L-36) and writes through the normal file-write path. Import and asset workflows remain fully available via the CLI (R-ASSET-001, R-CLI-001), so the GUI browser trails as v1.x polish rather than gating M5.
**Rationale.** Asset workflows are drag-and-drop for humans; the offscreen renderer (R-HEAD-004) already exists for thumbnails, so this is largely composition of existing capabilities into the expected human surface.

### R-HUX-008 — Visual scene-merge / conflict presentation `SHOULD`
**Requirement.** A **visual three-way scene-merge / conflict presentation** renders the output of the schema-aware structural merge driver (**R-FILE-012**, MUST) for humans: show base/ours/theirs at field-path granularity and let a human resolve. Pairs with R-FILE-012 (its human merge UI renders the same structured conflict envelope). Confirmed slipping under the v1-lean scope — `SHOULD` stands; the structural merge driver + machine-readable conflict envelope + `context resolve-conflict` (R-FILE-012, MUST) remain the convergence path.
**Rationale.** L-26 worktree parallelism converges via merges (R-FILE-012); a human resolving a scene merge needs a structural, field-path view, not a raw-JSON text conflict — but the visual layer can follow the (MUST) driver, hence SHOULD.

### R-HUX-009 — Zero-AI path is feature-complete `SHOULD` *(design principle)*
**Requirement.** As a **design principle**: the **zero-AI path is feature-complete relative to the AI path** — a complete game is buildable with **no AI usage at all**, entirely through the GUI and CLI. This **decouples tooling quality from the ai-game.dev subscription**: the subscription sells **AI usage only** — since the 2026-07-03 L-57 amendment there is **no royalty waiver or any other license nexus to it** [owner-ruled 2026-07-03]; the engine never withholds an editing capability to push AI usage.
**Rationale.** If the GUI/CLI were deliberately weaker than the AI path, "source-available engine" would be a bait-and-switch and the human experience would rot; keeping the zero-AI path complete is what makes the subscription an honest pricing choice rather than a functionality paywall.

### R-HUX-010 — In-editor contextual help `SHOULD`
**Requirement.** The editor offers **in-context help**: **tooltips on inspector fields and gizmos**, and a **getting-started panel** that references the R-QA-006 maintained samples. Full tutorials and community content are explicitly **post-v1**. This slips with the R-HUX-006 trailing bucket — the in-context help lands with the post-M5 authoring surfaces it documents (`ROADMAP.md` §1, M8.5), not with the observer-grade M5 editor. Still `SHOULD` (v1).
**Rationale.** Discoverability for humans learning the engine; tooltips + a getting-started panel are cheap and high-leverage, while a full tutorial/learning system is a large orthogonal effort deferred deliberately.

### R-HUX-011 — Human-interaction latency budget `SHOULD`
**Requirement.** A **human-interaction latency budget** is defined and measured for the core interactive loops — **gesture → viewport update**, **selection**, and **inspector commit** — as an **M5 exit criterion**, complementing the M1 AI-scale budgets (R-FILE-011 per-stage table, R-BRIDGE-008 session-query p99). The human loop has its own perceptual thresholds distinct from the agent throughput targets. Measured from **instrumented timestamps** in the real input→commit→derive→paint path, not synthetic benchmarks (see R-EDIT-001). The budget applies to the observer-grade M5 surface as shipped and extends to each trailing surface as it lands.
**Rationale.** The M1 budgets are about agent/scale throughput; a human notices interaction latency at a different (perceptual) threshold, so the human loop needs its own committed budget or the editor can be "fast for agents, laggy for people."

> **Authored-kind `notes` field (cross-cutting).** Every authored kind's schema **exposes a `notes`
> field**, and the docs surface it explicitly as **"how to comment"** — because L-32 bans JSON
> comments, `notes` is the schema-blessed place for human/AI annotations that tools preserve
> (L-32). This closes the "where do I leave a comment?" gap uniformly across all kinds rather than
> per-kind.

---

## 15. Play-in-editor & iteration

### R-PLAY-001 — Instant play without a build `MUST`
**Requirement.** The user can play the current Project state instantly in the Editor without producing a platform build.
**Rationale.** Explicit product requirement.

### R-PLAY-002 — Target-platform simulation `MUST`
**Requirement.** Play-in-editor simulates the selected target platform via a platform profile (input model, DPI/resolution, GPU feature tier, memory caps, frame pacing), so the in-editor experience matches the target build — including the web (WebGPU) target.
**Rationale.** Explicit product requirement; eliminates the editor/build gap.

### R-PLAY-003 — Hot reload on file change `MUST`
**Requirement.** Changing any relevant file (code, asset, scene) hot-reloads into the running editor/game while preserving game state where possible. "Preserving state where possible" is bounded honestly: a **component-schema shape/`x-ctx-storage` change is restart-class** — the session discards runtime state and re-instantiates from the new derivation, announced by a loud session event (R-LANG-010; the layout hash guards WASM modules against the same change) — and is explicitly excluded from state preservation. Data-value edits, asset swaps, and TS logic changes remain live-preserving hot reloads.
**Rationale.** Explicit Editor definition requirement. Under file authority this is not a separate mechanism: hot reload *is* the watch→hash→re-derive pipeline (R-FILE-002/005) pushing fresh derivation output through RuntimeKernel's loading seam (R-FILE-009).

### R-PLAY-004 — Well-defined live-edit semantics `SHOULD`
**Requirement.** Edits made while playing follow a defined policy that distinguishes authored data (persists) from runtime simulation state (discarded), avoiding accidental loss or scene pollution. Locked (L-51): the split is structural — authored edits are file writes that hot-reload and persist; runtime mutations are session state discarded on stop; "keep runtime state" is an explicit file-writing action.
**Rationale.** Avoids a classic engine footgun — made impossible by construction under file authority.

### R-PLAY-005 — Direct 3D scene manipulation `MUST`
**Requirement.** The Editor lets a user select, move, rotate, scale, and edit 3D objects and their properties in the scene view.
**Rationale.** Explicit Editor definition requirement.

---

## 16. Asset pipeline & content

### R-ASSET-001 — Import pipeline `MUST`
**Requirement.** A package-based importer framework converts source assets (glTF/FBX/PNG/WAV/etc.) into engine formats; imports are deterministic and cached/incremental; import runs headless in EditorKernel. Importers MUST be **run-deterministic** — fixed seeds, thread-count-independent reductions, pinned dispatch — and CI double-runs each importer and byte-compares the outputs.
**Rationale.** Core content workflow; must scale and run on CI/VPS; the shared cache (L-28) is only sound if two runs of the same importer produce identical bytes.

### R-ASSET-002 — Stable asset identity `MUST`
**Requirement.** Assets have stable identifiers that survive renames and moves; references use identity, not paths. Mechanism locked: GUID + sidecar `<asset>.meta.json` (L-36); dual-form references `{"$ref": guid, "path": hint}` (L-34).
**Rationale.** Prevents reference breakage at scale.

### R-ASSET-003 — Asset streaming & budgets `SHOULD` · `MUST` (memory-constrained targets: Web in v1 · Android when it lands · iOS in v2)
**Requirement.** Large worlds load assets on demand within configurable memory budgets. On **memory-constrained targets** — where the process has a hard, comparatively small memory ceiling — on-demand streaming within a memory budget is **MUST**, not SHOULD: a build for these targets cannot assume the whole world fits in RAM, so streaming is the difference between shipping and OOM-crashing. The conditional-MUST follows its platform (R-BUILD-001): **Web keeps it in v1** (the browser memory ceiling is a wedge platform); the **Android leg activates when trailing-SHOULD Android lands**; the **iOS leg is v2 with iOS**. Proximity-driven streaming decisions use the R-SIM-007 spatial index. It remains `SHOULD` on desktop targets with ample memory.
**Rationale.** Scalability for large games.

### R-ASSET-004 — Asset hot-reload `SHOULD`
**Requirement.** Re-imported assets live-swap into the running editor/game via handle invalidation.
**Rationale.** Iteration speed.

### R-ASSET-005 — Runtime content loading & chunked pack format `SHOULD` · `MUST` (memory-constrained targets: Web in v1 · Android when it lands · iOS in v2)
**Requirement.** RuntimeKernel exposes async **load/instantiate/unload of packed content units by GUID**. The build pack format is **chunked** for on-demand loading and future patching/DLC. The pack-format decision lands **before M8 freezes it** *(as-built: format v0 — the boundary rule, GUID addressing, and directory field set — was co-designed and recorded at M2 in the engine repo's `docs/chunk-pack-format.md`; M8 freezes format v1 and owns that doc's deferred items: chunk byte encoding, payload codec, the async streaming scheduler, per-platform variant selection, nested sub-unit granularity, the sourceScene path→GUID widening)*. On **memory-constrained targets**, async GUID-addressed load/instantiate/unload of chunked content units is **MUST** — it is the runtime mechanism that makes R-ASSET-003 streaming physically possible on those ceilings; the conditional-MUST follows its platform (R-BUILD-001): **Web keeps it in v1**; the **Android leg activates when Android lands**; the **iOS leg is v2 with iOS**. The **chunked pack format itself stays a v1 invariant** — one of the ruled cheap-later invariants (co-designed with the L-35 flatten output in M2, frozen before M8) precisely so the v2/trailing mobile legs need no format change: flatten emits GUID-addressable, independently loadable/unloadable content-unit boundaries, and this is the runtime that loads them. Remains `SHOULD` on ample-memory desktop targets.
**Rationale.** Every shipped game beyond trivial size loads content dynamically; a monolithic pack format frozen at M8 would force a breaking format change post-1.0.

---

## 17. Data model (scenes, prefabs, serialization)

### R-DATA-001 — Text-serializable scenes `MUST`
**Requirement.** Scenes and Project data serialize to a text format amenable to version control diff/merge. Format locked: canonical, schema-validated pure JSON (L-32); granularity + binary-sidecar rules (L-33); dual-form references (L-34).
**Rationale.** Team collaboration and AI-agent file editing.

### R-DATA-002 — Reusable templates with overrides `MUST`
**Requirement.** The data model supports reusable object templates (prefabs/scene-composition) with per-instance overrides and nesting. Model locked: scene composition (L-35); flattens in the derivation graph, zero runtime cost.
**Rationale.** Content reuse at scale; the highest-risk data-model feature to retrofit.

### R-DATA-003 — Undo/redo `REMOVED (2026-07-01)`
For the record: removed by owner ruling — the engine ships no undo/redo system. Git commits, branches, and worktrees are the history and rollback mechanism (see R-FILE-007); GUI session undo is client sugar over the write path (R-HUX-001).

### R-DATA-004 — Schema evolution & migration `MUST`
**Requirement.** Serialized data and component schemas are versioned; the engine migrates older data forward without loss. Mechanism locked (L-37): parse-time migration without touching disk; files rewritten only by explicit `migrate`; the compiled runtime format is regenerated cache with no compatibility burden.
**Rationale.** Projects and packages must survive engine/package upgrades.

### R-DATA-005 — Player save-game API `SHOULD`
**Requirement.** RuntimeKernel provides a save/load API for player state (progress, world snapshots), fully distinct from authored Project files (R-FILE-009): available in shipped builds, headless-capable, versioned like other serialized data (R-DATA-004).
**Runtime save migration (M2 groundwork).** The save format **records the per-component schemaVersion map** — the same per-payload stamps as authored files (L-32/L-37) — and RuntimeKernel ships a **minimal save-migration runner**: builds embed the sandboxed-tier migration functions for **exactly the compiled component set**, so an updated game loads older player saves through the same per-payload migration mechanism the editor uses at parse time. The engine declares a **save back-compat scope** (N versions), not an unbounded promise. Save identity uses the **composed-entity identity** (L-37, §1 Terminology), so a save taken before a re-derivation or engine upgrade still addresses the same entities.
**Rationale.** Player persistence is a runtime concern shaped by the same serialization layer built in M2 — designing it alongside avoids a retrofit.

### R-DATA-006 — Schema vocabulary & units law `MUST`
**Requirement.** The published per-kind JSON Schemas (L-32) share one **engine schema vocabulary**, pinned in M2 **before the first component schemas freeze**: **`x-ctx-type`** names engine semantic types beyond JSON's primitives (quaternion, color **with declared color space**, curve, gradient, bit-flags); **`x-ctx-storage`** declares the numeric width/layout the declarative component compiler derives storage from (feeds R-LANG-010 layout derivation); **`x-ctx-ref`** declares a reference field's **required target kind**, enforced by the derivation validator via meta lookup (a `$ref`/`$entity` to the wrong kind is a validate error, not a runtime surprise); and polymorphic fields use one **pinned tagged-union convention** — `{"type": "<ns>:<shape>", …}` — never per-package ad-hoc encodings. The **units law** is global: **SI units + radians everywhere** in authored data — no per-field unit choices; per-field **`x-ctx-units`** metadata is surfaced through schema introspection (R-CLI-005/013) so humans, GUIs, and agents render and reason about units without guessing.
**Rationale.** Every component schema written before a shared vocabulary exists invents its own encoding for the same dozen semantic types — the classic ecosystem-fracture retrofit — and mixed units (degrees here, radians there) are the classic silent-corruption bug an agent cannot see in JSON. One vocabulary + one units law, pinned before M2's first schemas, is cheap now and impossible later; typed refs close the ref-to-wrong-kind validation gap (pairs with the L-34 entity-ref entry).

---

## 17a. Localization (i18n)

> O-2 resolved by owner ruling [owner-ruled 2026-07-01] — split: string tables in M2 (`SHOULD`);
> text shaping / bidi / IME are declared UI-Provider capabilities (see R-UI-005); editor-UI
> localization is post-v1 (see §24).

### R-I18N-001 — String-table asset kind `SHOULD`
**Requirement.** A string-table asset kind ships in M2's asset-kind set: canonical JSON (L-32), locale variants, fallback chains, and ICU-style plural rules.
**Rationale.** Localization data retrofits poorly once UIs hard-code strings; as an M2 asset kind it costs little, and every downstream system (UI, dialogue) binds to string keys from day one.

---

## 18. Networking (accommodation)

### R-NET-001 — Netcode-ready core `SHOULD`
**Requirement.** The World provides the minimal hooks required for networking — network identity, authority, and dirty/delta snapshot metadata — even though the netcode implementation itself is a package (L-48). With the deterministic wedge promoted (R-SIM-005), the L-48 hooks are **validated inside v1** rather than deferred to post-v1 hardening: a state-sync harness exercises network identity/authority/dirty-tracking against a real scene in **M6**, and the **M8** wedge builds carry the validated metadata. Network ids bind to the **composed-entity identity** (L-37, §1 Terminology — deterministic id-path / stable hash) — the same identity saves (R-DATA-005) and query results use: one identity across sim, saves, and net.
**Rationale.** Multiplayer must be addable later without a core rewrite.

### R-NET-002 — Multiple netcode models supported `COULD`
**Requirement.** The hooks accommodate state-sync/replication and (with deterministic mode) lockstep approaches.
**Rationale.** Different genres need different netcode.

---

## 19. Security & trust

> **Agent-autonomy envelope.** The fully-autonomous headless envelope is the **sandboxed tier**:
> TS + WASM logic, file writes, queries, session control, and sandboxed builds run
> agent-autonomously end-to-end with no human in the loop. **Any native-tier action** — installing
> a native (C++) package, invoking a native importer, adding a from-source C++ dependency — **is a
> human-approval boundary by design**, not an accidental gap; at that boundary agents degrade to
> **request-approval-and-park** via the async consent protocol (R-SEC-011) — never an opaque
> failure, never a silent bypass.

### R-SEC-001 — Tiered package trust `MUST`
**Requirement.** WASM/TypeScript packages run sandboxed by default; native (C++) packages are an explicitly-trusted/reviewed tier with a loud consent gate at install. Untrusted third-party logic runs in the sandbox. **AI-installed packages default to the sandbox tier** — an agent can never silently introduce native code. (Locked — L-49.)
**Honest TS isolation.** The "sandboxed by default" claim is scoped honestly for the TS tier. In v1 **all TypeScript runs in ONE shared trust domain** — a single embedded JS VM per R-LANG-011 (one VM = one thread), so there is **no package-vs-package isolation between TS packages** in v1; the docs must **not** call TS "sandboxed per package." What v1 TS isolation actually is: a **constrained host ABI** — TS code has **no ambient `fs` / `net` / `process` / native-addon access**; the only capabilities it can reach are **engine-injected, capability-gated bindings** (R-SEC-002). Per-package TS isolates (isolate-pool / realm-per-package) are **post-v1**, investigated against the chosen backend (V8 isolates — L-61). The **WASM tier remains the genuinely-sandboxed tier** (no ambient capabilities by construction; import-gated per R-SEC-002). The native (C++) tier is honestly *not* sandboxed — consent-gated and reviewed (L-49). Crash-containment is likewise scoped honestly — see R-SEC-004 (`COULD`): the in-process TS tier is not crash-isolated in v1; WASM/subprocess tiers are the containment story.
**The autonomy envelope.** The trust tiers double as the **agent-autonomy envelope** (the §19 note above): the **fully-autonomous headless envelope is the sandboxed tier** — TS + WASM packages, file writes, queries, session control, sandboxed builds — where an agent operates end-to-end with no human in the loop. **ANY native-tier action** — installing a native (C++) package, invoking a native importer (R-SEC-006), adding a from-source C++ dependency (R-SEC-005 / L-42) — **is a human-approval boundary by design**. At that boundary an agent **degrades to request-approval-and-park**: it receives the machine-readable `consent_required` envelope (R-SEC-011), routes the approval out-of-band, and **resumes the same idempotency-keyed operation once granted** (R-CLI-016) — it never fails opaquely and never bypasses the gate. Component-type authoring stays inside the envelope (runtime-registered, no native rebuild — R-LANG-010).
**v1 scope statement: NO third-party native packages.** **v1 ships NO third-party native (C++) packages** — the native tier exists in v1 for first-party engine packages and from-source dependencies the developer explicitly adds. Consequently the **polished native-consent UX is retired from v1 scope** (the L-49 consent gate remains the design-time contract and any native-tier action still requires explicit human approval — R-SEC-011(a) scopes), the **from-source build-environment jail is `SHOULD` in v1** (R-SEC-005), and native **capability declarations stay advisory** (R-SEC-002). The trust-tier *model* is untouched — this narrows what v1 must build, not the architecture: the third-party native tier opens in v2 together with the marketplace-grade machinery (TUF-class trust root — R-SEC-009; third-party attestation verification — R-SIM-005; the conformance kit as gate of record — R-PKG-006).
**Rationale.** Supply-chain safety; native code cannot be transparently sandboxed — the boundary must be honest.

### R-SEC-002 — Capability enforcement `MUST` (WASM/TS tiers) · `SHOULD` (advisory native)
**Requirement.** Packages declare required capabilities (filesystem, network, etc.); the engine surfaces and can restrict them (L-49: enforced in the sandboxed tier; surfaced-and-advisory for the native tier). Enforcement is tiered, honestly: **WASM tier — MUST**, enforced by **import-gating** (a WASM module cannot call a host capability it was not granted, by construction). **TS tier — MUST at tier level**, enforced at the **injected-binding layer** (the shared TS domain is handed only the capability-gated bindings it was granted — R-SEC-001); it is **not per-package** in v1 (single shared domain). **Native tier — `SHOULD` / advisory** (consent-gated at install per L-49; native code cannot be forced to honour declarations).
**Rationale.** Least-privilege; marketplace safety. A blanket SHOULD would understate what the sandboxed tiers can and must enforce.

### R-SEC-003 — No secrets in project files `MUST`
**Requirement.** The engine and its tooling never require secrets to be stored in Project files; credentials live outside the Project (env/secure store). This pairs with **R-SEC-010**: because untrusted code runs as the OS user, "no secrets in project files" is reinforced by R-SEC-010's **scrubbed child-process environment** (importer/build/VM children do not inherit ambient secrets from the environment), the **no-ambient-network default** for the TS/WASM tiers (a leaked secret has no default egress), and the **optional pre-write/pre-commit secret scanning** surfaced as a diagnostic (catches a secret about to be written into a Project file).
**Rationale.** Security hygiene; aligns with org security posture.

### R-SEC-004 — Crash isolation for untrusted modules `COULD`
**Requirement.** A misbehaving sandboxed module cannot crash the whole engine; failures are contained and reported.
**Rationale.** Stability with third-party code.

### R-SEC-005 — Engine-driven installs run without lifecycle scripts `MUST`
**Requirement.** Engine-driven package installs (CLI, GUI, or agent-initiated) always run with lifecycle scripts disabled (`--ignore-scripts`) — in **all** trust tiers. A package that requires install scripts is classified **native-tier** and triggers the L-49 consent gate. Lockfile integrity hashes and pinned versions are enforced, including transitive dependencies. Package provenance/attestation verification SHOULD be surfaced when available — as of 2025-07 npm **trusted publishing (OIDC) is GA with provenance attestations published by default** (npm ≥ 11.5.1), verifiable via `npm audit signatures` / registry attestations; classic npm tokens were revoked registry-wide 2025-12.
**vcpkg from-source is itself build-time code execution.** The npm `--ignore-scripts` rule closes the JS side, but **vcpkg from-source builds (L-42) run each library's own build scripts** — arbitrary **build-time code execution** by definition, which cannot simply be "disabled" the way `postinstall` can. It is therefore governed: a from-source native build runs **under the L-49 native consent gate** (adding a from-source native dependency is a native-tier action, not a silent install) **and inside an isolated least-privilege build environment** (scrubbed environment per R-SEC-010, **fetch-only network**, and **no cache write outside its own content-addressed key** — L-28); the **source artifacts are SHA-pinned in the engine registry** and **verified before build** (R-SEC-009). An agent can never trigger a from-source native build without crossing the consent gate.
**v1 scope.** With **no third-party native packages in v1** (R-SEC-001), the isolated least-privilege **build-environment jail for from-source native builds is `SHOULD` in v1** (it hardens builds of dependencies the developer explicitly chose) and returns to MUST when the third-party native tier opens in v2. The `--ignore-scripts` rule, lockfile integrity, SHA-pinning, and the consent gate itself are unchanged MUSTs.
**Rationale.** `postinstall` is arbitrary code execution before the L-49 sandbox even exists; with agents installing packages autonomously, this is the single widest supply-chain hole.

### R-SEC-006 — Importer isolation `MUST`
**Requirement.** Importers parsing external interchange formats (glTF/FBX/PNG/…) execute **isolated** — in WASM or an unprivileged subprocess — with access limited to the input bytes and the cache output location. Native in-process importers require the L-49 native trust gate.
**The per-OS subprocess sandbox primitive.** "Unprivileged subprocess" is concrete per OS: the importer subprocess runs under a real OS sandbox primitive — **seccomp-bpf (Linux)**, a **Windows AppContainer or a restricted Job Object with dropped privileges (Windows)**, and **sandbox-exec / dropped privileges (macOS)** — restricting it to the input bytes and its own cache-output key, **no ambient network**, no broader filesystem. The subprocess write path is additionally path-jailed TOCTOU-safely (R-SEC-008). **Third-party importers MUST declare and run under the same isolation** primitive; an importer that demands more (in-process/native execution) **trips the L-49 native consent gate** — it is never granted silently because it happens to be an importer.
**Fuzzing model + staged per-OS lockdown.** The fuzzing obligation is a **model**, not a phrase: **continuous out-of-band fuzzing** (OSS-Fuzz-style, running beyond PR CI) with **committed, minimized seed corpora** (R-QA-011); **per-PR CI runs corpus regression only** — every crasher ever found, replayed fast; PRs are never gated on open-ended fuzz time. Fuzz targets: the **importers**, **authored-JSON parse/canonicalize**, **`context merge-file`**, and the **RPC frame decoder**. New crashes **auto-file issues with minimized reproducers**. The isolation rollout is **staged honestly**: v1 ships the **subprocess + TOCTOU-safe path jail (R-SEC-008) + scrubbed environment (R-SEC-010) + no-ambient-network + nightly fuzzing** everywhere, while the **full per-OS sandbox-primitive lockdown (seccomp-bpf / AppContainer / sandbox-exec) lands Linux-first** — the wedge's server platform — with the other OS primitives following as tracked milestone de-risk items, never silently assumed.
**Rationale.** Asset parsers are the classic RCE surface, and agents ingest untrusted assets from the internet as a matter of course.

### R-SEC-007 — Scoped attach tokens `MUST`
**Requirement.** Attach tokens (R-BRIDGE-007) carry **scopes**: read/query, file-write, session-control, build+install. The MCP adapter exposes only in-scope tools; the default scope for unrecognized clients is read/query. (Pairs with R-ARCH-001: equal capability surface; authorization may restrict.)
**Enforcement lives in the dispatcher, not the adapter.** Adapter-level tool filtering is **bypassable via direct RPC** and therefore cannot be the enforcement point; the **RPC dispatcher checks the attach token's scope on every method** regardless of client door (CLI/RPC/MCP). The single ambient `0600` token (R-BRIDGE-007) is **not the whole authorization story**. **File-write is effectively code execution**: therefore a **derivation-triggered dependency install requires the `build+install` scope** — a write that introduces a new dependency **without** that scope is a **diagnostic, not an auto-install** (R-FILE-003 / R-CLI-008); and **native-importer invocation is gated by the R-SEC-006 native trust gate independent of who wrote the asset** (writing an asset that would trigger a native importer does not, by itself, authorize running it).
**v1 slice.** v1 ships the **scope field in the token + dispatcher-level enforcement**, plus **launch-time operator-provisioned scopes** (R-SEC-011(a) pre-authorized tokens). The **runtime token-minting and interactive elevation flows are v2** (with the consent UX): in v1 a client's scopes are fixed at launch/attach, and an out-of-scope call fails with the machine-readable `consent_required` error (the R-SEC-011 catalog code reserves the slot) rather than triggering an elevation flow; elevation to a higher scope always goes through a consent gate, never automatically.
**Rationale.** Least privilege for a surface designed to be driven by autonomous agents; a read-only reviewer agent should not be able to install packages or trigger builds.

### R-SEC-008 — Project-root path jail `MUST`
**Requirement.** All engine-driven path resolution — refs, sidecars, importer inputs, CLI write targets, build packing — is confined to the project root plus explicitly configured cache/toolchain directories. Absolute paths, `..` escapes, and root-escaping symlinks/junctions are validation errors (machine-readable, R-FILE-003).
**The jail is TOCTOU-safe.** Path-jail enforcement is **not** validate-the-string-then-open (a time-of-check/time-of-use race a symlink swap defeats). It is **resolve-then-verify-by-fd**: open with **`O_NOFOLLOW` / `openat` relative to a jail-root fd** (Windows equivalents), then **re-`realpath` after open and confirm the opened inode is still inside the jail**, rejecting on mismatch. The TOCTOU-safe jail is applied on **every** engine-driven path operation — including the **importer-subprocess** input/output paths (R-SEC-006) and the **intent-log-resume** writes (R-FILE-004), not only front-door CLI writes. Violations emit through the R-CLI-008 error schema.
**Rationale.** A crafted `$ref` or asset path must not read or write outside the project; agents will follow malicious content into traversal bugs unless the jail is structural.

### R-SEC-009 — Cryptographic trust root & artifact signing `MUST`
**Requirement.** The engine has a **pinned cryptographic root of trust** and **per-artifact detached signatures**, with **mandatory verify-before-use** for every executable or trust-bearing artifact: **engine binaries**, the **pinned native toolchain** (L-42), **per-platform export templates** (R-BUILD-004), **native package source artifacts** (R-SEC-005), and **any cross-trust-domain cache entry** (code artifacts in the shared/remote cache — R-FILE-010 / R-BUILD-006). Verification **fails closed**: an artifact that does not verify against the pinned root is **refused, not used with a warning**. The root of trust is pinned (not TOFU) and its rotation is itself a signed, versioned operation. **This is the definition every other requirement's "signed / trusted / attested / verified" language refers to** — those words are meaningless without a named trust root, and R-SEC-009 is it. Every such clause elsewhere (R-SEC-005, R-FILE-010, R-VER-004, R-BUILD-004, R-OBS-006, R-SIM-005/L-54, R-BRIDGE-007) **cites R-SEC-009**.
**v1 = first-party release signing; TUF/Sigstore = the v2 upgrade.** In v1 every trust-bearing artifact is **first-party** (engine binaries, toolchains, export templates — no third-party native packages per R-SEC-001). v1 therefore ships **first-party release signing**: **one pinned publisher key**, per-artifact **detached signatures**, **verify-before-use, fail closed**, and **TLS + hash-pinned fetches** — the full fail-closed posture over a single-key root. The **TUF/Sigstore-style signed-metadata infrastructure** (delegations, thresholds, transparency) is the **v2 upgrade behind the SAME verify-before-use gate**, arriving with third-party artifact distribution; key rotation in v1 remains a signed, versioned operation. Nothing downstream changes: every requirement citing R-SEC-009 verifies against the pinned publisher key in v1 and the full trust root in v2. (Mirrored on L-58; the cross-trust-domain cache rules likewise collapse to one-trust-domain-per-machine in v1 — R-FILE-010.)
**Rationale.** A threat model that runs *as the OS user* needs a cryptographic anchor: without one, "signed," "trusted," "attested," and "verified" are forgeable self-declared flags or unauthenticated fetches. A backdoored toolchain or a poisoned export template is the worst case (it compromises every game built with it); a pinned trust root with per-artifact signatures and fail-closed verify-before-use is the single anchor that makes every downstream trust claim real.

### R-SEC-010 — Least-privilege daemon & exfiltration control `MUST`
**Requirement.** Because same-user untrusted code (agents, npm/vcpkg packages, assets, editor extensions) all run **as the OS user**, the daemon and its children are least-privilege by construction: (a) the **daemon runs least-privilege** (no elevation it does not need); (b) **importer, build, and VM child processes get a scrubbed environment** — they do **not** inherit ambient secrets/tokens from the parent environment (reinforces R-SEC-003); (c) the **TS and WASM tiers get no ambient network by default** — network is a granted capability (R-SEC-002), so untrusted logic has no default egress; (d) **optional pre-write / pre-commit secret scanning** is surfaced as a **diagnostic** (R-FILE-003 / R-CLI-008) — advisory, catching a credential about to be written into a Project file, never a silent block. Child-process isolation composes with the per-OS importer sandbox (R-SEC-006) and the isolated from-source build env (R-SEC-005).
**Rationale.** The trust boundary is honestly the OS user, and the threat model *runs as* that user — so the defence is least-privilege + no-ambient-secrets + no-default-egress, which shrinks what a compromised package or a runaway agent can reach and exfiltrate even though it shares the user's account. Secret-scanning as an advisory diagnostic fits the never-auto-fix / red-squiggle posture (R-FILE-003) rather than adding a blocking gate.

### R-SEC-011 — Non-interactive scope provisioning & async consent protocol `MUST` ((a) pre-authorized provisioning) · `SHOULD` ((b) async park-and-resume)
**Requirement.** Consent and scope elevation work **without an interactive human at the keyboard**, in two halves. **(a) Pre-authorized provisioning (MUST):** an operator MAY mint an **operator-pre-authorized scoped token** at daemon/agent launch carrying an **explicit scope set** (R-SEC-007 scopes, including `build+install` where intended); every such grant is **audited** (logged: who authorized, when, which scopes) — so a CI runner or long-running agent operates a known, deliberate envelope non-interactively. **(b) Async consent (SHOULD in v1):** any consent gate hit without the needed scope (the L-49 native gate, R-SEC-007 elevation) returns a machine-readable **`code=consent_required`** error through the R-CLI-008 envelope, carrying `{ scopeRequested, approvalRef, retriable-after-grant }`, routable to an **out-of-band asynchronous approver** (a human on another channel/queue). Once the referenced approval is granted, **the same idempotency-keyed operation (R-CLI-016) RESUMES** — retrying with the original key succeeds — park-and-resume, never fail-and-restart-from-scratch.
**v1 scope.** Half (a) is how v1 agents and CI run non-interactively at all (the R-SEC-007 v1 slice). Half (b) may slip in v1: with no third-party native packages (R-SEC-001) the native consent gate is rarely reachable, so the full out-of-band approval/resume machinery is `SHOULD`. The **`consent_required` error code stays reserved in the R-CLI-008 catalog from day one** (the catalog is additive-only, so reserving the slot now keeps the v2 protocol non-breaking), and an out-of-scope call in v1 already fails machine-readably with that code before the resume flow exists.
**Rationale.** The consent gates (L-49, R-SEC-007) would otherwise assume a human sits at the terminal; autonomous agents hit them mid-plan. Pre-authorized scoped tokens make intended autonomy explicit and auditable instead of ambient, and the async `consent_required` protocol turns "blocked on a human" into a resumable, machine-legible state instead of an opaque failure — the mechanism behind the R-SEC-001 autonomy-envelope statement's "degrade to request-approval-and-park."

---

## 20. Versioning & compatibility

### R-VER-001 — Stable kernel ABI/API `MUST`
**Requirement.** The kernel's public interfaces are versioned and stable; breaking changes are rare, semver-signalled, and follow a documented deprecation policy. Policy locked (L-44): source-level semver stability in v1, recompile per engine version with the cache absorbing cost; a frozen C ABI / versioned-interface seam arrives post-1.0 with binary distribution.
**Rationale.** The whole package ecosystem depends on kernel stability.

### R-VER-002 — Project forward-compatibility `MUST`
**Requirement.** A Project authored in an older engine version opens in a newer version with automated migration.
**Rationale.** Users must not lose work across upgrades.

### R-VER-003 — Reproducible engine/toolchain pinning `MUST`
**Requirement.** A Project pins the engine version (at minimum) and the native toolchain so builds are reproducible. (MUST because the engine-version pin is the arbiter of the R-BRIDGE-006 handshake and cannot be optional.) The pin set covers the **JS build toolchain** too: **Node.js + tsc + the bundler** are pinned per Project alongside the engine and native toolchain — the TS tier's shipped bundle is only reproducible if the tools that produce it are pinned. The **bundler output MUST be deterministic** (stable module ordering, no embedded timestamps). **Node.js is a stated developer prerequisite** for TS-tier authoring — the embedded runtime VM is **not** Node (it is V8 embedded directly — L-61); Node exists only at dev/build time and is enumerated by the R-BUILD-008 bootstrap. Reproducibility scope matches R-PKG-004: identical given the same machine/toolchain profile (content-addressed cache reuse), not bit-identical cross-machine binaries.
**Rationale.** Determinism of builds across machines/time; the attach handshake needs an authoritative answer to "which engine version should be running?"

### R-VER-004 — Side-by-side engine versions `MUST`
**Requirement.** Multiple engine versions install **side-by-side**; the Project pin (R-VER-003) selects which daemon/CLI serves a Project; the launcher/CLI resolves and can fetch pinned versions on demand. On-demand fetch of a pinned engine version (and its native toolchain) **MUST be signed + verified against the trust root (R-SEC-009) + TLS/cert-pinned, verify-before-execute, fail closed** — resolving "which engine should run" (R-VER-003) and then fetching a **backdoored** engine binary would defeat the whole pin; the fetched artifact is verified before it is ever executed.
**v1 scope: the LAYOUT is the day-one contract.** What must exist **from the first release** is the **versioned install-layout convention** — engine versions install to **`<root>/versions/<semver>/`**, and no version ever installs over another — because a first release that installs flat forecloses side-by-side forever. The **resolver/fetcher/launcher machinery lands with the second release** (when two versions first coexist and there is something to resolve); the **R-VER-003 Project pin stays MUST from day one** and is the arbiter the resolver will consult.
**Rationale.** Real teams run many projects on many engine versions concurrently; without side-by-side installs, R-BRIDGE-006's "use the Project-pinned engine" remediation is unactionable.

---

## 21. Collaboration (multi-client editing)

### R-COLLAB-001 — Concurrent human + AI editing `MUST`
**Requirement.** A human and an AI agent can edit the same Project concurrently through EditorKernel with a defined concurrency-control model. Mechanism locked (L-50): daemon write-queue serialization for attached clients; watcher convergence for detached writers; field-path conflict resolution (L-30); `--if-match` CAS; git worktrees for coarse-grained parallelism (L-26). CRDT co-editing is post-v1.
**Rationale.** Core to the AI-assisted development thesis.

### R-COLLAB-002 — Consistent live view `MUST`
**Requirement.** All attached clients see a consistent, live-updating Project state. This is satisfied by — and is a cross-reference to — R-BRIDGE-005 (multi-client attach) + R-BRIDGE-008 (sequenced snapshot-then-delta event stream); it adds no separate machinery.
**Rationale.** Coherent collaboration.

---

## 22. Profiling, debugging & observability

### R-OBS-001 — In-editor debugging `MUST`
**Requirement.** The Editor supports debugging the running game (inspect entities/components, breakpoints or equivalent for gameplay logic, live value editing).
**Rationale.** Explicit Editor definition requirement.

### R-OBS-002 — Profiling `MUST`
**Requirement.** The engine provides CPU/GPU/memory profiling with a frame timeline; a lightweight always-on HUD plus export to standard tools (e.g. Tracy/RenderDoc). Stack locked (L-47): counters + HUD, Tracy/RenderDoc export, and all profiling data CLI/RPC-queryable as JSON so agents can act on performance.
**Rationale.** Explicit Editor definition requirement; don't reinvent world-class profilers.

### R-OBS-003 — Remote/on-device profiling `SHOULD` (v2)
**Requirement.** Profiling data can be captured from a running Game Build on a target device. Deferred to **v2 alongside mobile** (the Q1/Q2 platform rulings): on-device capture serves mobile/console performance work, not the wedge — desktop/Linux-server/web are covered by L-47's local profiling stack plus the R-BUILD-009 smoke-run. The priority letter is a v2 priority.
**Rationale.** Real performance lives on-device, not only in the editor.

### R-OBS-004 — Uniform tracing across languages `SHOULD`
**Requirement.** The scheduler emits per-system trace spans regardless of authoring language (C++/TS/WASM).
**Rationale.** Coherent profiling across the polyglot boundary.

### R-OBS-005 — TS debugging, source-mapped end-to-end `MUST`
**Requirement.** TS debugging is source-map-mapped end-to-end: breakpoints and stepping via a standard protocol (CDP or DAP) exposed through EditorKernel; **TS-resolved stack traces** appear in every error/diagnostic surface, including headless CLI output. Debugger availability was an explicit §2d engine-choice scoring criterion — resolved by L-61 [spike-ratified 2026-07-02, owner]: V8's in-box CDP inspector (`v8-inspector.h`; Chrome DevTools / VS Code js-debug speak it natively) satisfies this requirement by configuration, not by building a debugger.
**Rationale.** "TS is the gameplay language" without a debugger is not a product; headless CI still needs symbolicated traces.

### R-OBS-006 — Crash reporting & update channel `SHOULD` (v2)
**Requirement.** Opt-in, privacy-gated crash reporting for the editor/daemon (minidumps + a symbol server) plus an update-check channel. The **update-check channel** and any update it delivers **MUST be signed + verified against the trust root (R-SEC-009) + TLS/cert-pinned, verify-before-execute, fail closed** — an update channel that ships an unverified binary is a direct code-execution path onto every developer's machine. (The symbol server / minidump upload path stays privacy-gated per O-6.) The crash-symbol/minidump backend is **off-the-shelf (GlitchTip-class), not bespoke infrastructure** — the engine ships the client-side capture + privacy gate and points at a standard backend.
**Rationale.** A tool this stateful will crash in the field; without minidumps, every crash report is an anecdote. Privacy-gating aligns with the O-6 posture.

---

## 23. Cross-cutting quality attributes

### R-QA-001 — Consistent cross-platform behavior `MUST`
**Requirement.** Game logic and content behave consistently across platforms; platform differences are confined to documented, surfaced areas (performance tiers, input model, platform limits).
**Rationale.** "No difference where it executes" for logic/semantics.

### R-QA-002 — Fast iteration loop `MUST`
**Requirement.** The default authoring loop (edit → see result) is near-instant for the TS tier; native-tier changes have a defined, optimized rebuild path. Native path locked (L-43): incremental rebuild + relink (lld/mold) + session restart; no hot-patch machinery; restarts are cheap because the daemon is stateless-restartable (L-19).
**Rationale.** Productivity; "play the game quickly."

### R-QA-003 — Documentation & discoverability `SHOULD`
**Requirement.** Public contracts (commands, kernel interfaces, package APIs, UI-Provider contract) are documented and versioned.
**Rationale.** Ecosystem growth and maintainability.

### R-QA-004 — Testability `SHOULD`
**Requirement.** Gameplay, UI, and systems are testable headless via file writes + the RPC/query surface and state assertions, without a GPU.
**Rationale.** Automated and AI-driven QA.

### R-QA-005 — Headless session control for testing `MUST`
**Requirement.** Via RPC/CLI: step exactly **N fixed ticks**; set/query the sim **seed**; inject synthetic **input events and action activations** (gameplay + UI); **record/replay** input streams; query the canonical **state hash**. (Cross-referenced from L-54 — the CI determinism gate is built on this surface.)
**The `simTick` counter.** The session exposes a **monotonic `simTick` counter** — the number of fixed ticks advanced since session start — **queryable via RPC** and **returned in every step-result envelope** (R-CLI-008). It is the stepping-specific replacement for idempotency keys: R-CLI-016 notes tick stepping is inherently non-idempotent, so on a lost acknowledgement the client **reads the counter and retries only the missing delta** ("I asked for 60 ticks, the counter shows 45 landed → step 15") instead of blindly re-stepping. The R-BUILD-009 smoke-run asserts against the same counter.
**Determinism TRIAGE, not just detection.** The state hash is a **hierarchical hash** — a per-tick root with **per-system and per-archetype sub-hashes**, plus a trace mode recording the hash tree per tick — so a determinism failure is **triaged automatically**, not merely detected: on divergence the harness **replay-bisects to the first divergent tick** and **diffs the first divergent system**, and **`context determinism diff`** reports `(tick, system, entity, componentField)`. The CI determinism matrix **names its platforms** — minimum **Linux-x64, Win-x64, macOS-ARM64** — as rows of the R-QA-012 fleet manifest (mirrored on L-54).
**The replay artifact is a versioned kind.** The record/replay artifact is a **versioned, schema'd kind**, pinned **before M3** builds tooling on it: input stream + seed + tick count + engine/protocol versions + a **content-hash manifest** of the project inputs it ran against + (in deterministic mode) the **expected per-tick hash trace**. Replay **verifies the manifest before running** — a replay against drifted content is reported as such, never silently divergent — and reports the **first-divergence tick**; replay outside deterministic mode is explicitly **labeled best-effort**.
**Rationale.** Every automated/AI QA loop the design promises assumes this surface; L-54's state-hash gate is unimplementable without it.

### R-QA-006 — Project templates & maintained samples `MUST` (default template = runnable skeleton) · `SHOULD` (template catalog + maintained samples)
**Requirement.** `context new` offers project templates; at 1.0 the engine maintains **≥ 2 sample projects**, which double as the agent few-shot corpus and as benchmark seeds. The maintained samples are **split into two sets with different acceptance bars**: a **human-onboarding set** (small, readable, tutorial-shaped — the getting-started references of R-HUX-010) and the **agent few-shot corpus** (breadth of kinds/verbs for machine learning-by-example + benchmark seeds). Both are "maintained" (kept building against the current engine); the split exists because the qualities that make a good human tutorial differ from those that make a good agent corpus.
**The DEFAULT template MUST be runnable.** `context new`'s **default template yields a minimal RUNNABLE skeleton** — a scene, a camera, and a startable session, such that the first query/step after `context new` succeeds without error — and this is **MUST for the default template** (the wider template catalog and samples are `SHOULD`). An agent's first contact with a fresh project must not be an empty-world error loop. Note the introspection split this pairs with: **`context describe` answers "what does the *contract* offer"** (R-CLI-013) while **`context query` answers "what is in *this world*"** (R-CLI-006) — a fresh default project answers both without error.
**Rationale.** Agents (and humans) learn the engine from working examples; samples that rot are worse than none — hence "maintained".

### R-QA-007 — Minimum-spec floor `MUST`
**Requirement.** For each target platform the engine commits a **reference minimum-spec device** and a **target frame-rate** on it, and a **CI benchmark** validates that a representative scene holds that frame-rate on (a proxy for) that device. This min-spec floor is the anchor for the runtime performance budgets: the R-LANG-012 TS frame budget is measured against it, and the R-REND-002 renderer floor is defined on it. Because L-56 removed the sub-WebGPU tier, the min-spec floor also **resolves R-REND-002's "graceful fallback"**: devices below the WebGPU floor are either served by an **explicitly-committed constrained path** or **scoped out of R-BUILD-001 honestly** — the design does not silently promise sub-min-spec support.
**Platform scope.** The floor table follows the platform set (R-BUILD-001): v1 commits floors for the v1 platforms (desktop, Linux server, web); the **Android floor activates when trailing-SHOULD Android lands** (its bench never blocks a wedge milestone before then); the **iOS reference-device floor is v2 with iOS**. The proxy device for each floor is **named in the R-QA-012 fleet manifest** and measured under the R-QA-009 methodology.
**Rationale.** "Runs on mobile," "graceful fallback," and "TS is fast enough" are only meaningful against a named device and a committed frame-rate; without a min-spec floor those are unfalsifiable, and the post-L-56 renderer has no lower tier to fall back to, so the floor must be stated rather than implied.

### R-QA-008 — Engine test architecture `MUST`
**Requirement.** The engine has a designed test SYSTEM, not only a list of CI gates: **unit tests per kernel component and per package**; **property + integration suites for the file-sync layer** (promoting the "property/fuzz tests over the sync layer" risk-table note to a requirement); **golden-file suites** for canonical serialization, schema migration, and structural merge (their corpora are R-QA-011 deliverables); and a **protocol conformance suite generated from the R-CLI-009 registry** (every verb/method/tool exercised against its schema, not only the parity check). **Each milestone's exit includes its own suite green — not only its demo.** Minimal v1 at M1: the kernel, file-sync, and canonical-serializer suites exist and gate.
**Rationale.** A gate LIST (sanitizers, determinism hash, parity, benchmarks) is not a test architecture; without per-layer suites the gates exercise the demo path only — and the file-sync layer, also the product moat, deserves a requirement rather than a risk-table note.

### R-QA-009 — Performance-gate methodology `MUST`
**Requirement.** Every CI-enforced performance target (R-FILE-011, R-LANG-012, R-QA-007, R-BUILD-006, the R-BRIDGE-008 session-query p99) runs under one **published methodology**: benchmarks execute on **named, dedicated, perf-isolated runner classes** (the R-QA-007 proxy device is defined **by name** in the R-QA-012 fleet manifest, never "some CI box"); each result is the **median of N ≥ 5 runs with dispersion recorded**; a gate **fails only on a breach confirmed against a rolling baseline within a documented variance band** (no single-run flake failures); and results are **archived as a time series** so drift is visible before it breaches. Minimal v1: **one bare-metal Linux perf box, median-of-5, a ±10% band**.
**Rationale.** The docs commit to a dozen numeric floors; a number enforced on a noisy shared runner with single-run sampling produces flake-driven reverts and quietly widened budgets. The methodology is what makes the floors real rather than aspirational.

### R-QA-010 — Daemon fault-injection harness `MUST` *(M1 — the seams cannot be retrofitted)*
**Requirement.** The file-sync layer is built on **injectable seams** — virtual filesystem, watcher, and clock — and a **deterministic-simulation test harness** drives them: **crash points between every durable step of every multi-file verb** (asserting resume-or-diagnostic per R-FILE-004's intent log); **watcher event loss, duplication, and reordering** (asserting convergence per R-FILE-002); **slow-client queue overflow** (asserting the gap-marker + re-snapshot path per R-BRIDGE-008). Failing seeds are **minimized and committed** as regression cases (R-QA-011). The seams are **M1 architecture** — a file-sync layer built directly against the OS cannot have them retrofitted.
**Rationale.** R-FILE-002/004 and R-BRIDGE-008 make strong crash/loss/overflow promises that ordinary integration tests never exercise; deterministic-simulation testing (the FoundationDB lesson) is the known way to prove them, and it is only possible if the seams exist from the first commit.

### R-QA-011 — Test corpora as versioned deliverables `MUST`
**Requirement.** The test corpora are **versioned, committed deliverables with owners**, not incidental fixtures: **migration fixtures per schema bump** (pre-migration input + golden post-migration output, added with every bump, kept forever); a **three-way merge corpus** with a case per documented conflict class (R-FILE-012, including the id/meta/binary rules); a **malformed-file corpus** with at least one case per error-catalog code (R-CLI-008) so every diagnostic is provably reachable; **importer fuzz seed corpora**, committed and minimized (R-SEC-006); and **composition edge-case scenes** (deep nesting, dense fan-in, instancing cycles, orphan overrides). The cross-implementation canonical-serialization test vectors (R-FILE-001) are part of this set.
**Rationale.** Golden-file testing (R-QA-008) is only as good as its corpora, and migrations especially are write-once-test-forever artifacts: if the fixture is not a deliverable of the schema bump itself, it never exists afterwards.

### R-QA-012 — CI fleet manifest `MUST`
**Requirement.** A **versioned CI fleet manifest** maps **every CI-enforced requirement to the runner class that enforces it** — OS/hardware, GPU presence, device proxy, isolation class (perf-isolated vs shared), and build flavor (sanitizer, deterministic, release). **Provisioning a named runner class is a milestone de-risk item** like any other dependency, and a gate whose runner class is **not yet provisioned is marked "advisory until provisioned" in the manifest** — visibly degraded, never silently green. The determinism matrix (R-QA-005), the perf boxes (R-QA-009), the GPU runner (the M4 visual-equivalence gate), the macOS build agent (R-BUILD-007), and the **N-daemons-on-one-box scenario row** (R-FILE-011 — the runner class validating composed watcher/fd/index-memory/cache budgets for multi-worktree hosts) are all manifest rows.
**Rationale.** The docs enforce requirements "in CI" across a dozen distinct hardware/OS/GPU contexts; without stating what CI *is*, unprovisioned gates become silently-skipped gates. The manifest makes the fleet a designed, versioned artifact and every gap honest.

### R-QA-013 — feature-coupled test delivery `MUST` [owner-ruled 2026-07-02]
**Requirement.** **Every merged change that adds or modifies behavior lands WITH rich tests exercising that behavior in the same PR** — happy path, edge cases, and failure paths — from the very first commit of the project, and **including infrastructure and tooling code** (CI gate scripts, corpus generators, build tooling), not only engine code. CI runs the affected suites per PR; a behavior change without accompanying tests does not merge. **Retroactive rule:** anything already implemented without coverage is a standing debt item to be closed explicitly — tracked, never silently accepted. **Spike/throwaway code (`spikes/`) is exempt from durable-coverage obligations but MUST carry runnable self-checks** (assertion-bearing demos registered with the test runner) so its claims stay executable, not narrative.
**Rationale.** R-QA-008 designs the test *system* and gates *milestone exits* on suites; this requirement makes test delivery **PR-granular** — coverage arrives with the feature, not in a later hardening pass. Owner directive 2026-07-02: "cover each tiny single feature in the project from early start with rich tests."

---

## 24. Open items to convert into requirements during review

Status as of 2026-07-01 (resolved under owner delegation unless marked open):

- **O-1.** ✔ Converted to **R-DATA-005** (`SHOULD`) — player save-game API.
- **O-2.** ✔ Resolved [owner-ruled 2026-07-01] — split: string-table asset kind in M2 (**R-I18N-001**, `SHOULD`); text shaping/bidi/IME = declared UI-Provider capabilities (R-UI-005; guaranteed on web-tech backends, deferred-and-documented on the minimal backend per L-53); **editor-UI localization is post-v1**.
- **O-3.** ✔ Resolved [owner-ruled 2026-07-01]: editor accessibility discipline REQUIRED from the first M5 component (**R-A11Y-001**, `MUST`); runtime accessibility-primitives package (**R-A11Y-002**, `SHOULD`, post-core); CVAA noted for future chat/voice packages.
- **O-4.** ✔ Console platforms: **WON'T (v1)** — revisit post-v1 alongside the platform seam.
- **O-5.** ✔ Marketplace / registry hosting: **post-v1** (public npm suffices for v1).
- **O-6.** ✔ Live-ops / telemetry: **post-v1**, privacy-gated when designed.
- **O-7.** ✔ Resolved [owner-ruled 2026-07-01], part of **L-57**: a **CLA with copyright assignment (or an exclusive unrestricted grant) is REQUIRED before any external contribution merges — DCO alone is INSUFFICIENT**; full copyright ownership is what keeps the Context Engine EULA's relicensing and royalty terms enforceable now that the repo is public from day one (L-57). A **dependency-license allowlist is enforced in CI from the first commit**: the allowlist is **deny-by-default** (an unknown or missing dependency license **fails CI**), **scans both the npm and the vcpkg transitive dependency graphs**, **fails on any copyleft license linked into the shipped engine cores** (EditorKernel / RuntimeKernel / a shipped game build), **permits permissive-licensed build-only tools** (build-time-only dependencies that never link into a shipped core), and **emits an SBOM**. EULA legal drafting is deferred to counsel (business-dept/legal task).
