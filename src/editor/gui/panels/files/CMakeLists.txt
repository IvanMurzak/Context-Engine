# src/editor/gui/panels/files/ — M9 e1: the Files observer panel (context_gui_panel_files, D10 read
# half / R-A11Y-001 / R-HUX-011) plus the M9 e2 WRITE half. Renders the project's file tree as a
# headless context_gui_uitree Panel, publishes `subject: "file"` selections through the c1
# typed-selection surface, consumes the c1/D3 `selection-focus` fact, and authors rename / move /
# delete through the boundary-clean FileWriteGateway seam (e2) — a pure virtual, so this library
# still links no assetdb/filesync type and stays Shell-hostable.
#
# Pure C++ over the F0b headless libs — NO CEF, NO assetdb/filesync — so it builds + unit-tests on the
# DEFAULT 3-OS matrix and the local dev gate, exactly like context_gui_panel_scenetree. The kernel-
# typed builder that walks the real project files + the asset database lives in
# gui/panels/builders/files_builder.h (context_gui_panel_builders) — this library stays
# boundary-clean and Shell-hostable under the D10 shell-boundary gate.

add_library(context_gui_panel_files STATIC
    src/files_model.cpp
    src/files_panel.cpp)

target_include_directories(context_gui_panel_files PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_uitree  -> uitree::Panel                (files_panel.h build_panel())
#   context_bridge      -> bridge::Stability            (files_panel.h derivation.settled)
# NO context_assetdb / context_filesync (D10): the kernel-typed builder lives in
# context_gui_panel_builders, so this library is BOUNDARY-CLEAN and Shell-hostable — the D10
# shell-boundary gate audits it on `context_editor`'s closure. Do NOT re-add a kernel-internal module
# here; whatever needs assetdb::/filesync:: types belongs in gui/panels/builders/.
# context_warnings is PRIVATE (no flag leak to a CEF-host consumer, per conventions.md). NO new
# third-party dependency (no vcpkg / license-allowlist change).
target_link_libraries(context_gui_panel_files
    PUBLIC context_gui_uitree context_bridge
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling gui/panels/scenetree
# pattern); tests/files_test.h is the tiny CHECK() harness. context_gui_panel_builders pulls
# context_compose/context_assetdb (std::filesystem), so static-link the GCC runtime (gated to GNU) to
# keep the local ctest gate deterministic against a PATH-shadowed libstdc++-6.dll (setup.md §
# Preconditions).
set(_context_gui_panel_files_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_panel_files_tests
    test_files_model
    test_files_panel
    test_files_write
    test_a11y)

# The tests link context_gui_panel_builders too: they exercise the kernel-side build_files_model
# through its home (tests are not on any audited closure, so the assetdb/filesync reach is fine —
# exactly the D10 split's point).
foreach(_t IN LISTS _context_gui_panel_files_tests)
    add_executable(context_gui_panel_files_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_panel_files_${_t}
        PRIVATE context_gui_panel_files context_gui_panel_builders context_warnings)
    target_link_options(context_gui_panel_files_${_t}
        PRIVATE ${_context_gui_panel_files_static_runtime})
    target_include_directories(context_gui_panel_files_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-panel-files-${_t} COMMAND context_gui_panel_files_${_t})
endforeach()
