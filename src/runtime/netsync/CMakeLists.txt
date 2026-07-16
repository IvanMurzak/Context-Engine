# src/runtime/netsync/ — the replication state-sync harness (M6 X2, R-NET-001 / L-48, issue X2).
# The layer that DRIVES the F0b in-storage L-48 replication metadata hooks (kernel World: per-entity
# network identity == the L-37 composed id, authority, dirty/delta versioning): an identity guard, a
# dirty-scanner (delta culling), snapshot capture of the dirty/delta since a peer cursor, and
# fail-closed atomic apply to a REPLICA world so it CONVERGES to the source. v1 is an in-process
# two-session harness (no network transport) — the R-NET-001 validation bar.
#
# It is a RUNTIME module composing on the kernel World (like runtime/session): it links ONLY
# context_kernel (World/Entity/Component + the L-48 hooks + the R-LANG-010 raw storage path it
# reconstructs replicated PODs through). The moving-body sim it replicates (context_physics3d /
# physics2d) is a TEST dependency only — the library itself is physics-agnostic (L-60: the kernel
# never links back, and netsync pulls in no package).

add_library(context_netsync STATIC
    src/state_sync.cpp)

target_include_directories(context_netsync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel: the public headers expose the kernel World/Entity/Component (the L-48
# replication hooks + the raw component storage the snapshots read/write). No other engine dependency
# — netsync is physics-agnostic.
target_link_libraries(context_netsync PUBLIC context_kernel)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_netsync PRIVATE context_warnings)

# On this MinGW host a stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's and be
# loaded first under ctest, so the test executables static-link the GCC runtime (gated to GNU — Apple
# Clang/libc++ and MSVC are untouched; the same self-contained-binary rationale as runtime/session,
# packages/physics3d, and src/cli).
set(_context_netsync_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). tests/netsync_test.h is the tiny CHECK() harness.
#   - test_state_sync: the generic state-sync core over synthetic POD components (no physics) —
#     capture/apply round-trip, DirtyScanner delta culling, the net-id identity guard, and the
#     snapshot-size-mismatch / authority-conflict fail-closed apply paths;
#   - test_errors: the net.* code strings pinned to their catalog identities (failure-path coverage).
# These link ONLY context_netsync.
set(_context_netsync_core_tests
    test_state_sync
    test_errors)

foreach(_t IN LISTS _context_netsync_core_tests)
    add_executable(context_netsync_${_t} tests/${_t}.cpp)
    target_link_libraries(context_netsync_${_t} PRIVATE context_netsync context_warnings)
    target_link_options(context_netsync_${_t} PRIVATE ${_context_netsync_static_runtime})
    target_include_directories(context_netsync_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME netsync-${_t} COMMAND context_netsync_${_t})
endforeach()

# --- the R-NET-001 two-session CONVERGENCE harness (the DoD centerpiece) -------------------------
# A REAL moving-body scene (dynamic context_physics3d + context_physics2d bodies) replicated
# source -> replica by dirty/delta over N fixed ticks; the replica, which never runs physics,
# CONVERGES to the source's positions/velocities. Links the physics packages (the moving-body sim) in
# addition to context_netsync. This is a convergence / netcode test, NOT a strict-FP determinism
# scene — it registers NO determinism-* ctest and is intentionally absent from the ci.yml
# `deterministic` job --target list.
add_executable(context_netsync_test_convergence tests/test_convergence.cpp)
target_link_libraries(context_netsync_test_convergence
    PRIVATE context_netsync context_physics3d context_physics2d context_warnings)
target_link_options(context_netsync_test_convergence PRIVATE ${_context_netsync_static_runtime})
target_include_directories(context_netsync_test_convergence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME netsync-test_convergence COMMAND context_netsync_test_convergence)

# --- the M8 a07 REPLICATION-AGAINST-PACKS gate (R-NET-001 / L-48) --------------------------------
# The M6 convergence harness re-run with the replication net-ids sourced FROM THE PACK: a real v1 pack
# is loaded back through the shipped runtime content seam (context_runtime_content over context_pack),
# each packed entity's composed identity (L-37) is read out, and the moving-body source->replica
# convergence is driven keyed by those pack-carried identities — validating the M8-exit clause that the
# wedge builds carry the validated L-48/R-NET-001 replication metadata. Links context_runtime_content +
# the pack/compose/serializer editor side (to build + load the pack) in addition to the physics packages.
# A CONVERGENCE / netcode test, NOT a strict-FP determinism scene — it registers NO determinism-* ctest
# and is intentionally absent from the ci.yml `deterministic` job --target list (like the sibling
# convergence test); it auto-runs on all three build-matrix legs via the `build` job's general step.
add_executable(context_netsync_test_packed_replication tests/test_packed_replication.cpp)
target_link_libraries(context_netsync_test_packed_replication
    PRIVATE context_netsync context_runtime_content context_pack context_compose context_serializer
            context_physics3d context_warnings)
target_link_options(context_netsync_test_packed_replication PRIVATE ${_context_netsync_static_runtime})
target_include_directories(context_netsync_test_packed_replication PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME netsync-packed-replication COMMAND context_netsync_test_packed_replication)
