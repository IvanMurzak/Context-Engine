# src/tests/integration/ — the M1 and M2 EXIT GATES (ROADMAP §1 M1 Exit / issue #36; M2 Exit / #68).
#
# The M2 exit gate (m2-exit-*, below) is the milestone-closing mirror of the M1 gate: five exit
# scenarios + an executable data-model seam checklist, each a blocking per-PR ctest on the 3-OS build
# matrix. Unlike M1 (whose criteria need a live daemon over the IPC wire) the M2 criteria are
# file-level data-model determinism proofs, so they run in-process over the real cli::run verb grammar
# + the compose/migrate/merge/schema/save libraries; m2-exit-1 additionally spawns the REAL `context`
# binary to prove the CLI authors a runnable scene across a true process boundary.
#
# ---------------------------------------------------------------------------------------------------
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

# ===================================================================================================
# The M2 EXIT GATE (ROADMAP §1 M2 Exit / issue #68) — five exit scenarios + the data-model seam
# checklist, each a BLOCKING per-PR ctest on every OS leg (registered in docs/ci-fleet-manifest.json
# as the m2-exit-* gates; run by the "M2 exit gate" step in .github/workflows/ci.yml):
#   m2-exit-1-cli-authoring           — author from the CLI, save/reload, GUID + $entity survive (L-19/34/37)
#   m2-exit-2-schema-migration        — a real schema bump, parse-time + bulk `context migrate` round-trip (L-37)
#   m2-exit-3-flatten-content-units   — flatten to GUID-addressable, independently load/unload units (R-ASSET-005)
#   m2-exit-4-composed-write-provenance — a composed write lands in the right file/level + provenance (R-CLI-006)
#   m2-exit-5-per-payload-migration   — per-payload migration round-trips against the R-QA-011 fixtures
#   m2-exit-6-seam-checklist          — the M2 data-model seam set, one assertion per seam (executable audit)
set(_m2_exit_tests
    # target-source                            ctest-name
    "test_m2exit1_cli_authoring_stable_refs"
    "test_m2exit2_schema_migration"
    "test_m2exit3_flatten_content_units"
    "test_m2exit4_composed_write_provenance"
    "test_m2exit5_per_payload_migration"
    "test_m2exit6_seam_checklist")

set(_m2_exit_names
    "m2-exit-1-cli-authoring"
    "m2-exit-2-schema-migration"
    "m2-exit-3-flatten-content-units"
    "m2-exit-4-composed-write-provenance"
    "m2-exit-5-per-payload-migration"
    "m2-exit-6-seam-checklist")

# Self-contained GCC runtime (mirrors m1-exit + src/cli): these executables use std::filesystem, whose
# symbols live in a newer libstdc++ a stale libstdc++-6.dll on PATH could shadow (STATUS_ENTRYPOINT_
# NOT_FOUND). Gated to GNU so the macOS/Clang + Windows/MSVC CI legs are untouched.
set(_m2_exit_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

list(LENGTH _m2_exit_tests _m2_exit_count)
math(EXPR _m2_exit_last "${_m2_exit_count} - 1")
foreach(_i RANGE ${_m2_exit_last})
    list(GET _m2_exit_tests ${_i} _t)
    list(GET _m2_exit_names ${_i} _name)

    add_executable(context_${_t} ${_t}.cpp)
    # The union of the M2 data-model libraries the gates exercise: cli (the verb grammar), compose
    # (flatten/content-units/provenance/identity), migrate (parse-time + bulk migration), merge
    # (dup-id + re-key), schema (vocabulary), serializer (sidecar refs), save (save-migration),
    # assetdb (meta). context_editorkernel carries bridge/derivation/filesync/contract/kernel PUBLICly.
    target_link_libraries(context_${_t}
        PRIVATE context_cli context_editorkernel context_compose context_migrate context_merge
                context_schema context_serializer context_save context_assetdb context_contract
                context_warnings Threads::Threads)
    target_link_options(context_${_t} PRIVATE ${_m2_exit_static_runtime})
    target_include_directories(context_${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../cli/tests        # process_util.h (m2-exit-1 spawns context)
        ${CMAKE_CURRENT_SOURCE_DIR}/../../editor/migrate/tests) # migrate_test.h (m2-exit-5 reference steps)
    # m2-exit-1 spawns the REAL built `context` binary; m2-exit-5 enumerates the R-QA-011 fixture
    # corpus. Both defines are set on every m2-exit target uniformly (harmless where unused).
    target_compile_definitions(context_${_t} PRIVATE
        CONTEXT_BINARY="$<TARGET_FILE:context>"
        CONTEXT_M2_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/editor/migrate/fixtures")
    add_dependencies(context_${_t} context)

    add_test(NAME ${_name} COMMAND context_${_t})
endforeach()

# The seam-checklist audit touches every data-model library and the flatten-heavy criteria compose
# deep graphs; cap each so a regression that live-locks a gate never hangs the CI job.
set_tests_properties(
    m2-exit-1-cli-authoring
    m2-exit-2-schema-migration
    m2-exit-3-flatten-content-units
    m2-exit-4-composed-write-provenance
    m2-exit-5-per-payload-migration
    m2-exit-6-seam-checklist
    PROPERTIES TIMEOUT 120)
