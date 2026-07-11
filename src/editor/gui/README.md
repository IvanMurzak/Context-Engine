# `src/editor/gui/` — CEF editor host + M5 editor-GUI foundation (M5-F0b)

The **substrate every M5 editor panel plugs into** (**R-EDIT-001** / **R-UI-007** / **L-41** /
**R-A11Y-001** / **R-SEC-007**, issue #152). Built **on** the F0a CEF build substrate
(`src/editor/cef/`, #151) — this is the editor *host* and its contract, F0a was only the fetch/link/
boot substrate.

## Layout (headless libraries vs the CI-only CEF host)

The design (R-EDIT-001 "testable-by-construction editor") separates a **headless UI-logic tier** from
the CEF host, so panel logic is CI-assertable **without booting CEF**:

- **`uitree/` — `context_gui_uitree`** (headless, default-built): the UI-logic tree every panel is
  built from. `UiNode` carries the accessibility model (ARIA role + accessible name + keyboard
  focusability + command binding); `Panel` is a headless-instantiable panel; `render_html()` emits
  semantic HTML. The **a11y-harness hook** lives here: `audit_a11y()` + `focus_order()` are the
  automated accessibility + keyboard-only-navigation checks, run on the **default 3-OS matrix** (no
  CEF). `builtin.h` is the placeholder panel the host renders — the same artifact on both sides of
  the CEF boundary.
- **`contract/` — `context_gui_contract`** (headless, default-built): the **R-EDIT-001 extension
  contract**. `ExtensionRegistry` registers panel/inspector/gizmo/asset-kind-editor contributions
  (deny-by-default on an out-of-window version or a non-conformant sandbox); `SandboxPolicy` is the
  concrete CEF extension trust boundary (Node integration OFF, isolated renderer, strict CSP,
  sandboxed iframe, no daemon-socket access); `ExtensionBridge` is the **capability-scoped bridge
  shim** — it clamps a panel to a scoped session over the ONE bridge dispatcher (R-SEC-007 default
  read/query), so a panel is constrained exactly like a scoped remote client.
- **`compositor/` — `context_gui_compositor`** (headless, default-built): the minimal **L-41**
  surface-handoff seam — the ratified per-platform CEF compositing mode selection (accelerated-OSR
  Windows / IOSurface macOS / software-OSR Linux behind the Mesa/X11-ozone gate; never
  `SendExternalBeginFrame`). **NOT** full 3D compositing — the surface seam the F1 viewport later
  fills.
- **`host/` — `context_gui_host`** (CI-ONLY, behind `CONTEXT_BUILD_GUI_CEF`): the CEF editor host.
  Boots CEF headless, registers the placeholder panel through the contract, attaches the
  capability-scoped bridge + runs one command (`describe`), selects the L-41 surface, renders the
  placeholder panel, exits 0 — the `editor-cef-smoke-boot` ctest.

## Building + testing

The three headless libs build + unit-test on the **default `dev` gate** (local Windows/GCC + the 3-OS
CI `build` matrix):

```bash
cmake -S src --preset dev && cd src && cmake --build --preset dev
ctest --preset dev -R "^gui-"        # gui-uitree-* / gui-contract-* / gui-compositor-*
```

The CEF **host** is a CI-only dependency path (like `src/editor/cef/` / V8 / wgpu-native — the
MSVC/Clang-ABI CEF prebuilt cannot link under the local Strawberry-GCC dev gate). It is built + booted
ONLY by the per-OS **`editor-cef-smoke` CI job** behind `-DCONTEXT_BUILD_GUI_CEF=ON`:

```bash
# On a toolchain that can link the CEF prebuilt (Linux/clang, macOS/clang, Windows/MSVC):
cmake -S src --preset dev -DCONTEXT_BUILD_GUI_CEF=ON
cmake --build --preset dev --target context_gui_host   # (run from src/)
ctest --preset dev -R "^editor-cef-smoke-" --output-on-failure   # Linux: wrap in xvfb-run -a
```

Linux is the primary (blocking) boot leg per the M5 exit criterion; macOS/Windows ride the standing
M4/M5 CI policy.
