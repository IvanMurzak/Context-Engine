# src/common/ — shared cross-cutting infrastructure with no engine-domain dependencies. Today: the
# ONE hardened std::system subprocess + scratch-temp-file runner (context_common / issue #146,
# post-PR-#134) that the shadercc WGSL leg, the esbuild + tsgo TS drivers, and the real-git merge
# test all consume, replacing their diverged private copies. Dependency-free (stdlib + the warnings
# baseline only), so it builds under EVERY toolchain including the local Ninja + Strawberry-GCC dev
# gate; consumers name the `context_common` target, resolved at generate time regardless of
# add_subdirectory order.

add_library(context_common STATIC
    src/subprocess.cpp)

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
