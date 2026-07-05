# src/runtime/save/ — player save-game groundwork (R-DATA-005 / L-37; M2 issue #66). The FIRST
# src/runtime/ (RuntimeKernel) target: src/runtime/ was a placeholder dir with no CMake target, so
# this wires the first one. A save is RuntimeKernel's OWN serialization (R-FILE-009 — never authored
# files): a per-component schemaVersion header (the L-32/L-37 stamps) + composed-identity-addressed
# entity records, plus a minimal save-migration runner. The runner reuses the editor's per-payload
# migration mechanism (context_migrate::migrate_payload) so a shipped build loads older saves through
# EXACTLY the mechanism the editor uses at parse time, rather than a second copy (R-DATA-005).

add_library(context_save STATIC
    src/save_document.cpp
    src/save_migration.cpp)

target_include_directories(context_save PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_serializer: the public headers expose the serializer JsonValue tree (save payloads)
# and Diagnostic. PUBLIC context_migrate: the save-migration runner reuses the shared per-payload
# migration primitive + the MigrationSet/MigrationBudget types. context_migrate is a serialization-
# layer leaf (it links ONLY context_serializer, no derivation/watcher machinery), so RuntimeKernel
# consuming the migration MECHANISM does not reach into EditorKernel derivation — R-FILE-009 holds:
# saves are the runtime's own serialization, and "the same per-payload migration mechanism the editor
# uses at parse time" is exactly what R-DATA-005 calls for.
target_link_libraries(context_save PUBLIC context_serializer context_migrate)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_save PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). Coverage: save serialize/parse round-trip + the parse failure
# paths (save_test.h harness), and the save-migration runner across a schema bump, composed-identity
# stability, back-compat scope, unknown component, newer-than, and all-or-nothing rollback.
add_executable(context_save_test_save_document tests/test_save_document.cpp)
target_link_libraries(context_save_test_save_document PRIVATE context_save context_warnings)
target_include_directories(context_save_test_save_document PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME save-test_save_document COMMAND context_save_test_save_document)

# The migration test REUSES the migrate module's reference steps (editor/migrate/tests/migrate_test.h:
# the test:sprite v1->v2->v3 chain the R-QA-011 fixtures pin) so the round-trip-across-a-schema-bump
# exercises the SAME migration the editor task ships — no duplicate migration set.
add_executable(context_save_test_save_migration tests/test_save_migration.cpp)
target_link_libraries(context_save_test_save_migration PRIVATE context_save context_migrate context_warnings)
target_include_directories(context_save_test_save_migration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/editor/migrate/tests)
add_test(NAME save-test_save_migration COMMAND context_save_test_save_migration)
