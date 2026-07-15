# `src/packages/ui/` — Runtime UI package (M7 T1, R-UI-002/005/006)

The **headless foundation** of the pluggable runtime UI system: a retained UI tree, an event + handler
model, dirty/damage tracking, the backend-agnostic **UI-Provider contract**, (`a2`) headless
**layout + hit-testing + focus order**, (`a4`) the **TypeScript authoring binding shim**, and (`a3`)
the **input routing glue** to the L-45 capture stack + the sim InputState sink.
`a1-ui-foundation` landed the tree, events, damage, the provider seam, and a null provider;
`a2-layout-hittest` added computed geometry; `a4-ts-authoring` added the `context.ui` authoring
surface; `a3-input-routing` added the router->session glue (this doc); `a7-font-substrate` added the text
substrate — FreeType glyph rasterization + the run-based `measure()` seam + the embedded default fonts
— as the sibling **`context_ui_text`** lib under `text/` (see `text/README.md`). The CLI verbs and
shaping/bidi (a8, which drops into a7's run-based, glyph-id-keyed, GPOS-offset-bearing seam without
rework) are later M7 tasks.

## What it provides

- **Retained tree** (`ui_tree.h` / `ui_node.h`) — `UiTree` owns a `UiNode` store addressed by stable
  `NodeId` handles (removed nodes are tombstoned, never reused, so a handle never repoints). Nodes carry
  a **closed `Role` vocabulary** (`role_name()` for a11y/introspection), a small **CSS-like `Style`**
  (visible, opacity, `Transform`, background/foreground `Color`, padding), optional text/name, and
  computed `bounds`. Build/mutate: `create_node` / `remove_node` / `reparent` / `set_style` / `set_text`
  / `set_bounds` / `set_visible`.
- **Events + handlers** (`events.h`) — pointer / focus / key / custom `Event`s dispatched
  **target-then-bubble**: `dispatch()` delivers to `ev.target`, then walks toward the root; a handler
  sets `ev.handled` to stop propagation. A minimal **focus model** (`set_focus`) emits `FocusLost` /
  `FocusGained`. Hit-testing (which node a pointer hits) is a **later task** — `dispatch` takes an
  explicit target for now.
- **Damage tracking** (`damage.h`) — mutations mark dirty regions computed **in the tree**; a structural
  change marks the whole surface. `DamageList::coalesce()` merges overlapping regions to a minimal set;
  `UiTree::take_damage()` returns the coalesced damage and resets it.
- **Layout + hit-testing + focus** (`layout.h`, `a2`) — computed geometry with **no GPU**. Per-node
  layout inputs (a small closed model on `UiNode::layout`: `Positioning` Flow/Absolute, a `Flow`
  Row/Column stack container with `gap`, `Anchor`-edge absolute positioning with stretch, `size`,
  `offset`) drive `compute_layout(tree, viewport)`, which writes each node's computed `bounds`
  top-down. It writes through `set_bounds`, so a **resize/reflow drives the a1 damage machinery** (only
  the moved rects are damaged; an unchanged re-layout produces none). `hit_test(tree, x, y)` returns the
  **top-most** node under a point (a child over its parent, a later sibling over an earlier one),
  **respecting visibility/opacity** (a `visible == false` or `opacity <= 0` node — and its whole subtree
  — is not hittable). `focus_order(tree)` is the **deterministic** focus/tab order: visible focusable
  nodes (the interactive roles, `is_focusable`) in document order, a hidden subtree culled.
- **UI-Provider contract** (`provider.h`) — `Capabilities` is the exact **R-UI-005** set
  (`gpu_driver`, `damage_repaint`, `composited_transforms`, `text_shaping`, `bidi`, `ime`). A
  `UiProvider` reports its capabilities and `present`s the tree under a `RepaintPlan`.
  `negotiate_repaint()` is the **fallback table**: a provider **without** `damage_repaint` falls back to
  a **full repaint**; a damage-capable one repaints only the coalesced dirty regions.
- **Null provider** (`null_provider.h`) — the R-UI-006 headless guarantee made concrete: all-false
  capabilities, and a `present()` that does **no rendering work** (never walks the tree). UI logic runs
  headless with zero render cost.
- **Input routing glue** (`input_routing.h`, `a3`, `context_ui_input`) — the L-45 **consumption** seam
  (R-SYS-007, D5/D6): a `UiInputRouter` binds a headless `UiTree` to the EXISTING input-package
  `InputRouter` and the ONE sim `InputState` sink. `focus()` / `blur()` push/pop a caller-installed
  capturing `ui` `InputContext` on the router (a modal HUD gains focus → gameplay sees no unbound
  input). `route_pointer()` hit-tests a2's computed rects: a **hit** dispatches a target-then-bubble UI
  pointer event (whose handlers `emit_action()` UI-originated **gameplay intents**) and consumes the
  pointer; a **miss** is **swallowed** while the ui context is capturing (the modal backdrop) or **falls
  through** to gameplay (routed through the `InputRouter`) while non-capturing. `route_events()` forwards
  keyboard/gamepad events to the router's existing capture arbitration. Every gameplay activation — a UI
  button's intent AND a fell-through device action — is a `session::ActionActivation` the caller injects
  through the SAME `Session::inject_action_at` path a key-bound action uses (**D5, one sink**; no parallel
  sim-path input). **D6**: the glue registers **no sim component** and the tree lives **outside** the sim
  `World`, so `hash_world` is bit-identical with UI present vs absent (UI is presentation). Like `a4` it
  is a **separate `context_ui_input` STATIC lib** (it links `context_input` → `context_session`), keeping
  the `context_ui` foundation pure stdlib.
- **TypeScript authoring binding shim** (`script_bindings.h`, `a4`, `context_ui_script`) — the R-UI-001
  authoring path (owner ruling (a): a **TS retained-tree API with CSS-like style props**, not an
  HTML/CSS parser). A `UiScriptContext` over a caller-owned `UiTree` + a numeric `StateStore`, plus a
  table of **doubles-only** `HostFunction`-compatible primitives (`ui_host_bindings()`: tree
  construction / style props / event handlers / read-only data binding) the V8 host binds as globals the
  authored `context.ui` TS surface wraps. Only doubles cross `JsEngine::bindHostFunction`, so roles /
  event kinds / node handles / state keys marshal as numbers (the numeric protocol the `decode_*`
  helpers police). Event dispatch calls back into the VM through a caller-installed **invoker**
  (`set_invoker`; the runtime/ts glue wires it to `JsEngine::callFunction("__ui_invoke", …)`), so an
  authored `onClick` runs in-VM. The shim is a **separate `context_ui_script` STATIC lib** that is
  **pure stdlib + JsEngine-free** (its `UiHostFunction` typedef is byte-identical to `js::HostFunction`
  but names no V8 header), so it builds + unit-tests on every toolchain with no V8 link; the js glue
  lives with the caller (`src/runtime/ts/`), CI-only for its V8 path. UI→state is only the action path
  (write/add); state→UI is read-only (data binding). Presentation (D6): the `StateStore` is not sim
  state and folds into no state hash.

## Load-bearing design decisions (locked here)

- **D1 — the UI-Provider contract shape.** The provider consumes the retained tree + damage list and
  reports a `Capabilities` struct; the engine negotiates + falls back (`negotiate_repaint`). The GPU
  backend (`src/render/ui/`, a later task) implements this same header.
- **D2 — a NEW runtime tree, not `context_gui_uitree`.** The editor tree (`src/editor/gui/uitree/`) is
  editor-scoped, ships only with the editor, and has no layout/events/damage. This runtime tree
  **borrows its design** (closed role vocabulary so R-A11Y-002 can hook it post-core) with **zero
  link-level sharing** — editor code never becomes a runtime dependency.
- **D3 — package placement.** Headless logic is `context_ui` (STATIC, `src/packages/ui/`); the GPU
  backend is `src/render/ui/` (needs `rhi.h`). The provider **contract header lives here**.
- **D6 — UI is presentation.** The tree lives **outside** the sim `World` and registers **no hashed sim
  component**. The `context_ui` foundation is **pure stdlib** and does not link the sim at all; every
  field here is presentation/observer state (`hash_world` is bit-identical with UI present or absent).
  The L-45 input-routing glue + session composition landed in `a3` as the **separate `context_ui_input`
  lib** (it links `context_input` → `context_session`), so the foundation keeps its zero-sim-dependency
  property; the glue's own D6 assertion (`hash_world` unchanged by UI presence) is pinned by
  `ui-test_input_routing`.

## Layering

`context_ui` is a **package, not kernel core** (L-60): a STATIC `context_<name>` library under
`src/packages/` linking only the warnings-as-errors baseline (`context_warnings` PRIVATE). Nothing under
`src/kernel/` links it. Unlike `context_input` (whose public API is defined in the session input
vocabulary and so links `context_session` PUBLIC), this foundation tier has **no sim dependency**.

The `a4` authoring shim is a **sibling STATIC lib `context_ui_script`** (links `context_ui` PUBLIC),
kept separate so the `context_ui` foundation stays minimal. It is **JsEngine-free** (pure stdlib): it
never names a V8/js header, so the doubles-only host-function table + `UiScriptContext` build + unit-test
on every toolchain with no V8 link. The js glue that binds the table to a live `JsEngine` and drives the
authored TS lives with the caller in `src/runtime/ts/` (CI-only for its V8 dependency path).

The `a3` input-routing glue is another **sibling STATIC lib `context_ui_input`** (links `context_ui`
PUBLIC + `context_input` PUBLIC). Unlike the pure-stdlib foundation it DOES need the sim seam —
`context_input`'s public API is the session input vocabulary `InputRouter::route()` returns, which
brings `context_session` + `context_kernel` transitively (the kernel never links back). Keeping it a
separate lib preserves the `context_ui` foundation's zero-sim-dependency charter (only code that drives
the sim links the sim).

## Tests

Headless `ui-*` ctests (R-QA-013, same PR): `ui-test_tree` (build/mutate), `ui-test_dispatch` (handler
dispatch + focus), `ui-test_damage` (coalescing + structural-vs-region damage), `ui-test_provider`
(capability negotiation/fallback table), `ui-test_null_provider` (null-provider zero-cost),
`ui-test_layout` (flow/absolute computed rects, resize/reflow → damage, hit-testing
overlap/nesting/hidden/zero-opacity, deterministic focus order), `ui-test_input_routing` (`a3`: the
router→session glue over a real `InputRouter` + `Session` — capture-mode swallows unbound gameplay
input, a non-capturing overlay passes through, a UI button press lands in `InputState` identically to a
key-bound action, and `hash_world` is unchanged by UI presence with no sim-component fork), and
`ui-test_script_bindings` (`a4`:
the doubles-only authoring shim — numeric-protocol decoders, `StateStore`, tree construction / style
props / event handlers / read-only data binding through both `UiScriptContext` and the host-function
table, the dispatch→handler invoker bridge, plus failure paths). Pure stdlib, so they build + run on all
three CI OS legs and auto-run in the general CI test step. The authored-TS-in-V8 end-to-end proof of the
`a4` surface is `ts-test_ui_ts_authoring` (`src/runtime/ts/tests/`, CI-only for its V8 path).
