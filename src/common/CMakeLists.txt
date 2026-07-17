# src/common/ — shared cross-cutting infrastructure with no engine-domain dependencies. Today: the
# ONE hardened std::system subprocess + scratch-temp-file runner (context_common / issue #146,
# post-PR-#134) that the shadercc WGSL leg, the esbuild + tsgo TS drivers, and the real-git merge
# test all consume, replacing their diverged private copies. Dependency-free (stdlib + the warnings
# baseline only), so it builds under EVERY toolchain including the local Ninja + Strawberry-GCC dev
# gate; consumers name the `context_common` target, resolved at generate time regardless of
# add_subdirectory order.

add_library(context_common STATIC
    src/subprocess.cpp
    src/verify_signature.cpp)

target_include_directories(context_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_common PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executable registered with ctest (the sibling module pattern;
# tests/common_test.h is the tiny CHECK() harness). The quoting/metacharacter policy is exercised for
# BOTH shell shapes on every OS leg via the explicit Shell parameter (happy + failure paths). The exe
# does stream file IO (the scratch read/write round-trip), so static-link the GCC runtime — gated to
# GNU so the MSVC/Clang CI legs are untouched (setup.md § the libstdc++ DLL-shadowing note).
add_executable(context_common_test_subprocess tests/test_subprocess.cpp)
target_link_libraries(context_common_test_subprocess PRIVATE context_common context_warnings)
target_link_options(context_common_test_subprocess
    PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
target_include_directories(context_common_test_subprocess PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME common-subprocess COMMAND context_common_test_subprocess)

# The R-SEC-009 verify-before-use gate (task a08): the C++ mirror of tools/verify_artifact.py's
# fail-closed coverage. Reuses the committed TEST-ONLY fixtures under tools/tests/fixtures/ (baked in
# as a compile define, the sibling test_merge_command CONTEXT_MERGE_CORPUS_DIR pattern) so the crypto
# paths verify a real committed signature; the exe streams file IO, so static-link the GCC runtime
# (GNU-gated) exactly like the subprocess test above. The crypto assertions self-skip where OpenSSH is
# absent; ssh-keygen is on every CI runner, so they run on all three build OS legs.
add_executable(context_common_test_verify_signature tests/test_verify_signature.cpp)
target_link_libraries(context_common_test_verify_signature PRIVATE context_common context_warnings)
target_link_options(context_common_test_verify_signature
    PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
target_include_directories(context_common_test_verify_signature PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_common_test_verify_signature
    PRIVATE CONTEXT_VERIFY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tools/tests/fixtures")
add_test(NAME common-verify_signature COMMAND context_common_test_verify_signature)
