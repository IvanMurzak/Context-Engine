# src/editor/migrate/ — per-component-payload schema versioning + parse-time migration (M2 wave 3:
# L-37 / R-DATA-004, issue #52). The registered migration set (current version per component type +
# the pure steps between versions) is, with the kind schemas, the R-FILE-005 pass-0 "registered
# schema + migration set": its content hash (set_hash) enters every pass-1 derivation cache key.
# migrate_document applies the per-payload chains IN MEMORY at parse time (disk truth never mutates
# silently); the explicit `context migrate` verb (src/cli/) is the only migration that writes disk.
# Depends only on the canonical serializer (deliberately schema-/contract-free: the derivation and
# composition layers consume THIS module, never the reverse).

add_library(context_migrate STATIC
    src/migration_set.cpp
    src/migrate_document.cpp)

target_include_directories(context_migrate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: the public headers expose the serializer's JsonValue/Diagnostic types.
target_link_libraries(context_migrate PUBLIC context_serializer)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_migrate PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern;
# tests/migrate_test.h is the tiny CHECK() harness + the reference migration steps the fixture
# corpus pins). Coverage spans the happy path (single/multi-step chains, multi-site documents,
# stamping, override rewrites), edge cases (selection matrix, path grammar, budget boundaries,
# set-hash sensitivity/order-independence), and failure paths (chain gaps, failing steps, id
# mutation, budget violations, sandboxed-tier refusal, newer-than stamps, rollback integrity).
set(_context_migrate_tests
    test_migration_set
    test_migrate_document
    test_path_transform
    test_migration_runner)

foreach(_t IN LISTS _context_migrate_tests)
    add_executable(context_migrate_${_t} tests/${_t}.cpp)
    target_link_libraries(context_migrate_${_t} PRIVATE context_migrate context_warnings)
    target_include_directories(context_migrate_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME migrate-${_t} COMMAND context_migrate_${_t})
endforeach()

# The R-QA-011 fixture corpus: pre-migration input + golden post-migration output per schema bump,
# committed under fixtures/ and kept forever. The test enumerates every fixture pair on disk (so a
# bump that forgets its goldens still fails: the reference steps demand their pairs), migrates each
# input, and byte-compares against the golden — plus the idempotence fixpoint (migrating a golden
# is a no-op). Uses std::filesystem, so it needs the self-contained GCC runtime (the setup.md
# libstdc++ DLL-shadowing quirk on MinGW hosts).
add_executable(context_migrate_test_fixtures tests/test_fixtures.cpp)
target_link_libraries(context_migrate_test_fixtures PRIVATE context_migrate context_warnings)
target_link_options(context_migrate_test_fixtures
    PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
target_include_directories(context_migrate_test_fixtures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_migrate_test_fixtures
    PRIVATE CONTEXT_MIGRATE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME migrate-test_fixtures COMMAND context_migrate_test_fixtures)
