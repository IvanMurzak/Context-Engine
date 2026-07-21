# src/editor/gui/panels/builders/ — M9 e05d3: the KERNEL-SIDE panel-model builders
# (context_gui_panel_builders, D10/D18, owner ruling 2026-07-20).
#
# The kernel-typed halves split OUT of context_gui_panel_scenetree / context_gui_panel_inspector so
# those panel libraries carry NO compose::/schema:: type on their PUBLIC link interface and are
# therefore Shell-hostable under the D10 shell-boundary gate (src/CMakeLists.txt,
# `context_assert_shell_boundary`) with the FORBIDDEN list byte-identical. THIS library is the side
# of the wire that may link the kernel: the daemon (`editor.scene-tree` / `editor.inspect` serving),
# the disk-backed override gateway (gui/viewport), the in-process M5 harnesses, and the panel test
# suites link it; nothing on the Shell's audited closure ever does.

add_library(context_gui_panel_builders STATIC
    src/scene_tree_builder.cpp   # ComposedScene -> SceneTreeModel (moved verbatim from scenetree)
    src/inspector_builder.cpp    # ComposedEntity + KindSchema -> InspectorModel; clean->compose write requests
    src/wire.cpp)                # model -> contract::Json (the daemon's editor.* reply payloads)

target_include_directories(context_gui_panel_builders PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_panel_scenetree -> scenetree::SceneTreeModel   (scene_tree_builder.h / wire.h)
#   context_gui_panel_inspector -> inspector::InspectorModel / OverrideWriteRequest
#   context_compose             -> compose::ComposedScene / WriteRequest (both builder headers)
#   context_schema              -> schema::KindSchema           (inspector_builder.h)
#   context_contract            -> contract::Json               (wire.h)
# context_warnings is PRIVATE (no flag leak to a consumer, per conventions.md). NO new third-party
# dependency (no vcpkg / license-allowlist change).
target_link_libraries(context_gui_panel_builders
    PUBLIC context_gui_panel_scenetree context_gui_panel_inspector context_compose context_schema
           context_contract
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling panel pattern);
# tests/builders_test.h is the tiny CHECK() harness. The MOVED build_scene_tree /
# build_inspector_model logic keeps its behavioral coverage in the scenetree / inspector / m5-exit
# suites (which link this library now); the tests HERE cover what is NEW in e05d3 — the
# clean->compose request conversion and the wire projection. The round-trip against the Shell-side
# parsers lives with the feeds (src/editor/shell/panels/tests/), which link both halves.
set(_context_gui_panel_builders_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_panel_builders_tests
    test_write_request   # clean->compose conversion: every target mode, member fidelity
    test_wire)           # wire projection: shape, hex hashes, canonical values, determinism

foreach(_t IN LISTS _context_gui_panel_builders_tests)
    add_executable(context_gui_panel_builders_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_panel_builders_${_t}
        PRIVATE context_gui_panel_builders context_warnings)
    target_link_options(context_gui_panel_builders_${_t}
        PRIVATE ${_context_gui_panel_builders_static_runtime})
    target_include_directories(context_gui_panel_builders_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-panel-builders-${_t} COMMAND context_gui_panel_builders_${_t})
endforeach()
