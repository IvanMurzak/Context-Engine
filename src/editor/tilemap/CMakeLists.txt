# src/editor/tilemap/ — the tilemap authoring WRITE path (M8.5 a18: R-2D-003 GUI half / R-CLI-001 /
# L-20 / L-33). The ONE cell-edit core BOTH the tile-painting GUI (gesture-end commit) and the
# `context tilemap paint|fill` CLI verbs call, so GUI == CLI parity holds by construction. Rewrites
# EXISTING chunks' packed u32-LE cell payloads + refreshes the owner's "$sidecar" hash members,
# committing through filesync's L-33 sidecar-first family planner + R-FILE-004 atomic writes — the
# M2 ctx:tilemap kind/sidecar FORMAT is not changed here (the schema stays in editor/schema/, the
# content rules in editor/kinds/).

add_library(context_tilemap_edit STATIC
    src/tilemap_edit.cpp)

target_include_directories(context_tilemap_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: the public header exposes filesync::FileStore and serializer::JsonValue.
# context_warnings is PRIVATE (no flag leak to consumers, per conventions.md).
target_link_libraries(context_tilemap_edit
    PUBLIC context_filesync context_serializer
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion executable registered with ctest (the sibling module pattern; tests/
# tilemap_edit_test.h is the tiny CHECK() harness). context_filesync pulls std::filesystem
# transitively, so static-link the GCC runtime (gated to GNU) to keep the local ctest gate
# deterministic against a PATH-shadowed libstdc++-6.dll (setup.md § Preconditions).
set(_context_tilemap_edit_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

add_executable(context_tilemap_edit_test tests/test_tilemap_edit.cpp)
target_link_libraries(context_tilemap_edit_test
    PRIVATE context_tilemap_edit context_warnings)
target_link_options(context_tilemap_edit_test PRIVATE ${_context_tilemap_edit_static_runtime})
target_include_directories(context_tilemap_edit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME tilemap-edit-core COMMAND context_tilemap_edit_test)
