# Context Game Engine

> **Name:** Context Game Engine ("Context") — chosen 2026-07-01.
> **Status:** Design phase CLOSED (2026-07-02, after five adversarial review rounds; final
> fresh-eyes verdict: **build-with-changes**, plan re-sequenced in `ROADMAP.md`). **All six M0
> spikes are COMPLETE and owner-ratified** [spike-ratified 2026-07-02, owner] — evidence in the
> engine repo's `spikes/*/FINDINGS.md`; resolutions encoded as L-60/L-61/L-62 + the L-41 tree +
> the R-LANG-008/009 amendment (`DESIGN-DECISIONS.md`). This folder is documentation only — no
> engine code lives here.
> **Location (2026-08-27):** this whole design category (`../README.md` and siblings) now lives
> inside this repo's own `.taskflow/2026-07-01-context-engine/` — previously tracked in the
> `software` workspace superproject at `.claude/design/context-engine/`. "The engine repo" below
> refers to `IvanMurzak/Context-Engine`, i.e. **this repo** — the design docs and the engine source
> (`src/`) are now siblings in the same repo, not cross-repo references.
> **Implementation status (2026-07-18):** 🏁 milestones **M0–M8.5 are COMPLETE** in the engine repo
> (`IvanMurzak/Context-Engine`) — **v1 wedge SCOPE-COMPLETE**. M8 (build pipeline, a01–a14) landed
> 2026-07-17; **M8.5 (wedge hardening, a15–a23) landed 2026-07-18** — the a23 exit gate (CE #302,
> 9 blocking `m85-exit-*` gates green on the 3-OS matrix) closes it; `ops1` (perf-runner) deferred →
> density numbers advisory-until-provisioned. A three-reviewer design review of M8/M8.5 ran 2026-07-15
> (`REVIEW-R6-2026-07-15.md`); the milestone was decomposed into 24 immutable specs (now in the sibling
> `../m8-build-pipeline/` + `../m85-wedge-hardening/` folders); live
> status board in `ROADMAP.md` §9. This folder remains
> the design authority; per-milestone as-built deltas live in the engine repo's `docs/`.
> **Open items (owner-level):**
> 1. **§1c material/shader detail** — the **WGSL tool half RESOLVED at M4 = Tint** (measured on
>    the real 36-module shader corpus: Tint 36/36 byte-deterministic vs naga 20/36; naga retained
>    as the second validator in CI — engine repo `docs/wgsl-tool-decision.md`). M4 shipped the
>    material IR + contract behind the `IShaderCompiler` seam with the
>    glslang→SPIR-V→SPIRV-Cross/Tint chain as cached derivation-graph nodes; the node-graph
>    authoring frontend remains open, post-v1 (`DESIGN-DECISIONS.md` §1c; R-REND-005).
> 2. **Funding/headcount** — for the recorded ~20–30 engineer-year v1 estimate (`ROADMAP.md` §4).
> 3. **EULA counsel review** — the counsel-reviewed LICENSE is an **M0 gate** before the first
>    public push (L-57; business-dept task). *Update 2026-07-13:* the repo went public with the
>    v0.2.1 EULA draft; counsel engagement remains parked at the CEO spend gate — item stays open.
> 4. **Context Sim + signing key** [updated 2026-07-15] — owner-ruled: **no pre-release for
>    now** (local dev/testing continues; the owner calls the release moment — `ROADMAP.md`
>    §2/§7). The R-SEC-009 **production signing key is MINTED** (2026-07-15, operator `.secrets`
>    store); pinning its public half into the engine trust root + the fetch-verify wiring land
>    at M8.
> All design history and per-round provenance: the `REVIEW-*.md` trackers (rounds 0–6).

## Documents

| File | What it is | Read it to… |
|---|---|---|
| **`REQUIREMENTS.md`** | The authoritative, ID'd, prioritized requirements list — the single normative home | **Review and approve what the engine must do** |
| **`ARCHITECTURE.md`** | Stable: vision, beachhead, principles, component breakdown, data/control flows, tech stack | Understand how it all fits together |
| **`ROADMAP.md`** | Volatile: milestones M0–M8.5, the "Context Sim" pre-release track, v2 scope, effort/economics, risks, CI tiering | Understand the build order and sequencing |
| **`DESIGN-DECISIONS.md`** | The decision record: locks L-1…L-62 with rationale, plus the one remaining open item (§1c's authoring-frontend half — the WGSL tool half resolved at M4 = Tint) | See why each choice was made; veto by ID |
| **`IMPLEMENTATION-PLAN.md`** | Stub — split 2026-07-02 into `ARCHITECTURE.md` + `ROADMAP.md`; kept for external links | — |
| **`REVIEW-*.md`** (7 trackers) | The full provenance record: every round's findings, owner rulings, and remediation status | Audit how the design got here |
| **`../m8-build-pipeline/` + `../m85-wedge-hardening/`** (24 specs + index) | Immutable M8/M8.5 task specs (groups, coefficients, model hints); live state ONLY in `ROADMAP.md` §9 | Dispatch the remaining v1 work |

## The engine in one paragraph

A minimal-kernel game engine where **every feature is a package** (so builds scale from a tiny
headless simulation to a full RTX-class 3D game — and **2D is first-class**, not an afterthought).
**Project files are the single source of truth**: a headless **EditorKernel** is a live derived
index over them (the language-server model) — every authored mutation is a file write, and
**GUI, CLI, and AI agents** are equal clients over one RPC/event surface — so the engine runs
with **no GPU** on cheap machines. The **human editor experience is first-class**, not a
second-class shadow of the AI surface: v1 ships the **observer-grade core** — viewport, play
controls, scene tree, an inspector whose edits are override writes, a Problems panel, session
undo — with the asset browser at v1.x, richer in-viewport authoring trailing within v1, and the
git-history UI + project launcher at v2 (§14c R-HUX-*). **A complete game is buildable with zero
AI usage** (the ai-game.dev subscription is a pricing lever, not a tooling gate). Game logic is
written **once in TypeScript** (npm ecosystem) and runs on every platform via an **embedded JS
VM** (JIT where legal — the no-JIT interpreter leg arrives with iOS in v2); performance-critical
code drops to the **C++/WASM native tier**. Runtime scale rests on committed, CI-enforced floors —
a shared **spatial acceleration structure** (culling + queries + streaming), a **JS-tier GC
budget**, a **TS frame budget**, and a per-platform **minimum-spec floor**. **What you play in
the editor is what ships**, because the editor embeds the same **RuntimeKernel** the build uses.
**UI is web technology (HTML/CSS/TS)** with a **pluggable backend**; v1 ships screen-space plus
basic render-to-texture world-space UI, and the XR-grade stack (OpenXR layers, raycast input)
builds on that same seam in v2. The engine is **source-available under the proprietary Context
Engine EULA** — the repo is **public from day one** — with a **2% royalty on gross revenue per
product above $200,000 per year** (annual threshold, resets yearly; marginal — applies only to
revenue above the threshold; the base is **gross receipts** — storefront/platform fees are not
deducted — and the royalty is **unconditional**: no subscription waiver of any kind — the EULA is
fully decoupled from ai-game.dev [owner-ruled 2026-07-03] (L-57)). Put plainly: the engine is
**free under $200k/year — the full engine, no subscription required, zero AI usage required**
(R-HUX-009); the subscription buys **AI usage**, not the engine, and never affects the royalty.

## v1 at a glance

| | |
|---|---|
| **Platforms (v1)** | Windows + macOS + Linux desktop · Linux server/headless · Web (WebGPU-only). **Android** is a trailing SHOULD (ships when the wedge is served; never blocks wedge milestones). **iOS is v2** [owner-ruled 2026-07-02]. |
| **Milestones** | **M0** foundations & six spikes → **M1** microkernel + file-authoritative EditorKernel skeleton → **M2** data model & asset pipeline → **M3** scripting/logic tier (contract freeze) → **M4** render module + RHI (T1 WebGPU) → **M5** observer-grade editor GUI + play-in-editor → **M6** core engine-system packages → **M7** runtime UI system → **M8** build pipeline (Linux/Windows/Web adapters first) → **M8.5** wedge hardening. **"Context Sim"** — the headless deterministic sim pre-release for RL/server-sim users — was designed as a post-M3 track but did not run (single-lane execution); shipping it as a product off the M8 build pipeline is an open owner item (`ROADMAP.md` §2/§7). |
| **MUST highlights** | File-authoritative, headless-complete, CLI-complete EditorKernel (the moat); deterministic mode on the wedge platforms with the CI state-hash gate; 2D first-class; the 100k-file scale envelope; the self-describing public contract (R-CLI-007…017; `protocolMajor=0` until the M3 freeze); first-party release signing, fail-closed. |
| **Deferred** | To **v2**: iOS, advanced graphics (RT, upscaling, GI, virtualized geometry), the XR-grade UI stack, the GUI history timeline + launcher, marketplace-grade trust (TUF/Sigstore, third-party native packages). Within **v1**: asset browser at v1.x; tilemap painting GUI + in-context viewport composition editing trail post-M5 (the M8.5 trailing-GUI bucket). |
| **Business model** | Source-available proprietary Context Engine EULA; public repo from day one; the engine is free — no subscription, no AI usage required. 2% royalty only on gross revenue per product above $200k/year (annual, resets yearly, marginal; gross-receipts base — no storefront/platform-fee netting; unconditional — the subscription waiver is removed, the EULA fully decoupled from ai-game.dev [owner-ruled 2026-07-03]) (L-57). |

## The durable moat

The moat is the **file-authoritative + headless-complete + CLI-complete core**: authored truth
lives in files (L-19), every capability runs with no GPU and no GUI (R-HEAD-001, R-CLI-001), and
the whole surface is one self-describing contract (R-CLI-007…017). Incumbent engines **cannot
retrofit these properties without re-architecting** — their scene truth lives in editor memory and
binary formats, and their tooling assumes a GUI process. The **MCP adapter is table stakes, not
the moat**: we ship MCP plugins for Unity/Godot/Unreal ourselves (the existing plugin family), so
"has an MCP server" differentiates no engine — which is exactly why the moat must sit below the
adapter, in the architecture. The demo no incumbent can match: **an agent builds, runs, verifies,
and ships a game with zero GPU and zero GUI.**

## Why a new engine and not MCP-on-Unity?

Because the differentiating capabilities are ones incumbent architectures forbid, and an MCP
layer cannot add them from the outside:

- **True headless-complete.** EditorKernel runs every authoring capability on a GPU-less,
  display-less host (R-HEAD-001); incumbent editors require a GPU/display context for core paths.
- **Files as the single source of truth.** An agent's file edit IS the mutation (L-19); an MCP
  plugin on an incumbent mediates a live in-memory scene it does not own, reconciling against —
  and racing — the editor's private state.
- **GPU-free parallel agent authoring.** Daemon-per-worktree (L-26) + the structural merge driver
  (R-FILE-012) let N agents author the same game in parallel on commodity boxes; incumbents run
  one GUI editor instance per seat.
- **Native determinism.** A designed-in deterministic tier with a CI state-hash gate (L-54);
  bolting determinism onto an incumbent's runtime from a plugin is not achievable.

Our own Unity/Godot/Unreal MCP plugin family is the proof-by-experience: an MCP adapter reaches
only what the host engine's architecture chooses to expose.

## Target beachhead [owner-ruled 2026-07-01]

v1 targets a deliberate wedge — the segments where the moat above is decisive today:

1. **Headless AI-training / RL environments** — deterministic, GPU-free, massively parallel
   simulation on commodity machines (R-HEAD-*, L-54).
2. **Deterministic server-authoritative simulation & multiplayer** — the native/WASM deterministic
   tier + netcode hooks (L-18/L-48/L-54) serving lockstep and state-sync server sims.
3. **AI-generated 2D and simple-3D games** — the agent authoring loop over 2D-first-class (L-55)
   and baseline-3D PBR content.

Target platforms for the wedge: **desktop + Linux-server + web**. **"General 3D engine" is the
destination, not the day-one claim** — fidelity, XR, and breadth arrive post-wedge (`ROADMAP.md`
§3). This beachhead is the **priority lens for milestone trade-offs**: when scope pressure hits,
work serving the wedge outranks general-engine breadth.

## Acknowledged competitive gaps at v1

Consciously accepted trade-offs, each with the reason it is acceptable — not blind spots:

| Gap | Why it is acceptable at v1 |
|---|---|
| **Rendering fidelity out-of-box** — advanced graphics (RT, upscaling, GI, virtualized geometry) are v2, not defaults | The wedge is headless/AI-first authoring of 2D and simple-3D games, not AAA visuals; fidelity arrives as opt-in packages without blocking the core loop |
| **Not open source** — proprietary source-available Context Engine EULA, not MIT/Apache — **Godot wins the FOSS-purist cohort** | A conscious owner decision: **accepted for ownership + royalty enforceability** over the grassroots/MIT distribution lever; mitigated by public-from-day-one source and the loudly-marketed free tier — free under $200k/year, full engine, no subscription, no AI required |
| **Asset-store maturity** — no marketplace; npm solves **code**, not assets (R-PKG-001) | Cold-start is deliberately seeded: first-party starter packs + the R-QA-006 maintained runnable templates; marketplace hosting is post-v1 (O-5) |
| **Console reach** — consoles are WON'T (v1) (O-4) | Console SDKs demand NDAs + per-title certification; the initial audience (desktop / Linux-server / web) does not need them; the platform seam keeps the door open post-v1 |
| **Middleware depth** — a thin third-party integration catalog vs incumbents' decades of it | The package contract (R-KERNEL-004) + optional FMOD/Wwise/Steam-Audio integrations (L-46) provide the seam; depth accretes with the ecosystem rather than gating v1 |
| **Learning content** — no tutorial library, courses, or community corpus | In-editor contextual help + maintained samples (R-HUX-010, R-QA-006) are the v1 floor; the primary early audience (agents) learns from `context describe` + the few-shot corpus, which ships with the engine |
| **Engine maturity** — no shipped-title track record | Unavoidable for any new engine; mitigated by CI-enforced budgets/floors (R-FILE-011, R-LANG-012, R-QA-007, R-BUILD-006) and by the engine building and smoke-running its own samples headless in CI from day one (R-BUILD-009) |
| **Editor depth at v1** — the v1 editor is **observer-grade** (viewport, play controls, scene tree, inspector, Problems panel); the GUI history timeline + launcher are v2, the asset browser v1.x, the tilemap painting GUI trails post-M5 | The wedge authors through agents + CLI first, and every capability is CLI/RPC-complete now (R-CLI-001) — the deferral changes presentation, not capability; R-HUX-009 stands (a complete game buildable with zero AI, via GUI + CLI, on the shipped surface) |
| **Marketplace-grade security is v2** — v1 ships **no third-party native packages**; first-party release signing (one pinned publisher key, fail-closed) rather than the full TUF/Sigstore root; consent UX, third-party attestation verification, and hostile-extension enforcement follow with the marketplace | v1's honest surface is first-party + sandboxed TS/WASM; the fail-closed verify-before-use gate ships from day one and the TUF upgrade slots behind the SAME gate (R-SEC-009) — the trust MODEL is unchanged, only its subjects |
| **Mobile reach at v1** — iOS is v2-first; Android is a trailing SHOULD [owner-ruled 2026-07-02] | Neither serves a wedge pillar; iOS alone dragged ~1+ engineer-year of toolchain (AOT, second VM backend, provisioning, mobile floors); the cheap-later invariants (WASM format, chunked pack, VM seam, platform seam) ship in v1 so mobile lands on existing seams |
| **RL raw-throughput gap** — vs GPU-vectorized simulators (Isaac / Brax / Madrona-class), Context does **NOT** compete on raw samples/sec | The pillar-1 pitch is **agent-authored environments + CPU-parallel commodity orchestration** — many cheap headless instances, no GPU — not batched-GPU physics throughput; committed ticks/sec/instance + instances-per-box targets are committed no later than the M8.5 exit (R-FILE-011; the Context Sim pre-release anchor was superseded — `ROADMAP.md` §2) |
| **TS single-core scripting ceiling** — TS gameplay throughput is single-core-bound (one JS VM = one thread) and does not scale with core count | Stated as contract, not buried in L-38: scaling comes from promoting hot systems to the **native/WASM tier** — the designed escape hatch (R-LANG-006/011/012) — and the CI-enforced TS frame budget keeps the single-lane floor honest |
| **CEF weight tax** — the desktop editor carries Chromium's binary size (measured ≈ 389 MB ship payload at CEF 149 [spike-ratified 2026-07-02, owner]; ~50 MB of all-languages locales is prunable) and its security-update cadence | A conscious L-15 trade for full web fidelity in the editor; the editor is a detachable client — the headless core and shipped games carry none of it (R-UI-007), and runtime UI backends stay pluggable (R-UI-002/008) |

## Locked technology stack

C++ core · TypeScript gameplay on an embedded JS VM (V8 in v1 — L-61) · WASM native-tier/module
format (wasmtime-Cranelift dev / WAMR-AOT-or-wasmtime-min shipped — L-62) · WebGPU-only web +
tiered RHI (T1 WebGPU / T2 native) · 2D first-class (sprites, tilemaps, Box2D-class physics) ·
npm + vcpkg · CEF editor UI · pluggable runtime UI · data-oriented ECS (custom archetype/SoA
core — L-60) ·
shared spatial acceleration index (culling + queries + streaming, R-SIM-007) ·
microkernel + packages · sim/render split · file-authoritative EditorKernel with a derived-world
generation + `settled` quiescence + incarnation-epoch event stream (R-BRIDGE-008) ·
first-class human editor experience layer (§14c R-HUX-*; v1 = the observer-grade editor) ·
CI-enforced perf floors (JS-tier GC budget, TS frame budget, per-platform min-spec floor) under
one published perf-gate methodology + CI fleet manifest, with a designed test system
(R-QA-008…012) · cryptographic trust root + signed artifacts, verify-before-use (R-SEC-009 /
L-58) · self-describing public contract — verb grammar, uniform error catalog, capability
negotiation, CLI≡RPC≡MCP parity (R-CLI-007…017) · source-available, public-from-day-one Context
Engine EULA with a 2% royalty above $200k/year (annual threshold, marginal — L-57).

## How to review

1. Go through `REQUIREMENTS.md`; for anything to change, cite the `R-*` ID.
2. `DESIGN-DECISIONS.md` is fully locked except **§1c**'s remaining half (the node-graph
   authoring-frontend detail, post-v1; the WGSL tool half resolved at M4 = Tint). Any lock may be
   vetoed by ID.
3. `ARCHITECTURE.md` and `ROADMAP.md` show how it fits together and the build order.
4. The `REVIEW-*.md` trackers hold every round's findings, rulings, and remediation status —
   consult them for why any passage reads the way it does.

## Name

**Context Game Engine** (short: **Context**) — chosen 2026-07-01.

Component naming [owner-ruled 2026-07-02]: **EditorKernel** (formerly "EditorCore") and
**RuntimeKernel** (formerly "RuntimeCore") — renamed across all design docs; pre-rename merged
PRs and early tracker text may still use the old names.

Follow-up: the bare word "context" is taken across npm / GitHub / domains (and collides with the
graphics "rendering context" and React/Android `Context`), so claim a distinct namespace early —
e.g. npm scope `@context-engine/*`, GitHub org `context-engine`, domain `contextengine.dev`.

---

_This is a living design. Update these files as decisions are made; keep `DESIGN-DECISIONS.md`
in sync when an open item becomes locked._
