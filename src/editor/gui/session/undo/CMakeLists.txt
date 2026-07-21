# src/editor/gui/session/undo/ — M5-F7: GUI session undo/redo over the file-write journal
# (context_gui_undo, R-HUX-001 / R-FILE-006 / R-FILE-007 / L-20 / L-21 / L-30, issue #162). Ctrl+Z/Y
# session undo with gesture-batch auto-checkpointing, scoped to the F3 inspector override-write
# surface. Undo/redo replays as CAS-guarded override writes through the EXISTING inspector gateway
# seam (the ONE write path) applying the shared L-30 rebase-or-drop policy, so it can NEVER clobber a
# concurrent writer (R-HUX-001). Reuses the `cas.mismatch` catalog code — no new mints.
#
# Pure C++ over the F0b headless uitree lib + the F3 inspector commit engine — NO CEF — so it builds +
# unit-tests on the DEFAULT 3-OS matrix and the local dev gate, exactly like context_gui_panel_inspector.
# The `.editor/session.json` disk write is the CEF-host integration path; this lib owns no disk (it
# (de)serializes to a JSON tree), mirroring the inspector panel.

add_library(context_gui_undo STATIC
    src/undo_journal.cpp)

target_include_directories(context_gui_undo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public header (undo_journal.h):
#   context_gui_panel_inspector -> inspector::OverrideWriteGateway / CommitResult /
#                                  commit_override_write   (the shared L-30 commit engine + seam)
#   context_gui_uitree          -> uitree::Panel            (build_panel())
#   context_serializer          -> serializer::JsonValue    (FieldEdit values + to_json())
# Since the e05d3 boundary split, context_gui_panel_inspector carries NO compose type on its PUBLIC
# interface (the seam is the boundary-clean inspector::OverrideWriteRequest, which undo_journal.cpp
# builds directly); the serializer helpers still arrive transitively. context_warnings is PRIVATE
# (no flag leak to a CEF-host consumer, per conventions.md). NO new third-party dependency.
target_link_libraries(context_gui_undo
    PUBLIC context_gui_panel_inspector context_gui_uitree context_serializer
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling gui/panels/inspector pattern);
# tests/undo_test.h is the tiny CHECK() harness. The lib pulls std::filesystem-adjacent libstdc++
# symbols transitively (via context_compose), so static-link the GCC runtime (gated to GNU) to keep the
# local ctest gate deterministic against a PATH-shadowed libstdc++-6.dll (setup.md § Preconditions).
# Target/exe names avoid the install/setup/update/patch tokens that trigger Windows UAC auto-elevation
# under ctest (setup.md).
set(_context_gui_undo_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_undo_tests
    test_undo_journal
    test_undo_a11y)

foreach(_t IN LISTS _context_gui_undo_tests)
    add_executable(context_gui_undo_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_undo_${_t}
        PRIVATE context_gui_undo context_warnings)
    target_link_options(context_gui_undo_${_t}
        PRIVATE ${_context_gui_undo_static_runtime})
    target_include_directories(context_gui_undo_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-session-undo-${_t} COMMAND context_gui_undo_${_t})
endforeach()
