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
    src/fetch_command.cpp
    src/migrate_command.cpp
    src/scaffold.cpp
    src/set_command.cpp
    src/wire_client.cpp)

target_include_directories(context_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# context_contract and context_bridge are PUBLIC (their headers appear in the CLI's public
# headers — contract types everywhere; wire_client.h exposes bridge::TransportClient for the
# operational wire commands); context_kernel, context_editorkernel, context_schema, and
# context_serializer are PRIVATE (only scaffold.cpp's runnable-template proof touches
# context_kernel; only editor_driver.cpp's `context editor smoke` in-process driver touches
# context_editorkernel; only scaffold.cpp touches context_schema — the L-32 `$schema` template
# headers — and context_serializer — the canonical tool-save). Every target links the
# warnings-as-errors baseline (conventions.md).
target_link_libraries(context_cli
    PUBLIC context_contract context_bridge
    PRIVATE context_kernel context_editorkernel context_schema context_serializer context_warnings)

# `context set` / `context query --overrides` (M2 composed write path, src/editor/compose/ +
# src/editor/filesync/): PRIVATE — set_command.cpp is the only consumer, driving compose's IO-free
# plan_write()/override_hygiene() and filesync's R-FILE-004 atomic_write + content_hash.
target_link_libraries(context_cli PRIVATE context_compose context_filesync)

# `context migrate` (M2 wave 3, src/editor/migrate/): PUBLIC — migrate_command.h names
# migrate::MigrationSet in its test-seam signature, and the ctest executable that injects its own
# set reaches the module's include path through this target.
target_link_libraries(context_cli PUBLIC context_migrate)

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
    test_migrate_command
    test_scaffold
    test_set_command)

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

# --- cross-process R-CLI-017 e2e: a REAL daemon with a lowered --large-result-threshold serves an
# oversized `describe` as a largeResult HANDLE; a separate `context fetch` process retrieves +
# reassembles it over resource.read (full + range + unknown-handle failure). Same harness shape as
# the attach e2e (CONTEXT_BINARY define, built after `context`, reaped daemon, capped runtime).
add_executable(context_cli_test_fetch_e2e tests/test_fetch_e2e.cpp)
target_link_libraries(context_cli_test_fetch_e2e PRIVATE context_cli context_warnings)
target_link_options(context_cli_test_fetch_e2e PRIVATE ${_context_cli_static_runtime})
target_include_directories(context_cli_test_fetch_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_cli_test_fetch_e2e
    PRIVATE CONTEXT_BINARY="$<TARGET_FILE:context>")
add_dependencies(context_cli_test_fetch_e2e context)
add_test(NAME cli-test_fetch_e2e COMMAND context_cli_test_fetch_e2e)
set_tests_properties(cli-test_fetch_e2e PROPERTIES TIMEOUT 60)
