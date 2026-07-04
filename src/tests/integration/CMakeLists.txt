# src/tests/integration/ — the M1 EXIT GATE (ROADMAP §1 M1 Exit / issue #36).
#
# Five integration tests, one per exit criterion, each registered with ctest under the `m1-exit-`
# prefix so CI can gate the milestone by name (blocking per-PR on the 3-OS build matrix — the §6
# tier that hosts the CLI ≡ RPC ≡ MCP ≡ introspection + additive-catalog proof):
#   m1-exit-1-files-as-truth    — CLI verb ≡ raw text edit -> identical derived World (L-19)
#   m1-exit-2-live-attach       — 2nd CLI process attaches live via the handshake (R-CLI-010)
#   m1-exit-3-crash-recovery    — real kill -9 mid-multi-file-write, nothing lost (R-FILE-004)
#   m1-exit-4-contract-parity   — CLI ≡ RPC ≡ MCP ≡ introspection + additive-only (R-CLI-008/009)
#   m1-exit-5-scope-enforcement — read token cannot install/build via direct RPC (R-SEC-007)
#
# Criteria 2/3 (and the cross-process legs of 1/4/5) exercise the REAL `context daemon` process
# over the REAL loopback IPC wire (#35) — the built binary's path is threaded in as CONTEXT_BINARY
# (the proven src/cli e2e pattern), and the cli tests' header-only process helper is reused via an
# include path rather than a copy.

set(_m1_exit_tests
    # target-source                     ctest-name
    "test_exit1_files_as_truth"
    "test_exit2_second_client_attach"
    "test_exit3_crash_recovery_kill9"
    "test_exit4_contract_parity"
    "test_exit5_scope_enforcement")

set(_m1_exit_names
    "m1-exit-1-files-as-truth"
    "m1-exit-2-live-attach"
    "m1-exit-3-crash-recovery"
    "m1-exit-4-contract-parity"
    "m1-exit-5-scope-enforcement")

# Self-contained GCC runtime (mirrors src/cli, src/editor/editorkernel): these executables use
# std::filesystem, whose symbols live in a newer libstdc++; a stale libstdc++-6.dll on PATH would
# otherwise shadow the toolchain's and abort the ctest gate with STATUS_ENTRYPOINT_NOT_FOUND.
# Gated to GNU so the macOS/Clang and Windows/MSVC CI legs are untouched.
set(_m1_exit_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

list(LENGTH _m1_exit_tests _m1_exit_count)
math(EXPR _m1_exit_last "${_m1_exit_count} - 1")
foreach(_i RANGE ${_m1_exit_last})
    list(GET _m1_exit_tests ${_i} _t)
    list(GET _m1_exit_names ${_i} _name)

    add_executable(context_${_t} ${_t}.cpp)
    # context_editorkernel PUBLICly carries the five composed libraries (bridge, derivation,
    # filesync, contract, kernel) — the in-process compose legs need them directly. context_cli is
    # the shipped CLI dispatch path the parity gate drives (cli::run). Threads for the kill-9
    # test's blocked-request helper thread (Threads::Threads is found at top level).
    target_link_libraries(context_${_t}
        PRIVATE context_editorkernel context_cli context_warnings Threads::Threads)
    target_link_options(context_${_t} PRIVATE ${_m1_exit_static_runtime})
    target_include_directories(context_${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../cli/tests) # process_util.h (header-only, test-only)
    # The REAL daemon/attach child processes are the built `context` binary.
    target_compile_definitions(context_${_t} PRIVATE CONTEXT_BINARY="$<TARGET_FILE:context>")
    add_dependencies(context_${_t} context)

    add_test(NAME ${_name} COMMAND context_${_t})
endforeach()

# Cross-process tests spawn daemon children that could block if something regresses; cap every
# criterion so CI never hangs. The kill-9 gate gets extra headroom: its worst case is 5 race
# retries x (daemon boot 15s + mid-op poll 20s + kill reap 15s) + the recovery scenario — ~300s
# on a slow leg, so 600 keeps a 2x margin without letting a real hang eat the job budget.
set_tests_properties(
    m1-exit-1-files-as-truth
    m1-exit-2-live-attach
    m1-exit-4-contract-parity
    m1-exit-5-scope-enforcement
    PROPERTIES TIMEOUT 120)
set_tests_properties(m1-exit-3-crash-recovery PROPERTIES TIMEOUT 600)
