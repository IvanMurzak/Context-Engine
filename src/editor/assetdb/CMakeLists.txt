# src/editor/assetdb/ — the M2 asset database (issue #53: L-36 / R-ASSET-002). GUID + sidecar
# <asset>.meta.json identity (the `platforms` block reserved), the bounded lazy GUID index
# (R-FILE-011(e): scan reads sidecars only, never payloads), the REAL RefTargetResolver meta lookup
# behind the schema module's x-ctx-ref seam (R-DATA-006), dual-form reference checking + the
# ref-hint-healing write surface (L-34), and the meta-first move/rename + raw-move GUID healing
# operations (R-FILE-003's enumerated daemon writes, R-FILE-004's dependency-safe order).

add_library(context_assetdb STATIC
    src/guid.cpp
    src/meta.cpp
    src/asset_database.cpp
    src/ref_heal.cpp)

target_include_directories(context_assetdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: the public headers expose the schema module's RefTargetResolver/SchemaSet (and through
# it the serializer's JsonValue) plus the filesync FileStore seam the passes operate over.
target_link_libraries(context_assetdb PUBLIC context_schema context_filesync)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_assetdb PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo has no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist
# entry); each test is a plain assertion-bearing executable registered with ctest (the sibling
# module pattern; tests/assetdb_test.h is the tiny CHECK() harness). Coverage spans the happy path
# (scan/index/resolve, tool moves, raw-move healing, ref healing), edge cases (kindless assets,
# meta-less moves, hint enrichment, id-path shapes), and failure/crash paths (duplicate GUIDs,
# orphaned/malformed sidecars, dangling refs, ambiguous healing refusal, and the R-QA-010 crash
# windows around the meta-first write order via the fault-injectable MemoryFileStore).
set(_context_assetdb_tests
    test_guid
    test_meta
    test_asset_database
    test_move_heal
    test_ref_heal)

# Self-contained GCC runtime for the ctest executables: keeps the local MinGW ctest gate immune to
# a stale libstdc++-6.dll shadowing the toolchain's (mirrors filesync/bridge/editorkernel/cli).
set(_context_assetdb_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

foreach(_t IN LISTS _context_assetdb_tests)
    add_executable(context_assetdb_${_t} tests/${_t}.cpp)
    target_link_libraries(context_assetdb_${_t} PRIVATE context_assetdb context_warnings)
    target_link_options(context_assetdb_${_t} PRIVATE ${_context_assetdb_static_runtime})
    target_include_directories(context_assetdb_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME assetdb-${_t} COMMAND context_assetdb_${_t})
endforeach()
