# src/editor/bridge/ — the EditorKernel bridge (M1): the daemon-per-worktree, the single-instance
# lock (R-BRIDGE-001 / R-ARCH-005), the JSON-RPC 2.0 RPC dispatcher with R-SEC-007 scope enforcement
# (dispatching OVER the single contract registry — it does not re-declare verbs), and the
# client-facing R-BRIDGE-008 event stream (distinct from the kernel-internal EventBus). Consumes
# context_contract (registry + envelope + handshake) and context_kernel (LogEvent forwarding).

add_library(context_bridge STATIC
    src/scope.cpp
    src/lock.cpp
    src/event_stream.cpp
    src/dispatcher.cpp
    src/daemon.cpp
    src/transport.cpp
    src/resource_store.cpp)

target_include_directories(context_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# context_contract + context_kernel are PUBLIC: their headers appear in the bridge's public headers
# (dispatcher/daemon expose contract types; event_stream forwards kernel::LogEvent). Threads::Threads
# is PUBLIC because the IPC transport uses <thread> (backoff sleep) and downstream tests spawn a
# server thread (-pthread on Linux; a no-op on Windows). Every Context target links the
# warnings-as-errors baseline (conventions.md).
target_link_libraries(context_bridge
    PUBLIC context_contract context_kernel Threads::Threads
    PRIVATE context_warnings)

# Self-contained GCC runtime for the bridge ctest executables. The lock + daemon use std::filesystem
# (and thus newer libstdc++ symbols); on a MinGW host a stale libstdc++-6.dll elsewhere on PATH can
# shadow the toolchain's and be loaded first, yielding STATUS_ENTRYPOINT_NOT_FOUND at launch. Static
# libgcc + libstdc++ make the test binaries self-contained and the local ctest gate deterministic.
# Gated to GNU so the macOS/Clang and Windows/MSVC CI legs (which use libc++ / the MSVC runtime) are
# untouched (mirrors src/cli/CMakeLists.txt).
set(_context_bridge_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling src/editor/contract/ +
# src/kernel/ pattern); tests/bridge_test.h is the tiny CHECK() harness.
set(_context_bridge_tests
    test_scope
    test_lock
    test_event_stream
    test_dispatcher
    test_daemon
    test_transport
    test_resource_store)

foreach(_t IN LISTS _context_bridge_tests)
    add_executable(context_bridge_${_t} tests/${_t}.cpp)
    target_link_libraries(context_bridge_${_t} PRIVATE context_bridge context_warnings)
    target_link_options(context_bridge_${_t} PRIVATE ${_context_bridge_static_runtime})
    target_include_directories(context_bridge_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME bridge-${_t} COMMAND context_bridge_${_t})
endforeach()
