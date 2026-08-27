# M7 — Runtime UI system (pluggable): single-lane task decomposition

> **AMENDED + TASKS DESIGNED (2026-07-13).** The four §Owner-checkpoints below were RULED
> 2026-07-13: **(a)** TS retained-tree API + CSS-like style props is the v1 authoring form;
> **(b)** backends beyond null + engine-integrated defer to trailing-v1/v1.x; **(c)** text is
> **FULL shaping-grade** (HarfBuzz-class shaping; bidi included per the R-UI-005 grouping — that
> inference and the IME-stays-`false` disposition are both flagged for owner veto in
> `tasks/README.md` §rulings) in M7 — amends T7, split into a7+a8; **(d)**
> world-space RTT **includes curved surfaces** (mesh UV raycast→UV→events) — amends T8, split
> into a9+a10. Rulings also recorded in `../core/ROADMAP.md` §M7. The implementable
> breakdown (12 specs with importance/complexity + status board) lives in
> [`2026-07-13-m7-runtime-ui/`](2026-07-13-m7-runtime-ui/ROADMAP.md) — execution hold state
> lives ONLY in that ROADMAP's §Human-approval gates.
> **Adversarially reviewed 2026-07-14** (3 independent reviewers: code ground-truth / spec +
> external conformance / consistency); all confirmed findings applied — the body below is
> post-ruling, post-review text.

> Decomposition 2026-07-13 (architect pass, verified against the checked-out CE repo at
> `engines/context/Context-Engine`). Feeds M7 task files + `implement-task` dispatch — PLANNING ONLY.
> Design authority: `../core/ROADMAP.md` §1 M7 + §3 v2 XR deferral; REQUIREMENTS §14
> R-UI-001/002/003/005/006 (+R-UI-008 deferral call, R-A11Y-002 seam); DESIGN-DECISIONS L-16/L-45/L-53.
> Owner rule: CE tasks run **1-at-a-time** (2026-07-05) — this is a strict sequence, not waves.

## Scope (honest, against design + as-built)
**IN (v1/M7, per owner rulings 2026-07-13):** UI-Provider contract + capabilities (GPU-driver,
damage repaint, GPU-composited transforms/opacity, headless logic — R-UI-005); engine-integrated
default backend; screen-space overlay + world-space render-to-texture UI — **flat panels (a9)
AND curved UV-mapped surfaces (a10, ruling d)** (R-UI-003, L-16 non-XR raycast→UV→events);
**shaping-grade text — HarfBuzz-class shaping + bidi (ruling c; a7+a8)**.
**Exit:** in-game HUD + a world-space RTT panel; UI driven/asserted headless via CLI (R-UI-006).
**OUT (v2, owner-ruled 2026-07-01):** the XR-device leg — OpenXR compositor layers,
controller/gaze/hand-raycast input, stereo — R-UI-004 + XR-grade parts of R-UI-003/L-16
(pointer raycast→UV on curved meshes is IN per ruling d).
**OUT (deferred):** optional CEF-runtime + minimal backends (R-UI-008 SHOULD, ruling b; L-53
says v1 capability-matrix *rows* are desktop+web — rows ≠ extra backends); HTML/CSS-file parsing
in the default backend (ruling a — arrives with the CEF backend); **IME** (declared-`false`
capability — OS text-entry integration, no text-input widget in the M7 exit scope; flagged for
owner veto in tasks/README); vertical text / justification / hyphenation (declared out of the
shaping scope); R-A11Y-002 runtime accessibility package (post-core, D2 keeps the seam).

## As-built seams (verified in the repo)
- **`src/editor/gui/uitree/`** (`context_gui_uitree`, M5-F0b) — headless *editor* UI-logic tree:
  `UiNode` (ARIA `Role` vocab, label/focusable/command, `render_html()`), `Panel`+`Command`, a11y
  audit. Pure stdlib. It has **no layout, no styling, no event/handler model, no damage tracking,
  no data binding** — it is an a11y/logic skeleton for editor panels, namespaced
  `context::editor::gui::uitree` under `editor/` (not shippable in an exported game).
- **`src/render/`** — L-39 extract (`extract.h`: `const World&` → double-buffered
  `RenderSnapshot`; render never mutates sim); `render_world.h` render-side components (float
  `Transform`/`Renderable`/`PbrMaterial` — texture fields are opaque handles, **no dynamic-texture
  registry exists yet**); `sprite/` 2D path (ortho camera, painter's-order sort layers, same-atlas
  batching); `offscreen_scene.h` — **RTT + readback proof exists** against `rhi.h` (fake backend =
  no-GPU ctest; wgpu backend = CI). Golden corpus scenes (`triangle3d/sprite2d/lit3d/viewport`)
  are **hand-listed in ci.yml** (render + render-web jobs) with committed `goldens/` baselines.
- **`src/packages/input/`** (`context_input`, M6 P7) — the **L-45 UI-capture stack already
  exists**: layered `InputContext`s (`gameplay`/`ui`), capturing UI contexts swallow unbound
  events; `InputRouter::route()` is a pure function RETURNING `session::TickInputs`/
  `ActionActivation` — the CALLER injects into the ONE sim input sink (`Session::inject_action_at`
  → `InputState`; samples carry that glue today, a3 owns the UI-side wiring); the package
  registers **no sim component** (test_feed.cpp). M7 plugs into this — it does NOT build new
  input arbitration. R-QA-005 note (D5/D6): record/replay records at the POST-arbitration sim
  sink (`TickInputs`/`ActionActivation`), never raw pointer events — replay must not depend on
  presentation-tree hit-testing state.
- **`src/runtime/js/` + `src/runtime/ts/`** — real V8 host (`bindHostFunction`, zero-copy
  views) + esbuild/tsgo TS toolchain. **V8 selection is toolchain AUTO-DETECT, not a CMake
  toggle** (js/CMakeLists.txt: MSVC/clang 64-bit → real V8; local GCC → stub; inverse escape
  hatch `CONTEXT_JS_FORCE_STUB`); tests branch at RUNTIME on `v8BackendAvailable()` — the
  m6-exit-2 pattern. The R-UI-001 TS authoring surface rides these.
- **`src/tests/integration/`** — exit-gate pattern: `test_m6exit*.cpp` (m2/m5 have `m*_exit_test.h`
  helper headers; m6 does not — a12's `m7_exit_test.h` is a new file, not a copy), riding
  sample games (`samples/platformer-2d`, `samples/roll-3d`) through the session surface headless.
- **No raycast and no runtime mesh/UV data exist** (verified): `src/packages/spatial/` exposes
  ONLY `query_aabb`/`query_radius` (broad-phase); physics3d colliders are sphere/box; repo-wide
  grep for ray APIs = zero hits; `Renderable.mesh_id` is an opaque "later wave" handle and lit
  proofs bake geometry into WGSL. a10's hit→UV chain (ray traversal + ray-vs-triangle + UV
  interpolation + a mesh/UV data seam) is GREENFIELD — spatial serves only broad-phase pruning.
- **Web golden target is a compiled subset**: `src/render/web/` builds only triangle3d+sprite2d
  today; lit3d/viewport goldens are native-only blocking per `goldens/manifest.json` ("browser
  coverage joins when the lit web proof lands"). A web golden for a NEW scene requires extending
  the Emscripten target's sources + `web_main.cpp` + the harness scene list — not just ci.yml.
- **CI (`.github/workflows/ci.yml`)** — build job builds **every** target via `--preset dev` (no
  Not-Run risk for exit gates — the m6 precedent comment at ~L216); the general ctest step's `-E`
  exclusion regex (L133) and a named per-gate `ctest -R` step must BOTH change for a new gate
  family; the `render` job builds **only** `--target context_render_wgpu_offscreen` and the
  golden dump/compare lists are hardcoded (L812-827) — new scenes edit ci.yml; `render-web`
  mirrors via `tools/web_golden_run.py`. Gates register in `docs/ci-fleet-manifest.json`.
- **No text/font rendering exists anywhere in the engine** (grep: only a miniaudio false hit).
  Text for the HUD is greenfield — budgeted as its own task below.

## Load-bearing design decisions (make in T1, cheap to state, expensive to retrofit)
- **D1 — UI-Provider contract shape.** A backend-agnostic seam header in the UI package (the
  `JsEngine`/`TsToolchain` seam precedent): provider consumes the retained tree + damage list,
  reports a `Capabilities` struct (gpu_driver, damage_repaint, composited_transforms, text_shaping,
  bidi, ime — R-UI-005); engine negotiates + falls back (no damage support ⇒ full repaint). Two
  in-repo providers prove pluggability (R-UI-002): the **null/headless provider** (R-UI-006) and
  the **engine-integrated GPU provider** (T6).
- **D2 — runtime UI tree is NEW, not `context_gui_uitree`.** The editor tree is editor-scoped
  (under `src/editor/`, ships with the editor, no layout/events/damage). Runtime UI must ship in
  exported games and needs layout+events+damage+binding. Verdict: new runtime-grade tree in
  `src/packages/ui/`, **borrowing the design** (fluent API, closed role vocabulary so R-A11Y-002
  can hook it post-core) but **zero link-level sharing** — editor code never becomes a runtime
  dependency. A later editor-tree unification is explicitly out of M7.
- **D3 — package placement split.** Headless logic = `src/packages/ui/` (`context_ui` STATIC,
  composes on session/kernel like `context_input`; kernel never links back — L-60). GPU backend =
  `src/render/ui/` (needs `rhi.h`; render module already "requires GPU" per ARCHITECTURE §2). The
  provider contract header lives in the package; `src/render/ui/` implements it.
- **D4 — world-space RTT rides the existing seams.** Panel = UI tree rendered into an offscreen
  texture (the `offscreen_scene.h` machinery generalized to a persistent per-panel target) bound
  to a world quad via a render-side `UiPanel` component picked up by the L-39 extract. Requires
  the first **dynamic-texture registry** entry (the "later wave" the `render_world.h` handle
  fields reserved). Flat quads first (a9); **curved UV-mapped meshes join in M7 per ruling d
  (a10)** — the same RTT seam is what v2 XR builds on (L-16).
- **D5 — input arbitration is L-45 consumption, not invention.** UI focus installs/activates a
  *capturing* `ui` InputContext; pointer events hit-test against the headless tree's computed
  rects; UI-originated gameplay intents emit `ActionActivation` into the ONE `InputState` sink.
- **D6 — determinism posture: UI is presentation.** The UI tree lives OUTSIDE the sim `World`;
  `context_ui` registers **no hashed sim component** (the `context_input` test_feed precedent);
  sim→UI is read-only (state queries / snapshot data binding); UI→sim is ONLY the action path.
  `hash_world` must be bit-identical with UI enabled vs absent — asserted by the exit seam
  checklist. Render-side `UiPanel` components are float presentation state, like
  `render::Transform` — never hashed.

## Task sequence (single-lane; each PR ships its tests — R-QA-013)

### T1 — `context_ui` foundation: retained tree + events + damage + UI-Provider contract + null provider  (L)
**Goal:** `src/packages/ui/` package skeleton: runtime `UiTree`/`UiNode` (role vocab, style props,
visibility/opacity/transform), event+handler model (pointer/focus/key/custom), dirty/damage
tracking computed IN the tree (any backend consumes it), the `UiProvider` contract header +
`Capabilities` struct, and the null provider (logic-only, zero render cost).
**Ids:** R-UI-002, R-UI-005 (capability enum), R-UI-006. **Locks D1/D2/D3/D6.**
**Files:** `src/packages/ui/{CMakeLists.txt,README.md,include/context/packages/ui/*,src/*,tests/*}`;
one `add_subdirectory(packages/ui)` line in `src/CMakeLists.txt` (no per-packages CMakeLists exists).
**DoD:** headless ctests (`ui-*` family): tree build/mutate, handler dispatch, damage coalescing,
provider negotiation/fallback table, null-provider zero-cost assertion. Local dev gate green (pure
stdlib, all 3 OS legs). **CI:** none beyond default — build job builds all targets; new `ui-*`
ctests auto-run in the general step (prefix taxonomy note added to CLAUDE.md test table).

### T2 — Headless layout + hit-testing  (M/L)
**Goal:** computed geometry without a GPU: anchored/absolute positioning + a stack/flex-lite flow
container; per-node computed rects; point hit-testing (top-most, respects visibility/opacity);
deterministic focus order. Layout is headless because R-UI-006 assertion AND input hit-testing
(T3) need rects with no renderer.
**Ids:** R-UI-006, R-UI-003 (geometry base). **Files:** `src/packages/ui/` only.
**DoD:** layout unit tests incl. resize/reflow → damage propagation; hit-test edge cases
(overlap, nested, hidden). **CI:** general step only.

### T3 — Input routing integration (L-45 consumption)  (M)
**Goal:** UI focus lifecycle installs a *capturing* `ui` `InputContext` on the existing
`InputRouter`; pointer→hit-test→UI events; unconsumed input falls through to gameplay; UI-emitted
gameplay intents go through `session::ActionActivation` (ONE sink, D5/D6).
**Ids:** R-SYS-007, L-45, R-UI-006. **Files:** `src/packages/ui/` (router glue + tests);
`src/packages/input/` touched only if a hook is genuinely missing (expected: none — README says
the stack is complete).
**DoD:** ctest: capture-mode swallows unbound events (HUD-with-focus ⇒ gameplay sees nothing);
non-capturing overlay passes through; a UI button press lands in `InputState` identically to a
key-bound action; **hash_world unchanged by UI presence** (first D6 assertion).
**CI:** general step only.

### T4 — TS authoring surface (`context.ui`)  (M/L)
**Goal:** the R-UI-001 authoring path: V8-host bindings (`bindHostFunction` pattern) exposing
tree construction, style props, event handlers, and read-only data binding to state queries;
authored-TS example under `src/runtime/ts/examples/` + a `samples/` UI sample mirroring
`samples/input-bindings/`. Scope per owner ruling (a): **TS API + CSS-ish style props**, not an
HTML/CSS parser.
**Ids:** R-UI-001 (v1 shape), R-UI-006. **Files:** `src/packages/ui/` (binding shims),
`src/runtime/js|ts/` (registration + examples), `samples/ui-hud/` (authored sample).
**DoD:** ctest drives an authored `.ts` HUD headless: build tree from TS, dispatch event, assert
state readback. **CI:** V8 links automatically on the MSVC/clang CI legs (toolchain auto-detect;
local GCC builds the stub) — branch at RUNTIME on `v8BackendAvailable()`, the m6-exit-2 pattern;
there is no CMake toggle to flip.

### T5 — CLI drive/assert verbs (`context ui …`)  (M)
**Goal:** the "driven/asserted headless via CLI" exit leg: one-shot verbs ≡ RPC ≡ MCP (R-CLI-009
registry): `ui dump` (tree+rects JSON), `ui query <node>`, `ui send <event>` (click/focus/key/
text), `ui assert`. Mints the **`ui.*` error-catalog domain** (append-only tail of
`src/editor/contract/src/error_catalog.cpp` — the one shared anchor; fine single-lane).
**Ids:** R-UI-006, R-CLI-008/009. **Files:** `src/cli/`, `src/editor/contract/` (registry +
catalog), `src/packages/ui/` (introspection shims), `samples/` (corpus exercise).
**DoD:** parity test (CLI ≡ RPC ≡ MCP), catalog additive-check green.
**CI:** general step + **samples-corpus gate** — it asserts breadth of the R-CLI-009 registry, so
new `ui.*` verbs MUST be exercised by the samples corpus in the same PR or that gate reds.

### T6 — Engine-integrated backend: screen-space overlay  (L)
**Goal:** `src/render/ui/` implementing the provider contract over `rhi.h`: UI extract →
`UiRenderSnapshot` riding the L-39 double-buffer; quad batching (reuse `sprite/` sort/batch
concepts); overlay pass after 3D; **damage-based repaint** (consume T1 damage lists);
**GPU-composited transforms/opacity** (composite-time, no relayout).
**Ids:** R-UI-002 (default backend), R-UI-003 (screen-space), R-UI-005 (gpu_driver +
damage_repaint + composited_transforms = true). **Files:** `src/render/ui/`,
`src/render/CMakeLists.txt`, `offscreen_scene.h`-adjacent golden plumbing.
**DoD:** fake-backend ctests (damage → minimal draw set; composite math) on all legs; wgpu golden
scene `ui-hud` (colored rects; text comes in T7) SSIM-gated native + web.
**CI (3 edits):** new golden scene ⇒ (1) bake the `golden ui-hud` subcommand into
`context_render_wgpu_offscreen` (keeps the render job's hand-maintained `--target` list
UNCHANGED — do not add a second exe); (2) add dump+compare lines to the render job loops; (3)
mirror in render-web + commit `goldens/` baselines + `goldens/manifest.json` entry.
**No wall-clock asserts** (damage-efficiency asserted structurally — draw-count, not ms; else
CONTEXT_TSAN_BUILD widening in the same PR).

### T7 — Text: font asset + glyph atlas + basic layout  (M/L)
> **SUPERSEDED by owner ruling (c) — see a7 + a8.** The paragraph below is the PRE-ruling plan
> kept for provenance: its "shaping/bidi/IME declared `false`" statement and its LTR-only scope
> are the OPPOSITE of the ruled outcome (`text_shaping`/`bidi` = true; IME stays false). The
> operative specs are `2026-07-13-m7-runtime-ui/tasks/a7-font-substrate.md` and
> `a8-text-shaping.md`.
**Goal:** greenfield text: a permissively-licensed embedded default font (license-gate entry!),
glyph-atlas rasterization (stb_truetype-class, license-vetted), basic left-to-right line layout in
the headless tree (rects for hit-test/assert), atlas-textured quad draw in `render/ui/`.
**Shaping/bidi/IME are declared `false` capabilities** — documented deferral per R-UI-005/L-53.
**Ids:** R-UI-005 (text capability honesty), R-UI-001 (HUD legibility). **Files:**
`src/packages/ui/` (measure/layout), `src/render/ui/` (atlas+draw), `tools/check_licenses.py`
allowlist, third-party vendoring per repo pattern.
**DoD:** headless measure tests; `ui-hud` golden regenerated WITH text (reviewed rebaseline —
never automatic, goldens/README.md). **CI:** golden rebaseline only (lists already wired by T6).

### T8 — World-space render-to-texture panel  (M)
**Goal:** persistent per-panel offscreen target (generalize `offscreen_scene.h`); the first
dynamic-texture registry entry binding it to a render-side `UiPanel` component (flat quad,
scalable/rotatable/positionable via `render::Transform`); extract picks it up (D4).
**Ids:** R-UI-003 (world-space v1 core), L-16 (non-XR half — this RTT seam is the v2 landing pad).
**Files:** `src/render/ui/`, `src/render/include/context/render/render_world.h` (UiPanel
component, additive), `src/render/src/extract.cpp` (additive walk).
**DoD:** fake-backend RTT logic tests; golden scene `ui-worldpanel` (panel on a rotated quad in a
lit 3D scene), native + web. **CI:** same 3-edit golden wiring as T6 (subcommand into the existing
exe + both jobs' lists + baselines).

### T9 — Capability matrix + provider conformance suite  (S/M)
**Goal:** `docs/ui-capability-matrix.md` (the published L-53 matrix; v1 rows = desktop + web;
null + engine-integrated provider columns; **capability values per ruling (c): `text_shaping` =
`bidi` = true, `ime` = false — a11 governs, this line pre-dated the ruling**) + a
reusable provider conformance ctest suite ANY provider must pass (the R-UI-002 pluggability
proof and the R-UI-008 on-ramp without shipping extra backends now).
**Ids:** R-UI-002, R-UI-008 (seam only), L-53. **Files:** `docs/`, `src/packages/ui/tests/`.
**DoD:** both in-repo providers pass the same suite; matrix cross-checked against the
`Capabilities` structs by a test (rots-if-broken). **CI:** general step.

### T10 — EXIT: HUD in games + m7-exit-* gates  (M/L)
**Goal:** platformer-2d gets a real HUD (score/health via TS data binding); roll-3d gets a
world-space panel; the blocking exit gates land (below) + `docs/ci-fleet-manifest.json` rows +
the "M7 exit gate" named CI step. **Ids:** milestone exit; R-UI-001/002/003/005/006.
**Files:** `samples/platformer-2d/`, `samples/roll-3d/`, `src/tests/integration/`
(`test_m7exit*.cpp`, `m7_exit_test.h`), `.github/workflows/ci.yml`, fleet manifest.
**DoD:** all gates green on all 3 build-matrix legs; goldens updated (reviewed).
**CI:** the full tripwire drill — see next section.

## M7 exit gate (blocking `m7-exit-*` ctests, m5/m6 pattern)
- **`m7-exit-1-hud-headless`** — platformer-2d with its authored TS HUD headless-steps N fixed
  ticks through the session surface: HUD tree populated by data binding, a UI button press routes
  through L-45 capture into `InputState` and moves the player, gameplay input NOT swallowed when
  HUD is non-capturing. No GPU (null provider). (R-UI-001/006, R-SYS-007.)
- **`m7-exit-2-cli-drive`** — the ROADMAP criterion verbatim: drive + assert the HUD via the REAL
  `context` binary's `ui.*` verbs (`ui send click` → `ui assert` state change → `ui dump` rects) —
  the samples-corpus execution style. (R-UI-006, R-CLI-009.)
- **`m7-exit-3-worldpanel`** — the world-space RTT panel: fake-backend leg asserts the
  panel-target → dynamic-texture → extract chain headless; the SSIM assertion of the rendered
  `ui-worldpanel` golden stays owned by the SIBLING render/render-web jobs (the m4/m5 split:
  exit ctest = logic chain, golden jobs = pixels). (R-UI-003, L-16.)
- **`m7-exit-4-determinism-presentation`** — hash_world over an M6 gameplay scene is bit-identical
  with UI absent / null provider / (CI legs) GPU provider attached; `context_ui` registers no
  hashed sim component. UI is presentation — this pins D6 forever. (R-QA-005 boundary.)
- **`m7-exit-5-seam-checklist`** — executable audit, one assertion per seam (m2/m5/m6 mirror):
  provider contract + negotiation fallback, null-provider parity vs GPU provider tree state,
  damage list consumption, L-45 single-sink rule, RTT-seam presence, `ui.*` catalog discipline,
  capability-matrix ⟷ `Capabilities` struct consistency, **plus the two ruled-scope seams:
  shaped-text capability truth (`text_shaping`/`bidi` true and backed by a passing shaping
  ctest) and curved-panel interaction presence (a10's UV-raycast path registered + its ctest
  green)** — so the exit bar encodes rulings (c)/(d), not the pre-ruling minimum.
**Wiring (the Not-Run = RED drill):** (1) extend the build job's general `-E` regex (ci.yml ~L133)
with `^m7-exit-`; (2) add the named blocking "M7 exit gate" step (`ctest -R "^m7-exit-"`) after
the M6 step; (3) build job builds every target via `--preset dev` so no `--target` list changes
(state this in the step comment, the m6 precedent); (4) the strict-FP `deterministic` job's
hand-maintained list stays UNCHANGED (m7-exit-4 registers no `determinism-*` name — the m6-exit-3
alias precedent); (5) fleet-manifest rows for all five.

## Risks / unknowns
- **Text is the biggest unknown** (nothing exists; the ruled shaping stack pulls real third-party
  surface — HarfBuzz [SPDX `MIT-Modern-Variant`, an allowlist ADD] + a bidi/itemization lib
  [SheenBidi/Apache-2.0 — FriBidi is LGPL and the deny-by-default gate excludes it] + UAX #14
  line breaking [libunibreak/Zlib or a scoped simple-class implementation]; font licensing (OFL
  provenance treatment, runtime rasterization only); atlas quality on lavapipe/SwiftShader SSIM
  legs — pick per-scene tolerances carefully, goldens/manifest.json).
- **Curved-panel interaction (a10) is greenfield** — no raycast API exists anywhere (spatial =
  broad-phase only), no mesh colliders, no runtime mesh/UV data seam; budgeted at complexity 8.
- **Web goldens for new scenes extend the Emscripten target** (sources + web_main + harness
  list), not just ci.yml; a9's lit-scene golden is native-blocking first (the lit3d/viewport
  manifest precedent) with browser coverage joining when the lit web proof lands.
- **V8-path exit coverage:** m7-exit-1's TS half is CI-only (toggle-OFF local stub) — mirror the
  m6-exit-2 split (local asserts the C++ tree half; CI legs run authored TS) or the local dev
  gate silently loses the criterion.
- **Dynamic-texture registry** is net-new render infra (handles exist, registry doesn't) —
  keep it minimal (panel targets only), don't accrete the M8 asset-texture registry into T8.
- **Damage-repaint proof** must be structural (draw counts), not wall-clock (L-37/TSan lesson).
- **Scope creep magnets:** styling richness (keep a small closed style-prop set), editor-tree
  unification, R-A11Y-002 primitives, vertical text/justification/hyphenation — all named OUT
  above. (Curved panels and shaping are IN per rulings d/c — not creep.)
- **`error_catalog.cpp` / registry.cpp** shared anchors: only T5 mints — single-lane makes this
  safe, but a concurrent non-M7 CE task minting codes would conflict; sequence via TD.

## Owner checkpoints — RULED 2026-07-13 (see the banner; kept for provenance)
> All four questions below were answered by the owner on 2026-07-13: (1) → TS API (ruling a);
> (2) → defer backends (ruling b); (3) → full shaping-grade, NOT the LTR floor (ruling c —
> overrode the recommendation); (4) → curved surfaces IN M7 (ruling d — overrode the
> recommendation). The original questions:
1. **R-UI-001 v1 authoring shape:** TS retained-tree API + CSS-ish style props now, with
   HTML/CSS-*file* fidelity arriving via the optional CEF runtime backend later — or an
   HTML/CSS-subset parser in the engine-integrated backend in M7? (Recommend: TS API; parser is
   a milestone by itself and the MUST's letter is "web technologies (HTML/CSS + TS)" — needs an
   explicit owner reading for M7.)
2. **R-UI-008 backends deferral:** is contract + null + engine-integrated provider (+ conformance
   suite) sufficient for M7, deferring CEF-runtime/minimal backends to trailing-v1 (M8.5 window)
   or v1.x? (R-UI-002 MUST = the contract; R-UI-008 is SHOULD. Recommend: defer.)
3. **Text floor for the exit HUD:** basic LTR glyph-atlas text with shaping/bidi/IME declared
   unsupported in the published matrix (honest L-53 deferral) — acceptable for v1? And which
   permissively-licensed default font to embed (license-gate + redistribution call)?
4. **World-space v1 = flat quads only** (curved surfaces ride the v2 XR leg with L-16)? R-UI-003's
   text says "flat or curved" — confirm flat-only satisfies the v1 "basic RTT world-space" bar.
