# src/cli/ — the `context` CLI binary (M1). Parses the R-CLI-007 verb grammar, dispatches against
# the single registry in src/editor/contract/ (so CLI ≡ RPC ≡ MCP ≡ introspection by construction,
# R-CLI-009), and returns the uniform R-CLI-008 envelope. `context new` scaffolds the R-QA-006
# runnable default template and PROVES it startable by booting context_kernel — the CLI's concrete
# consumption of the microkernel.

# The CLI logic as a library so the ctest executables can link it without pulling in main().
add_library(context_cli STATIC
    src/app.cpp
    src/attach_command.cpp
    src/bench_command.cpp
    src/daemon_command.cpp
    src/editor_driver.cpp
    src/scaffold.cpp)

target_include_directories(context_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# context_contract is PUBLIC (its headers appear in the CLI's public headers); context_kernel,
# context_editorkernel, and context_serializer are PRIVATE (only scaffold.cpp's runnable-template
# proof touches context_kernel; only editor_driver.cpp's `context editor smoke` in-process driver
# touches context_editorkernel; only scaffold.cpp's canonical tool-save touches context_serializer —
# the public headers expose only contract types). Every target links the warnings-as-errors
# baseline (conventions.md).
target_link_libraries(context_cli
    PUBLIC context_contract
    PRIVATE context_kernel context_editorkernel context_serializer context_warnings)

# Self-contained GCC runtime for the CLI executables. context_cli uses std::filesystem (the
# `context new` scaffolder), whose libstdc++ symbols live in a NEWER libstdc++-6.dll; on a MinGW
# host a stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's and be loaded first,
# yielding a STATUS_ENTRYPOINT_NOT_FOUND at launch. Statically linking libgcc + libstdc++ makes the
# `context` binary self-contained (no external C++-runtime DLL dependency) and the local ctest gate
# deterministic regardless of PATH pollution. Gated to GNU: Apple Clang rejects -static-libstdc++
# (uses libc++) and MSVC has its own runtime model, so the macOS/Clang and Windows/MSVC CI legs are
# untouched; on Linux GCC it is a harmless, common self-contained-binary choice.
set(_context_cli_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# The `context` binary itself — a thin main() over context_cli.
add_executable(context src/main.cpp)
target_link_libraries(context PRIVATE context_cli context_warnings)
target_link_options(context PRIVATE ${_context_cli_static_runtime})

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling src/kernel/ pattern);
# tests/cli_test.h is the tiny CHECK() harness.
set(_context_cli_tests
    test_bench_command
    test_cli
    test_editor_driver
    test_scaffold)

foreach(_t IN LISTS _context_cli_tests)
    add_executable(context_cli_${_t} tests/${_t}.cpp)
    target_link_libraries(context_cli_${_t} PRIVATE context_cli context_warnings)
    target_link_options(context_cli_${_t} PRIVATE ${_context_cli_static_runtime})
    target_include_directories(context_cli_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME cli-${_t} COMMAND context_cli_${_t})
endforeach()

# test_scaffold additionally asserts the scaffolded files are canonicalization fixpoints
# (R-FILE-001 tool-save rule) AND that both templates validate clean against the registered engine
# kind schemas (R-DATA-006/#47 — the templates carry the L-32 `$schema` header from their first
# byte), so it links the serializer + schema modules directly (PRIVATE in context_cli, so their
# include paths do not propagate through it).
target_link_libraries(context_cli_test_scaffold PRIVATE context_serializer context_schema)

# --- cross-process e2e (R-QA-013 / issue #34): spawn a REAL `context daemon` process + a SEPARATE
# `context attach` process; the attach process drives the daemon over the wire and a 2nd `context
# daemon` proves the R-BRIDGE-001 attach signal. The test needs the built `context` binary's path,
# threaded in as a compile-time define ($<TARGET_FILE:context> resolves per-config), and must be built
# AFTER `context`. It links context_cli only for the tiny CHECK() harness + std process helpers.
add_executable(context_cli_test_attach_e2e tests/test_attach_e2e.cpp)
target_link_libraries(context_cli_test_attach_e2e PRIVATE context_cli context_warnings)
target_link_options(context_cli_test_attach_e2e PRIVATE ${_context_cli_static_runtime})
target_include_directories(context_cli_test_attach_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_cli_test_attach_e2e
    PRIVATE CONTEXT_BINARY="$<TARGET_FILE:context>")
add_dependencies(context_cli_test_attach_e2e context)
add_test(NAME cli-test_attach_e2e COMMAND context_cli_test_attach_e2e)
# The daemon child can block if something goes wrong; cap the cross-process test so CI never hangs.
set_tests_properties(cli-test_attach_e2e PROPERTIES TIMEOUT 60)
