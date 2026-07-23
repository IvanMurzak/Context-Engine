# `src/editor/shell/` — the native Shell (`context_editor`)

The first time the engine draws an interactive window in production (M9 e04). A real OS window with
the single-threaded owner loop, a windowed-OSR browser composited through e03's present path, the
per-window compositor (including the `PET_POPUP` second OSR layer), the OS input pump with region
arbitration, real per-monitor DPI, and the C-F2 CPU present fallback.

**The engineering doc is [`docs/shell.md`](../../../docs/shell.md)** — it explains the model, the
traps each piece exists to avoid, the test map, and the deferred interactive-Windows verification.
This file is the directory map.

## Targets

| Target | What | Built where |
|---|---|---|
| `context_editor_shell` | The Shell proper — window seam + Win32 backend, DPI, input arbitration, compositor, editor-state persistence, the owner loop. CEF-free and GPU-backend-free. | Everywhere (local dev gate + all three `build` legs) |
| `context_editor` | The app. Boots a window, binds a browser, attaches a present path, runs the owner loop. | Everywhere |
| `context_editor_cef` | The windowed-OSR CEF binding — the ONE piece that cannot build locally. | `CONTEXT_BUILD_GUI_CEF` only (`editor-cef-smoke` job) |
| `context_editor_panels` (`panels/`) | The panel COMPOSITION ROOT — the only target that links a `context_gui_panel_*` library, reached by the executables and never by `context_editor_shell` (D10; see `docs/shell.md` § 6). | Everywhere |

## Files

| Path | Role |
|---|---|
| `include/.../dpi.h`, `src/dpi.cpp` | Per-monitor DPI: the DPI is stored, the scale factor derived. |
| `include/.../input.h`, `src/input.cpp` | Region map + arbitration + the capture stack + focus-class key routing (03 §6). |
| `include/.../editor_state.h`, `src/editor_state.cpp` | `.editor/editor-state.json` — the Shell is its SINGLE writer (03 §1). Debounced, crash-safe. |
| `include/.../window.h`, `src/window.cpp` | The `IWindowBackend` seam, the headless backend, and the PURE Win32 message decoder. |
| `src/win32_window.cpp` | The Windows OS calls only (`RegisterClassExW`/`CreateWindowExW`/WndProc/per-monitor-v2 DPI). Reports an honest gap on macOS/Linux (e12). |
| `include/.../browser.h`, `src/browser.cpp` | The CEF-free browser seam + the scripted host the smoke and tests drive. |
| `include/.../compositor.h`, `src/compositor.cpp` | The layer stack, damage, the resize protocol, `PET_POPUP`, and both present paths (03 §4). |
| `include/.../shell.h`, `src/shell.cpp` | `WindowManager` / `EditorWindow` / the owner loop, and the D10 authenticated attach. |
| `include/.../panel_host.h`, `src/panel_host.cpp` | e05d1: the PANEL-AGNOSTIC `panel.*` bridge surface over the e05b roster — render, command, gesture verbs, the D6 state pair (04 §3-§4). Knows no panel id. |
| `include/.../themes_bridge.h`, `src/themes_bridge.cpp` | e06b: the `themes.get` surface — reads + WATCHES `~/.context/themes/*.theme.json` and publishes the raw bytes with a generation counter (06 §4). Bytes only: the theme SCHEMA lives in editor-core (theme.ts), so a malformed theme is rejected there and never becomes a broken UI. D10 boundary-clean (plain `std::filesystem`), like its `keybindings_bridge.h` sibling. |
| `panels/` | e05d1: the composition root. Binds a `PanelProvider` per hostable panel and projects the daemon's `diagnostics` topic onto the Problems model. |
| `app/editor_main.cpp` | `context_editor`'s entry point. |
| `smoke/shell_smoke_main.cpp` | The **Session-0-safe** smoke — e04's blocking CI requirement. |
| `cef/` | The windowed-OSR CEF binding + its live boot smoke. |
| `tests/` | The `editor-shell-*` ctest family. |

## Working on this locally

The Shell builds and tests fully under the normal dev gate:

```sh
cmake -S src --preset dev
cd src && cmake --build --preset dev && ctest --preset dev -R "^editor-shell-"
```

**The CEF binding is the exception** — a CI-only dependency path (the MSVC/Clang-ABI prebuilt cannot
link under the local Strawberry-GCC gate). Its local pre-flight is the `-fsyntax-only` gate
`setup.md` § Preconditions describes: fetch the pinned CEF **headers** (`tools/fetch_cef.py`, which
SHA-verifies), then compile the TU against them with no prebuilt library. That catches every
signature and type break locally instead of one CI round-trip at a time:

```sh
python tools/fetch_cef.py --triple x86_64-pc-windows-msvc --dest src/build/dev/_cef
c++ -std=c++20 -fsyntax-only -Wall -Wextra -isystem src/build/dev/_cef \
  -I src/editor/shell/cef/include -I src/editor/shell/include -I src/editor/client/include \
  -I src/editor/contract/include -I src/render/present/include -I src/render/include \
  -I src/kernel/include \
  src/editor/shell/cef/src/cef_shell.cpp
```
