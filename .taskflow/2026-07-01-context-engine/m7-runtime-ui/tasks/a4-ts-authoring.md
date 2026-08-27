---
id: a4-ts-authoring
title: TS authoring surface (context.ui) — the owner-ruled v1 authoring form
group: A
sequence: 4
repo: "."
base_branch: "main"
depends_on: []
importance: 9
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [T4, ruling-a]
---
## Goal
The R-UI-001 authoring path per owner ruling (a): **TS retained-tree API with CSS-like style
props** (NOT an HTML/CSS parser — file-level fidelity arrives with the later optional CEF
backend). V8-host bindings (`bindHostFunction` pattern) exposing tree construction, style props,
event handlers, read-only data binding to state queries; authored HUD sample.

## Scope & seams
`src/packages/ui/` (binding shims), `src/runtime/js|ts/` (registration + example under
`src/runtime/ts/examples/`), `samples/ui-hud/` (authored sample mirroring
`samples/input-bindings/` — new content kind entries ride the samples-corpus gate).

## Definition of Done
- [ ] ctest drives an authored `.ts` HUD headless: build tree from TS, dispatch event, assert
      state readback.
- [ ] V8 path split per the m6-exit-2 precedent: local GCC gate asserts the stub/toolchain half;
      the real-V8 ctest runs on the MSVC/clang CI legs. Mechanism (verified): V8 is toolchain
      AUTO-DETECT in js/CMakeLists.txt (no CMake toggle; `CONTEXT_JS_FORCE_STUB` is the inverse
      hatch) — branch at RUNTIME on `v8BackendAvailable()`.
- [ ] samples-corpus gate green (new sample carries its `$schema`; L-33 hex ids).
