# src/editor/gui/panels/problems/ — M5-F4: the Problems observer panel (context_gui_panel_problems,
# R-HUX-005 / R-FILE-003 / R-BRIDGE-008 / R-A11Y-001 / R-HUX-011, issue #158). Renders the R-FILE-003
# structured diagnostics as a headless context_gui_uitree Panel (a list grouped by file + the inline
# markers editors draw), drives click-to-navigate (emitting a navigation event other editors consume),
# and handles the R-BRIDGE-008 provisional->stable promotion + stale-marker discard on
# derivation.settled. Read-only observer: no writes, NO new error-catalog codes.
#
# Pure C++ over the F0b headless libs + the R-BRIDGE-008 stability enum — NO CEF — so it builds +
# unit-tests on the DEFAULT 3-OS matrix and the local dev gate, exactly like the sibling scenetree
# panel. The primary CI assertion (diagnostics fixture -> list + markers, promotion, navigation) never
# boots CEF.

add_library(context_gui_panel_problems STATIC
    src/problems_model.cpp
    src/problems_panel.cpp)

target_include_directories(context_gui_panel_problems PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_uitree  -> uitree::Panel        (problems_panel.h build_panel())
#   context_bridge      -> bridge::Stability    (problems_model.h / problems_panel.h stability field)
# context_warnings is PRIVATE (no flag leak to a CEF-host consumer, per conventions.md). NO new
# third-party dependency (no vcpkg / license-allowlist change).
target_link_libraries(context_gui_panel_problems
    PUBLIC context_gui_uitree context_bridge
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling gui/panels/scenetree +
# gui/uitree pattern); tests/problems_test.h is the tiny CHECK() harness. context_bridge pulls
# std::filesystem transitively (its lock/daemon), so static-link the GCC runtime (gated to GNU) to keep
# the local ctest gate deterministic against a PATH-shadowed libstdc++-6.dll (setup.md § Preconditions).
set(_context_gui_panel_problems_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_panel_problems_tests
    test_problems_model
    test_problems_panel
    test_a11y)

foreach(_t IN LISTS _context_gui_panel_problems_tests)
    add_executable(context_gui_panel_problems_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_panel_problems_${_t}
        PRIVATE context_gui_panel_problems context_warnings)
    target_link_options(context_gui_panel_problems_${_t}
        PRIVATE ${_context_gui_panel_problems_static_runtime})
    target_include_directories(context_gui_panel_problems_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-panel-problems-${_t} COMMAND context_gui_panel_problems_${_t})
endforeach()
