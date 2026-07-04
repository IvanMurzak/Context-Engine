# src/editor/filesync/ — the M1 file-sync layer (R-FILE-002/004, R-QA-010): atomic write-temp-rename
# IO, the watch-hash-reconcile pipeline (watcher-as-hint, hash-authoritative, persisted `.editor/index`,
# expected-writes self-echo suppression, dropped-event crawl safety net), and the crash-recovery intent
# log (HMAC-integrity-checked, jail/CAS-safe resume). Built ON the kernel platform seams (Clock /
# TaskRunner / EventBus, consumed read-only) with the layer's own richer FileStore + Watcher seams so
# the deterministic fault-injection harness can drive crash points and watcher loss/dup/reorder.

add_library(context_filesync STATIC
    src/content_hash.cpp
    src/sha256.cpp
    src/hmac.cpp
    src/path_jail.cpp
    src/file_store.cpp
    src/native_file_store.cpp
    src/atomic_io.cpp
    src/watcher.cpp
    src/native_watcher.cpp
    src/reconcile_index.cpp
    src/expected_writes.cpp
    src/reconciler.cpp
    src/intent_log.cpp
    src/sidecar.cpp)

target_include_directories(context_filesync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Consumes the microkernel read-only (Clock / TaskRunner / EventBus platform seams). PUBLIC because
# the reconciler's public header forward-declares those kernel types, so consumers need the kernel's
# include path. Threads::Threads is PUBLIC because the native watcher backends collect OS events on
# a private std::thread inside this STATIC library — every executable that links context_filesync
# needs -pthread at final link on Linux/GCC (a no-op on Windows/MSVC; found at top level).
# context_serializer is PUBLIC because sidecar.h (the L-33 binary-sidecar module) exposes
# serializer::SidecarRef in its API — the sanctioned serializer integration: the $sidecar reference
# SHAPE is read by the serializer's tree utilities, while resolution, the R-SEC-008 jail, and
# on-disk verification stay in this layer. context_serializer is a leaf (stdlib-only), so no cycle.
target_link_libraries(context_filesync PUBLIC context_kernel context_serializer Threads::Threads)

# FSEvents (the macOS native watcher backend) lives in the CoreServices framework. PUBLIC for the
# same static-library reason: the flag must reach the final link of every consuming executable.
if(APPLE)
    target_link_libraries(context_filesync PUBLIC "-framework CoreServices")
endif()

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_filesync PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo has no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist entry);
# each test is a plain assertion-bearing executable registered with ctest. See tests/filesync_test.h.
# Coverage spans happy paths, edge cases, AND failure/crash paths (torn writes, dropped watcher
# events, corrupt/foreign/jail-escaping/moved-on intent-log entries, and — for the native on-disk
# store — real-FS atomic-rename/fsync/stat/list + on-disk crash recovery).
set(_context_filesync_tests
    test_content_hash
    test_sha256
    test_atomic_io
    test_path_jail
    test_watcher
    test_native_watcher
    test_reconcile_index
    test_expected_writes
    test_reconciler
    test_intent_log
    test_native_file_store
    test_native_crash_recovery
    test_sidecar)

# Self-contained GCC runtime for the ctest executables. The native store + its tests pull std::filesystem
# (newer libstdc++ symbols); on a MinGW host a stale libstdc++-6.dll elsewhere on PATH can shadow the
# toolchain's and be loaded first, yielding STATUS_ENTRYPOINT_NOT_FOUND at launch. Static libgcc +
# libstdc++ make the test binaries self-contained and the local ctest gate deterministic. Gated to GNU
# so the macOS/Clang and Windows/MSVC CI legs are untouched (mirrors src/editor/bridge, editorkernel, cli).
set(_context_filesync_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

foreach(_t IN LISTS _context_filesync_tests)
    add_executable(context_filesync_${_t} tests/${_t}.cpp)
    target_link_libraries(context_filesync_${_t} PRIVATE context_filesync context_warnings)
    target_link_options(context_filesync_${_t} PRIVATE ${_context_filesync_static_runtime})
    target_include_directories(context_filesync_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME filesync-${_t} COMMAND context_filesync_${_t})
endforeach()
