# src/runtime/determinism/ — the deterministic-build ATTESTATION (M6-F0a, R-SIM-005 / L-54, anchored
# in R-SEC-009, issue #170). Produces a `deterministic:true` attestation ONLY from the actually-
# applied, verified strict-FP flags, never a self-declared bit: this library is compiled under
# whatever FP flags the build applied, and produce_attestation() cross-checks compiler-set macros
# (__FAST_MATH__ / MSVC _M_FP_*) — which the compiler sets from the REAL flag — against the flags
# CMake recorded it applied. The determinism.attestation_* fail-closed codes are the source-of-truth
# strings the contract error catalog registers (the promote-a-local-string pattern; no contract link).

add_library(context_determinism STATIC src/attestation.cpp)

target_include_directories(context_determinism PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_determinism PRIVATE context_warnings)

# The strict-FP flags the build ACTUALLY applied for this compiler, recorded by the parent scope's
# CONTEXT_DETERMINISTIC flag block in src/CMakeLists.txt (empty on a non-deterministic build). Baked
# in as a compile definition so the attestation is a record of the APPLIED flags, not a source literal
# — the anti-self-declaration property R-SIM-005 requires.
target_compile_definitions(context_determinism PRIVATE
    CONTEXT_DETERMINISTIC_FLAGS="${CONTEXT_DETERMINISTIC_APPLIED_FLAGS}")

# On this MinGW host a stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's and be
# loaded first under ctest; static-link the GCC runtime (gated to GNU — Apple Clang/libc++ and MSVC
# untouched) so the attestation test executable is self-contained (the sibling session/cli rationale).
set(_context_determinism_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The `determinism-attestation` ctest runs under BOTH the default build's Determinism gate (where the
# real produce_attestation() must report an HONEST false) and the dedicated deterministic-preset CI
# job (where it must PRODUCE deterministic:true from the verified flags on clang / Apple clang / MSVC).
add_executable(context_determinism_test_attestation tests/test_attestation.cpp)
target_link_libraries(context_determinism_test_attestation PRIVATE context_determinism context_warnings)
target_link_options(context_determinism_test_attestation PRIVATE ${_context_determinism_static_runtime})
target_include_directories(context_determinism_test_attestation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME determinism-attestation COMMAND context_determinism_test_attestation)
