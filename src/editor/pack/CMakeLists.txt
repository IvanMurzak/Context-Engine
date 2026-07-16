# src/editor/pack/ — the M8 build-side chunked content-pack WRITER (R-ASSET-005 / L-35 / L-37,
# docs/chunk-pack-format.md, task a01): packs flatten-emitted content units (src/editor/compose) into
# a v1 chunked, GUID-addressed archive with a directory (GUID->chunk index, per-chunk FNV-1a-64 hash,
# format/engine version header). Deterministic — the same (units, sidecars, engine version) produce
# byte-identical bytes (cache-keyable per R-FILE-010). Ships a synchronous build-side verification
# READER too (round-trip + golden gating); the runtime async streaming loader is the a02 task.

add_library(context_pack STATIC
    src/pack_format.cpp
    src/pack_reader.cpp
    src/pack_writer.cpp)

target_include_directories(context_pack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_compose: the public writer header exposes the compose ContentUnitSet / ComposedScene
# it packs (and reuses format_stable_id). context_compose links context_serializer PUBLIC, so the
# canonical writer + JsonValue tree the writer/reader build on come through transitively; named here
# too for clarity (pack_reader.cpp verifies chunk hashes via serializer::canonical_hash_of).
target_link_libraries(context_pack PUBLIC context_compose context_serializer)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_pack PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo has no C++ test framework — each test is a plain assertion-bearing executable registered
# with ctest. Coverage: the writer byte layout + round-trip (write -> read -> the parsed directory
# and chunk contents equal the input, composed-identity addressing preserved), determinism (same
# source packed twice is byte-identical — the R-FILE-010 cache-key property), packed sidecars, and
# the reader's self-verification failure paths (bad magic, truncation, a flipped byte -> hash
# mismatch). test_corpus replays the COMMITTED golden pack corpus (tests/corpus/ — the R-QA-011
# versioned deliverable: nested-instance content + sidecar payloads), byte-comparing each freshly
# built pack against its committed <name>.pack golden so a format/encoding change is a reviewed diff.
set(_context_pack_tests
    test_pack_writer
    test_pack_corpus)

foreach(_t IN LISTS _context_pack_tests)
    add_executable(context_pack_${_t} tests/${_t}.cpp)
    target_link_libraries(context_pack_${_t} PRIVATE context_pack context_warnings)
    target_include_directories(context_pack_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    # Self-contained GCC runtime: the corpus test walks committed goldens with std::filesystem +
    # stream file IO; on this repo's Windows/GCC host such an exe must static-link the GCC runtime or
    # a stale PATH libstdc++-6.dll aborts it at launch (see targets/context-engine/setup.md). Applied
    # to every pack test exe for determinism; gated to GNU so the MSVC/Clang CI legs are untouched.
    target_link_options(context_pack_${_t} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
    add_test(NAME pack-${_t} COMMAND context_pack_${_t})
endforeach()

target_compile_definitions(context_pack_test_pack_corpus PRIVATE
    PACK_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")

# The golden REBASELINE tool (NOT a ctest): rewrites tests/corpus/<name>.pack from the committed
# <name>.scenes.json case files. Golden pack bytes are platform-independent (little-endian + FNV +
# canonical JSON), so a Windows-built golden matches the Linux/macOS CI legs. Run it only for an
# intentional, reviewed format change (docs/chunk-pack-format.md), never to paper over a red gate.
add_executable(context_pack_golden_gen tests/pack_golden_gen.cpp)
target_link_libraries(context_pack_golden_gen PRIVATE context_pack context_warnings)
target_include_directories(context_pack_golden_gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_options(context_pack_golden_gen PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
target_compile_definitions(context_pack_golden_gen PRIVATE
    PACK_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
