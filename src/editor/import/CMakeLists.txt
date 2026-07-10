# src/editor/import/ — the M2 importer framework (issue #60: R-ASSET-001, R-SEC-006/008/010,
# R-FILE-010, R-REND-006). Package-based importers (glTF / PNG / WAV) convert source assets into
# canonical engine-format descriptors: RUN-DETERMINISTIC (the CI double-run byte-compare gate,
# R-ASSET-001), ISOLATED (the v1 R-SEC-006 slice — the shared R-SEC-008 path jail + a scrubbed
# environment R-SEC-010 + no ambient network + input-bytes-only), incremental via the exhaustive
# R-FILE-010 cache key, with a data-driven per-platform transcode SKELETON. Mesh import reserves a
# UV2 lightmap channel (R-REND-006). Fuzz seed corpora are committed (R-QA-011) and replayed per PR.

add_library(context_import STATIC
    src/platform_profile.cpp
    src/import_settings.cpp
    src/cache_key.cpp
    src/sandbox.cpp
    src/sandbox_lockdown.cpp
    src/isolated_runner.cpp
    src/importer_registry.cpp
    src/importers/png_importer.cpp
    src/importers/wav_importer.cpp
    src/importers/gltf_importer.cpp)

target_include_directories(context_import PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PRIVATE: the serializer (canonical JSON parse/emit + the content-hash family) and filesync (the
# raw-byte content hash + the shared R-SEC-008 path jail) are used only in the .cpp implementation —
# no public import header exposes their types, so consumers do not inherit them.
target_link_libraries(context_import PRIVATE context_serializer context_filesync)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_import PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo carries no C++ test framework (that would pull a vcpkg dep + a license-allowlist entry);
# each test is a plain assertion-bearing executable registered with ctest (the sibling-module
# pattern; tests/import_test.h is the tiny CHECK() harness + the in-code asset byte-builders).
# Coverage spans the happy path (parse -> canonical descriptor, per-platform transcode rows, cache
# keying, isolation policy), edge cases (empty settings, platform overrides, UV2 populated vs
# reserved, multi-artifact keying), and failure paths (bad signatures, truncation, CRC mismatch,
# non-PCM/non-2.0 formats, jail escape, registry collisions).
set(_context_import_tests
    test_platform_profile
    test_import_settings
    test_cache_key
    test_sandbox
    test_registry
    test_png_importer
    test_wav_importer
    test_gltf_importer)

# Self-contained GCC runtime for the ctest executables: keeps the local MinGW ctest gate immune to a
# stale libstdc++-6.dll shadowing the toolchain's (the std::filesystem corpus walk needs it; mirrors
# assetdb/filesync/cli). No-op on MSVC/Clang CI legs.
set(_context_import_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

foreach(_t IN LISTS _context_import_tests)
    add_executable(context_import_${_t} tests/${_t}.cpp)
    target_link_libraries(context_import_${_t} PRIVATE context_import context_serializer context_warnings)
    target_link_options(context_import_${_t} PRIVATE ${_context_import_static_runtime})
    target_include_directories(context_import_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME import-${_t} COMMAND context_import_${_t})
endforeach()

# The R-ASSET-001 double-run byte-compare GATE and the R-QA-011 / R-SEC-006 fuzz-corpus replay both
# iterate the committed seed corpus, so they receive its absolute directory as argv[1]. The corpus
# is a committed R-QA-011 deliverable under tests/corpora/ (regenerable via tests/corpora/generate_corpus.py).
set(_context_import_corpora "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpora")
foreach(_ct IN ITEMS test_determinism test_fuzz_corpus)
    add_executable(context_import_${_ct} tests/${_ct}.cpp)
    target_link_libraries(context_import_${_ct} PRIVATE context_import context_serializer context_warnings)
    target_link_options(context_import_${_ct} PRIVATE ${_context_import_static_runtime})
    target_include_directories(context_import_${_ct} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME import-${_ct} COMMAND context_import_${_ct} "${_context_import_corpora}")
endforeach()
