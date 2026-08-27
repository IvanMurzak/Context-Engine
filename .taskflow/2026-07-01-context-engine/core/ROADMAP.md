# Context Game Engine — Roadmap

> **Engine name:** Context Game Engine ("Context").
> **Status:** Design phase complete (2026-07-01). **Implementation status (2026-07-18):** **M0–M8.5
> COMPLETE** in the engine repo (`IvanMurzak/Context-Engine`). **🏁 M8 (build pipeline) ✅ COMPLETE
> 2026-07-17 — 14/14 tasks a01–a14 landed** (pack format→writer, chunked loader, per-platform
> transcode, wgpu-native backend pin, build-core CLI, Linux/Windows/macOS/Web export adapters,
> headless-smoke + packed-determinism gates, trust-root pin + fetch-verify, `context doctor`,
> build-time budgets, and the 6 blocking `m8-exit-*` gates). OS-signing trilogy live — Ed25519 pack
> signing + Windows Authenticode + macOS Developer-ID/notarize, verify-before-use failing closed
> under the pinned trust root. **🏁 M8.5 (wedge hardening) ✅ COMPLETE 2026-07-18 — v1 wedge scope-complete; a15–a23 all landed (a23 exit gate = [CE #302](https://github.com/IvanMurzak/Context-Engine/pull/302), 9 blocking `m85-exit-*` gates), ops1 (perf-runner) deferred → density numbers advisory-until-provisioned.** Prior: **M7 (runtime UI) ✅ COMPLETE —
> 12/12 tasks landed** (a1 foundation `c4f142c`; a2 layout+hit-test `0cc1626`; a6 GPU backend
> `126aac7`; a4 TS authoring `e897efc2`; a3 input routing `2574c36d`; a9 world-space RTT panel
> `cbe5570`; a5 CLI verbs `7e1902a`; a7 font substrate `a932376`; a8 shaping-grade text
> `65186e3f`; a10 curved world-space UI `3f240eb8`; a11 capability matrix + conformance suite
> `c2d5438`; a12 M7 exit gate `743f82e`). The five blocking `m7-exit-*` gates are live + green on
> the 3-OS CI matrix; a7/a8 text-dep gate was ✅ approved (Fable agent). This folder
> remains the design authority; per-milestone as-built deltas live in the engine repo.
> A three-reviewer design review of the remaining stages (M8/M8.5) ran **2026-07-15** — findings
> and dispositions in `REVIEW-R6-2026-07-15.md`; owner-open items consolidated in §7.
> This is the **volatile half** of the design package: milestones, the "Context Sim" pre-release
> track, v2 scope, economics, risks, and CI tiering. Its sibling **`ARCHITECTURE.md`** holds the
> stable half — vision, architecture,
> components, technology stack, and the key data/control flows.
> **Companion docs:** `REQUIREMENTS.md` (what to build), `DESIGN-DECISIONS.md` (locked choices +
> open options). Per-round provenance for every decision lives in the `REVIEW-*.md` trackers.

---

## 1. Milestones (M0–M8.5)

Each milestone is independently demonstrable and de-risks the next. Order reflects dependency:
foundational data/execution first, then rendering, then advanced features. The design forked a
**"Context Sim" pre-release track (§2)** after M3; as-built the track did not run — execution was
single-lane M0→M7 [owner-directed 2026-07-05] — and §2 records the disposition.

### M0 — Foundations & spikes (proof it can work)
- Repo, build system (CMake), C++ toolchain + package manager (vcpkg) + caching. **Signed-fetch
  verification of the toolchain + engine into the bootstrap** (pinned trust root, per-artifact
  detached signatures, verify-before-execute, fail closed — R-SEC-009 / L-58) so the very first
  toolchain/engine download is trust-anchored. *(As-built 2026-07-15: the fail-closed verify
  gate, pinned trust-root mechanism, and fail-closed test suite exist — engine `docs/signing.md`,
  `tools/verify_artifact.py` — but the production key is deliberately unminted, the trust root
  empty, and the toolchain/engine fetch paths not yet wired through it; both land at M8.)*
  **CLA-with-copyright-assignment gate**
  [owner-ruled 2026-07-01]: a CLA with copyright assignment — or an exclusive unrestricted
  grant — is REQUIRED before any external contribution merges; DCO alone is INSUFFICIENT. Plus
  the **dependency-license allowlist** in CI **from the first commit** (L-57):
  **deny-by-default** (unknown/missing license = CI fail), **scans npm + vcpkg transitive
  graphs**, **fails on copyleft linked into shipped cores**, permits permissive build-only tools,
  and **emits an SBOM** (O-7 / L-57). **Versioned install layout from the first release**: the
  packaging skeleton installs to `<root>/versions/<semver>/` from day one (R-VER-004) — a flat
  first install would foreclose side-by-side engine versions forever.
- **Public-from-day-one sequencing** [owner-ruled 2026-07-01]: the engine repo is **PUBLIC from
  the start of development** under the custom **Context Engine EULA** (un-gated Unreal
  mechanism — L-57). Two **M0 gate items** therefore precede the first public push: **(1)** a
  **counsel-reviewed LICENSE** (the EULA) exists in the repo **BEFORE the first public push** —
  never push public un-licensed; **(2)** the **engine name is settled before the repo flips
  public** (settled 2026-07-01 — §7): if naming was unresolved at bootstrap time, **bootstrap
  the repo PRIVATE and flip it public once named**.
- **The six spikes:**
  1. **WebGPU triangle** on Win/Mac/Linux/Web — the web leg binds the browser's WebGPU while
     native runs wgpu-native (the M0–M4 dev backend picked by this spike), and the spike
     exercised both SPIR-V→WGSL tools (Tint and Naga, byte-identical output) — the WGSL tool
     choice is the M4 deliverable (R-REND-005).
  2. **JS engine embedding** — measuring interpreter-mode throughput, **ArrayBuffer detach
     cost — a HARD GATING criterion (R-LANG-009)**, GC-pause behaviour (R-SIM-008), and the
     debugger story (R-LANG-009/011, R-OBS-005) — the four §2d scoring criteria (§2d closed as
     L-61 on this spike's data).
  3. **WASM module execution** — the embedding/execution leg. The native-AOT leg
     (wamrc / wasm2c+clang / Cranelift-class) with its committed size+throughput acceptance bar
     is **v2 with iOS** — no v1 target requires it (R-LANG-005) [owner-ruled 2026-07-02].
  4. **ECS spike.**
  5. **CEF accelerated-OSR + native-viewport shared-texture compositing** with the L-41 fallback
     decision tree.
  6. **Parse+canonicalize+hash + merge-driver throughput benchmark** (front-loaded, weeks 1–4):
     a **throwaway 100k-synthetic-file** benchmark — generate 100k synthetic canonical-JSON files
     (the R-FILE-011 shape mix) and measure the full parse→canonicalize→hash pipeline (the
     R-FILE-011(a) cold/fresh-attach bound — the derivation key is the CANONICAL-content hash, so
     raw-byte digests don't count) plus `context merge-file`-class structural three-way merge
     throughput. This spikes the **moat's own perf claim BEFORE the architecture is committed**:
     if parse throughput misses the target by **5×**, the fix touches **L-32 (format) / L-33
     (granularity)** and must be known before M1/M2 freeze the file model. Task file:
     `2026-07-02-context-m0-spike-parse-bench`.
- **Spike status: all six COMPLETE and owner-ratified** [spike-ratified 2026-07-02, owner] —
  evidence in the engine repo's `spikes/*/FINDINGS.md`; resolutions encoded as **L-60 (ECS),
  L-61 (JS engine = V8), L-62 (WASM runtimes)**, the **L-41 per-platform CEF tree**, the
  **R-LANG-008/009 allocator-identity amendment**, and the **L-32/L-33 measured confirmation**
  (no format/granularity change — 14× inside the 5× miss line). One deliberate carry-forward:
  the SPIR-V→WGSL **tool recommendation moved to M4** on byte-identical Naga/Tint spike output
  (R-REND-005) rather than being forced at M0.
- **Exit:** the six hardest unknowns are demonstrated in isolation — including the SPIR-V→WGSL
  tool evaluation (both tools exercised, byte-identical output; the recommendation itself moved
  to M4 per the 2026-07-02 ratification), and the parse+canonicalize+hash + merge-driver benchmark reporting its
  throughput tables against the R-FILE-011(a) projection and the 5× miss threshold, with an
  explicit L-32/L-33 change recommendation — or none — before M1/M2 freeze the file model; the
  bootstrap verifies signed toolchain/engine artifacts and fails closed on a bad signature; **no
  public push happens before the two public-push gate items above (counsel-reviewed LICENSE +
  settled name) are satisfied** [owner-ruled 2026-07-01]. *(As-built: the repo went public
  2026-07-13 with the v0.2.1 **draft** EULA — counsel review remains open, §7; the name was
  settled 2026-07-01. The signed-fetch exit clause is satisfied at mechanism level — the
  production key mints at first release, M8.)*

### M1 — Microkernel & file-authoritative EditorKernel skeleton
- World (data-oriented ECS), fixed-timestep Scheduler, module registry, event bus, resource
  handles, platform seam. **The sim→render timing contract is designed here** (R-SIM-002/L-39):
  tick-rate policy, render-side interpolation/extrapolation between fixed ticks, and the
  high-refresh presentation rule (144 Hz display on a 60 Hz tick = interpolated frames, never
  re-simulation).
- **Generated-registration DCE proof (R-KERNEL-003 — an M1 de-risk item):** the build-time
  **generated registration TU** (only referenced packages registered — static-initializer
  self-registration prohibited in shipped builds, since it structurally defeats `--gc-sections`)
  + **uniform LTO across from-source ports** + linker GC is proven here with a measured
  size/compile-action delta showing an unreferenced package leaves zero footprint.
- File-sync layer (atomic IO, watcher + hash reconciliation with **unconditional re-hash of hinted
  paths** + a **low-frequency background full re-hash crawl** as the dropped-event safety net,
  expected-writes table with **short-TTL entries**, persisted reconcile index `.editor/index` —
  R-FILE-002) + a **crash-recovery intent log** under `.editor/` (fsync-before / clear-after,
  resume-or-diagnose on restart, multi-file verbs idempotent — R-FILE-004) + derivation-graph
  skeleton with **derivation-side backpressure** (coalesced batch passes, queue-depth/bounded-lag
  signal, load-shed to queried/visible subgraphs — R-FILE-013) + RPC/query surface with the
  **read-your-writes barrier** (`--after-hash`/`--after-generation` — R-CLI-006); minimal CLI
  (file-rewriter verbs); EditorKernel-as-library + daemon + single-instance byte-range lock acquired
  as the **atomic first action** of any write-capable instantiation (try-lock-failure = attach
  signal — R-BRIDGE-001/R-ARCH-005) + bridge IPC; event stream incl. the `log` topic, the
  **derived-world generation counter**, the **`derivation.settled{generation}` quiescence event**,
  the **`stability` field** on diagnostics, and the **incarnation epoch** (`incarnationId` +
  post-derivation totally-ordered `seq`) — R-BRIDGE-008.
- **Public-contract cluster lands BEFORE the RPC/event schema freezes (R-CLI-007…017):** the verb
  grammar + package namespace + core-flag set (R-CLI-007), the uniform result envelope + versioned
  error-code catalog + exit-code table (R-CLI-008, into which R-FILE-003 / R-BRIDGE-006 / R-SEC-008
  / R-PKG-005 diagnostics fold), the single registry that generates CLI ≡ RPC ≡ MCP ≡ introspection
  (R-CLI-009), the query-language spec (EBNF, operator set, total ordering, unified cursor —
  R-CLI-012), whole-contract self-description `context describe --json` (R-CLI-013), event/topic +
  subscription introspection (R-CLI-014) and the subscribe/unsubscribe/ack protocol (R-CLI-015),
  client-supplied idempotency keys (R-CLI-016), the transport-portable large-result handle
  (R-CLI-017), and the batch/`--atomic-plan` contract (R-CLI-011). The attach uses the
  **capability-negotiation handshake** (R-CLI-010, replacing R-BRIDGE-006 version-equality), and
  **scope enforcement lives in the RPC dispatcher, not the MCP adapter** (R-SEC-007, MUST) with
  scoped-token minting + consent-gated elevation. Freezing the schema before this cluster exists
  would bake in an under-specified contract. The **composed write-path contract joins this
  pre-freeze list** — default-outermost override writes, `--edit-template` / `--at-instance
  <idPath>`, and the provenance-CHAIN result shape (R-CLI-006); it is public contract and cannot
  slip past M2. Contract trims: the handshake **carries** `{protocolMajor, capabilities[]}` from
  day one with hard-fail behavior (negotiation activates at the second released protocol
  version — R-CLI-010); `--atomic-plan` and the idempotency replay-store are `SHOULD` with their
  flags reserved (R-CLI-011/016); the large-result handle ships the portable-URI format with
  same-FS fetch only (R-CLI-017); scope enforcement's v1 slice = token scope field + dispatcher
  checks + launch-time operator scopes, minting/elevation deferred (R-SEC-007).
  **`protocolMajor=0` staging (R-CLI-004):** M1 ships this cluster **explicitly UNSTABLE at
  `protocolMajor=0`** — the contract may break without deprecation cycles until the **M3 freeze**
  (`protocolMajor` → 1, after the agent corpus has exercised the surface). Staged landing:
  **grammar/envelope/registry/describe (R-CLI-007/008/009/013) at M1**; the **full query-language
  EBNF (R-CLI-012)** and the **subscription/topic-introspection detail (R-CLI-014/015)** complete
  **by M3**. `context new`'s **runnable default template (the R-QA-006 MUST half)** also lands at
  M1 with the minimal CLI.
- **Test-system foundations (R-QA-008/009/010/012 — the seams are M1 architecture):** the
  file-sync layer is built on **injectable seams** (virtual FS, watcher, clock) and the
  **deterministic fault-injection harness** exercises crash points between durable steps, watcher
  loss/dup/reorder, and slow-client overflow from M1 (R-QA-010 — not retrofittable); the
  **kernel, file-sync, and canonical-serializer suites** of the R-QA-008 test architecture exist
  and gate M1 exit; the **cross-implementation canonical test-vector corpus** (R-FILE-001)
  lands with the serializer; the **CI fleet manifest** (R-QA-012) and the **perf-gate
  methodology** (R-QA-009 — one bare-metal Linux perf box, median-of-5, ±10% band) stand up with
  the first benchmarks.
- Synthetic 100k-file benchmark project + perf harness — the R-FILE-011 targets (index-warm
  attach ≤ 5 s; fresh-attach = **parse+canonicalize+hash-throughput-bounded** with progress;
  async-streamed fan-out) are tracked from day one, not bolted on later, and include a
  **multi-worktree cache-contention scenario** (R-FILE-010), a **dense-reference synthetic**
  (edges O(refs) > O(files) — R-FILE-011(e)), and the **N-daemons-on-one-box scenario**
  (R-FILE-011). **M1 exit includes the per-stage latency budget table**
  (watch/hash/parse/validate/compose/instantiate/fan-out), the session-query p99 ≤ 5 ms budget,
  the cursor/paged query contract, and inspector push-subscriptions. Per the §6 CI tiering, the
  **per-PR 100k-file gate is the 10k proxy until M3** (the full 100k benchmark runs nightly).
- **Exit:** mutate a scene file via CLI verb AND via raw text edit and observe the identical
  derived World headless; a second CLI attaches live via the capability-negotiation handshake
  (R-CLI-010); `kill -9` the daemon mid-multi-file-write and lose nothing (intent-log resume, its
  entries HMAC-integrity-checked and re-jailed/re-CAS'd on resume — R-FILE-004) and reconnect with a
  fresh snapshot on the new incarnation; **CI proves CLI ≡ RPC ≡ MCP ≡ introspection parity
  (R-CLI-009) and enforces the error-code catalog is additive-only (R-CLI-008)**; a read/query-scoped
  token cannot install a package or trigger a build via **direct RPC** (dispatcher-level scope
  enforcement — R-SEC-007).
- **De-risks:** R-ARCH-*, R-BRIDGE-*, R-KERNEL-*, R-HEAD-001..003, R-FILE-001..013,
  R-CLI-006..017, R-SEC-007, R-QA-005, R-QA-008/009/010/012, R-QA-006 (runnable-default-template
  MUST half) + the R-SIM-002 timing contract.

### M2 — Data model & asset pipeline
- Canonical-JSON scene serialization (L-32) with **stable intra-file ids + id-keyed child
  collections** (the L-33/L-35 schema invariant — it MUST land here; realized as
  arrays-of-objects-with-`id`, map form forbidden — R-FILE-001 [spike-ratified 2026-07-02,
  owner]), scene composition +
  overrides (L-35), parse-time schema migration (L-37). **Flatten emits GUID-addressable,
  independently loadable/unloadable content-unit boundaries** and those boundaries are
  **co-designed here with the R-ASSET-005 chunked pack format** (not deferred to M8 — L-35). A
  **composition nesting-depth cap + fan-in diagnostic** land as M2 schema invariants
  (R-FILE-011(e)).
- Asset database (GUID + sidecar meta — L-36), importer framework (**run-deterministic** and
  **isolated** — R-ASSET-001/R-SEC-006), incremental import via the shared cache (L-28),
  per-platform transcode; mesh import reserves UV2 (R-REND-006); **string-table asset kind**
  (R-I18N-001); tilemap asset kind (R-2D-003); player save-game groundwork (R-DATA-005).
- **M2 data-model seams (the M2 schema CANNOT ship without them):** nested-instance **override
  id-path addressing** + innermost-out precedence (L-35); the **composed write path**
  (default-outermost, `--edit-template`/`--at-instance`, provenance chains — R-CLI-006);
  **per-component-payload schema versions** (the `componentVersions` header map + per-payload
  parse-time migration selection — L-32/L-37); the **package-migration execution contract**
  (sandboxed-tier-only migrations, the migration-set hash in the cache key, the
  `schema.newer_than_package` downgrade rule — L-37/R-FILE-005/010/R-PKG-005); **override
  migration** (paths transformed with payloads; orphan-override diagnostics; dangling overrides
  excluded from flatten — L-37/R-FILE-012); the **entity-ref value type** (`{"$entity": …}`,
  id-path form, cross-scene-file refs to non-instanced scenes prohibited v1 — L-34);
  **composed/runtime identity** (deterministic id-path, stable across re-derivation/upgrade; ONE
  identity for saves/net/queries — L-37); **id-allocation + merge rules** (collision-resistant
  random ≥ 64-bit ids; duplicate-id diagnostic + re-key verb; meta/binary whole-file ours/theirs —
  L-33/R-FILE-012); the **schema vocabulary + units law** (R-DATA-006 —
  `x-ctx-type`/`x-ctx-storage`/`x-ctx-ref`/tagged unions/SI + radians, pinned BEFORE the first
  component schemas freeze); **binary-sidecar authoring rules** (magic + version header,
  `{"$sidecar": …}` refs, owned-satellite moves, sidecar-first write order — L-33; needed day one
  for tilemaps); **scene-level state** as singleton components on the scene-root entity
  (inert-under-instancing by default, `composable` opt-in; bakes are derived — L-35); **runtime
  save-migration groundwork** (per-component schemaVersion map in saves, minimal runner,
  back-compat scope — R-DATA-005); the **meta `platforms` reservation** (L-36); and the advisory
  override-hygiene tooling (staleness query, redundant-override GC, direct-cycle diagnostic —
  L-35).
- **Exit:** author a scene from the CLI, save/reload with stable references, migrate a schema;
  flatten a composed scene into GUID-addressable content units that the (co-designed) chunk format
  can load/unload independently. **The data-model seam set above is IN the frozen M2 schema** —
  M2 does not exit with any of them missing; a composed-entity write lands in the correct
  file/level and reports its provenance chain; a per-payload migration round-trips against its
  R-QA-011 fixtures.
- **De-risks:** the highest-retrofit-cost cluster (R-DATA-001/002/004/005, R-ASSET-*), plus
  R-DATA-006 + the data-model seam set, plus R-SEC-003 (the no-secrets-in-project-files posture
  is designed/validated with the file-format + meta work here).

### M3 — Scripting & logic tier
- Embedded JS engine + TS toolchain; declarative component-type authoring (R-LANG-010) →
  **runtime-registered, data-driven archetype storage** + TS accessors + WASM layout + published
  schemas (feeding R-CLI-005) — **defining a component type requires NO native rebuild in v1**;
  a native-codegen fast path (C++ structs for hot types) is an optional optimization, never the
  only path (R-LANG-010).
- **Replay artifact pinned at M3 entry (R-QA-005):** the versioned, schema'd replay kind —
  input stream + seed + tick count + engine/protocol versions + content-hash manifest + expected
  per-tick hash trace — is pinned **before** M3 tooling builds on it.
- **Contract freeze at M3 exit (R-CLI-004):** `protocolMajor` bumps **0 → 1** after the agent
  corpus (R-QA-006) has exercised the surface; the full query-language EBNF (R-CLI-012) and the
  subscription/topic-introspection detail (R-CLI-014/015) complete here at the latest, and the
  R-CLI-010 deprecation lifecycle activates. Engine-driven npm installs land with the TS
  toolchain here under the R-SEC-005 rules (`--ignore-scripts`, lockfile integrity, SHA pins).
- Zero-copy view protocol under the R-LANG-009 lifetime rules (valid one system invocation;
  structural changes via command buffers; detach-on-exit; debug Proxies); `(query, executor)`
  system model across C++/TS/WASM; safe parallel scheduler (TS = single lane — R-LANG-011) +
  sanitizer CI; TS debugging source-mapped end-to-end (R-OBS-005).
- **Exit:** gameplay authored in TS mutates the shared World; a C++/WASM system does the same;
  both scheduled safely; **the TS throughput floor is measured on the wedge platforms' shipped
  VM configuration** (R-LANG-011 — the interpreter-mode bar follows iOS to v2).
- **De-risks:** R-LANG-*, R-SIM-006, R-OBS-005, R-CLI-005, R-SEC-005 + the R-CLI-004 contract freeze.

### M4 — Render module & RHI (T1 WebGPU)
- Tiered RHI: T1 WebGPU everywhere including web — **no WebGL2 backend in v1** (L-56);
  sim→render extract + double-buffer. "Graceful fallback" is **feature degradation within T1/T2**,
  not a sub-WebGPU tier (R-REND-002). The native T1 backend for **M0–M4 development is
  wgpu-native** [spike-ratified 2026-07-02, owner] (pinned, SHA-verified prebuilts; **not a
  permanent lock** — Dawn is re-evaluable here at M4 alongside the WGSL tool decision;
  acquisition/port burden — §5 risk row, with the narrow signed-prebuilt exception per
  R-SEC-009); the **web RHI backend binds the BROWSER's WebGPU** (`webgpu.h`→JS), **not** a Dawn
  cross-compile. *As-built through M7: wgpu-native (v29.0.1.1, SHA-pinned official prebuilts)
  remained the native backend; the Dawn re-evaluation was NOT performed at M4 — re-homed to M8,
  where the export templates freeze the shipped backend and Dawn's new official vcpkg port
  (~2026-06) changes the acquisition calculus (§5 risk row). **DECIDED at M8 (a04, 2026-07-16):
  RETAIN wgpu-native v29.0.1.1** — ship pin frozen; measured rationale + re-evaluation triggers in
  engine `docs/native-webgpu-backend-decision.md` (CE PR #254).*
- **Spatial acceleration structure** (R-SIM-007): the incrementally-updated broad-phase index —
  a package the render path depends on, **not** kernel core — **feeds render culling** so extract
  is bounded by the visible set (makes L-39 real) and also backs R-CLI-006 spatial queries and
  R-ASSET-003 streaming.
- **Sprite/2D rendering path**: orthographic projection, sprite batching, sorting layers,
  texture atlases (R-2D-001, L-55) — same renderer, not a fork.
- Material/shader system with cross-compilation — the named chain: **glslang/DXC → SPIR-V →
  SPIRV-Cross; SPIR-V→WGSL via Tint or Naga — the tool decision is made HERE at M4** on the
  engine's real shader corpus (M0 spike: byte-identical output through both — R-REND-005); shader
  compilation + variant generation run as **derivation-graph nodes** (cached per R-FILE-010);
  PBR + real-time lighting/shadows; the material contract + pack format carry lightmap inputs
  (R-REND-006).
- **Min-spec floor benchmark** (R-QA-007): a representative scene holds the committed target
  frame-rate on each platform's reference min-spec device (a proxy for it in CI).
- **Exit:** render a scene (3D **and** 2D) on desktop + web; **visuals identical within the T1
  feature set** — desktop renders through the native backend (wgpu-native per the M0 pick; Dawn
  re-evaluable at M4), web through the
  **browser's** WebGPU implementation, so "identical" is asserted against the same T1 semantics,
  not bit-identical frames from one shared implementation. The visual-equivalence METHOD: a
  **golden-scene corpus rendered offscreen per backend**, compared with a **named perceptual
  metric (SSIM-class) + per-scene tolerances** on the R-QA-012 GPU runner class; **rebaselines
  are reviewed changes**, never automatic; minimal v1 = **Linux-Vulkan + one browser blocking,
  other backends advisory** until their R-QA-012 runner rows are provisioned. Headless still
  runs with the render module absent; the min-spec floor bench is green.
- **De-risks:** R-REND-*, R-2D-001, R-HEAD-002, R-SIM-007, R-QA-007.

### M5 — Editor GUI & play-in-editor (observer-grade)
M5's v1 deliverable is an **observer-grade editor**: the **viewport (3D + 2D)** + **play
controls** + **scene tree** + **inspector whose edits are override writes** + the **Problems
panel** (R-HUX-005).
- CEF-embedded editor GUI; native viewport; inspectors — built **on** the editor-extension
  contract designed here (R-EDIT-001); accessibility discipline from the first component
  (R-A11Y-001: semantic HTML, ARIA, full keyboard nav). Hot reload is not a separate feature
  here: the GUI surfaces the L-22 watch→derive pipeline.
- **Scope of the REQUIREMENTS.md §14c human-editor cluster at M5:** GUI session undo/redo over
  the file-write journal, scoped to the shipped observer surface (R-HUX-001); per-verb `--help`
  is MUST and the in-GUI command palette is SHOULD (R-HUX-004); Problems panel + inline
  diagnostic markers (R-HUX-005); the **zero-AI-path-feature-complete principle** (R-HUX-009)
  and the **human-interaction latency budget** (gesture→viewport, selection, inspector commit —
  R-HUX-011, measured from instrumented timestamps) are continuous `SHOULD` targets, not hard M5
  gates; every authored kind's schema surfaces a `notes` field as the human "how to comment"
  affordance (L-32). **R-HUX-002 (git-abstraction history timeline) and R-HUX-003 (GUI launcher /
  project manager) are v2** (§3); **R-HUX-007 (GUI asset browser) is SHOULD v1.x**; **R-HUX-008
  (visual scene-merge presentation, pairs with R-FILE-012) slips** (SHOULD).
- **Trailing post-M5 within v1** (homed in the M8.5 trailing-v1 GUI bucket): the **tile-painting
  GUI + 2D viewport-authoring mode** [owner-ruled 2026-07-02] (the tilemap asset kind stays M2 —
  R-2D-003); **R-HUX-006's in-context scene-instance/override viewport editing** (MUST core +
  SHOULD affordances — the GUI face of L-35; the observer-grade M5 editor keeps
  inspector-override edits only); and **R-HUX-010 in-editor contextual help**. R-HUX-011 keeps
  at M5.
- **Editor testability:** a **per-OS CEF boot smoke gates M5**; panel logic is CI-assertable
  headless via the R-EDIT-001 UI-logic tree; R-A11Y-001 is enforced by an automated a11y scan +
  keyboard-only nav test per panel.
- **CEF packaging note** [spike-ratified 2026-07-02, owner]: since ~M138 the Windows **CEF
  sandbox requires the `bootstrap.exe` launch model** (the app builds as a DLL) — the
  sandbox-vs-not packaging decision is an M5 work item and does not affect the compositing seam
  (L-41). The compositing plan of record is the L-41 per-platform tree: accelerated OSR primary
  on Windows (software branch compiled-in), IOSurface + CEF-internal pacing on macOS (never
  `SendExternalBeginFrame` — cef#4033), software-OSR shipped default on Linux behind a
  Mesa/X11-ozone capability gate.
- Platform profiles + device emulation; play-in-editor == build fidelity; live-edit semantics
  (L-51).
- **Exit:** **open a project, inspect it (scene tree + inspector + Problems panel), play it, and
  make an override edit through the inspector** — matching a web (WebGPU) build for what it
  renders — plus the **per-OS CEF boot smoke** and the **automated a11y scan + keyboard-only
  navigation gates green on every shipped panel** (R-EDIT-001/R-A11Y-001). The fuller "author
  and play a game in the editor" bar reads against the post-M5 trailing surface, not M5.
- **De-risks:** R-PLAY-*, R-UI-007, R-EDIT-001, R-A11Y-001, R-2D-003, R-OBS-001, R-HUX-001..011.

### M6 — Core engine-system packages
> The design front-ran the **determinism/physics slice + the R-QA-005 session surface on the
> "Context Sim" pre-release track** (§2). As-built the track did not run — the slice landed here
> in normal single-lane sequence (§2 records the disposition).
- Physics (3D **plus the 2D Box2D-class package** — R-2D-002, L-55), animation + skeletal
  (+ animation-graph asset kind — R-SYS-008), particles, spline, audio, input (with UI/gameplay
  routing).
- **JS-tier GC discipline lands with the gameplay systems (R-SIM-008):** engine-provided
  **pooled / no-allocation math + query APIs** for hot systems, the VM configured
  incremental/generational with a **scheduled inter-tick GC window**, and the **GC-pause profiler
  channel** (L-47) so the per-frame GC-pause budget is observable — this is where the R-LANG-012
  frame budget meets real gameplay allocation patterns.
- **The deterministic wedge lands here:** deterministic mode (R-SIM-005 — **MUST on the wedge
  platforms**) ships with the physics/gameplay packages, and the **L-54 CI state-hash gate is in
  v1 and gates M6** — hierarchical hash + auto-triage per R-QA-005, on the named wedge-platform
  matrix (min Linux-x64, Win-x64, macOS-ARM64 — R-QA-012); the **L-48/R-NET-001 hooks are
  validated** by a state-sync harness against a real scene (identity = the L-37 composed id).
- **Exit:** a small but real 3D game AND a small 2D game (moving/animated physics objects,
  particles, sound, input); GC pauses stay inside the inter-tick budget (R-SIM-008); the
  determinism gate is green on the wedge-platform matrix and the netcode-hook harness passes.
- **De-risks:** R-SYS-*, R-2D-002, R-SIM-008, R-SIM-005 + L-48/R-NET-001 validation.

### M7 — UI system (runtime, pluggable)
- UI-Provider contract + capabilities (GPU-driver, damage repaint, GPU-composited transforms, headless logic).
- Engine-integrated default backend; screen-space + world-space (render-to-texture) UI. The XR
  leg — raycast input, OpenXR compositor layers, R-UI-004 + the XR-grade parts of R-UI-003/L-16 —
  is **v2** (§3) [owner-ruled 2026-07-01]; screen-space + basic render-to-texture world-space UI
  stay v1.
- **M7 scope rulings [owner-ruled 2026-07-13]** (M7 decomposition checkpoint): **(a)** the v1
  authoring form is a **TS retained-tree API with CSS-like style properties** (R-UI-001's
  HTML/CSS-file fidelity arrives later with the optional CEF runtime backend; R-UI-002's
  contract is the seam); **(b)** M7 ships contract + null/headless provider + engine-integrated
  provider + conformance suite — further backends (CEF-runtime/minimal) are trailing-v1/v1.x
  (R-UI-008 SHOULD); **(c)** M7 text rendering is **full shaping-grade** (HarfBuzz-class shaping
  — NOT an LTR-only floor); **(d)** world-space RTT panels include **curved surfaces** (mesh
  UV-mapped raycast→UV→events) in M7 — the v2 XR deferral retains only the OpenXR
  compositor-layer/stereo/XR-input leg.
- **Exit:** in-game HUD + a world-space (render-to-texture) panel (the VR/XR panel leg is v2);
  UI driven/asserted headless via CLI.
- **De-risks:** R-UI-001..006 (R-UI-004 and R-UI-003's XR-grade parts are v2).
- **Implementation (2026-07-15): ✅ COMPLETE — all 12 single-lane tasks a1–a12 landed** —
  `<software_root>/.claude/plans/designs/2026-07-13-m7-runtime-ui/` (spec + status board).
  **Landed: a1** (`context_ui` foundation, PR #224 → `c4f142c`), **a2** (headless layout +
  hit-testing + focus order, PR #226 → `0cc1626`), **a6** (engine-integrated GPU screen-space
  backend, PR #230 → `126aac7`), **a4** (TS authoring surface, PR #228 → `e897efc2`), **a3**
  (input routing, PR #234 → `2574c36d`), **a9** (world-space RTT panel flat + dynamic-texture
  registry, PR #232 → `cbe5570`), **a5** (CLI drive/assert verbs + ui.* error domain, PR #236 →
  `7e1902a`), **a7** (FreeType font substrate + glyph atlas + embedded OFL fonts, PR #238 →
  `a932376`), **a8** (shaping-grade text — HarfBuzz + SheenBidi bidi + libunibreak, PR #240 →
  `65186e3f`), **a10** (curved world-space UI — ray-vs-triangle + UV mapping, PR #242 →
  `3f240eb8`), **a11** (published capability matrix + provider conformance suite, PR #244 →
  `c2d5438`), **a12** (M7 exit gate — HUD in platformer-2d + panel in roll-3d + five blocking
  `m7-exit-*` CI gates, PR #246 → `743f82e`). The a7/a8 text-dep gate was ✅ APPROVED (owner
  delegated to a Fable decision agent → GO; conditions in the design's `DECISION-a7a8-deps.md`).
  **M7 EXIT MET:** the five `m7-exit-*` gates (hud-headless · cli-drive · worldpanel ·
  determinism-presentation · seam-checklist) are live + green on all 3 CI legs, encoding owner
  rulings (c) shaping-grade text and (d) curved panels into the permanent exit bar.

### M8 — Build pipeline for all platforms
> **Implementation (2026-07-17): 🏁 ✅ COMPLETE — all 14 single-lane tasks a01–a14 landed** (§9 board).
> Pack format v1 (writer #248 / loader #250 / transcode #252) · shipped wgpu-native backend pinned
> (#254) · build-core CLI (#258) · export adapters Linux (#260) / Windows+Authenticode (#270) /
> macOS+notarize (#279) / Web-emdawnwebgpu (#274) · headless-smoke + packed L-54 determinism gate
> (#262) · trust-root pin + fetch-verify + release signing (#266) · `context doctor` (#268) ·
> build-time budgets advisory-until-ops1 (#277) · 6 blocking `m8-exit-*` gates (#282). CI enforces
> the milestone bar; OS-signing trilogy live (Ed25519 + Authenticode + Developer-ID/notarize).
> Trailing Android SHOULD + iOS (v2) unchanged. **M8.5 (wedge hardening) is NEXT.**

The v1 adapter set is **Windows + macOS + Linux desktop, Linux server/headless, and Web** (MUST);
the **Android leg is trailing SHOULD** — it ships when the wedge is served and never blocks this
milestone; the **iOS leg is v2's first deliverable** (R-BUILD-001) [owner-ruled 2026-07-02].
Within the v1 set the adapters land **Linux + Windows + Web first, macOS desktop trailing** (it
needs the R-BUILD-007 macOS agent + signing/notarization pipeline anyway); Android per its
trailing-SHOULD ruling, when that leg lands.
- Export/toolchain adapters for the v1 set (+ the trailing Android leg; the iOS adapter lands in
  v2); CLI-driven headless builds **per agent** — Apple targets run on a **macOS build agent with
  Xcode** (R-BUILD-007); per-platform asset variants. On-demand toolchain/export-template fetches
  are **signed + verified against the trust root** (R-SEC-009 / R-BUILD-004, MUST for
  engine-fetched components per R-BUILD-008). Web-build user docs carry the mid-2026
  **Linux-browser WebGPU caveat** (Firefox-Linux/Chrome-Linux legs still rolling out;
  Windows/macOS/iOS/Android evergreen browsers all shipped) — the post-v1 WebGL2 escape hatch
  stays the documented fallback (L-56).
- **Code signing + minimal packaging are MUST** (the R-BUILD-005 split): **macOS-desktop
  signing/notarization** in v1 (Developer ID cert + hardened-runtime signing + headless
  `notarytool` with an App-Store-Connect API key + stapling — macOS 15+ Gatekeeper removed the
  control-click bypass, so skipping this is an effective distribution blocker); **Windows
  Authenticode signing** in v1 (a signing hook in the Windows export adapter —
  signtool-compatible; **Azure Artifact Signing** [the GA rename of Azure Trusted Signing] or a
  developer-supplied cert; a `context doctor` signing-prereq check; honest SmartScreen note — a
  signed new publisher still builds reputation over time); APK/AAB signing **plus Google
  developer-verification registration** when the trailing Android leg lands (identity +
  package/key registration — global on certified devices from 2027, covering sideloads —
  R-BUILD-001); iOS provisioning + entitlements move to v2 with iOS;
  store-submission/age-rating hooks stay COULD.
  **`context doctor`** validates each agent's toolchain (presence/versions, fetchable vs
  preinstalled — R-BUILD-008) and every build failure lands in the R-CLI-008 error catalog
  (`build.*` codes).
- **Committed build-time budgets** (cold / incremental / clean-CI) enforced by the **CI
  build-time benchmark** (R-BUILD-006), with the per-platform transcode and **the LTO/DCE final
  links (per-build, cache-exempt)** budgeted separately from the from-source C++ compile. The
  **WASM-AOT and JS-VM bytecode-precompile budget lines are v2 with iOS** (R-BUILD-006);
  "per-platform transcode" reads as the **v1 platform set**, with the Android/ASTC legs
  activating when trailing-SHOULD Android lands.
- Pack format is the **chunked** R-ASSET-005 format (GUID-addressed content units, on-demand
  loading, patch/DLC-ready). *As-built: format **v0 (draft, pre-freeze)** was co-designed at M2 —
  engine `docs/chunk-pack-format.md` froze the boundary rule, GUID addressing (the L-37 composed
  identity), and the directory field set, with the flatten→content-unit proof landed. M8 freezes
  **format v1** and owns that doc's explicit deferred list: on-disk chunk byte encoding, payload
  codec, the async streaming scheduler (R-ASSET-003), per-platform variant selection, nested
  sub-unit granularity, and the sourceScene path→GUID widening.*
- **Exit:** one Project → the full v1 platform-set builds (the original "six platform builds" bar
  re-read per the platform rulings: Android when the trailing leg lands; iOS in v2), **each
  produced headless from the CLI on an agent satisfying that target's toolchain manifest,
  orchestrated across the build-agent pool (≥ 1 macOS host for the Apple targets)**; **desktop
  artifacts are signed** (macOS signed + notarized, headless; Windows Authenticode-signed) with
  verify-before-use failing closed under the minted production key (R-SEC-009); mobile
  artifacts are signed/installable (R-BUILD-005 MUST half — Android when the trailing leg lands;
  iOS in v2); the **headless smoke-run is green on every headless-capable target** (packed
  artifact boots + steps N ticks on the shipped RuntimeKernel — R-BUILD-009); the **wedge-platform
  builds additionally pass the L-54 determinism gate against the SHIPPED RuntimeKernel** and carry
  the validated **L-48/R-NET-001 replication metadata** (the M6 harness re-run against packed
  builds); the wedge-target smoke-run is a **blocking per-PR gate** per the §6 CI tiering.
- **De-risks:** R-BUILD-* (explicitly including the two heaviest non-fetchable toolchain legs:
  **Android SDK/Gradle/JDK provisioning** — trailing with the Android leg — and **Xcode
  signing/provisioning on the macOS agent** — the iOS provisioning half is v2; macOS-desktop
  signing stays), R-ASSET-005. Named de-risk items added 2026-07-15: **pin the minted production
  signing key** (MINTED 2026-07-15 — §7) into `tools/trust-root/allowed_signers` and **wire the
  R-BUILD-004/R-VER-004 fetch paths through it**; **provision the perf-isolated
  runner class** (else the R-BUILD-006 build-time budgets ship advisory-only — R-QA-012);
  **decide the shipped native WebGPU backend** (wgpu-native vs Dawn — the deferred M4
  re-evaluation; Dawn now has an official vcpkg port) — ✅ **RESOLVED a04 2026-07-16: RETAIN
  wgpu-native v29.0.1.1** (CE PR #254); **freeze pack-format v1** from the M2 v0
  deferred list; **Windows signing-cert procurement** (lead-time item).

### M8.5 — Wedge hardening — ✅ COMPLETE (2026-07-18, v1 wedge scope-complete; ops1 deferred/advisory)
v1 absorbs only the hardening the wedge needs; the marketplace-grade remainder is explicit v2
(§3).
- **Trust-tier basics** (the L-49/R-SEC-001 v1 shape): the sandboxed tiers enforce as specified —
  WASM import-gating, the TS constrained ABI, no-ambient-network (R-SEC-002/010);
  **adversarial validation/red-teaming of the M1-shipped dispatcher scope enforcement +
  launch-time operator-provisioned scoped tokens** (R-SEC-007/R-SEC-011(a) — shipped at M1,
  hardened here, not rebuilt); **first-party release signing verified fail-closed** (R-SEC-009) —
  all under the explicit v1 statement of **no third-party native packages** (R-SEC-001).
- **L-50 concurrency validation end-to-end:** the multi-client file-authority model (write queue,
  watcher convergence, field-path conflicts, CAS, lost-update events) exercised by a human+agent
  co-editing scenario plus a multi-worktree merge scenario (R-FILE-012 driver + conflict
  envelope), driven on the R-QA-010 fault-injection seams.
- **Profiling HUD + Tracy/RenderDoc export + `context profile --json`** (L-47) — the wedge's
  performance-visibility floor.
- **Coordinated vulnerability disclosure + EU CRA readiness (advisory, added 2026-07-15):**
  publish a CVD/security policy and stand up EU Cyber Resilience Act reporting readiness
  (reporting obligations from 2026-09-11; main obligations 2027-12-11) — applicability analysis
  routed to business/legal counsel (§7); pairs with the R-SEC-009/O-7 posture already in place.
- **Trailing-v1 GUI bucket:** the post-M5 v1 GUI deliverables land in this milestone's window:
  the **tile-painting GUI + 2D viewport-authoring mode** ([owner-ruled 2026-07-02] — R-2D-003),
  **in-context scene-instance/override viewport editing + the viewport quality-bar affordances**
  (R-HUX-006), and **in-editor contextual help** (R-HUX-010). Sequenced inside v1 after the
  observer-grade M5 editor — a sequencing home, not a scope cut.
- **Team-scale honesty:** the exit below validates a **2-client co-edit** (one human + one
  operator-scoped agent). **60-client integration throughput** — merge-queue depth,
  semantic-conflict rate, per-box daemon RAM at N clients — is **acknowledged v1.x work**, not a
  v1 claim; semantic-conflict mitigation is the **R-QA-005 sim-level test pass inside the merge
  convergence gate** (R-FILE-012).
- **Exit:** an operator-scoped agent and a human co-edit and merge across worktrees with no lost
  updates; a sandboxed package cannot exceed its granted capabilities; signed-artifact
  verification fails closed on a tampered artifact; profiling data is JSON-queryable on a live
  session. The **trailing-GUI surface is exercised**: paint a tilemap in the 2D
  viewport-authoring mode, make an in-context instance/override edit in the viewport (the
  R-HUX-006 MUST core), and surface contextual help (R-HUX-010). The **R-FILE-011
  ticks/sec/instance + instances-per-box orchestration-density targets are committed and
  benchmarked** (re-homed from the Context Sim anchor — §2).
- **De-risks:** R-SEC-001/002/007/009/010/011(a), R-COLLAB-*, L-50, L-47, R-2D-003 (GUI half),
  R-HUX-006, R-HUX-010, + the R-FILE-011 orchestration-density commitment.

### Unscheduled v1 SHOULDs — the v1.x ledger (added 2026-07-15)

Explicit homes for v1-lettered items with no milestone bullet above, so nothing slips silently:

- **R-PKG-006 package conformance kit** (`SHOULD` v1): dogfooded opportunistically on first-party
  packages within M8/M8.5; becomes the marketplace gate of record in v2 (§3).
- **R-QA-006 maintained-samples half** (≥ 2 samples at 1.0): **as-built** —
  `samples/platformer-2d` + `samples/roll-3d` are the two maintained samples, gated by the
  blocking samples-corpus CI step; M8 adds them to the packed-build smoke (R-BUILD-009).
- **v1.x (post-M8.5 minor stream, pre-v2):** **⚠️ the interactive Editor WINDOW shell — NOT built as of
  M8.5** (verified 2026-07-18; see the closing progress-log note): the GUI is **headless-first /
  observer-grade** — the CEF substrate, the OSR compositor, and every panel (inspector / scene-tree /
  problems / tile-paint / viewport-override / contextual-help) are built + CI-verified through the headless
  UI-logic tree, but there is **no native OS window** that presents the composited output on screen or
  routes live mouse/keyboard input (`editor_host` boots CEF *windowless / off-screen* at 1 fps purely for
  the `editor-cef-smoke` CI boot; no `SetAsChild`/`SetAsPopup`, no window/message-loop/input code anywhere in
  `src/editor/gui/`). Getting a clickable window = build the windowing shell (native window + swapchain
  present of the accelerated-OSR shared texture + input routing into the panel UI-logic tree) — a bounded,
  undecomposed task, and the **lead v1.x deliverable** — **now DESIGNED as M9:
  [`../m9-editor/`](../m9-editor/README.md)** (design v1.1, adversarially reviewed 2026-07-18;
  full VS Code-grade windowing/docking/tear-out, N viewports, package panels, theme system,
  signed standalone app; awaiting `/design-tasks` + dispatch). — Also: R-HUX-007 GUI asset browser; the
  CEF-runtime / minimal UI backends (R-UI-008, per the M7 ruling); 60-client integration throughput (the
  team-scale honesty item above).
- **Opportunistic / explicitly unscheduled in v1:** R-SIM-004 (opt-in low-level memory control),
  R-A11Y-002 (runtime accessibility package, post-core), R-NET-002 (COULD).

---

## 2. The "Context Sim" pre-release track

**As designed (2026-07-01):** a pre-release track forked after M3 — the **M6 determinism/physics
slice + the R-QA-005 headless session surface** shipped as **"Context Sim"** (~month 12–15), a
headless, deterministic, parallel-orchestratable simulation product for **RL / server-sim users**
(beachhead pillars 1–2 — ARCHITECTURE.md §1.1), while M4 (render) and M5 (observer editor)
proceeded in parallel.

**As-built (recorded 2026-07-15): the track did NOT run.** Execution was single-lane M0→M7 under
the 2026-07-05 owner directive (tasks 1-at-a-time, no parallel milestones); the
determinism/physics slice + session surface landed in normal sequence at M6, and **no pre-release
shipped** (zero releases/tags in the engine repo as of 2026-07-15). Two things survive the
non-run:

1. **The product idea** — the headless deterministic sim artifact for pillars 1–2 — is now
   shippable from the **M8 build pipeline** rather than a mid-roadmap fork. **Owner-ruled
   2026-07-15: no pre-release for now** — development/testing continue locally and the owner
   calls the release moment (§7).
2. **The orchestration-density commitment** — R-FILE-011's ticks/sec/instance +
   instances-per-box targets were anchored to the pre-release. Re-anchored 2026-07-15: they are
   **committed no later than the M8.5 exit** (earlier, at the Context Sim launch, if the owner
   ships one). R-FILE-011, ARCHITECTURE §1.1, and the README acknowledged-gaps row carry the
   same re-anchor.

---

## 3. v2 (post-wedge)

**v1 = M0–M8.5** on the platform set **Windows + macOS + Linux desktop, Linux server/headless,
and Web (MUST)**, with **2D + baseline-3D PBR**. **Android is a trailing SHOULD** — v1-capable,
ships when the wedge is served, never blocks wedge milestones [owner-ruled 2026-07-02]. **iOS
moves to v2 as its FIRST deliverable** [owner-ruled 2026-07-02]. Deferred to explicit v2 —
post-wedge, once the beachhead (ARCHITECTURE.md §1.1) is served:

- **Advanced graphics** [owner-ruled 2026-07-01] — T2 native RHI paths, RTX-class ray tracing,
  FSR/DLSS/XeSS upscaling, GI, virtualized geometry (R-GFX-001…004). Bar: a showcase scene with
  advanced features on capable hardware, gracefully degrading elsewhere; zero footprint when
  unused.
- **XR / world-space-UI stack** [owner-ruled 2026-07-01] — OpenXR compositor layers, raycast
  (controller/gaze/hand) input, stereo rendering (R-UI-004 + the XR-grade parts of R-UI-003;
  L-16). **Screen-space UI and basic render-to-texture world-space UI stay in v1** (R-UI-003's
  non-XR core, M7).
- **v2 (iOS)** — v2's first deliverable [owner-ruled 2026-07-02]: the platform leg plus
  everything it drags — the WASM→native-AOT toolchain spike + acceptance bar (R-LANG-005; the
  L-62 AOT legs are the measured menu), the **second embedded JS-VM backend** (the
  constrained-target pick deferred by L-61 — re-run the JS-engine spike battery incl. Hermes/JSC;
  the multi-backend seam ships in v1, the backend returns here), iOS provisioning/entitlements
  (the R-BUILD-005 MUST half), the
  JS bytecode precompile (R-BUILD-006), the interpreter-mode CI budget (R-LANG-011/012), and the
  iOS min-spec/streaming floors (R-QA-007, R-ASSET-003/005). The cheap-later invariants shipped
  in v1 (WASM module format, chunked pack, multi-backend VM seam, platform seam) are the landing
  pads.
- **Marketplace-grade trust & security** — the third-party NATIVE package tier + its consent UX
  (R-SEC-001), the TUF/Sigstore trust-root upgrade behind the same verify-before-use gate
  (R-SEC-009), cross-trust-domain cache machinery (R-FILE-010), third-party
  determinism-attestation verification (R-SIM-005), runtime token minting/elevation flows
  (R-SEC-007) + the async park-and-resume consent protocol at full strength (R-SEC-011(b)), the
  per-OS importer lockdown beyond Linux (R-SEC-006), hostile-extension enforcement + red-teaming
  (R-EDIT-001), the remote-exposure door + mutual auth (R-BRIDGE-007); the package conformance
  kit becomes the marketplace gate of record (R-PKG-006).
- **Human-onboarding editor layer** — the git-abstraction history timeline (R-HUX-002:
  checkpoint timeline / named restore points / background auto-commit) and the GUI launcher /
  project manager (R-HUX-003: create-from-template + engine-version pick);
  cross-engine-version project-migration hardening (exercised when a second engine version
  exists; R-VER-002's mechanism unchanged — the kernel API stays source-level semver per L-44 in
  v1, the frozen C ABI is post-1.0); crash reporting + the update channel (R-OBS-006,
  off-the-shelf GlitchTip-class backend); remote/on-device profiling (R-OBS-003, with the mobile
  push).
- **Contract completions** — capability-negotiation behavior at the second released protocol
  version (R-CLI-010), the idempotency replay-store (R-CLI-016), cross-machine large-result
  fetch (R-CLI-017), the package-namespace collision checker (R-CLI-007), `--atomic-plan`
  (R-CLI-011), live package-topic introspection (R-CLI-014), and the engine-version
  resolver/fetcher/launcher (R-VER-004 — with the second release).

The beachhead is the priority lens (ARCHITECTURE.md §1.1): these features serve the "general 3D
engine" destination, not the day-one wedge — deferring them is a conscious sequencing decision,
not a capability retraction (the package seams — R-GFX-005, L-14/L-16 — keep both doors open).

> **Deferred (post-v1):** MetaHuman-class digital humans (R-GFX-006), console platforms,
> marketplace hosting, live-ops telemetry.

---

## 4. Effort estimate & economics [owner-open]

> **Status: OPEN — owner decision required on funding/headcount.** This section records the
> estimate honestly; it deliberately does **not** resolve it.

- **Effort to v1 (M0–M8.5): ~20–30 engineer-years.** The single most underestimated slice is
  **M1**: the file-sync layer + derivation graph + public-contract cluster + fault-injection
  seams is realistically **18–24 months of work on its own** — and it is also the product moat
  (§5 risk table), so underfunding it forfeits the thesis rather than trimming it. The
  **"Context Sim" pre-release track (§2)** existed partly to make this spend survivable —
  as-built the track did not run (§2); the first shippable artifact instead arrives with the M8
  build pipeline, and the market-testing role transfers to the [owner-open] Context Sim launch
  decision (§7).
- **The wedge generates approximately NO royalty revenue.** The L-57 royalty triggers on shipped
  **game** revenue above $200k/year — but the beachhead pillars (RL/training environments,
  server-authoritative sims) are **not royalty events** in any near term. **v1-era revenue is
  therefore the ai-game.dev subscription** (AI usage), not engine royalties; the royalty is a
  long-tail v2+ instrument that matures with pillar 3 (shipped AI-generated games). The two
  streams are **fully decoupled** [owner-ruled 2026-07-03]: the royalty is unconditional (the
  formerly-recorded subscription waiver is REMOVED — no subscription affects the EULA) and its
  base is gross receipts (no storefront/platform-fee netting).
- **Funding / headcount: OPEN owner decision.** Team size, runway, and funding source are
  deliberately **not** decided here; the estimate above is the input to that owner decision
  (tracked in §7 "Open").

> *As-built note (2026-07-15): M0–M7 landed 2026-07-02 → 2026-07-15 via single-lane AI-agent
> execution — the wall-clock framing above predates that cadence; the engineer-year figures stand
> as the original human-team estimate this section records.*

---

## 5. Risks & mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| **C++ has no Cargo** — ABI hell / build times (the direct cost of choosing C++) | High | vcpkg manifest-mode from-source + one pinned Clang toolchain + shared artifact cache (L-42); source-level semver in v1 — the frozen C-ABI seam is deliberately post-1.0 (L-44); EditorKernel hides the mess behind one "add package" UX. **Committed build-time budgets (cold/incremental/clean-CI) enforced by a CI build-time benchmark (R-BUILD-006)**; hermetic CI seeds a trusted read-only warm remote cache so cold-CI is not the default; per-platform transcode budgeted separately (per-build, not cached-once) over the v1 platform set — the WASM-AOT budget line is v2 with iOS (R-BUILD-006) |
| **No memory-safety guarantee** in C++ concurrent ECS | High | Declared read/write access sets; TSan/ASan/UBSan-gated CI; data-oriented design that simplifies ownership |
| **Trust boundary is the OS user — same-user untrusted code** (agents, npm/vcpkg packages, assets, editor extensions all run *as the user*; per-user ACL / single-token "isolation" does not defend against it) | High | Honest tiering: WASM is the genuinely-sandboxed tier; v1 TS is ONE shared trust domain behind a constrained host ABI (no ambient fs/net/process — R-SEC-001/002); least-privilege daemon + scrubbed child-process env + no-ambient-network TS/WASM + advisory secret scanning (R-SEC-010); per-OS importer-subprocess sandbox (seccomp/AppContainer/Job Object — R-SEC-006); dispatcher-level scoped tokens with consent-gated elevation (R-SEC-007); TOCTOU-safe path jail (R-SEC-008); CEF editor-extension sandbox (R-EDIT-001) |
| **Unsigned supply chain** — "signed/trusted/attested/verified" were undefined; forgeable flags + unauthenticated fetches; a backdoored toolchain/export template compromises every build | High | Pinned cryptographic trust root + per-artifact detached signatures + mandatory verify-before-use, fail closed (R-SEC-009 / L-58) for engine binaries, pinned toolchain, export templates, native package sources, and cross-trust-domain cache entries; signed+verified+cert-pinned engine/toolchain/template/update fetches (R-VER-004, R-BUILD-004, R-OBS-006); code artifacts trusted from cache only if signed or same-domain, remote warm cache signed+verified (R-FILE-010 / R-BUILD-006); from-source vcpkg builds SHA-pinned + consent-gated + isolated (R-SEC-005 / L-42); determinism attestations produced/verified not self-declared (R-SIM-005 / L-54); deny-by-default license allowlist + SBOM (O-7); CI error-catalog additive-only + CLI≡RPC≡MCP conformance (R-CLI-008/009) |
| **Data-model retrofit cost** (prefabs/overrides, stable IDs) | High | Build M2 early and deliberately; text serialization + GUID identity from day one |
| **Determinism vs TS gameplay** contradiction | Medium | Determinism restricted to native/WASM tier; JS explicitly non-deterministic; state-sync netcode as default |
| **Determinism drift across platforms / modules** (transcendentals, third-party WASM, FMA/contraction, platform libm) — a silent divergence breaks lockstep/replay | Medium | Determinism is a **whole-build property** (R-SIM-005 / L-54): strict-FP engine-wide on the sim path, FMA/contraction pinned, one shipped deterministic transcendental math lib (no platform libm), and **every sim-tier module MUST carry a `deterministic:true` build attestation or the deterministic build fails**; the CI state-hash gate exercises transcendentals + a representative third-party WASM system + physics; a per-project conformance harness hashes the game's own systems |
| **Runtime perf/scale gaps** — O(N) culling/queries, JS GC stalls, TS single-core throughput | Medium | Shared broad-phase spatial index feeds culling + queries + streaming, O(result + log N) (R-SIM-007); JS-tier GC discipline — pooled/no-alloc APIs + inter-tick GC window + GC-pause profiler channel (R-SIM-008); CI-enforced TS frame budget on the reference min-spec (R-LANG-012) with the min-spec floor bench (R-QA-007); derivation-side backpressure under sustained write load (R-FILE-013) |
| **Web-UI compatibility limits** (native subset renderer) | Medium | Pluggable backends; CEF option for full fidelity (desktop); document per-platform capability matrix |
| **iOS/console JIT & browser-engine bans** | Medium | AOT WASM on those platforms; interpreted UI JS; per-platform best-available UI backend. With iOS v2-first and consoles WON'T (v1), this is a **v2 risk row**; the v1 mitigation is the kept cheap-later seams — WASM module format, multi-backend VM seam, platform seam (R-BUILD-001/L-40) |
| **Advanced graphics are multi-year each** | Medium | Ship as opt-in packages; lean on vendor/open SDKs (FSR/DLSS/XeSS, RTXGI/DDGI); tiered fallback; **v2 — deferred out of v1 entirely** [owner-ruled 2026-07-01] |
| **Native code iteration slowness** | Medium | Keep iteration in the TS tier; fast incremental relink (mold/lld); optional Live++ on Windows |
| **CEF accelerated-OSR compositing fails on a platform** (the L-41 seam) | Low (was Medium — Windows measured, tree ratified) | M0 spike PROVEN on Windows (2.5 µs/frame composite p50; software fallback measured ~4×, safe) [spike-ratified 2026-07-02, owner]; L-41 per-platform tree is the plan of record: Windows accelerated primary + software compiled-in · macOS IOSurface with CEF-internal pacing (never `SendExternalBeginFrame`, cef#4033) · Linux software-OSR shipped default behind a Mesa/X11 capability gate; branch "windowed CEF + viewport hole" rejected, inverse composition an unexercised last resort |
| **SPIR-V→WGSL translation maturity** (Tint/Naga) — the SOLE web shader path post-L-56; a translation gap or miscompile has no fallback renderer behind it | Medium | Tool choice (Tint vs Naga) is an **M4 deliverable** measured on the engine's real shader corpus (R-REND-005; the M0 spike rendered byte-identical images through both — divergence risk low [spike-ratified 2026-07-02, owner]); shader compilation runs as cached derivation-graph nodes so regressions surface in CI, not at ship; the L-56 WebGL2 re-introduction stays the documented post-v1 escape hatch |
| **Dawn/wgpu-native acquisition & port burden** — premise updated 2026-07-15: **Dawn now HAS an official vcpkg port** (since ~2026-06, incl. a Tint-tools feature — the 2026-07-02 "neither has a port" premise is dead on the Dawn half; microsoft/vcpkg#41847), though it remains heavy from-source (~1–2 GB dependency fetch, tens-of-minutes builds) and publishes **no official native prebuilts**; **wgpu-native still has no vcpkg port** (official SHA-pinned prebuilts only) | Medium | M0 spike measured the asymmetry [spike-ratified 2026-07-02, owner]: **wgpu-native was the M0–M7 backend** (pinned, SHA-verified official prebuilts, 7 s clean setup — not a permanent lock); the **Dawn re-evaluation was not performed at M4 — re-homed to M8** (export templates freeze the shipped backend; the vcpkg port makes Dawn L-42-conformant from-source, removing its need for the prebuilt carve-out); narrow **signed-prebuilt exception** (verified per R-SEC-009) stands for build-hostile heavy libs as a deliberate L-42 carve-out |
| **emdawnwebgpu web-leg build constraints** (measured, M0 WebGPU spike [spike-ratified 2026-07-02, owner]) — `-sALLOW_MEMORY_GROWTH` **breaks WebGPU device acquisition** (TextDecoder on a resizable ArrayBuffer); `-sUSE_WEBGPU` is removed from Emscripten (emdawnwebgpu is the maintained path); blocking polls need Asyncify | Medium | Fixed-memory web builds (size the heap up front) until upstream fixes land; pin the emdawnwebgpu package; the memory-growth constraint is spike-measured (2026-07-02), not upstream-documented — **re-test against the then-current emdawnwebgpu at M8 template time and file/link the upstream issue** so "track the upstream issues" is actionable; JSPI mid-2026: Chrome-stable since 137, Firefox flagged, Safari TP — **Asyncify remains the M8 default** (JSPI not yet broadly available) |
| **Toolchain-matrix maintenance** — the per-target toolchain manifest (5 compiler families: clang, clang-cl+MSVC STL, Apple clang, NDK clang, Emscripten LLVM) + per-triplet vcpkg port patches for non-desktop targets is an ongoing engineering cost, not a one-time setup | Medium | The manifest is explicit and versioned per engine release (R-PKG-002); `context doctor` + the fetchable/preinstalled split (R-BUILD-008) make breakage diagnosable; CI builds every triplet per release so patch rot surfaces immediately; macOS-agent constraint isolated in R-BUILD-007 rather than leaking everywhere |
| **2D-in-v1 scope growth** (L-55 adds a renderer path, a physics package, tilemap tooling) | Medium | 2D rides existing seams — same renderer (R-2D-001), same package contract (R-2D-002), same asset pipeline (R-2D-003); scope fenced to sprites/tilemaps/Box2D-class physics; anything beyond is post-v1 |
| **Scope is very large** (multi-year) | High | Strict milestone gating; each M demonstrable; defer non-core (MetaHuman, consoles, marketplace) |
| **File-sync layer correctness** (dropped watcher events, torn writes, bulk git ops) | High | Watcher-as-hint + content-hash reconciliation; atomic temp+rename IO; property/fuzz tests over the sync layer at requirement level (the R-QA-008 suites over injectable seams + the R-QA-010 deterministic fault-injection harness, both M1 architecture); this layer is also the product moat — invest accordingly |

---

## 6. CI tiering

The CI-enforced gates run in two tiers. Runner classes come from the **R-QA-012 fleet manifest**;
performance gates run under the **R-QA-009 methodology** (named perf-isolated runners, median of
N ≥ 5, rolling baseline + variance band, time-series archive).

| Tier | Gates |
|---|---|
| **Blocking, per-PR** | sanitizers (TSan/ASan/UBSan) · determinism state-hash on the wedge platforms (L-54 / R-QA-005) · importer double-run byte-compare (R-ASSET-001) · CLI ≡ RPC ≡ MCP ≡ introspection parity + error-catalog-additive-only (R-CLI-008/009) · license allowlist / SBOM / CLA (O-7) · headless smoke-run on the wedge targets (R-BUILD-009 — the packed-artifact gate **activates at M8**; the samples-corpus smoke covered M1–M7) · the 10k-file benchmark variant (fast R-FILE-011 proxy — as-built it **remains the per-PR stand-in past M3**: the full 100k benchmark stays nightly, and numeric perf gates are advisory-until-provisioned because the perf-isolated runner class is not yet provisioned — R-QA-012) · fuzz-corpus regression replay (R-SEC-006 — never open-ended fuzz time per PR) |
| **Nightly / trend** | full 100k-file benchmark + multi-worktree contention + dense-reference synthetic (R-FILE-011/010) · build-time budgets (R-BUILD-006) · TS frame budget (R-LANG-012) · min-spec floor (R-QA-007) · backpressure / dirty-set latency under sustained write load (R-FILE-013) · continuous fuzzing (R-SEC-006) · session-query p99 (R-BRIDGE-008) |

Every gate carries a written **red-X policy** — one of **blocking** (merge stops), **advisory**
(reported and tracked, does not stop the merge — including any gate whose R-QA-012 runner class is
not yet provisioned), or **quarantine-with-issue** (known-flaky: auto-quarantined WITH an owned
issue, never silently retried to green). Nightly breaches file issues against the owning
milestone.

---

## 7. Decided vs. still open

- **Decided:** the full lock list (L-1…L-62) with rationale, the business-model terms (L-57), and
  the O-* dispositions live in `DESIGN-DECISIONS.md`; this roadmap does not mirror them. The name
  is settled (**Context Game Engine**, chosen 2026-07-01 — README §Name; the namespace-claim
  follow-up rides the repo-public flip). The M0 spikes are complete and ratified [spike-ratified
  2026-07-02, owner] — see the M0 spike-status entry in §1. Per-round provenance — what changed
  in which review round and why — lives in the `REVIEW-*.md` trackers in this folder.
- **Open:**
  - **EULA counsel review** — the repo went public 2026-07-13 with the **v0.2.1 draft** EULA;
    counsel engagement remains parked at the CEO spend gate (L-57; business-dept task). The
    original "counsel-reviewed LICENSE before the first public push" M0 gate is therefore moot
    as a gate — the counsel review itself stays open.
  - **Funding / headcount** for the ~20–30 engineer-year v1 estimate (§4) [owner-open].
  - **Context Sim product disposition** [owner-ruled 2026-07-15] — **no pre-release for now**:
    development and testing continue locally; **the owner will call the release moment**. The
    R-FILE-011 orchestration-density targets stay anchored to the M8.5 exit (§2); this item
    remains listed only as the owner's pending release trigger.
  - **Production signing key** [MINTED 2026-07-15, owner-directed] — Ed25519 keypair generated
    per engine `docs/signing.md` §Minting and stored in the operator's gitignored `.secrets`
    store (custody: offline operator copy now → GitHub **environment-protected secret**, model
    B, when M8 release automation lands). Remaining M8 work: pin the public half into
    `tools/trust-root/allowed_signers` (reviewed engine commit) + wire the
    R-BUILD-004/R-VER-004 fetch paths through the gate.
  - **EU Cyber Resilience Act applicability** [advisory, added 2026-07-15] — reporting
    obligations start 2026-09-11 (main obligations 2027-12-11); applicability analysis for a
    commercial source-available engine routed to business/legal counsel (pairs with the M8.5
    CVD-readiness item).
  - **§1c material/shader detail** (DESIGN-DECISIONS.md §1c) — the **WGSL tool half RESOLVED at
    M4 = Tint** (measured, 36/36 vs naga 20/36; engine repo `docs/wgsl-tool-decision.md`); the
    node-graph authoring-frontend detail remains open, post-v1 (R-REND-005); §2d closed as L-61.

---

## 8. Immediate next steps (updated 2026-07-15 — M8 era)

1. **M8 — build pipeline** (single-lane, per the owner's dispatch directive): adapters land
   **Linux + Windows + Web first, macOS desktop trailing** (needs the R-BUILD-007 macOS agent +
   signing/notarization pipeline); the Windows Authenticode signing hook + cert procurement
   (lead-time item); freeze **pack-format v1** from the M2 v0 deferred list (engine
   `docs/chunk-pack-format.md`); wire **R-BUILD-004/R-VER-004 fetch-verify** through the pinned
   root; ship `context doctor` (R-BUILD-008) + the `build.*` error-catalog codes (R-CLI-008);
   activate the **R-BUILD-009 packed-artifact wedge smoke** as the blocking per-PR gate (§6).
2. **Owner items (§7):** signing key MINTED 2026-07-15 (custody → environment-protected secret
   at M8); Context Sim ruled 2026-07-15 (no pre-release until the owner calls it); counsel EULA
   engagement (CEO spend gate); funding/headcount.
3. **M8 runner provisioning (R-QA-012):** the macOS build agent and the perf-isolated Linux box
   (else the R-BUILD-006 build-time budgets remain advisory-only) are named de-risk items —
   provision alongside M8, not after it.
4. **M8.5 — wedge hardening** thereafter: the L-50 co-edit/merge validation, the trailing-GUI
   exit clause, the CVD/CRA readiness item, and the re-anchored R-FILE-011
   orchestration-density commitment (§2).

---

## 9. Execution — M8/M8.5 task waves + status board (decomposed 2026-07-15)

Static task specs live in [`../m8-build-pipeline/`](../m8-build-pipeline/README.md) +
[`../m85-wedge-hardening/`](../m85-wedge-hardening/README.md) — **immutable**; this board is the ONLY
place task state exists.

**Status rules.** **(1)** This board is the single home of task state — no `status` fields in
task files, no copies in other docs; the software plan store holds ONE thin pointer task for
the whole design (`.claude/plans/tasks/2026-07-15-context-engine-m8-m85.md`). **(2)** Single
writer: only the orchestrating TD flips rows + appends the progress log, after ground-truth
verification (merged PR, green CI) — implementers report, they don't edit. **(3)** The ready
set is computed from `needs` + ✅, never stored.

**Dispatch discipline (owner-ruled):** group `a` is ONE merge-conflict domain → strictly
sequential in listed order, dispatched **single-lane** via `implement-task`
(`target=context-engine`), **no per-step model overrides** [owner-ruled 2026-07-05/2026-07-10];
`model_hint` is advisory. `ops1` runs in parallel (operator/human-assisted).

**Human-approval gates:** a08 (owner loads the private signing key into the protected GitHub
environment + approves the trust-root pin) · a10 (Windows signing-cert procurement — money) ·
a13 (owner approves reusing the Apple creds from `.secrets/apple-*`) · ops1 (perf-runner
hardware/hosting spend).

### Waves

- **Wave 1 — M8 (build pipeline):** `a01 → a14` single-lane ∥ `ops1` (parallel, operator).
- **Wave 2 — M8.5 (wedge hardening):** `a15 → a23` single-lane (starts after a14; a15–a17 have
  no hard M8 dependency but stay in-lane per the conflict-domain rule).

### Status board

| Task (spec) | needs | repo/base | imp/cx | model | Status | Run / PR | Updated |
|---|---|---|---|---|---|---|---|
| [a01-pack-format-v1-writer](../m8-build-pipeline/a01-pack-format-v1-writer.md) | — | . / main | 9/8 | top | ✅ done | [CE #248](https://github.com/IvanMurzak/Context-Engine/pull/248) · sw #337 | 2026-07-15 |
| [a02-runtime-chunked-loader](../m8-build-pipeline/a02-runtime-chunked-loader.md) | a01 | . / main | 9/8 | top | ✅ done | [CE #250](https://github.com/IvanMurzak/Context-Engine/pull/250) · sw #339 | 2026-07-15 |
| [a03-platform-variants-transcode](../m8-build-pipeline/a03-platform-variants-transcode.md) | a01 | . / main | 7/6 | mid | ✅ done | [CE #252](https://github.com/IvanMurzak/Context-Engine/pull/252) · sw #341 | 2026-07-16 |
| [a04-shipped-backend-decision](../m8-build-pipeline/a04-shipped-backend-decision.md) | — | . / main | 7/5 | mid | ✅ done | [CE #254](https://github.com/IvanMurzak/Context-Engine/pull/254) · sw #343 | 2026-07-16 |
| [a05-build-core-cli](../m8-build-pipeline/a05-build-core-cli.md) | a01,a03,a04 | . / main | 9/8 | top | ✅ done | [CE #258](https://github.com/IvanMurzak/Context-Engine/pull/258) · sw #349 | 2026-07-16 |
| [a06-linux-adapters](../m8-build-pipeline/a06-linux-adapters.md) | a05 | . / main | 8/6 | mid | ✅ done | [CE #260](https://github.com/IvanMurzak/Context-Engine/pull/260) · sw #352 | 2026-07-16 |
| [a07-smoke-run-packed-determinism](../m8-build-pipeline/a07-smoke-run-packed-determinism.md) | a06 | . / main | 10/7 | mid | ✅ done | [CE #262](https://github.com/IvanMurzak/Context-Engine/pull/262) · sw #358 | 2026-07-17 |
| [a08-trust-root-pin-fetch-verify](../m8-build-pipeline/a08-trust-root-pin-fetch-verify.md) | a05 · ✅ key→`release`-env + ✅ owner GO (2026-07-17) | . / main | 10/7 | top⬆ | ✅ done | [CE #266](https://github.com/IvanMurzak/Context-Engine/pull/266) · sw #362 | 2026-07-17 |
| [a09-context-doctor](../m8-build-pipeline/a09-context-doctor.md) | a05,a08 | . / main | 7/6 | mid | ✅ done | [CE #268](https://github.com/IvanMurzak/Context-Engine/pull/268) · sw #363 | 2026-07-17 |
| [a10-windows-adapter-authenticode](../m8-build-pipeline/a10-windows-adapter-authenticode.md) | a05 · ✅ owner GO + `AZURE_*`→`release`-env | . / main | 8/6 | top⬆ | ✅ done | [CE #270](https://github.com/IvanMurzak/Context-Engine/pull/270) · sw #364 | 2026-07-17 |
| [a11-web-export-adapter](../m8-build-pipeline/a11-web-export-adapter.md) | a02,a05 | . / main | 8/8 | top | ✅ done | [CE #274](https://github.com/IvanMurzak/Context-Engine/pull/274) · sw #371 | 2026-07-17 |
| [a12-build-time-budgets](../m8-build-pipeline/a12-build-time-budgets.md) | a06 · ops1 (blocking-flip only) | . / main | 7/5 | mid | ✅ done | [CE #277](https://github.com/IvanMurzak/Context-Engine/pull/277) · sw #378 | 2026-07-17 |
| [a13-macos-adapter-notarization](../m8-build-pipeline/a13-macos-adapter-notarization.md) | a05 · ✅ owner GO + 6 `apple-*`→`release`-env | . / main | 7/7 | top⬆ | ✅ done | [CE #279](https://github.com/IvanMurzak/Context-Engine/pull/279) · sw #388 | 2026-07-17 |
| [a14-m8-exit-gate](../m8-build-pipeline/a14-m8-exit-gate.md) | a06–a13 | . / main | 10/6 | mid | ✅ done | [CE #282](https://github.com/IvanMurzak/Context-Engine/pull/282) · sw #392 | 2026-07-17 |
| [a15-profiling-surface](../m85-wedge-hardening/a15-profiling-surface.md) | — (in-lane after a14) | . / main | 7/6 | mid | ✅ done | [CE #288](https://github.com/IvanMurzak/Context-Engine/pull/288) · sw #396 | 2026-07-18 |
| [a16-l50-concurrency-validation](../m85-wedge-hardening/a16-l50-concurrency-validation.md) | — (in-lane) | . / main | 8/7 | mid | ✅ done | [CE #285](https://github.com/IvanMurzak/Context-Engine/pull/285) · sw #394 | 2026-07-18 |
| [a17-trust-tier-red-team](../m85-wedge-hardening/a17-trust-tier-red-team.md) | a08 ✅ | . / main | 9/6 | top⬆ | ✅ done | [CE #286](https://github.com/IvanMurzak/Context-Engine/pull/286) · #283 · sw #395 | 2026-07-18 |
| [a18-tilemap-painting-gui](../m85-wedge-hardening/a18-tilemap-painting-gui.md) | — (in-lane) | . / main | 7/7 | mid | ✅ done | [CE #294](https://github.com/IvanMurzak/Context-Engine/pull/294) · sw #399 (a21+a18 bundled) | 2026-07-18 |
| [a19-viewport-override-editing](../m85-wedge-hardening/a19-viewport-override-editing.md) | — (in-lane) | . / main | 8/7 | mid | ✅ done | [CE #298](https://github.com/IvanMurzak/Context-Engine/pull/298) · sw #401 | 2026-07-18 |
| [a20-contextual-help](../m85-wedge-hardening/a20-contextual-help.md) | — (in-lane) | . / main | 5/4 | fast | ✅ done | [CE #300](https://github.com/IvanMurzak/Context-Engine/pull/300) · sw #403 | 2026-07-18 |
| [a21-orchestration-density-targets](../m85-wedge-hardening/a21-orchestration-density-targets.md) | a14 · ops1 DEFERRED→advisory | . / main | 8/5 | mid | ✅ done | [CE #292](https://github.com/IvanMurzak/Context-Engine/pull/292) (TD-merged; sw bump bundled w/ a18) | 2026-07-18 |
| [a22-cvd-cra-readiness](../m85-wedge-hardening/a22-cvd-cra-readiness.md) | business reply `20260715-165440-5cd64a` (soft) | . / main | 6/3 | fast | ✅ done | [CE #290](https://github.com/IvanMurzak/Context-Engine/pull/290) · sw #398 · pvr✓ · CRA draft | 2026-07-18 |
| [a23-m85-exit-gate](../m85-wedge-hardening/a23-m85-exit-gate.md) | a15–a22 ✅ | . / main | 10/6 | mid | ✅ done | [CE #302](https://github.com/IvanMurzak/Context-Engine/pull/302) `4b7456f` (issue #301 closed; 9 blocking `m85-exit-*` gates, 3-OS CI green) · [sw #406](https://github.com/IvanMurzak/ai-game-dev-software/pull/406) — FINAL M8.5 task | 2026-07-18 |
| [ops1-perf-runner-provisioning](../m85-wedge-hardening/ops1-perf-runner-provisioning.md) | 🚧 owner: hardware spend | . / main | 6/3 | fast | ⬜ pending | | |

### Progress log

- **2026-07-15** — M8/M8.5 decomposed (24 tasks: 14 M8 + 9 M8.5 + 1 ops) after the R6 design
  review; board initialized, all pending. Execution starts on explicit owner GO.
- **2026-07-15** — Owner GO to execute M8. Decisions: (1) **strict single-lane** a01→a14
  (confirms the LOCKED conflict-domain ruling over the "up to 4 parallel" ask — ground-truth
  shows the only disjoint pair is a01∥a04, everything after a05 shares the CLI registry / error
  catalog / ci.yml). (2) **Clear gates now** — owner loads the minted Ed25519 key into a GitHub
  environment-protected secret + approves the trust-root pin (a08), approves reusing
  `.secrets/apple-*` in the engine repo's protected env (a13), and authorizes the Windows
  signing-cert spend (a10); the full M8 set incl. signing/notarization runs.
- **2026-07-15** — a01 dispatched (single-lane, `implement-task` target=context-engine).
- **2026-07-15** — ✅ **a01 landed.** CE PR [#248](https://github.com/IvanMurzak/Context-Engine/pull/248)
  merged (`3d82a09`, 38/38 CI green, refine applied, 0 CI-fix attempts); issue #247 closed;
  software pointer bumped via PR #337 (`e85d2e8`). Pack format v1 frozen + build-side writer landed.
- **2026-07-15** — a02 dispatched (run `c86dbec4ddd7`). Signing gates de-risked out-of-band: a10
  (Azure Trusted Signing) + a13 (Apple Developer ID) reusable from existing repos + `.secrets/`,
  **no business dependency** (→ `memory/context-engine-m8-signing-reuse-map.md`).
- **2026-07-15** — ✅ **a02 landed.** CE PR [#250](https://github.com/IvanMurzak/Context-Engine/pull/250)
  merged (`25dcbe38`; one in-diff heap-use-after-free fixed in-place, one transient render-web flake
  cleared on rerun); issue #249 closed; software pointer PR #339 (`b2c1eb62`). Retrospective's
  shared-step edit (`03-refine.md` `/simplify` realignment) captured surgically (`057e34ad`).
  **Watch item:** `render (web, emscripten)` `emdawnwebgpu` port-fetch network flake seen twice →
  CE `ci.yml` port-caching/retry hardening candidate (low priority, non-blocking).
- **2026-07-15** — a03 dispatched (run `658dcc1dcfea`).
- **2026-07-16** — ✅ **a03 landed.** CE PR [#252](https://github.com/IvanMurzak/Context-Engine/pull/252)
  merged (`f46acba2`, 38/38 green); issue #251 closed; software pointer PR #341 (`47609ade`).
  Retrospective doc fixes captured (`b553ea1a`): `03-refine` Step 5 widened (profile pre-push
  hand-audits bind as gates) + CE `conventions.md` GCC `linux`/`unix`/`i386` predefined-macro
  blind-spot bullet; separate `test.md` step-citation pointer-rot fixed (`1df4e743`).
  **a03 follow-ups (durable):** (1) texture half still needs PNG texel DEFLATE-decode (a separate
  open deferral; DoD met without it) → M8/M8.5 follow-up; (2) `transcode.*` error codes not yet in
  the contract error catalog → **fold into a05** (build-core-cli owns the catalog); (3) `build_container`
  ~2× peak RSS → perf follow-up. Transferable QA lesson logged: test the frozen spec's PROPERTY,
  not the implementation mechanism (new tests + golden ratified a latent sidecar-addressing bug
  that 03-refine caught).
- **2026-07-16** — a04 dispatched (run `616f63af86b3`) — WebGPU backend decision (wgpu-native vs Dawn).
- **2026-07-16** — ✅ **a04 landed: RETAIN wgpu-native v29.0.1.1.** CE PR
  [#254](https://github.com/IvanMurzak/Context-Engine/pull/254) merged (`0dd8baa3`, 38/38 green);
  issue #253 closed; software pointer PR #343 (main CE ptr == `0dd8baa3`, verified). Decision doc
  `docs/native-webgpu-backend-decision.md`; §1-M4 as-built row + §1-M8 de-risks updated to RESOLVED.
  Ship backend pin frozen; the R-SEC-009 signed-prebuilt carve-out stands. Teardown hook timed out →
  leaked worktree manually destroyed (exit 0). Watch item: `m6-exit-2-gc-budget` macOS ~1.2% timing
  overshoot (pre-existing flake, cleared on rerun) → CE `ci.yml` tolerance/retry follow-up.
- **2026-07-16** — a05 dispatched (run `a8dea84ad39b`) — build-core-CLI chokepoint; brief folds in the
  a03 follow-up to register `transcode.*` codes in the additive error catalog alongside `build.*`.
- **2026-07-16** — **a08 owner gate (custody model B) cleared.** Owner created the protected
  `release` environment on `IvanMurzak/Context-Engine` (required reviewer = IvanMurzak) and loaded
  the Ed25519 private key into the `RELEASE_SIGNING_KEY` environment secret; TD-verified via
  `gh api .../environments/release` + `gh secret list --env release`. Remaining a08 owner action =
  approve the trust-root pin PR when a08 opens it. a08 brief will read `RELEASE_SIGNING_KEY` from the
  `release` environment.
- **2026-07-16** — ✅ **a05 landed.** CE PR [#258](https://github.com/IvanMurzak/Context-Engine/pull/258)
  merged (`6dc50523`, 34/34 CI green); issue #257 closed; software pointer bumped via PR #349
  (`65afc4b0`, CE `0dd8baa3`→`6dc50523`). The implement-task run `a8dea84ad39b` **died at CI-wait**
  (stale `pid:1` liveness stub, no resumable `next.json`) with one red leg: `sanitize (ASan+UBSan)`
  test #80 `cli-test_build_command`. **Root cause = the known V8-duplicate-typeinfo false-positive**
  (`docs/sanitizer-v8-false-positives.md`, #201): the new test prints benign UBSan `vptr` diagnostics
  over valid `std::ofstream` code, and printing them drives the demangler whose scratch allocations
  LSan reports as leaks (37810 B / 38 allocs, all `__cxa_demangle`) — the new test was simply missing
  from the CLI-wide `LSAN_OPTIONS` suppression list that 14 sibling tests already carry. **TD-direct
  fix** (run dead+unresumable; a fresh implement-task would branch off main and lose a05's work; the
  fix is one line by an established in-file pattern): added `cli-test_build_command` to the
  `CONTEXT_SANITIZE` `set_tests_properties` suppression list + folded the run's correct in-flight
  polish (doc default-out-path `<target>.pack` to match code) → `02dc698` on PR #258. Worktree
  destroyed, stale lockfile cleaned. Transferable: a NEW cli unit test that constructs iostream
  objects in-process MUST be added to that suppression list in the same PR, or the sanitize leg reds.
- **2026-07-16** — a06 (linux-adapters) is now ready (a05 ✅); dispatching single-lane.
- **2026-07-16** — ✅ **a06 landed.** CE PR [#260](https://github.com/IvanMurzak/Context-Engine/pull/260)
  merged (`5b97d1f0`, all required checks green + MERGEABLE); issue #259 closed; software pointer
  bumped via PR #352 (`07b2fa58`, CE `6dc50523`→`5b97d1f0`). Linux desktop + server/headless export
  adapters shipped (tarball packaging, headless render-DCE size/symbol audit test, per-PR
  `linux-export` CI job). Run `a42c53e4552d` **halted on a SOFT CI-wait timeout** (3600s cap), NOT a
  failure: 04-wait-ci fixed a real in-diff regression in-place (`5893806` — the new `linux-export`
  job omitted the `runtime-host-*` ctest executables from its `--target` list → "Not Run = RED"),
  after which every REQUIRED check went green; only the ADVISORY `shader-crosscompile (windows)`
  (`continue-on-error`) leg was still running at the cap. TD verified green + merged. **Pipeline
  self-improvements captured** (2 surgical commits to main): (1) `wait_ci.py --advisory-checks`
  (a running continue-on-error leg no longer holds the gate on an all-required-green PR; +22 tests,
  91 green) — would have prevented this soft-timeout; (2) profile `test.md` linux-export CI doc.
  **Open CE-repo follow-up (owner, low-pri):** extend `src/runtime/host/CMakeLists.txt` comment + the
  profile `test.md` tripwire enumeration to cover restricted-`--target` CI jobs (the "Not Run = RED"
  class). Transferable lesson (fed to a07's brief): a NEW CI job with a restricted `--target` list
  MUST build every ctest executable it later selects by regex, or the gate reds "Not Run".
- **2026-07-16** — a07 (smoke-run-packed-determinism) ready (a06 ✅); dispatching single-lane.
- **2026-07-17** — ✅ **a07 landed (M8 exit-gate machinery).** CE PR
  [#262](https://github.com/IvanMurzak/Context-Engine/pull/262) merged (`e1199ab8`); issue #261
  closed; software pointer bumped via PR #358 (`61c3698d`). R-BUILD-009 headless smoke-run (build
  CLI launches the pack it just produced, steps N ticks against the SHIPPED RuntimeKernel) +
  packed-wedge L-54 determinism-hash gate + M6 replication harness re-run against packs; blocking
  per-PR wedge smoke + fleet-manifest rows. Run `cf3d39b28c6a` **completed cleanly end-to-end**
  (manager self-merged + self-bumped the pointer — no TD merge needed). Sole CI red was an
  out-of-diff flake (`m6-exit-2-gc-budget` ASan overshoot, not the diff), cleared by a same-HEAD
  full rerun. Pipeline self-improvements captured to main (2 surgical commits): 04-wait-ci
  pre-rerun `in_progress` guard + advisory-leg `gh run cancel` escape hatch; the `test.md`
  CI-surface-sync auto-landed by the 05-land teardown zero-touch capture.
  **Open CE-repo follow-up (owner, low-pri):** `m6-exit-2-gc-budget` wall-clock GC-budget assertion
  is widened only under TSan, not ASan → marginally flakes `sanitize (ASan+UBSan)` under runner
  load; add an ASan-aware budget widen (same `CONTEXT_TSAN_BUILD`-style pattern).
- **2026-07-17** — a08 (trust-root pin + fetch-verify) is the next ready task (needs a05 ✅). It is
  a **SECURITY GATE**: it pins the Ed25519 release-signing public key into the engine trust root
  and wires verify-before-use. The private key is already loaded (owner cleared the custody gate
  2026-07-16). Per the gate rule, TD is holding dispatch to confirm GO with the owner + the merge
  policy for the pin PR (owner must approve the trust-root pin before it lands).
- **2026-07-17** — **a08 SECURITY GATE cleared — owner GO (AskUserQuestion): "Dispatch + auto-merge."**
  The M8 set was pre-authorized (2026-07-15) and the custody gate is cleared (key in the protected
  `release` env); owner elected to auto-merge the pin PR on green CI rather than hold it for separate
  review. a08 dispatched single-lane (run `bd205c71ceed`). Brief carries the exact PUBLIC
  `allowed_signers` line to pin (from `.secrets/context-engine/README.md`, fingerprint
  `SHA256:4f8ZHq0v…`), the verify-before-use wiring targets (R-BUILD-004 toolchain + R-VER-004 seam;
  third-party wgpu/CEF/V8/wasmtime stay OUT), and the protected-env signing job — with a hard rule
  that only the public line is committed and the private key is never echoed/logged/tracked.
  Also filed 2 CE follow-up issues from the a06/a07 retrospectives: #263 (m6-exit-2 ASan-flake widen)
  and #264 (restricted-`--target` Not-Run tripwire doc).
- **2026-07-17** — ✅ **a08 landed (fail-closed trust chain is REAL).** CE PR
  [#266](https://github.com/IvanMurzak/Context-Engine/pull/266) merged (`51713091`); issue #265
  closed; software pointer bumped via PR #362 (`33e2d513`). Run `bd205c71ceed` completed cleanly
  (self-merged + self-bumped) through one MAJOR-CI loop-back. **TD security verification (guardian
  duty) — all clean:** (1) `tools/trust-root/allowed_signers` carries EXACTLY the one production
  PUBLIC line (fingerprint `SHA256:4f8ZHq0v…`), header documents private key lives ONLY in the
  `RELEASE_SIGNING_KEY` protected env; (2) merged PR #266 diff scanned — ZERO private-key body /
  base64 blob, the only `RELEASE_SIGNING_KEY` hits are correct `${{ secrets.* }}` workflow env-refs
  + a `[ -z ]` guard, private-key prefix absent from the diff. Delivered: verify-before-use wired
  fail-closed into R-BUILD-004 toolchain + R-VER-004 seam (third-party fetchers stay out), the
  protected-env signing job, and `build.template_unverified`/`build.toolchain_fetch_failed` codes.
  **Bonus:** 03-refine caught + fixed a real fail-closed classification bug — Windows OpenSSH
  `ssh-keygen` raw exit `-1` was misclassified `ConfigError` instead of `Refused` (both fail-closed,
  not a hole) — at `bfc7627`, with a platform-parameterized regression test; the lesson was captured
  back into the 03-refine pipeline doc.
- **2026-07-17** — a09 (context-doctor) ready (a05,a08 ✅); dispatching single-lane.
- **2026-07-17** — **a10 custody gate pre-cleared (owner GO).** Confirmed `.secrets/azure-trusted-signing-sp.json`
  holds the complete Azure Trusted Signing Service Principal (`appId`/`password`/`tenant`, all present —
  the same creds prod already signs with in `GameDev-MCP-Server/deploy_server_executables.yml`). Owner
  elected (AskUserQuestion) to load them into the protected `release` environment (custody model B, matches
  a08). TD loaded the 3 GitHub env secrets into `IvanMurzak/Context-Engine` env `release` (values piped via
  stdin, never echoed): `AZURE_CLIENT_ID`←appId, `AZURE_CLIENT_SECRET`←password, `AZURE_TENANT_ID`←tenant.
  Non-secret config for the a10 workflow (goes in yaml, not secrets): endpoint `https://eus.codesigning.azure.net`,
  account `ivan-murzak`, cert profile `ai-game-dev-cert`; the a10 signing job runs with `environment: release`.
  a10 is now fully unblocked (money authorized 2026-07-15 + subscription active + secrets loaded).
- **2026-07-17** — ✅ **a09 landed.** CE PR [#268](https://github.com/IvanMurzak/Context-Engine/pull/268)
  merged (`9b06bf5`); issue #267 closed; software pointer bumped via PR #363 (`cc8816e`). `context doctor`
  (R-BUILD-008) — per-target toolchain/env diagnosis with machine-readable remediation, fetchable-vs-
  preinstalled split, file-sync budget checks, signing-prereq reachability (checks only, no secrets
  surfaced). Run `f4e8243cab19` completed clean on the first pass (no halts/loops). Retrospective doc fix
  captured (samples-corpus gate registration for new stable CLI verbs). **9/14 M8 build-pipeline tasks done.**
- **2026-07-17** — a10 (windows-adapter-authenticode) ready (a05 ✅, all gates cleared); dispatching single-lane.
- **2026-07-17** — ✅ **a10 landed.** CE PR [#270](https://github.com/IvanMurzak/Context-Engine/pull/270)
  merged (`e4976796`); issue #269 closed; software pointer bumped via PR #364 (`8db87954`). Windows
  desktop export adapter + v1-MUST Authenticode signing hook (Azure Trusted Signing via
  `azure/trusted-signing-action@v0`, guarded `if: env.AZURE_CLIENT_ID != ''` → honest unsigned state
  on a no-secrets fork PR; timestamping mandatory; `context doctor` reports signing-prereq state).
  Run `6269ec49067a` **halted on a SOFT CI-wait timeout** (3600s cap) — NOT a code defect: 04 fixed a
  real in-diff PowerShell `$flavor:` ParserError in the `windows-export` job in-place (`0e88b77`), then
  the ubuntu-latest REQUIRED legs sat QUEUED the whole window under **GitHub Actions hosted-runner
  queue congestion**. TD polled the queue to drain (ci-wait, ~19 min) → 36/36 green → merged. **TD
  security verification — all clean:** merged diff scanned, ZERO literal secret/key-body; every
  `AZURE_*` occurrence is a `${{ secrets.* }}` env-ref, an `if: env.AZURE_CLIENT_ID != ''` presence
  guard, or `env_present()` in doctor — no value committed; non-secret endpoint/account/profile config
  only. Tier-1 pipeline improvement captured (02-implement dropped-relay recovery). **10/14 M8 done.**
  ⚠ **Infra watch (owner):** account-wide GH Actions queue congestion (my single-lane CE runs + the
  concurrent mcp-authorize release flow) is causing soft CI-wait timeouts on the M8 lane — runs stay
  correct and land on re-poll, but throughput is degraded; the a10 retrospective suggests checking the
  account Actions concurrency limit / trimming simultaneous macOS matrix legs in CE `ci.yml`.
- **2026-07-17** — a11 (web-export-adapter) ready (a02,a05 ✅); dispatching single-lane.
- **2026-07-17** — **a13 custody gate pre-cleared (owner GO).** Owner asked to locate the Apple signing
  creds; TD confirmed all 6 are present + well-formed in `.secrets/` (no MacMini needed): `apple-devid.p12`
  (valid PKCS#12) + `.p12.pass` + `apple-sign-identity.txt` + `apple-api-key.p8` (valid) + `apple-api-key-id.txt`
  + `apple-api-issuer.txt`. Cross-verified as the proven working set — `IvanMurzak/AI-Game-Dev-App` already
  signs with 5 of the 6 (loaded 2026-06-09); the 6th, `MAC_SIGN_IDENTITY`, the App doesn't need
  (electron-builder auto-derives it from the cert) but CE's manual `codesign -s` path (per the
  GameDev-MCP-Server template) does. Owner elected (AskUserQuestion) to load into the protected `release`
  env now. TD loaded the 6 GitHub env secrets into `IvanMurzak/Context-Engine` env `release` (values piped
  via stdin, never echoed; `.p12`/`.p8` base64-encoded): `MAC_CSC_LINK`←base64(apple-devid.p12),
  `MAC_CSC_KEY_PASSWORD`←.p12.pass, `MAC_SIGN_IDENTITY`←apple-sign-identity.txt,
  `APPLE_API_KEY_B64`←base64(apple-api-key.p8), `APPLE_API_KEY_ID`←apple-api-key-id.txt,
  `APPLE_API_ISSUER`←apple-api-issuer.txt. The `release` env now carries the full 10-secret signing set
  (Ed25519 + Azure + Apple). a13 is fully unblocked; its notarize job runs with `environment: release`.
- **2026-07-17** — ✅ **a11 landed (lossless session-limit recovery).** CE PR
  [#274](https://github.com/IvanMurzak/Context-Engine/pull/274) merged (`62618711`); issue #273 closed;
  software pointer bumped via PR #371 (`94407be4`). Web export adapter — Emscripten/emdawnwebgpu
  template, fixed-memory heap, chunked-pack range fetch feeding the a02 loader. The FIRST manager
  **died mid-run at 04-wait-ci on the account session limit** (not a defect); TD recovered LOSSLESSLY —
  re-spawned the manager with `--resume` on the same run/worktree/PR (2 prior `fix(ci-wait):` commits
  intact). The resumed 04 classified the `render (web, emscripten)` real in-diff compile failure
  (inline `EM_ASM` browser-JS mis-parsed as C++: multichar constants + `-Wgnu-designator`) as MAJOR →
  looped to 03-refine → restructured into named `EM_ASYNC_JS` (`13b82a6`) → 38/38 green → landed.
  **Open CE follow-up (owner, coverage): #275** — the web-export CI does not actually exercise HTTP-Range
  chunked streaming (the golden test server ignores `Range` → full-file 200; the 1065 B fixture is < one
  65536 B chunk, so it passes by coincidence); needs a 206/Range-aware server + a multi-chunk fixture to
  truly verify the DoD. **11/14 M8 done.** Reusable lesson: a session-limit death mid-pipeline is
  recovered by RESUME (same run_id + preserved worktree/PR), never by re-dispatch.
- **2026-07-17** — a12 (build-time budgets) ready — `depends_on: ops1` is BLOCKING-FLIP-ONLY (spec Dep
  note: dev starts now, numbers advisory-until-provisioned); a06 ✅; dispatching single-lane.
- **2026-07-17** — ✅ **a12 landed.** CE PR [#277](https://github.com/IvanMurzak/Context-Engine/pull/277)
  merged (`0778f4c3`); issue #276 closed; software pointer bumped via PR #378 (`199e25d7`). R-BUILD-006
  committed build-time budgets (`bench/build_time.py` + `build-time-budget.json`) + a `build-time bench
  (warm cache)` CI job — the JOB blocks (harness green + time-series archive), the numeric budget GATE is
  **advisory** (`continue-on-error`) until ops1 provisions `perf-linux-bare-metal` (recorded in the fleet
  manifest); synthetic-regression breach detected advisory-red. Run `0715a040c6f5` completed but its
  **teardown worktree-destroy hook timed out (300s, couldn't ff the shared checkout)** — the run itself
  landed fine; the zero-touch capture #381 got the test.md build-time-bench doc but LEFT two improver edits
  uncaptured. TD reconciled: discarded the stale working-tree copies (avoiding a build-time-bench doc
  REGRESSION), ff'd the shared checkout to origin, and re-applied the two clean additive edits (05-land
  ≥180000ms Bash-timeout on the submodule sync — the very timeout class that failed this teardown; test.md
  CEF-symlink Windows CI-only carve-out). Concurrent-flow dirty files left untouched. **12/14 M8 done.**
  Reusable lesson: a teardown-hook ff timeout can leave improver edits uncaptured AND the shared checkout
  behind origin — reconcile by discarding stale working-tree copies, `merge --ff-only origin/main`, then
  re-apply only the genuinely-missing additive bits (never blind-commit a working-tree that may carry a
  stale removal).
- **2026-07-17** — a13 (macos-adapter-notarization) ready (a05 ✅, custody pre-cleared); dispatching single-lane.
- **2026-07-17** — ✅ **a13 landed — OS-signing trilogy complete (Ed25519 + Windows + macOS).** CE PR
  [#279](https://github.com/IvanMurzak/Context-Engine/pull/279) merged (`3e68498`); issue #278 closed;
  software pointer bumped via PR #388 (`27591a9`). macOS desktop export adapter — Developer-ID codesign
  (hardened runtime + timestamp) + `notarytool submit --wait` (with the mandatory JSON `status==Accepted`
  assertion) + staple, on `macos-latest`, guarded on the signing secrets (honest unsigned state on a
  no-secrets fork PR), creds scrubbed at job end (R-SEC-010). Run `66982b685b22`: 03-refine caught + fixed
  a **real security-relevant in-diff defect** — presence-only Mach-O parsing mis-read the arm64 linker's
  auto-embedded AD-HOC signature as a real Developer-ID signature; fixed via `CS_ADHOC`-flag parsing
  (`89c55fd`). **TD security verification — all clean:** PR #279 diff scanned, ZERO literal cert/key/password;
  every `MAC_*`/`APPLE_*` occurrence is a `${{ secrets.* }}` assignment, a `$VAR` use, or a `${VAR:-}`
  presence guard. The a13 test.md CI-surface sync (all 3 export legs: linux/windows/macos) landed via the
  teardown zero-touch capture #389. **13/14 M8 done — a14 (M8 exit-gate) now unblocked (a06–a13 all ✅).**
- **2026-07-17** — a14 (M8 exit-gate) ready — all of a06–a13 ✅; dispatching single-lane (the M8-closing gate).
- **2026-07-17** — 🏁 ✅ **a14 landed — M8 (BUILD PIPELINE) COMPLETE (14/14: a01–a14).** CE PR
  [#282](https://github.com/IvanMurzak/Context-Engine/pull/282) merged (`7723a2f`); issue #281 closed;
  software pointer bumped via PR #392 (`f088cdd7`). Run `0f9688f7cd9e` completed clean in one pass
  (43/43 CI green, 0 in-place fixes, no retrospective). Added the permanent blocking `m8-exit-*` ctest
  family (6 gates) + CI wiring per the M6/M7 precedent: the full v1 build set produced headless per-agent
  (Linux desktop/server + Windows + Web green, macOS on its leg), desktop artifacts signed (macOS
  notarized + Windows Authenticode) with verify-before-use failing closed under the a08 trust root,
  headless smoke green, packed wedge builds passing L-54 determinism + L-48/R-NET-001 replication, wedge
  smoke blocking per-PR; "Not Run = RED" audit passed, fleet-manifest rows validated; the a12 build-time
  budget number honestly recorded advisory-until-ops1. **The M8 milestone bar now lives as CI, not prose.**
  **M8 = DONE. Next: M8.5 (wedge hardening, a15–a23) — Wave 2 — pending owner GO at the wave boundary.**
- **2026-07-17** — **Wave boundary: owner elected to REVIEW M8 first (AskUserQuestion) — M8.5 dispatch HELD**
  pending explicit owner GO. Leak-hygiene audit at the boundary found ~10+ leaked superproject worktrees
  under `.claude/worktrees/` (`15c2b4aa`, `25291f8f`, `278ef04f`, `365f5907`, `5387a3cd`, `7ca1074f`,
  `9ced0222`, `a4862189`, `b2cd2123`, `b973f2c3`, …) + a stale `worktree-*` branch — NONE from the a05–a14
  runs (all self-cleaned; CE fork branches deleted on merge); they belong to the concurrent flow / historical
  runs. `gc --clean` is destructive and some may back LIVE concurrent runs → NOT cleaned; awaiting owner GO +
  a no-live-run confirmation. M8 review entrypoint available: CE `release-sign.yml` is `workflow_dispatch`-
  triggerable → a real end-to-end signed+notarized artifact via the protected `release` environment (owner is
  the required reviewer).
- **2026-07-18** — 🟢 **M8.5 GO (owner, via `/strategy:design-implement`).** Owner ruled **(1) push 2–3
  concurrent code lanes** (relaxes the single-lane conflict-domain rule for this wave, accepting land-time
  conflict-resolution + session-limit risk) and **(2) defer ops1** (a21/a23 record honest *advisory* perf
  numbers; no hardware spend now). **Wave 1 dispatched concurrently: a15 ∥ a16 ∥ a17** — the board's flagged
  "no-hard-dep" trio and the most file-disjoint set (only a15 adds a CLI verb; a16/a17 are test-suite tasks
  overlapping only on `ci.yml`). Each briefed to prefer NEW test files over shared-file edits to minimize
  cross-lane conflicts. Dispatched via `launch_implementer.py --no-wait` (isolated worktrees, ceiling=0),
  batch `2026-07-18T03-42-17Z`. **Merge order at land = a15 → a16 → a17** (TD resolves `ci.yml`/registry
  conflicts on each later land). GUI cluster a18/a19/a20 (high mutual overlap) + a21/a22/a23 follow in later
  waves. a22 (docs-only, disjoint) is a candidate parallel lane pending its business-reply soft gate.
- **2026-07-18** — ⚠️ **Wave 1 SWEPT — all 3 concurrent lanes killed mid-run, re-queued.** The
  `run_in_background` launcher processes (harness-tracked) were stopped simultaneously (~25 min in),
  cascading to the nested `claude --print` children. Recovery: children confirmed dead; each run had
  provisioned a worktree (each **over-provisioned all 30 submodules** — known over-provision issue) and made
  partial progress (a15 substantial profiling WIP; a17 1 commit + CE issue #283; a16 1 test file). **No CE
  branches/PRs pushed → no landed work.** WIP salvaged to `/c/tmp/m85-salvage/*.patch` (a15 1801L / a17 818L /
  a16 268L); 3 worktrees destroyed; CE issue #283 kept OPEN for a17 reuse. **Root cause: harness sweeps
  `run_in_background` tasks → nested-claude concurrent dispatch is unreliable this session.** Board reverted
  a15/a16/a17 → pending; mechanism decision pending owner (reliable single-lane in-session vs. retry detached
  concurrency). Leak `gc` still deferred (many concurrent-flow worktrees live).
- **2026-07-18** — Owner GO: **retry detached concurrency.** Re-dispatched a15/a16/a17 as
  **detached+breakaway Win32 processes** (`CREATE_BREAKAWAY_FROM_JOB` → escape the harness job that swept
  the first attempt); helper `runs/<batch>/detached_ce_launch.py`. All 3 survived the boot this time.
  **Then a self-collision surfaced:** a15's inner implement-task pre-flight saw its OWN batch + sibling PIDs
  + a16/a17's just-started runs, mistook the intentional siblings for a "concurrent second flow", and
  **stood down cleanly** (no run/worktree/PR — the exact failure the TD-token memory warns about; my briefs
  omitted the sibling-authorization). a16 (wt `f0243573881a`) + a17 (wt `45f6c3e6d98f`, reuses CE #283) did
  NOT false-alarm — both provisioned worktrees and are implementing. **Fix: re-dispatched a15 ONLY (PID 17896)
  with a corrected brief** naming a16/a17 as intentional parallel siblings + TD token `M85W1-TD-GO`. Monitor
  re-armed (`bszr0h1j9`) on PIDs 17896/31508/93368. Lesson reaffirmed: every concurrent CE lane's brief MUST
  authorize its siblings or it self-collides.
- **2026-07-18** — ✅ **a16 LANDED (first M8.5 task).** All 3 lanes opened PRs (a15 #288 / a16 #285 / a17
  #286). **a16 auto-landed end-to-end** — CE PR [#285](https://github.com/IvanMurzak/Context-Engine/pull/285)
  MERGED `88ef242e` (issue #284 closed), 42/42 CI green, 0 CI-fix attempts; software pointer bumped via
  [sw #394](https://github.com/IvanMurzak/ai-game-dev-software/pull/394). **Operating-model correction:
  implement-task AUTO-LANDS** (merge + pointer-bump + teardown on green CI) — the TD verifies + tracks, does
  NOT hand-merge (the 2026-06-29 "parks at wait-ci" note is stale for this pipeline version). Post-a16:
  a15 #288 still MERGEABLE (UNSTABLE=CI running), a17 #286 mergeability recomputing; both children still
  running and will auto-land. a16 run surfaced 3 non-blocking follow-ups (deferred to wave boundary):
  (1) spec wording — a16's "lost-update events" seam implies a daemon event enum M1 didn't ship (design-
  authority reword); (2) `/code-review --fix` introduced a CI-only Apple-libc++ `std::to_string(chrono rep)`
  break (caught by CI); (3) `01-handoff.md` token-budget compaction (~3112 vs ~1500).
- **2026-07-18** — ✅ **a17 LANDED** (2nd of Wave 1). CE PR
  [#286](https://github.com/IvanMurzak/Context-Engine/pull/286) MERGED `9e914cd9` (on top of a16's
  `88ef242e`; issue #283 closed), pointer bumped [sw #395](https://github.com/IvanMurzak/ai-game-dev-software/pull/395)
  — sw main CE pointer `9e914cd9` verified. a17 **auto-rebased on a16's merge, no conflict halt** (the
  concurrent-land the owner accepted resolved cleanly). Found + fixed real trust-tier issues in-lane
  (`fix:`/`harden`). **a15 (#288) is the last Wave-1 lane — still running (PID 17896), MERGEABLE post-a16/a17.**
  a17 follow-up (deferred): new `security-redteam-*` ctests linking `context_js`/`context_cli` need the
  `LSAN_OPTIONS`/`TSAN_OPTIONS` V8-suppression wiring 3 sibling test families already carry.
- **2026-07-18** — 🏁 ✅ **Wave 1 (M8.5) COMPLETE — a15/a16/a17 all landed via 3 CONCURRENT detached lanes.**
  a15 CE [#288](https://github.com/IvanMurzak/Context-Engine/pull/288) MERGED `d0602acc` (CE main HEAD) ·
  [sw #396](https://github.com/IvanMurzak/ai-game-dev-software/pull/396). Concurrency validated end-to-end:
  3 lanes → 3 PRs → 3 clean sequential auto-lands (a16 `88ef242e` → a17 `9e914cd9` → a15 `d0602acc`), **zero
  conflict halts** despite shared `ci.yml`/`error_catalog` (each lane's "prefer new files" briefing + the
  pipeline's rebase-in-wait-ci handled it). Cost of getting there: 1 harness bg-sweep + 1 self-collision, both
  recovered. **Remaining M8.5: a18/a19/a20 (GUI), a21 (density, advisory), a22 (docs/CVD), a23 (exit gate,
  LAST); ops1 deferred.** **Owner ruling 2026-07-18: NEXT wave (Wave 2) runs on model `fable`; subsequent
  waves on planned models. Reboot-stop was REVOKED — continue launching waves.**
- **2026-07-18** — 🔵 **Wave 2 (M8.5) DISPATCHED on model `fable` (owner directive) — a18 ∥ a21 ∥ a22,
  concurrent detached.** Composition = the conflict-safe disjoint trio: a18 (tilemap GUI), a21 (density
  bench, advisory/ops1-deferred), a22 (CVD/CRA docs). GUI siblings a19/a20 overlap a18 → serialize into
  Wave 3; a23 exit-gate LAST. Detached+breakaway PIDs a18=22420 / a21=15124 / a22=52844; briefs carry the
  sibling-authorization header (`M85W2-TD-GO`) from the start (Wave-1 self-collision fix). Monitor `bfv3glsx7`.
  a22 brief carves out the repo-admin setting (private-vuln-reporting) + the business-reply gate
  (envelope `20260715-165440-5cd64a`) as **TD-handled** (its lane just lands SECURITY.md + a draft-marked CRA doc).
  **⚠️ FABLE OVERRIDE — REVERT OBLIGATION:** to run Wave 2 on fable, `steps/02-implement.md` + `steps/03-refine.md`
  were flipped `model: opus → fable` (manager-mode has no per-run override; the change landed folded into
  concurrent-flow commit `641592fa` since its `git add -A` swept the uncommitted edit). **These two files MUST
  be reverted to `model: opus` once Wave 2's implement/refine steps finish** — Wave 3+ run on planned/opus
  models, NOT fable. 01-handoff/04-wait-ci/05-land stayed sonnet.
- **2026-07-18** — ✅ **a22 LANDED** (first Wave-2 task; docs, fast). CE
  [#290](https://github.com/IvanMurzak/Context-Engine/pull/290) `deaa1d14` · sw #398. TD follow-ups: GitHub
  **private-vulnerability-reporting ENABLED** on the CE repo (`{"enabled":true}`); CRA note **draft-marked**
  pending the business/legal reply to envelope `20260715-165440-5cd64a` (only the send-audit log exists so
  far — DoD "explicitly awaited" is met; reconcile the note when the reply lands). a18 (#294) + a21 (#292)
  still in wait-ci.
- **2026-07-18** — 📋 **CE CI speed-up PLAN delivered** (investigation subagent) →
  `.claude/plans/designs/2026-07-18-ce-ci-speedup-plan.md`. Critical path = ubuntu `sanitize` 9m21s doing a
  full C++ compile with NO cross-run cache (`SCCACHE_GHA_ENABLED` disabled 2026-07-05 over churn). Plan:
  **P1** sccache local-dir + `actions/cache` on the 4 critical jobs (~55% wall-clock) + **P3** de-duplicate
  per-branch caches (reclaims ~3.6 GB) → CI ~9m54s→~4-5m AND cache 6.63GB→~4.9GB (LOWER than now, ≥5.1GB
  headroom). Owner-gated: P2 (sccache on the self-hosted Windows box — operator install), P4b/P4c (leg moves),
  P6 (advisory-bench PR pruning — CLAUDE.md "blocking harness"). **Impl (P1+P3+P2-workflow-side) via
  implement-task on OPUS, dispatched AFTER Wave 2 fully lands + the fable revert** (avoids `ci.yml` conflict +
  wrong model).
- **2026-07-18** — ⚠️ **Wave 2 hit the account SESSION LIMIT mid-wait-ci** (3 Fable lanes + investigation
  subagent, cumulative) → a18/a21 children died before landing (a22 had already merged). Recovery:
  **a21 (#292) was 39/39 green → TD-merged** (CE main `1325d89d`); its sw pointer bump is DEFERRED to bundle
  with a18's (a18's merge sits on top). **a18 (#294) had 1 real red — `sanitize (ASan+UBSan)` — a KNOWN
  uninstrumented-V8-prebuilt UBSan vptr FALSE-POSITIVE** (textbook stream code in `merge_command.cpp`; the new
  `tilemap-paint-parity` ctest links `context_cli`→V8 but lacks the suppression wiring sibling CLI-linking
  families carry — exactly a17's retrospective note + `docs/sanitizer-v8-false-positives.md`). Delegated the
  suppression fix to a focused sub-agent (works in a18's existing worktree, pushes to #294; TD merges after CI).
  **FABLE REVERT DONE:** `02-implement`/`03-refine` back to `model: opus`. Wave 3+ on planned/opus. Remaining:
  a18 land (+ bundled a21/a18 pointer bump), then a19→a20 (GUI, serialize), a23 (exit gate). Then the CI
  speed-up implement-task (opus).
- **2026-07-18** — 🏁 ✅ **Wave 2 (M8.5) COMPLETE — a18/a21/a22 all landed (Fable).** a18 sanitizer-FP fix
  went green (39/39) → CE [#294](https://github.com/IvanMurzak/Context-Engine/pull/294) MERGED `002aff46`;
  a21+a18 pointer bumped together via [sw #399](https://github.com/IvanMurzak/ai-game-dev-software/pull/399)
  (CE pointer `002aff46`, verified). **M8.5 progress: a15–a18, a21, a22 ✅ (8 of 9 code tasks + a22).
  REMAINING: a19 (viewport override) → a20 (contextual help) [GUI, serialize, opus] → a23 (exit gate, needs
  a15–a22); ops1 deferred.** NEXT ACTION: dispatch the CI-speed-up implement-task (opus, P1+P3) — done first
  so a19/a20/a23's CI runs faster; then the GUI cluster. Pacing single-lane after the session-limit hit.
- **2026-07-18** — ✅ **CE CI SPEED-UP landed** (owner's investigate→plan→implement request; NOT an M8.5 task).
  Plan `.claude/plans/designs/2026-07-18-ce-ci-speedup-plan.md`; impl P1+P3 via implement-task (opus) →
  CE #296 `ed3e4c8f` / sw #400. Re-enabled sccache (local-disk, week-salted, save-on-main-only, one saver per
  namespace) + de-duped per-branch vcpkg caches. Cache **6.27 GiB/10 GB** (under watch line); ~55% CI speed-up
  shows on the next warm PR. P2 (Windows-box sccache)/P4b/P5/P6 deferred (owner-gated). — Then 🔵 **a19
  (viewport override) DISPATCHED** single-lane on **opus** (PID 88616, Wave 3); brief pre-empts the a18 V8-vptr
  UBSan-FP via the LSan-suppression hint. Next after a19: a20 (contextual help) → a23 (M8.5 exit gate).
- **2026-07-18** — 🏁🎉 ✅ **M8.5 (wedge hardening) COMPLETE — M0–M8.5 = v1 wedge SCOPE-COMPLETE.** Wave 3
  landed the trailing tasks: a19 CE [#298](https://github.com/IvanMurzak/Context-Engine/pull/298) · a20 CE
  [#300](https://github.com/IvanMurzak/Context-Engine/pull/300) · then a23 (FINAL exit gate) CE
  [#302](https://github.com/IvanMurzak/Context-Engine/pull/302) `4b7456f` MERGED (issue #301 closed) · sw
  [#406](https://github.com/IvanMurzak/ai-game-dev-software/pull/406) (CE pointer `4b7456f` verified on sw main).
  **a23 = solo TD dispatch (run `14f1e7496419`, opus impl/refine + sonnet wait-ci/land; pre-flight confirmed no
  competing run — the board's "PID 84264" was a hand-off placeholder).** Encoded the M8.5 exit criteria as
  **9 blocking `m85-exit-*` ctests** (1a co-edit + 1b worktree-merge → L-50; 2a sandbox-caps + 2b
  tamper-fail-closed → trust-tier/signing a17+a08; 3 profiling-json → a15/L-47; 4a tilemap-paint + 4b
  override-edit + 4c contextual-help → trailing GUI a18/a19/a20; 5 density-commitment → a21) with full ci.yml
  target/named-step/exclusion wiring + `docs/ci-fleet-manifest.json` rows; every exit clause → exactly one gate.
  CI 42/42 green across the 3-OS matrix (1 in-place `fix(ci-wait):` TSAN-suppression on `m85-exit-3` + 1
  out-of-diff rerun of a pre-existing M6 `m6-exit-2-gc-budget` ASan flake). **ops1 (perf-runner) remains
  DEFERRED — density numbers advisory-until-provisioned (honest, per a21/a23); it is the sole outstanding §9
  item and is owner-money-gated.** Retrospective human-only follow-up (non-blocking, filed): extend the
  `CONTEXT_TSAN_BUILD` sanitizer-aware budget widen in `src/tests/integration/test_m6exit2_gc_budget.cpp` to
  also cover ASan+UBSan (kills the recurring ~1.8% real-time overshoot flake). Also still open: `01-handoff.md`
  token-budget compaction (~3112 vs ~1500).
- **2026-07-18** — 📝 **Status honesty note (owner-verified): "v1 wedge scope-complete" does NOT yet include an
  interactive Editor WINDOW.** What IS done + runnable today: the headless file-authoritative EditorKernel
  daemon (`context daemon --project <dir>`), the full CLI/RPC/MCP authoring surface (38 verbs), deterministic
  sim (`context session step/hash`), the packed-build pipeline, AND the GUI panel LOGIC + OSR compositor + CEF
  substrate (all headless-CI-verified via the UI-logic tree). What is NOT built: the **interactive presentation
  shell** — a native OS window that draws the compositor's accelerated-OSR output on screen and routes live
  mouse/keyboard into the panels. `editor_host` deliberately runs CEF **windowless/off-screen** (the
  `editor-cef-smoke` boot); there is no `SetAsChild`/`SetAsPopup` or window/input code in `src/editor/gui/`.
  This is a deliberate observer-grade/headless-first scope choice — now recorded as the **lead v1.x item**
  (see §"v1.x ledger"). Correcting an earlier over-optimistic framing (an MSVC+CEF build would still produce
  NO window — the shell must be built, not just toggled on). ops1 remains the only outstanding §9 task
  (owner-money-gated). — Also committing here the co-managing-flow's M8.5-COMPLETE board/banner updates that
  were left uncommitted in the working tree, so the milestone status is durable.
