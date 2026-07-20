# src/editor/gui/a11y/ — M5-F6: the editor-UI accessibility enforcement harness (context_gui_a11y,
# R-A11Y-001 / R-EDIT-001, issue #154). Hardens the F0b a11y-harness *hook* into the real per-panel
# gate: scan every registered editor panel over the headless context_gui_uitree model (semantic/ARIA
# audit + keyboard-only focus order), emit a JSON report, and fail on any violation. Pure C++ over
# context_gui_uitree — NO CEF — so it builds + tests on the DEFAULT 3-OS matrix and the local dev gate;
# the editor-cef-smoke CI job additionally runs tools/a11y_scan.py over the report for the axe-class
# DOM scan + coverage-manifest cross-check (Linux blocking). coverage.manifest.jsonl + registry.cpp
# are the append-only shared anchors M5 fan-out panels extend (one entry each, union-merge).

add_library(context_gui_a11y STATIC
    src/harness.cpp
    src/registry.cpp)

target_include_directories(context_gui_a11y PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# context_gui_uitree is PUBLIC: harness.h / registry.h expose uitree::Panel / uitree::A11yViolation in
# their public API (the registry maps ids to headless panel factories). context_gui_panel_scenetree is
# PRIVATE — registry.cpp instantiates the M5-F2 panel to scan it, but the panel type never appears in
# a11y's public headers (registry.h maps ids to uitree::Panel factories). Each fan-out panel the
# registry scans adds its lib here likewise. context_gui_contract is PRIVATE too: since M9 e05b
# registry.cpp DERIVES the scan list from contract::builtin_contributions() (the single built-in
# roster), but no contract type appears in a11y's public headers. context_warnings is PRIVATE so no
# warning flags leak to the CEF host consumer (conventions.md). NO new third-party dependency.
target_link_libraries(context_gui_a11y
    PUBLIC context_gui_uitree
    PRIVATE context_gui_contract
            context_gui_panel_scenetree context_gui_panel_inspector context_gui_panel_problems
            context_gui_panel_tilemap context_gui_viewport context_gui_viewport_edit
            context_gui_playbar context_gui_help context_gui_undo context_warnings)

# harness.cpp uses std::ostringstream and the scan exe uses std::ofstream, so static-link the GCC
# runtime (gated to GNU) to keep the local ctest gate deterministic against a PATH-shadowed
# libstdc++-6.dll (setup.md § Preconditions). Target/exe names avoid the install/setup/update/patch
# tokens that trigger Windows UAC auto-elevation under ctest (setup.md).
set(_context_gui_a11y_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# The scan harness executable — registered as the gui-a11y-scan ctest (scans every registered panel,
# asserts a11y-clean, non-zero exit on any violation). The editor-cef-smoke CI job runs it with
# --out <file> to dump the report for tools/a11y_scan.py.
add_executable(context_gui_a11y_scan src/main.cpp)
target_link_libraries(context_gui_a11y_scan PRIVATE context_gui_a11y context_warnings)
target_link_options(context_gui_a11y_scan PRIVATE ${_context_gui_a11y_static_runtime})
add_test(NAME gui-a11y-scan COMMAND context_gui_a11y_scan)

# --- unit tests (R-QA-013: behavior ships WITH its tests in the same PR) -------------------------
add_executable(context_gui_a11y_test_harness tests/test_harness.cpp)
target_link_libraries(context_gui_a11y_test_harness PRIVATE context_gui_a11y context_warnings)
target_link_options(context_gui_a11y_test_harness PRIVATE ${_context_gui_a11y_static_runtime})
target_include_directories(context_gui_a11y_test_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME gui-a11y-test_harness COMMAND context_gui_a11y_test_harness)

# The standing coverage-contract guard (gui-a11y-coverage ctest): the built-in roster,
# registered_panels(), the scanned panels, and coverage.manifest.jsonl must all name the SAME panel
# set (and the roster's ORDER), so a panel added to one anchor without the others fails the CEF-free
# default 3-OS build matrix in its own PR (not just the CEF-gated a11y_scan.py leg).
# It reads the manifest via std::ifstream, so static-link the GCC runtime like the harness above.
# context_gui_contract is linked EXPLICITLY here (not just transitively): context_gui_a11y links it
# PRIVATE, so on a static library CMake propagates the link but NOT the include directories — and this
# test includes builtin_roster.h directly to cross-check the roster anchor.
add_executable(context_gui_a11y_test_coverage tests/test_coverage.cpp)
target_link_libraries(context_gui_a11y_test_coverage
    PRIVATE context_gui_a11y context_gui_contract context_warnings)
target_link_options(context_gui_a11y_test_coverage PRIVATE ${_context_gui_a11y_static_runtime})
target_include_directories(context_gui_a11y_test_coverage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_gui_a11y_test_coverage PRIVATE
    CONTEXT_GUI_A11Y_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/coverage.manifest.jsonl")
add_test(NAME gui-a11y-coverage COMMAND context_gui_a11y_test_coverage)
