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

# M5-F6 (issue #154): the a11y enforcement harness (context_gui_a11y) — the headless per-panel
# accessibility gate built ON uitree. Unconditional (CEF-free), so it builds + tests on the default
# 3-OS matrix + local dev gate; the editor-cef-smoke CI job additionally runs tools/a11y_scan.py over
# its report. Added after uitree (which it links). M5 fan-out panels add their own add_subdirectory
# stanza here (this file is the shared sub-anchor).
add_subdirectory(a11y)

# The CEF editor host + the per-OS editor-cef-smoke boot (host/). Behind CONTEXT_BUILD_GUI_CEF (the
# same toggle the F0a substrate uses). MUST be added AFTER src/editor/cef in src/CMakeLists.txt so the
# shared CEF acquisition (fetch + libcef_dll_wrapper) runs there first; host/ reuses it (see
# host/CMakeLists.txt). Early-returns to a no-op when the toggle is OFF (the default matrix + local
# dev gate never enter it).
if(CONTEXT_BUILD_GUI_CEF)
    add_subdirectory(host)
endif()
