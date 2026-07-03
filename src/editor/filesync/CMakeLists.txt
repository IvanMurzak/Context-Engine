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
    src/atomic_io.cpp
    src/watcher.cpp
    src/reconcile_index.cpp
    src/expected_writes.cpp
    src/reconciler.cpp
    src/intent_log.cpp)

target_include_directories(context_filesync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Consumes the microkernel read-only (Clock / TaskRunner / EventBus platform seams). PUBLIC because
# the reconciler's public header forward-declares those kernel types, so consumers need the kernel's
# include path.
target_link_libraries(context_filesync PUBLIC context_kernel)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_filesync PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo has no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist entry);
# each test is a plain assertion-bearing executable registered with ctest. See tests/filesync_test.h.
# Coverage spans happy paths, edge cases, AND failure/crash paths (torn writes, dropped watcher
# events, corrupt/foreign/jail-escaping/moved-on intent-log entries).
set(_context_filesync_tests
    test_content_hash
    test_sha256
    test_atomic_io
    test_path_jail
    test_watcher
    test_reconcile_index
    test_expected_writes
    test_reconciler
    test_intent_log)

foreach(_t IN LISTS _context_filesync_tests)
    add_executable(context_filesync_${_t} tests/${_t}.cpp)
    target_link_libraries(context_filesync_${_t} PRIVATE context_filesync context_warnings)
    target_include_directories(context_filesync_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME filesync-${_t} COMMAND context_filesync_${_t})
endforeach()
