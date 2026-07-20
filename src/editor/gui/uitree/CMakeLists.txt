# src/editor/gui/uitree/ — M5-F0b: the headless-instantiable UI-logic tree (context_gui_uitree,
# R-EDIT-001 / R-A11Y-001, issue #152). The seam every M5 fan-out panel is built ON, so panel logic
# (palette/undo/Problems/inspector) + the a11y audit are CI-assertable WITHOUT booting CEF. Pure
# stdlib — no CEF, no third-party dep — so it builds + tests on the DEFAULT 3-OS matrix and the local
# dev gate, unlike the CI-only CEF host (gui/host/).

add_library(context_gui_uitree STATIC
    src/node.cpp
    src/panel.cpp
    src/builtin.cpp)

target_include_directories(context_gui_uitree PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Every Context target links the warnings-as-errors baseline (conventions.md). PRIVATE: no warning
# flags leak to consumers — in particular the CEF host (gui/host/), which links this lib but must use
# CEF's own warning posture (the repo -Wpedantic baseline is incompatible with CEF headers).
target_link_libraries(context_gui_uitree PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling src/editor/contract/ +
# src/editor/bridge/ pattern); tests/uitree_test.h is the tiny CHECK() harness. render_html uses
# std::ostringstream, so static-link the GCC runtime (gated to GNU) to keep the local ctest gate
# deterministic against a PATH-shadowed libstdc++-6.dll (setup.md § Preconditions).
set(_context_gui_uitree_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_uitree_tests
    test_node
    test_panel
    test_a11y
    test_escaping)

foreach(_t IN LISTS _context_gui_uitree_tests)
    add_executable(context_gui_uitree_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_uitree_${_t} PRIVATE context_gui_uitree context_warnings)
    target_link_options(context_gui_uitree_${_t} PRIVATE ${_context_gui_uitree_static_runtime})
    target_include_directories(context_gui_uitree_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-uitree-${_t} COMMAND context_gui_uitree_${_t})
endforeach()
