# `src/packages/input/` — Input package (M6 P7, R-SYS-007 / L-45)

The authoring/mapping/**routing** front-end for player input. It maps raw device events —
keyboard / mouse / gamepad / touch / VR-controller — through stackable **action maps** + a layered
**UI-capture stack** (L-45) into the mapped **action** layer, with a deterministic **UI-vs-gameplay
focus arbitration** (R-SYS-007), plus runtime **rebinding**. This is the last of the seven M6 WAVE4
gameplay packages.

## The load-bearing seam — feed the existing sink, do not fork

The headless session (`src/runtime/session/`) already owns the **one** sim-facing input sink: a
world-singleton `InputState` component that the `input` system folds injected
`session::ActionActivation` / `session::InputEvent` into each tick (the R-QA-005 record/replay/inject
surface). This package is the **front-end** that turns raw device events into that mapped **action**
layer and **feeds that same sink**. `InputRouter::route()` returns a `session::TickInputs` of
`session::ActionActivation` — the **existing** types the sink consumes — so there is **no parallel
sim-path input representation**; replay and determinism keep resting on the single `InputState`. The
package registers **no sim component of its own** (asserted by `tests/test_feed.cpp`).

## What it provides

- **Action maps + input contexts** (`action_map.h`) — a `Binding` maps a raw `(device, code)` source to
  a named action; an `InputContext` is a named, layered (`gameplay` / `ui`), optionally **capturing**
  set of bindings.
- **`InputRouter`** (`input_router.h`) — installs contexts by unique id, maintains an active **stack**
  (top = highest priority), and `route()`s a tick's raw events into mapped actions honoring the
  arbitration below. Supports runtime **rebinding** (`rebind()`).
- **UI-vs-gameplay focus arbitration (R-SYS-007 / the L-45 layered UI-capture stack)** — per event,
  walk the stack top-down: the highest-priority context that **binds** the source owns it; a
  **capturing UI** context that does *not* bind it **swallows** it so gameplay below never sees it
  (the "UI has focus, gameplay gets no input" rule); a **non-capturing** overlay lets unbound input
  fall through to the next lower context.
- **`ctx:input-bindings`** authored kind — the schema lives in `src/editor/schema/` (`engine_schemas()`);
  its referential-integrity rules (unique action / context ids, bindings resolving to a declared action)
  live in `src/editor/kinds/input_bindings.h`; a few-shot corpus sample lives at
  `samples/input-bindings/`.

## Determinism

Routing is a **pure, deterministic function of `(stack, events)`** — no float, no hidden state — and the
mapped actions are integer, so the device → action → `InputState` pipeline hashes byte-identically on the
Linux-x64 / Win-x64 / macOS-ARM64 wedge matrix. The `determinism-input-scene` ctest
(`tests/determinism_input_scene.cpp`) proves it against a cross-platform golden; its target is on the
strict-FP `deterministic` CI job's `--target` list.

## Errors

`errors.h` is the source of truth for the fail-closed `input.*` codes (`invalid_context` /
`duplicate_context` / `unknown_context` / `unknown_action`) that the contract error catalog registers in
its F0a-reserved `input.*` block — the same promote-a-local-string pattern as the sibling packages, so
this package never links the contract layer (dependency direction stays package → session/kernel, L-60).

## Layering

`context_input` links `context_session` **PUBLIC** (its public API is defined in the session input
vocabulary — the seam), which brings `context_kernel` transitively. The kernel never links back.
