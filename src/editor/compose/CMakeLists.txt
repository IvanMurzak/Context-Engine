# src/editor/compose/ — the M2 scene-composition READ path (L-35 / R-DATA-002, issue #51):
# scenes instance other scenes with id-path-addressed, innermost-out override entries (the
# outermost instancing scene wins) plus explicit structural add/remove; composition flattens in
# the derivation graph at zero runtime cost. Stable intra-file ids (L-33), the scene-root entity
# (inert-by-default under instancing, `composable` opt-in), the R-FILE-011(e) nesting-depth cap +
# fan-in diagnostic + instancing-cycle diagnostic, deterministic composed identity (L-37), and the
# winning-value-first provenance chains of the R-CLI-006 read side. The composed WRITE path
# (`--edit-template` / `--at-instance`) is a separate follow-up task.

add_library(context_compose STATIC
    src/compose_write.cpp
    src/content_unit.cpp
    src/flatten.cpp
    src/json_pointer.cpp
    src/scene_model.cpp
    src/stable_id.cpp)

target_include_directories(context_compose PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_serializer: the public headers expose the serializer's JsonValue tree (scene
# docs, composed entity values) and the emitters produce canonical JSON. PUBLIC context_schema:
# the scene model binds on the schema module's kind id (kind_schema.h) and shares its read
# helpers (json_access.h).
target_link_libraries(context_compose PUBLIC context_serializer context_schema)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_compose PRIVATE context_warnings)

# The disk-backed composed-write resolver (project_resolver.h): a filesync-aware WriteResolver shared
# by the CLI `context set` (src/cli/set_command.cpp) AND the GUI in-context viewport override editor
# (src/editor/gui/viewport/, R-HUX-006) so both drive the SAME plan_write over the SAME on-disk read.
# Kept in a SEPARATE target so context_compose itself stays filesync-free (pure serializer/schema).
add_library(context_compose_project STATIC src/project_resolver.cpp)
target_include_directories(context_compose_project PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# PUBLIC context_compose (the WriteResolver interface + scene model) + context_filesync (the
# NativeFileStore + path jail) appear in the public header; context_serializer for canonicalize.
target_link_libraries(context_compose_project
    PUBLIC context_compose context_filesync context_serializer
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo has no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist
# entry); each test is a plain assertion-bearing executable registered with ctest. Coverage spans
# the happy path (instancing, override precedence, provenance chains, composed identity), edge
# cases (structural add/remove, inert vs composable roots, depth-cap boundary, fan-in threshold,
# id-space rules), and failure paths (cycles, missing scenes, orphan overrides, malformed
# entries, the entity budget).
#
# test_corpus additionally replays the COMMITTED composition edge-case corpus (tests/corpus/ —
# the R-QA-011 versioned deliverable this task starts: deep nesting, dense fan-in, cycles, orphan
# overrides), mirroring the serializer's test-vector corpus pattern.
set(_context_compose_tests
    test_stable_id
    test_json_pointer
    test_scene_model
    test_flatten
    test_flatten_diagnostics
    test_content_unit
    test_compose_write
    test_override_hygiene
    test_corpus)

foreach(_t IN LISTS _context_compose_tests)
    add_executable(context_compose_${_t} tests/${_t}.cpp)
    target_link_libraries(context_compose_${_t} PRIVATE context_compose context_warnings)
    target_include_directories(context_compose_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    # Self-contained GCC runtime: test_corpus walks the committed corpus with std::filesystem; on
    # this repo's Windows/GCC host a std::filesystem user must static-link the GCC runtime or a
    # stale PATH libstdc++-6.dll aborts the exe at launch with STATUS_ENTRYPOINT_NOT_FOUND (see
    # targets/context-engine/setup.md). Applied to every compose test exe for determinism; gated
    # to GNU so the MSVC/Clang CI legs are untouched.
    target_link_options(context_compose_${_t} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
    add_test(NAME compose-${_t} COMMAND context_compose_${_t})
endforeach()

target_compile_definitions(context_compose_test_corpus PRIVATE
    COMPOSE_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
