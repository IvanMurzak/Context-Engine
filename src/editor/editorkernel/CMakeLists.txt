# src/editor/editorkernel/ — the M1 EditorKernel composition. Wires the merged file-authoritative
# libraries into ONE runnable headless attach path (R-ARCH-005 / R-BRIDGE-008): file -> filesync
# watch-hash-reconcile -> incremental derivation -> derived context::kernel::World (+ generation) ->
# bridge daemon (single-instance lock + client event stream + R-SEC-007 scope dispatch). It COMPOSES
# the merged components via their public headers; it does not re-implement them.

add_library(context_editorkernel STATIC
    src/editor_kernel.cpp
    src/kernel_server.cpp)

target_include_directories(context_editorkernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# All five composed libraries are PUBLIC: the EditorKernel's public header exposes their types
# (bridge Daemon/StartOutcome/ScopeSet, derivation DerivationGraph/WriteTicket/BarrierResult, filesync
# Reconciler/ReconcileChange, contract Envelope, kernel World), so a consumer needs their include
# paths. context_derivation already links context_filesync + context_kernel, and context_bridge links
# context_contract + context_kernel, so several are transitively available; they are named explicitly
# because this target uses each directly. Every Context target links the warnings-as-errors baseline.
target_link_libraries(context_editorkernel
    PUBLIC context_bridge context_derivation context_filesync context_contract context_kernel
    PRIVATE context_warnings)

# The L-37 migration set + tool-save stamping (M2 wave 3, src/editor/migrate/): PUBLIC — the
# config header exposes migrate::MigrationSet (as an opaque seam pointer; only forward-declared),
# and the .cpp calls migrate_document/hash_combine on the save paths.
target_link_libraries(context_editorkernel PUBLIC context_migrate)

# Self-contained GCC runtime for the editorkernel ctest executables. The composition pulls the bridge
# daemon's single-instance lock (std::filesystem) and thus newer libstdc++ symbols; on a MinGW host a
# stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's and be loaded first, yielding
# STATUS_ENTRYPOINT_NOT_FOUND at launch. Static libgcc + libstdc++ make the test binaries
# self-contained and the local ctest gate deterministic. Gated to GNU so the macOS/Clang and
# Windows/MSVC CI legs (libc++ / MSVC runtime) are untouched (mirrors src/editor/bridge, src/cli).
set(_context_editorkernel_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling src/editor/bridge pattern);
# tests/editorkernel_test.h is the tiny CHECK() harness. The composed-loop integration smoke lives in
# test_editor_kernel (attach handshake + CLI-verb edit + raw edit + read-your-writes barrier +
# scope-denial exit-class 6).
set(_context_editorkernel_tests
    test_editor_kernel
    test_editor_kernel_native
    test_kernel_server)

foreach(_t IN LISTS _context_editorkernel_tests)
    add_executable(context_editorkernel_${_t} tests/${_t}.cpp)
    target_link_libraries(context_editorkernel_${_t} PRIVATE context_editorkernel context_warnings)
    target_link_options(context_editorkernel_${_t} PRIVATE ${_context_editorkernel_static_runtime})
    target_include_directories(context_editorkernel_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME editorkernel-${_t} COMMAND context_editorkernel_${_t})
endforeach()
