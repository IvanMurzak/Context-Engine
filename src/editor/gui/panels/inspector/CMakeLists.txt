# src/editor/gui/panels/inspector/ — M5-F3: the inspector panel (context_gui_panel_inspector,
# R-EDIT-001 / R-DATA-002 / L-35 / L-20 / L-30 / R-CLI-005 / R-A11Y-001 / R-HUX-011, issue #160).
# Renders the selected entity's component schemas (via R-CLI-005 introspection) as a headless
# context_gui_uitree Panel of editable widgets, and commits edits as L-35 override writes through the
# EXISTING `context set` write path (single source of truth — no parallel writer), CAS-guarded (L-20
# gesture-end), applying the L-30 rebase-or-drop policy under a concurrent writer. Reuses existing
# error-catalog codes (cas.mismatch, compose.*) — no new mints.
#
# Pure C++ over the F0b headless libs + the M2 compose write path + the schema introspection surface —
# NO CEF — so it builds + unit-tests on the DEFAULT 3-OS matrix and the local dev gate, exactly like
# context_gui_panel_scenetree. The primary CI assertion (schema -> widget tree -> override-write
# envelope -> CAS rebase-or-drop under concurrency) never boots CEF.

add_library(context_gui_panel_inspector STATIC
    src/inspector_model.cpp
    src/inspector_panel.cpp)

target_include_directories(context_gui_panel_inspector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_uitree  -> uitree::Panel                 (inspector_panel.h build_panel())
#   context_serializer  -> serializer::JsonValue         (inspector_model.h field values,
#                                                         inspector_panel.h staged edits/FieldState)
# NO context_compose / context_schema (M9 e05d3, D10 owner ruling): the kernel-typed builders and
# the compose::WriteRequest construction moved to context_gui_panel_builders, and the gateway seam
# takes the boundary-clean inspector::OverrideWriteRequest — so this library is BOUNDARY-CLEAN and
# Shell-hostable (the D10 shell-boundary gate audits it on `context_editor`'s closure). Do NOT
# re-add a kernel-internal module here; whatever needs compose::/schema:: belongs in
# gui/panels/builders/. context_serializer is deliberately NOT on the gate's FORBIDDEN list (it is
# the value DOM, not a kernel module). context_warnings is PRIVATE (no flag leak to a CEF-host
# consumer, per conventions.md). NO new third-party dependency (no vcpkg / license-allowlist change).
target_link_libraries(context_gui_panel_inspector
    PUBLIC context_gui_uitree context_serializer
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling gui/panels/scenetree +
# gui/uitree pattern); tests/inspector_test.h is the tiny CHECK() harness. context_compose /
# context_schema pull std::filesystem-adjacent libstdc++ symbols transitively, so static-link the GCC
# runtime (gated to GNU) to keep the local ctest gate deterministic against a PATH-shadowed
# libstdc++-6.dll (setup.md § Preconditions). Target/exe names avoid the install/setup/update/patch
# tokens that trigger Windows UAC auto-elevation under ctest (setup.md).
set(_context_gui_panel_inspector_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_panel_inspector_tests
    test_inspector_model
    test_inspector_panel
    test_a11y)

# The tests link context_gui_panel_builders too: they exercise the MOVED build_inspector_model /
# override_write_request through their new kernel-side home (tests are not on any audited closure,
# so the compose/schema reach is fine — that is exactly the D10 split's point).
foreach(_t IN LISTS _context_gui_panel_inspector_tests)
    add_executable(context_gui_panel_inspector_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_panel_inspector_${_t}
        PRIVATE context_gui_panel_inspector context_gui_panel_builders context_warnings)
    target_link_options(context_gui_panel_inspector_${_t}
        PRIVATE ${_context_gui_panel_inspector_static_runtime})
    target_include_directories(context_gui_panel_inspector_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-panel-inspector-${_t} COMMAND context_gui_panel_inspector_${_t})
endforeach()
