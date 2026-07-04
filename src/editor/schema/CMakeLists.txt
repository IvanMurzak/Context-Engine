# src/editor/schema/ — the engine schema vocabulary + units law + per-kind versioned schema
# publication (M2 wave 2: R-DATA-006, L-32/L-34, issue #47). Pins the x-ctx-type / x-ctx-storage /
# x-ctx-ref annotations, the ONE tagged-union convention, and the SI+radians units law BEFORE the
# first component schemas freeze; compiles the engine kind schemas (scene, project) into the
# registration set the contract registry enumerates through `describe` (R-CLI-005/013); and provides
# the document validator the derivation validate node runs (R-FILE-003 pointer+line/column
# diagnostics). Depends only on the canonical serializer's JSON tree (deliberately contract-free —
# the contract registry consumes THIS module, never the reverse).

add_library(context_schema STATIC
    src/vocabulary.cpp
    src/kind_schema.cpp
    src/validator.cpp)

target_include_directories(context_schema PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: the public headers expose the serializer's JsonValue/Diagnostic types.
target_link_libraries(context_schema PUBLIC context_serializer)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_schema PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo has no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist
# entry); each test is a plain assertion-bearing executable registered with ctest (the sibling
# module pattern; tests/schema_test.h is the tiny CHECK() harness). Coverage spans the happy path
# (every semantic type's valid class, the engine kinds compiling, the scaffold-shaped scene
# validating), edge cases (units/storage/tag grammars, pointer location, version selection), and
# failure paths (every semantic type's invalid classes, the units LAW rejecting non-SI schemas,
# wrong-kind refs through the resolver seam, ad-hoc union encodings, malformed notes).
set(_context_schema_tests
    test_vocabulary
    test_kind_schema
    test_validator)

foreach(_t IN LISTS _context_schema_tests)
    add_executable(context_schema_${_t} tests/${_t}.cpp)
    target_link_libraries(context_schema_${_t} PRIVATE context_schema context_warnings)
    target_include_directories(context_schema_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME schema-${_t} COMMAND context_schema_${_t})
endforeach()
