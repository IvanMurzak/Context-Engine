# src/editor/gui/panels/tilemap/ — M8.5 a18: the tilemap-painter panel (context_gui_panel_tilemap,
# R-2D-003 GUI half / R-EDIT-001 / R-A11Y-001 / L-20 / L-55). The headless logic of the tile-painting
# GUI: the 2D orthographic viewport-authoring mode (ortho camera, pixel<->cell grid snap), the tile
# palette, the paint/erase/fill tools, the keyboard-cursor authoring path, and the L-20 gesture
# lifecycle whose gesture-end commit runs the ONE editor/tilemap write-path core the `context tilemap
# paint|fill` CLI verbs also run (R-CLI-001 parity by construction).
#
# Pure C++ over the F0b headless libs + the shared write-path core — NO CEF — so it builds +
# unit-tests on the DEFAULT 3-OS matrix and the local dev gate, exactly like the sibling panels.

add_library(context_gui_panel_tilemap STATIC
    src/tilemap_paint_model.cpp
    src/tilemap_paint_panel.cpp)

target_include_directories(context_gui_panel_tilemap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_uitree   -> uitree::Panel           (tilemap_paint_panel.h build_panel())
#   context_tilemap_edit -> tme::CellEdit           (tilemap_paint_model.h gesture batch)
#     (which itself exposes filesync::FileStore + serializer::JsonValue PUBLICly).
# context_warnings is PRIVATE (no flag leak to a CEF-host consumer, per conventions.md). NO new
# third-party dependency (no vcpkg / license-allowlist change).
target_link_libraries(context_gui_panel_tilemap
    PUBLIC context_gui_uitree context_tilemap_edit
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling gui/panels pattern);
# tests/tilemap_test.h is the tiny CHECK() harness + shared fixture. context_filesync (via the edit
# core) pulls std::filesystem transitively, so static-link the GCC runtime (gated to GNU) to keep the
# local ctest gate deterministic against a PATH-shadowed libstdc++-6.dll (setup.md § Preconditions).
set(_context_gui_panel_tilemap_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_panel_tilemap_tests
    test_paint_model
    test_paint_panel
    test_a11y)

foreach(_t IN LISTS _context_gui_panel_tilemap_tests)
    add_executable(context_gui_panel_tilemap_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_panel_tilemap_${_t}
        PRIVATE context_gui_panel_tilemap context_warnings)
    target_link_options(context_gui_panel_tilemap_${_t}
        PRIVATE ${_context_gui_panel_tilemap_static_runtime})
    target_include_directories(context_gui_panel_tilemap_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-panel-tilemap-${_t} COMMAND context_gui_panel_tilemap_${_t})
endforeach()
