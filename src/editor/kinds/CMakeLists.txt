# src/editor/kinds/ — the first content asset KINDS' semantics (M2 wave 4: R-2D-003 tilemap +
# R-I18N-001 string-table, issue #61). The CONTENT rules the small schema dialect cannot express:
# the L-33 ~1 MB split-nudge over tilemap chunk cell payloads, stable-id uniqueness, and the
# string-table's CLDR plural-category selection + fallback-chain resolution + semantic validation.
# The per-kind JSON SCHEMAS themselves live in src/editor/schema/ (engine_schemas(), auto-registered
# by the contract registry loop); this module is the behaviour ON TOP of the registered schemas.
# Depends ONLY on the canonical serializer's JSON tree — schema/filesync are TEST-only deps (fixture
# validation against the real registration + the real L-33 sidecar codec round-trip).

add_library(context_kinds STATIC
    src/tilemap.cpp
    src/string_table.cpp
    src/anim_graph.cpp
    src/audio_bus.cpp
    src/audio_event.cpp)

target_include_directories(context_kinds PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: the public headers expose the serializer's JsonValue type.
target_link_libraries(context_kinds PUBLIC context_serializer)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_kinds PRIVATE context_warnings)

# --- tests (R-QA-013: behaviour ships WITH its tests in the same PR) -----------------------------
# Plain assertion executables registered with ctest (the sibling module pattern; tests/kinds_test.h
# is the CHECK() harness). Coverage spans schema-fixture validation against the REAL engine_schemas()
# registration (valid + every invalid class), the L-33 split-nudge / region / id semantics, CLDR
# plural evaluation across the rule families, fallback-chain resolution (multi-hop + cycle + implicit
# source), key/plural resolution, the semantic validators, canonical round-trips, and the
# author-headless-then-query-back smoke. test_tilemap also links context_filesync to prove the
# binary-sidecar day-one round-trip through the real PR #54 codec.
#
# Self-contained GCC runtime for the ctest executables: context_filesync pulls std::filesystem (newer
# libstdc++ symbols); on a MinGW host a stale libstdc++-6.dll elsewhere on PATH can shadow the
# toolchain's and be loaded first, yielding STATUS_ENTRYPOINT_NOT_FOUND at launch. Static libgcc +
# libstdc++ make the test binaries self-contained. Gated to GNU so the macOS/Clang and Windows/MSVC
# CI legs are untouched (mirrors src/editor/filesync, bridge, editorkernel, cli).
set(_context_kinds_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# test_tilemap: kinds + schema (fixture validation) + filesync (the real sidecar codec).
add_executable(context_kinds_test_tilemap tests/test_tilemap.cpp)
target_link_libraries(context_kinds_test_tilemap
    PRIVATE context_kinds context_schema context_filesync context_warnings)
target_link_options(context_kinds_test_tilemap PRIVATE ${_context_kinds_static_runtime})
target_include_directories(context_kinds_test_tilemap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME kinds-tilemap COMMAND context_kinds_test_tilemap)

# test_string_table: kinds + schema (fixture validation).
add_executable(context_kinds_test_string_table tests/test_string_table.cpp)
target_link_libraries(context_kinds_test_string_table
    PRIVATE context_kinds context_schema context_warnings)
target_link_options(context_kinds_test_string_table PRIVATE ${_context_kinds_static_runtime})
target_include_directories(context_kinds_test_string_table PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME kinds-string-table COMMAND context_kinds_test_string_table)

# test_anim_graph: kinds + schema (fixture validation against the REAL engine_schemas() ctx:anim-graph
# registration) + the referential-integrity semantic analyzer (M6 P3, issue #180).
add_executable(context_kinds_test_anim_graph tests/test_anim_graph.cpp)
target_link_libraries(context_kinds_test_anim_graph
    PRIVATE context_kinds context_schema context_warnings)
target_link_options(context_kinds_test_anim_graph PRIVATE ${_context_kinds_static_runtime})
target_include_directories(context_kinds_test_anim_graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME kinds-anim-graph COMMAND context_kinds_test_anim_graph)

# test_audio_kinds: kinds + schema (fixture validation against the REAL engine_schemas() ctx:audio-bus
# + ctx:audio-event registration) + the referential-integrity semantic analyzers (M6 P6, issue #184).
add_executable(context_kinds_test_audio tests/test_audio.cpp)
target_link_libraries(context_kinds_test_audio
    PRIVATE context_kinds context_schema context_warnings)
target_link_options(context_kinds_test_audio PRIVATE ${_context_kinds_static_runtime})
target_include_directories(context_kinds_test_audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME kinds-audio COMMAND context_kinds_test_audio)
