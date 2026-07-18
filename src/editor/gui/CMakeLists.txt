# src/editor/gui/ — M5-F0b: the CEF editor host + the R-EDIT-001 extension contract + the
# headless-instantiable UI-logic tree + the minimal L-41 compositing seam + the a11y-harness hook
# (issue #152; L-15 / L-41 / R-EDIT-001 / R-UI-007 / R-A11Y-001 / R-BRIDGE-008 / R-SEC-007). The
# substrate every M5 panel plugs into — built ON the F0a CEF build substrate (src/editor/cef/, #151).
#
# Layering (deliberate): the THREE headless libraries — uitree, contract, compositor — are pure C++
# with NO CEF dependency, so they build + unit-test on the DEFAULT 3-OS matrix and the local dev gate.
# The CEF HOST (host/) is the ONLY CEF-dependent target: a CI-only dependency path like src/editor/cef
# / V8 / wgpu-native (the MSVC/Clang-ABI CEF prebuilt cannot link under the local Strawberry-GCC dev
# gate). It is added ONLY behind CONTEXT_BUILD_GUI_CEF, and it LINKS the three headless libs — so
# "every built-in panel is built ON the extension contract from day one" (R-EDIT-001) holds even for
# the boot smoke.

add_subdirectory(uitree)
add_subdirectory(contract)
add_subdirectory(compositor)

# The M5 built-in panels (panels/) — headless libraries built ON uitree + the R-EDIT-001 contract,
# default-built + unit-tested on the 3-OS matrix WITHOUT CEF (M5-F2 scene-tree onward). Added here,
# after the three headless libs it builds on and before the CEF host toggle, so the panels are part
# of the default gate. Added BEFORE a11y, which links + scans each registered panel.
add_subdirectory(panels)

# The M5-F1 native viewport OBSERVER panel (viewport/) — a headless library built ON uitree + the
# L-41 compositor seam + the render RHI abstraction (context_render), default-built + unit-tested on
# the 3-OS matrix WITHOUT CEF (issue #164). A sibling of panels/ (the DoD path is gui/viewport/), added
# after them and BEFORE a11y, which links + scans the registered viewport panel. Its render pixels +
# golden-scene equivalence are the render-layer proof (src/render/viewport_scene.h + goldens/).
add_subdirectory(viewport)

# The M5-F5 play-in-editor PLAYBAR (playbar/) — a headless library built ON uitree + the runtime
# session (context_session) + the render extract (context_render), default-built + unit-tested on the
# 3-OS matrix WITHOUT CEF (issue #166). Drives session-control (play/pause/stop/step) over
# src/runtime/session/, honors the L-51 edit/play split + L-22 hot reload, and yields the rendered play
# frame the F1 viewport observes (the SAME render::RenderSnapshot — no second render path). Added after
# viewport (its panel test links context_gui_viewport to assert the play->viewport path) and BEFORE
# a11y, which links + scans the registered playbar panel. Mints the reserved play.* codes.
add_subdirectory(playbar)

# The M5 GUI SESSION-state surfaces (session/) — headless affordances layered on the panel write
# seams (R-FILE-006 ephemeral session state). Added AFTER panels/ because M5-F7 session undo/redo
# (session/undo/, issue #162) builds ON context_gui_panel_inspector's L-30 commit engine + gateway
# seam. CEF-free, so it is part of the default 3-OS gate.
add_subdirectory(session)

# M8.5 a20 (issue a20): in-editor contextual help (help/) — a headless library built ON uitree + the
# ONE contract registry (context_contract), default-built + unit-tested on the 3-OS matrix WITHOUT CEF
# (R-HUX-010). Generates per-verb help from live introspection (never parallel docs) + the R-QA-006
# getting-started references. Added AFTER the panels it documents and BEFORE a11y, which links + scans
# the registered Help panel (builtin.help). A thin help layer — no new content pipeline, no dependency.
add_subdirectory(help)

# M5-F6 (issue #154): the a11y enforcement harness (context_gui_a11y) — the headless per-panel
# accessibility gate built ON uitree. Unconditional (CEF-free), so it builds + tests on the default
# 3-OS matrix + local dev gate; the editor-cef-smoke CI job additionally runs tools/a11y_scan.py over
# its report. Added after uitree + panels — it links each registered panel lib to instantiate + scan
# it. M5 fan-out panels register into a11y/registry.cpp + coverage.manifest.jsonl (the shared
# sub-anchors) and add their lib to a11y/CMakeLists.txt.
add_subdirectory(a11y)

# The CEF editor host + the per-OS editor-cef-smoke boot (host/). Behind CONTEXT_BUILD_GUI_CEF (the
# same toggle the F0a substrate uses). MUST be added AFTER src/editor/cef in src/CMakeLists.txt so the
# shared CEF acquisition (fetch + libcef_dll_wrapper) runs there first; host/ reuses it (see
# host/CMakeLists.txt). Early-returns to a no-op when the toggle is OFF (the default matrix + local
# dev gate never enter it).
if(CONTEXT_BUILD_GUI_CEF)
    add_subdirectory(host)
endif()
