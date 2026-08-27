# Context Game Engine — Architecture

> **Engine name:** Context Game Engine ("Context").
> **Status:** Design phase complete (2026-07-01). **Implementation status (2026-07-15):** M0–M7
> COMPLETE in the engine repo (`IvanMurzak/Context-Engine`); M8 (build pipeline) next. This folder
> remains the design authority; per-milestone as-built deltas live in the engine repo.
> This is the **stable half** of the design package: vision, architecture, component breakdown,
> technology stack, and the key data/control flows. Its sibling **`ROADMAP.md`** holds the
> volatile half — milestones, the
> "Context Sim" pre-release track, v2 scope, economics, risks, and CI tiering.
> **Companion docs:** `REQUIREMENTS.md` (what to build), `DESIGN-DECISIONS.md` (locked choices +
> open options). Per-round provenance for every amendment lives in the `REVIEW-*.md` trackers.

---

## 1. Vision

A modular, cross-platform game engine and editor where:

- **The core is minimal** and every feature is a package, so builds scale from a tiny headless
  simulation to a full RTX-class 3D game.
- **Everything is CLI-controllable and headless-capable**, so both humans and AI agents can build
  games — including on cheap, GPU-less machines.
- **Game logic is written once** (default TypeScript, npm ecosystem) and runs identically on every
  platform; the system handles per-platform translation.
- **What you play in the editor is what ships**, because the editor embeds the same runtime the
  build uses, parameterized by a target-platform profile.
- **UI is modern web technology**, pluggable, and renderable both on screen and on 3D/curved
  surfaces for VR/AR/XR (the XR-grade stack is v2 — see ROADMAP.md §3 [owner-ruled 2026-07-01]).

### 1.1 Target beachhead [owner-ruled 2026-07-01]

v1 targets a deliberate wedge, on **desktop + Linux-server + web**:

1. **Headless AI-training / RL environments** — deterministic, GPU-free, massively parallel
   simulation on commodity machines. **Parallel headless instance orchestration** — many engine
   instances stepped/seeded/hashed in parallel from one controller over the R-QA-005 session
   surface — is the **wedge demo scenario** for this pillar, backed by R-SIM-005's MUST status on
   the wedge platforms. Competitor honesty: vs GPU-vectorized simulators
   (Isaac/Brax/Madrona-class) Context does **NOT** compete on raw samples/sec; this pillar's
   pitch is **agent-authored environments + CPU-parallel commodity orchestration** — many cheap
   headless instances, no GPU — with committed ticks/sec/instance + instances-per-box targets set
   no later than the M8.5 exit (R-FILE-011; the Context Sim pre-release anchor was superseded —
   README.md acknowledged-gaps row; ROADMAP.md §2).
2. **Deterministic server-authoritative simulation & multiplayer** — the native/WASM deterministic
   tier (L-18/L-54) + netcode hooks (L-48).
3. **AI-generated 2D and simple-3D games** — the agent authoring loop over 2D-first-class (L-55)
   and baseline-3D PBR content.

**"General 3D engine" is the destination, not the day-one claim.** This beachhead is the
**priority lens for milestone trade-offs**: when scope pressure hits a milestone, work that serves
the wedge outranks general-engine breadth — the v2 re-sequencing in ROADMAP.md §3 (advanced
graphics + the XR UI stack → explicit v2) is its first application.

---

## 2. Architecture overview

### 2.1 Layered model

```
        CLIENTS (equal capability surface; authorization may restrict — R-SEC-007)
   ┌──────────────┬──────────────────┬────────────────┐
   │   GUI (CEF)  │  CLI (embeds     │    AI / MCP     │
   │              │  EditorKernel)     │                 │
   └──────┬───────┴────────┬─────────┴───────┬────────┘
          │ file writes    │ file writes     │ file writes   (ALL authored mutations)
          ▼                ▼                 ▼
   ┌───────────────────────────────────────────────────┐
   │      PROJECT FILES — the single source of truth    │  canonical JSON + sidecars; a git repo
   │      (scenes · assets+meta · packages · config)    │  by convention; worktree = instance
   └──────────────────────┬────────────────────────────┘
                          │ watch · hash · derive (L-22)
                          ▼
   ┌───────────────────────────────────────────────────┐
   │   EditorKernel (library; shown as the shared daemon) │  headless; runs on a monitorless VPS
   │  file-sync layer · derivation graph · RPC surface  │  single instance per project
   │  · asset pipeline · build orchestration            │  (byte-range lock on .editor/lock)
   │                                                    │
   │   embeds ▼  (play-in-editor = host RuntimeKernel     │
   │             with a target platform profile)        │
   │        ┌─────────────────────────────────────┐     │
   │        │            RuntimeKernel              │     │
   │        └─────────────────────────────────────┘     │
   └──────────────────────▲────────────────────────────┘
                          │ RPC + event stream (JSON-RPC 2.0 + MCP): queries,
                          │ session actions, derived outputs — never authored mutations
          └───────────────┴── all clients also attach here ──┘

   Every Game Build ships the SAME RuntimeKernel:
   ┌────────────────────────────────────────────────────┐
   │                   RuntimeKernel                       │
   │  ┌───────────── Simulation Core (GPU-free) ─────┐   │  always present, no rendering
   │  │ World (ECS) · Scheduler · module registry ·  │   │
   │  │ event bus · resource handles · platform seam │   │  ← the microkernel
   │  │ + logic VM (JS/WASM) · physics · gameplay    │   │  ← packages
   │  └──────────────────┬───────────────────────────┘   │
   │        one-way snapshot (sim → render)               │
   │  ┌──────────────────▼── Presentation modules ─────┐ │  loaded only when needed:
   │  │ render (RHI · lighting · materials · post ·    │ │  render/UI require a GPU;
   │  │ 2D/sprites) · UI · audio (GPU-independent)     │ │  audio runs headless too
   │  └────────────────────────────────────────────────┘ │
   └────────────────────────────────────────────────────┘
        Windows   macOS   Linux   Android   iOS   Web (WebGPU)
```

The platform row reads with the platform rulings: **v1 = Windows + macOS + Linux desktop, Linux
server/headless, and Web (MUST); Android trailing SHOULD; iOS v2-first** (R-BUILD-001)
[owner-ruled 2026-07-02].

### 2.2 The four load-bearing principles

1. **Headless core, thin clients.** All capability is in EditorKernel; GUI/CLI/AI are equal clients
   with the same public contract — file writes for mutations + one RPC surface; per-client
   authorization MAY restrict access (R-SEC-007; R-ARCH-001).
   (⇒ CLI-completeness, automation, single-instance bridge.)
2. **Simulation is authoritative; rendering is a detachable observer.** State lives in the
   simulation; render reads a one-way snapshot and can be removed entirely. (⇒ GPU-free headless,
   determinism, Factorio-class optimization.)
3. **Minimal kernel; everything is a package.** The kernel is ~6 interfaces; all features compose
   onto it and dead-code-eliminate out of builds that don't use them. (⇒ tiny builds, modularity.)
4. **Files are the project (EditorKernel only).** Authored state lives only in project files;
   EditorKernel is a restartable incremental derivation engine over them — the language-server
   model. Mutations are file writes; RPC serves queries/session actions/derived outputs.
   RuntimeKernel is exempt: it consumes only compiled derivation output — fed live in the editor,
   baked into packed binaries in builds. (⇒ AI-first authoring, git-native collaboration, crash
   safety, hot reload for free.) *(L-19…L-31, R-FILE-\*)*

---

## 3. Component breakdown

### 3.1 EditorKernel (headless authoring engine — library first, daemon second)
- **File-sync layer** — the single mutation surface is the project files themselves (L-19):
  atomic temp+rename writes, one serialized write queue, OS watcher as hint + content-hash
  reconciliation (L-22). Invalid/mid-edit files keep last-good derived state + machine-readable
  diagnostics (red-squiggle model, never auto-fix unasked).
- **Derivation graph** — incremental, content-hash-memoized: parse → validate → compose →
  instantiate; doubles as the asset-import cache; hot reload is this same pipeline (L-22).
- **Version-control integration** — git is the undo/history system (L-21): new Projects are git
  repos by default; EditorKernel stays correct under branch switches/checkouts/reverts via the
  reconcile pipeline; no engine undo/redo subsystem.
- **RPC surface** — queries, session actions (play/pause/step), derived outputs (build, import,
  screenshot); never mutates authored state.
- **Asset pipeline** — importers (packages), stable-ID asset database, per-platform transcoding;
  a set of derivation-graph nodes. Runs headless.
- **Build orchestration** — drives per-platform export via toolchain adapters; CLI-invokable.
- **Bridge server** — local IPC, single-instance lock, heartbeat/stale-lock recovery, multi-client fan-out.
- **Sandboxed-tier execution environment** (the R-FILE-005 stratification's VM home) — EditorKernel
  hosts the **constrained-ABI VM/WASM runtime** (the L-49 sandboxed tier) as a **component of
  EditorKernel itself**, not only of RuntimeKernel play sessions: package-shipped schema migrations
  (L-37 — sandboxed-tier-only) and definition-kind processing run in it **during derivation**. It
  boots **before pass-1 parsing**; the daemon cold-start order is: **lock → index → watcher → VM
  → registration (pass 0) → content parse (pass 1)**.
- **Capability probing** — detects display/GPU; degrades gracefully when absent.

### 3.2 RuntimeKernel (game execution)
- **Microkernel:** World (data-oriented ECS), fixed-timestep Scheduler, module registry, event bus, resource-handle registry, platform seam.
- **Simulation Core:** the kernel + simulation packages (logic VM, physics, gameplay systems). GPU-free.
- **Render Module (optional):** RHI + rendering packages + UI + audio presentation. Requires GPU.
- **Sim→render seam:** one-way snapshot / extract into a render world; double-buffered.

### 3.3 Clients
- **GUI:** desktop editor built with CEF (web UI) embedded in the C++ host; renders the 3D viewport natively; commits authored changes as file writes at gesture end (L-20); a detachable client.
- **CLI:** **embeds EditorKernel as a library** (R-FILE-008): attaches to a live daemon when present, else operates in-process with identical semantics. Mutation verbs are **file rewriters**; queries/session actions/derived outputs go over the RPC surface. Full parity with GUI.
- **AI/MCP:** agents edit files directly and drive the same RPC surface via the built-in MCP adapter (R-BRIDGE-007); headless-first; scoped attach tokens (R-SEC-007).

### 3.4 Package system
- **Engine packages (C++):** vcpkg (manifest mode, from-source — L-42), composed at build time, DCE'd for minimal builds.
- **Gameplay/content packages (npm):** TypeScript + assets + WASM modules; one-line JSON dependency.
- **EditorKernel** unifies both behind a single "add package" experience.

---

## 4. Technology stack (locked)

| Concern | Choice |
|---|---|
| Engine/systems language | **C++** (modern) |
| Default gameplay language | **TypeScript** (embedded JS VM, in-process, shared World; VM = **V8** for v1 desktop/server — **L-61** [spike-ratified 2026-07-02, owner]; the constrained-target no-JIT pick is deferred to the v2 iOS leg, re-running the spike battery incl. Hermes/JSC). **One embedded backend in v1** — the web target exercises the second-backend seam (the browser's engine); the second embedded backend returns with iOS in v2 [owner-ruled 2026-07-02] — multi-backend seam kept (proven in the M0 spike: two opposite memory-ownership models behind one seam with checksum parity) |
| Native-tier logic & module format | **WebAssembly** (per-platform JIT/AOT backend; scope per L-40 — the TS tier runs on a JS VM, not WASM). Runtimes locked by role (**L-62** [spike-ratified 2026-07-02, owner]): **wasmtime-Cranelift** in EditorKernel/dev; **WAMR-AOT or wasmtime-min+`.cwasm`** in shipped builds (packaging-time toggle; shipped WASM-tier footprint 0.6–1.1 MB); the web target runs the browser's own WASM engine |
| Optional performance/determinism tier | **C++ / WASM** (data-oriented, arenas, no-GC) |
| Rendering | **WebGPU** baseline (T1, all platforms incl. web — WebGL2 removed from v1 per L-56) + **T2 native** Vulkan/DX12/Metal for advanced |
| Native WebGPU backend | **wgpu-native** is the native T1 backend for **M0–M4 development** [spike-ratified 2026-07-02, owner] — pinned, SHA-verified official prebuilts (measured 7 s clean setup+build) vs Dawn's no-official-native-prebuilts, tens-of-minutes-to-hours from-source. **NOT a permanent lock**: Dawn is re-evaluable at M4 alongside the WGSL tool decision (the web leg is already Dawn-lineage via emdawnwebgpu, so both ecosystems stay continuously exercised); the **web build binds the BROWSER's WebGPU** (`webgpu.h`→JS shim), **not** a Dawn cross-compile. Note (premise updated 2026-07-15): **Dawn now has an official vcpkg port** (since ~2026-06, incl. a Tint-tools feature; microsoft/vcpkg#41847) as well as its official CMake build, but remains heavy from-source (~1–2 GB dependency fetch, tens-of-minutes builds) and publishes no official native prebuilts; **wgpu-native still has no vcpkg port** (official SHA-pinned prebuilts only). *As-built through M7: wgpu-native v29.0.1.1 (SHA-pinned prebuilts) is the backend; the promised M4 Dawn re-evaluation was not performed — re-homed to M8* (ROADMAP.md §5 risk row); the narrow **signed-prebuilt exception** (verified per R-SEC-009) stands for build-hostile heavy libs like this, as a deliberate carve-out from L-42 from-source |
| Shader toolchain | **glslang/DXC → SPIR-V → SPIRV-Cross** (HLSL/MSL/GLSL backends); **SPIR-V→WGSL via Tint or Naga** (tool choice = M4 deliverable, **not locked**; sole web path post-L-56 — flagged maturity risk, ROADMAP.md §5; M0 spike evidence [spike-ratified 2026-07-02, owner]: the same shader through Naga (native) and Tint (Chrome) rendered **byte-identical images** — divergence risk low). Shader compilation + variant generation run as **derivation-graph nodes** (cached, keyed per R-FILE-010 — R-REND-005) |
| Advanced graphics | modular packages: RTX-class RT, FSR/DLSS/XeSS upscaling, GI, virtualized geometry — **v2** (ROADMAP.md §3) [owner-ruled 2026-07-01] |
| 2D | first-class in v1 (L-55): sprite path in the M4 renderer, tilemap asset kind + 2D viewport mode, 2D physics package |
| Engine package manager | **vcpkg** (manifest mode; from-source + pinned Clang toolchain + shared artifact cache — L-42) |
| Content package manager | **npm** |
| Editor UI | **CEF** (Chromium) embedded in the C++ host |
| Runtime UI | **pluggable backend** behind a UI-Provider contract (engine-integrated default; CEF/minimal options) |
| ECS | data-oriented / archetype: **custom archetype/SoA World core, in-kernel** (**L-60** [spike-ratified 2026-07-02, owner]); **flecs = design reference + fallback**; **EnTT rejected** (paged sparse-set → no row-aligned columns → no zero-copy views; compile-time typing → no runtime-registered layouts) |
| Physics (3D) | package — **as-built at M6**: custom fixed-point (Q16) deterministic `physics3d` package; the **Jolt-class strict-FP route was evaluated and REJECTED** at M6-F0a (engine `docs/physics-determinism-decision.md`, 2026-07-11; pairs with L-54/R-SIM-005) |
| Physics (2D) | package — **as-built at M6**: custom Box2D-class fixed-point `physics2d` package (L-55/R-2D-002; same Q16 determinism substrate as 3D) |
| Spatial acceleration | package — **as-built at M4/M6** (engine `src/packages/spatial`): incrementally-updated broad-phase shared by render culling, spatial queries, and streaming (R-SIM-007 — a package the render/query paths depend on, **not** kernel core) |
| Audio | **miniaudio** default package; optional FMOD/Wwise/Steam Audio integrations (L-46) |
| XR | OpenXR (compositor layers for world-space UI) — **v2** (ROADMAP.md §3) [owner-ruled 2026-07-01]; screen-space + basic render-to-texture world-space UI stay v1 (R-UI-003) |

See `DESIGN-DECISIONS.md` for rationale and the still-open sub-decisions.

---

## 5. Key data & control flows

Per the drift-pair rule, these flows point at the normative R-* requirements rather than
mirroring their full texts — `REQUIREMENTS.md` is the single normative home.

### 5.1 CLI → running editor (single-instance bridge)
1. Client computes a Project key (canonical path hash).
2. Reads `<project>/.editor/instance.json` (pid, endpoint, engine + protocol versions).
3. If a live daemon responds → **capability-negotiation handshake (R-CLI-010** — the normative
   home; it supersedes the R-BRIDGE-006 version-equality check**)**: client and daemon exchange
   `{ protocolMajor, supportedCapabilities[], minClientProtocol }`. **v1 behavior is hard-fail on
   mismatch** (one released protocol, `protocolMajor=0` until the M3 freeze — R-CLI-004) with the
   machine-readable R-CLI-008 error — no silent fallback, no second daemon; negotiation to a
   capability subset activates at the second released protocol version. Success → forward the
   command, exit.
4. Else acquire OS advisory lock, spawn EditorKernel (in-process library or daemon), write
   instance file, connect.

### 5.2 Play-in-editor
- EditorKernel instantiates the **shipping** RuntimeKernel with a selected **platform profile**
  (input model, DPI, GPU tier, memory caps, frame pacing) + device emulation. Same code as the build.

### 5.3 Sim → render
- Simulation writes component data; an **extract** step copies render-relevant state into a
  render world (double-buffered); the render module reads that snapshot on its own thread.
- Headless: the render module is not loaded; the extract/render steps do not run.

### 5.4 Cross-language systems
- The scheduler holds systems as `(component-access query, executor)`; the executor is native
  C++ or "invoke the WASM/JS VM." Data is shared via zero-copy views; boundary crossed once per
  system per frame.

### 5.5 Logic portability (locked — L-40)
- **TS tier:** author TS → bundle/transpile to JS → embedded JS VM on every platform. v1 ships
  **one embedded JIT-capable backend** plus the browser's own engine on the web target; the
  "interpreter where JIT is banned" leg (iOS) and the interpreter-mode throughput floor are **v2
  with iOS** (R-BUILD-001 / L-40 / R-LANG-011) [owner-ruled 2026-07-02]. TS does **not** compile
  to WASM in v1. TS systems run on a single scheduler lane (R-LANG-011); the v1 throughput floor
  is measured and published on the wedge platforms' shipped VM configuration.
- **WASM tier:** native/perf-tier modules (C++/Rust/AssemblyScript) and sandboxed third-party
  logic compile to WASM; RuntimeKernel selects JIT or AOT per platform — **on the web target the
  BROWSER compiles the WASM module itself** (no engine-side AOT for web — R-LANG-005). With iOS
  in v2 and consoles WON'T (v1), **no v1 target needs native AOT** — the WASM→native AOT
  toolchain spike (wamrc / wasm2c+clang / Cranelift-class) and its size+throughput acceptance bar
  move to v2 with iOS (R-LANG-005) [owner-ruled 2026-07-02]; the WASM module format and the
  JIT/AOT selection seam stay v1 invariants.

### 5.6 Authoring mutation (any client — GUI, CLI, or AI text edit)
1. Writer performs an atomic file write (GUI commits at gesture end; CLI verbs are file
   rewriters; AI edits text directly).
2. Watcher hints + content-hash reconciliation mark the file dirty.
3. The derivation graph recomputes only the dirty subgraph (parse → validate → compose →
   instantiate); diagnostics update.
4. Change events fan out to all attached clients; the embedded RuntimeKernel live-swaps affected
   resources via handle invalidation (= hot reload).
5. Shipped builds skip all of this: the build pipeline freezes derivation output into packed
   binaries that RuntimeKernel loads directly (L-24).
