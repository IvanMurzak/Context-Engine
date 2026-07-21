# src/editor/gui/panels/scenetree/ — M5-F2: the scene-tree observer panel (context_gui_panel_scenetree,
# R-EDIT-001 / R-BRIDGE-008 / L-35 / R-A11Y-001 / R-HUX-011, issue #155). Renders the composed
# derived-world hierarchy (instances/overrides visible) as a headless context_gui_uitree Panel, drives
# selection, and re-renders on the R-BRIDGE-008 derivation.settled event (stability-aware). Read-only:
# no writes, no new error-catalog codes.
#
# Pure C++ over the F0b headless libs + the M2 read path — NO CEF — so it builds + unit-tests on the
# DEFAULT 3-OS matrix and the local dev gate, exactly like context_gui_uitree/contract/compositor.
# The primary CI assertion (fixture tree + selection + stable re-render) never boots CEF.

add_library(context_gui_panel_scenetree STATIC
    src/scene_tree_model.cpp
    src/scene_tree_panel.cpp)

target_include_directories(context_gui_panel_scenetree PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_uitree  -> uitree::Panel                (scene_tree_panel.h build_panel())
#   context_bridge      -> bridge::Stability            (scene_tree_panel.h derivation.settled)
# NO context_compose (M9 e05d3, D10 owner ruling): the kernel-typed builder moved to
# context_gui_panel_builders, so this library is BOUNDARY-CLEAN and Shell-hostable — the D10
# shell-boundary gate audits it on `context_editor`'s closure. Do NOT re-add a kernel-internal
# module here; whatever needs compose::/schema:: types belongs in gui/panels/builders/.
# context_warnings is PRIVATE (no flag leak to a CEF-host consumer, per conventions.md). NO new
# third-party dependency (no vcpkg / license-allowlist change).
target_link_libraries(context_gui_panel_scenetree
    PUBLIC context_gui_uitree context_bridge
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling gui/uitree + gui/contract
# pattern); tests/scenetree_test.h is the tiny CHECK() harness. context_compose / context_bridge pull
# std::filesystem transitively, so static-link the GCC runtime (gated to GNU) to keep the local ctest
# gate deterministic against a PATH-shadowed libstdc++-6.dll (setup.md § Preconditions).
set(_context_gui_panel_scenetree_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_panel_scenetree_tests
    test_scene_tree_model
    test_scene_tree_panel
    test_a11y)

# The tests link context_gui_panel_builders too: they exercise the MOVED build_scene_tree through
# its new kernel-side home (tests are not on any audited closure, so the compose reach is fine —
# that is exactly the D10 split's point).
foreach(_t IN LISTS _context_gui_panel_scenetree_tests)
    add_executable(context_gui_panel_scenetree_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_panel_scenetree_${_t}
        PRIVATE context_gui_panel_scenetree context_gui_panel_builders context_warnings)
    target_link_options(context_gui_panel_scenetree_${_t}
        PRIVATE ${_context_gui_panel_scenetree_static_runtime})
    target_include_directories(context_gui_panel_scenetree_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-panel-scenetree-${_t} COMMAND context_gui_panel_scenetree_${_t})
endforeach()
