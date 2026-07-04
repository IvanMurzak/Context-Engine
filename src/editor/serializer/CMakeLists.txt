# src/editor/serializer/ — the REAL canonical-JSON serializer (M2 wave 1: R-FILE-001, L-32/L-33,
# issue #42): strict JSON parse -> NFC-normalized, key-sorted, ECMAScript-number-formatted canonical
# bytes + the canonical-content hash, with the ASCII quick-check NFC fast path and the machine-
# readable encoding diagnostics (BOM/CRLF -> R-FILE-003). Replaces the M1 whitespace placeholder
# BEHIND the derivation parse-node seam (src/editor/derivation/canonical_parse.h delegates here).
# Deliberately dependency-free (stdlib only): this is the foundation layer every other M2 data-model
# component builds on, and the default build must stay vcpkg-less.

add_library(context_serializer STATIC
    src/canonical.cpp
    src/json_parse.cpp
    src/nfc.cpp)

target_include_directories(context_serializer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_serializer PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo has no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist entry);
# each test is a plain assertion-bearing executable registered with ctest. Coverage spans the happy
# path (parse -> canonical bytes -> fixpoint), edge cases (number-notation boundaries, NFC
# composition/exclusions/Hangul, key ordering, escapes), and failure paths (grammar violations,
# duplicate keys, invalid UTF-8/surrogates, depth cap, NaN/Infinity rejection).
#
# test_vectors additionally replays the COMMITTED cross-implementation test-vector corpus
# (tests/vectors/ — R-QA-011: a versioned deliverable every writer implementation must reproduce
# byte-exactly) and test_property runs the seeded serialize∘parse idempotence + re-canonicalization
# fixpoint properties (the M0 parse-bench fixpoint method, in-repo).
set(_context_serializer_tests
    test_json_parse
    test_ecma_number
    test_nfc
    test_canonical_write
    test_vectors
    test_property
    test_nfc_bench)

foreach(_t IN LISTS _context_serializer_tests)
    add_executable(context_serializer_${_t} tests/${_t}.cpp)
    target_link_libraries(context_serializer_${_t} PRIVATE context_serializer context_warnings)
    target_include_directories(context_serializer_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME serializer-${_t} COMMAND context_serializer_${_t})
endforeach()

# test_vectors walks the committed corpus with std::filesystem: on this repo's Windows/GCC host a
# std::filesystem user must static-link the GCC runtime or a stale PATH libstdc++-6.dll aborts the
# exe at launch with STATUS_ENTRYPOINT_NOT_FOUND (see targets/context-engine/setup.md). Gated to
# GNU so the MSVC/Clang CI legs are untouched.
target_link_options(context_serializer_test_vectors PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
target_compile_definitions(context_serializer_test_vectors PRIVATE
    SERIALIZER_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
