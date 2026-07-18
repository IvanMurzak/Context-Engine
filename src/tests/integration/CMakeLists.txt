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

# ===================================================================================================
# The SAMPLES CORPUS gate (R-QA-006 agent few-shot corpus; R-BUILD-009 / R-QA-008; issue #106) — was
# the M3-exit contract-freeze READINESS artifact (protocolMajor 0->1, PR #113, now FROZEN at 1); it
# continues post-freeze as the maintained breadth-of-surface gate (see samples/README.md).
# One blocking per-PR ctest on every OS leg (`samples-corpus`, run by the "Samples corpus gate" step
# in .github/workflows/ci.yml): Leg A spawns the REAL `context` binary to build + headless-smoke-run
# each committed sample project; Leg B exercises the breadth of the stable one-shot CLI surface in
# process over a staged copy and asserts the corpus covers the R-CLI-009 registry (verbs ≡ methods ≡
# tools by construction) + every registered file kind — the maintained rots-if-broken gate.
add_executable(context_test_samples_corpus test_samples_corpus.cpp)
# context_cli (the shipped verb grammar, cli::run) transitively carries its private backing libraries
# (compose/merge/migrate/session/pkg/…) for linking; context_contract PUBLICly exposes the ONE
# registry the coverage assertion enumerates. Self-contained GCC runtime (std::filesystem), gated to
# GNU so the macOS/Clang + Windows/MSVC CI legs are untouched (mirrors m1/m2-exit + src/cli).
target_link_libraries(context_test_samples_corpus
    PRIVATE context_cli context_contract context_warnings Threads::Threads)
target_link_options(context_test_samples_corpus PRIVATE ${_m2_exit_static_runtime})
target_include_directories(context_test_samples_corpus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cli/tests) # process_util.h (header-only, test-only)
# Leg A spawns the REAL built `context` binary; both legs read the committed corpus under samples/
# (repo root — one level above CMAKE_SOURCE_DIR, which is src/ under the `-S src` configure).
target_compile_definitions(context_test_samples_corpus PRIVATE
    CONTEXT_BINARY="$<TARGET_FILE:context>"
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
add_dependencies(context_test_samples_corpus context)

add_test(NAME samples-corpus COMMAND context_test_samples_corpus)
# Leg A spawns child processes + Leg B records/replays sessions; cap so a regression never hangs CI.
# 180 (not 120): Leg A is now 6 spawns (3 projects x validate/migrate) at a 15s per-spawn ceiling —
# 180 keeps the worst case surfacing its per-spawn diagnostics instead of truncating the whole gate.
set_tests_properties(samples-corpus PROPERTIES TIMEOUT 180)

# ===================================================================================================
# The M6 EXIT-A SAMPLE-GAME SMOKE GATES (issue #194; R-QA-005 / R-QA-013 / R-HEAD-002) — one per
# sample game. Each game's runtime mirror (games/) registers as a session SCENARIO (session.h §
# scenario registry) and PARSES its authored samples/<game>/ scene for the entity layout, so the
# authored corpus is load-bearing: rename/move an authored entity and the smoke gate goes red
# (rots-if-broken beyond schema validity). The gates drive the REAL RuntimeKernel session surface —
# simTick, the ordinary injection sink via the REAL context_input router, the hierarchical state
# hash — headless, GPU-free, audio on miniaudio's NULL backend, and assert a cross-platform golden
# (the integer/fixed-point sim law makes the digests portable). These are the per-game proofs the
# EXIT-B m6-exit-* gates ride; they run in the build job's ordinary ctest pass on all three OS legs.
#
# The games library sits ABOVE the session + packages (test-support): the context_session LIBRARY
# gains no package dependency (L-60 layering unchanged — the determinism_gameplay_scene precedent).
add_library(context_sample_games STATIC
    games/roll3d_scenario.cpp
    games/platformer2d_scenario.cpp)
target_include_directories(context_sample_games PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/games)
target_link_libraries(context_sample_games
    PUBLIC context_session context_physics3d context_physics2d context_animation context_particles
           context_input context_audio context_simmath
    PRIVATE context_serializer context_warnings)

add_executable(context_test_game_roll3d test_game_roll3d.cpp)
target_link_libraries(context_test_game_roll3d PRIVATE context_sample_games context_warnings)
target_link_options(context_test_game_roll3d PRIVATE ${_m2_exit_static_runtime})
target_include_directories(context_test_game_roll3d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_game_roll3d PRIVATE
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
add_test(NAME game-smoke-roll-3d COMMAND context_test_game_roll3d)
set_tests_properties(game-smoke-roll-3d PROPERTIES TIMEOUT 120)

add_executable(context_test_game_platformer2d test_game_platformer2d.cpp)
target_link_libraries(context_test_game_platformer2d PRIVATE context_sample_games context_warnings)
target_link_options(context_test_game_platformer2d PRIVATE ${_m2_exit_static_runtime})
target_include_directories(context_test_game_platformer2d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_game_platformer2d PRIVATE
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
add_test(NAME game-smoke-platformer-2d COMMAND context_test_game_platformer2d)
set_tests_properties(game-smoke-platformer-2d PROPERTIES TIMEOUT 120)

# ===================================================================================================
# The M5 EXIT GATE (ROADMAP §1 M5 Exit / issue #168) — the observer-editor milestone-closing mirror of
# the M1/M2/M4 gates: the headless end-to-end walkthrough + the a11y/keyboard-nav + coverage gate + the
# M5 seam checklist, each a BLOCKING per-PR ctest on every OS leg (registered in
# docs/ci-fleet-manifest.json as the m5-exit-* gates; run by the "M5 exit gate" step in
# .github/workflows/ci.yml):
#   m5-exit-1-walkthrough    — open -> inspect (scene-tree F2 / inspector F3 / Problems F4) -> play (F5)
#                              -> override-edit (F3) -> undo (F7) -> viewport (F1) observes the SAME
#                              render snapshot; R-HUX-011 human-loops measured from instrumented timestamps
#   m5-exit-2-a11y-coverage  — a11y scan + keyboard-only nav green on every panel; coverage.manifest.jsonl
#                              complete (the a11y registry ⟷ the manifest; F4 Problems now registered)
#   m5-exit-3-seam-checklist — the M5 observer-editor seam set, one assertion per seam (executable audit)
#
# All three drive the LANDED headless editor-GUI panels (uitree / scenetree / inspector / problems /
# viewport / playbar / undo — CEF-free, R-EDIT-001 testable-by-construction) IN PROCESS: no CEF, no GPU,
# no daemon, so they run on the default 3-OS build matrix ("headlessly across the OS matrix"). The per-OS
# CEF boot smoke (editor-cef-smoke), the native-vs-web golden-scene SSIM equivalence (render / render-web
# jobs vs goldens/), and the axe-class DOM a11y re-scan (tools/a11y_scan.py) are the SIBLING CI-job gates
# the M5 exit references.
set(_m5_exit_tests
    "test_m5exit1_walkthrough"
    "test_m5exit2_a11y_coverage"
    "test_m5exit3_seam_checklist")

set(_m5_exit_names
    "m5-exit-1-walkthrough"
    "m5-exit-2-a11y-coverage"
    "m5-exit-3-seam-checklist")

# Self-contained GCC runtime (mirrors m1/m2-exit + src/cli): m5-exit-2 uses std::ifstream to read the
# coverage manifest, whose newer-libstdc++ symbols a stale libstdc++-6.dll on PATH could shadow
# (STATUS_ENTRYPOINT_NOT_FOUND). Gated to GNU so the macOS/Clang + Windows/MSVC CI legs are untouched.
set(_m5_exit_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

list(LENGTH _m5_exit_tests _m5_exit_count)
math(EXPR _m5_exit_last "${_m5_exit_count} - 1")
foreach(_i RANGE ${_m5_exit_last})
    list(GET _m5_exit_tests ${_i} _t)
    list(GET _m5_exit_names ${_i} _name)

    add_executable(context_${_t} ${_t}.cpp)
    # The union of the M5 headless editor-GUI libraries the exit gates drive (all CEF-free, default-built
    # on the 3-OS matrix): the a11y harness + registry, the R-EDIT-001 extension contract, the L-41
    # compositor seam, every observer panel (scene-tree / inspector / problems / viewport / playbar) + the
    # F7 session undo, over uitree. context_compose / context_serializer / context_render back the
    # composed-world + `context set` write-path + render-snapshot seams the walkthrough exercises.
    target_link_libraries(context_${_t}
        PRIVATE context_gui_a11y context_gui_contract context_gui_compositor
                context_gui_panel_scenetree context_gui_panel_inspector context_gui_panel_problems
                context_gui_viewport context_gui_playbar context_gui_undo context_gui_uitree
                context_compose context_serializer context_render context_warnings)
    target_link_options(context_${_t} PRIVATE ${_m5_exit_static_runtime})
    target_include_directories(context_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # m5-exit-2 cross-checks the a11y registry against the on-disk coverage manifest (declared == scanned).
    target_compile_definitions(context_${_t} PRIVATE
        CONTEXT_GUI_A11Y_MANIFEST="${CMAKE_SOURCE_DIR}/editor/gui/a11y/coverage.manifest.jsonl")

    add_test(NAME ${_name} COMMAND context_${_t})
endforeach()

# The walkthrough chains seven panels + a compose/flatten; cap each so a regression never hangs CI.
set_tests_properties(
    m5-exit-1-walkthrough
    m5-exit-2-a11y-coverage
    m5-exit-3-seam-checklist
    PROPERTIES TIMEOUT 120)

# ===================================================================================================
# Sanitize preset only: context_cli links context_js (M6 X1 `context profile gc`), so on the
# V8-capable legs every m1/m2/samples gate executable here — and the REAL `context` binary the
# cross-process legs spawn — carries the uninstrumented rusty_v8 archive with its duplicate
# C++-runtime typeinfo. UBSan's vptr check false-positives on valid iostream code in the
# instrumented libraries, and printing those diagnostics makes the sanitizer's demangler allocate
# scratch that LSan then reports as a leak — failing the gate with no Context defect (sanitize
# (ASan+UBSan, ubuntu) leg, CI run 29197875018). Same fix as src/cli: wire the narrow
# demangler/symbolizer suppressions (runtime/ts/lsan-suppressions.txt — they can never mask a
# `context::` leak); spawned children inherit the env. Inert on the stub (non-V8) build. The
# m5-exit gates link only the gui libraries (no context_cli, no V8), so they are not wired.
if(CONTEXT_SANITIZE)
    set_tests_properties(
        ${_m1_exit_names}
        ${_m2_exit_names}
        samples-corpus
        PROPERTIES ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ts/lsan-suppressions.txt")
endif()

# ===================================================================================================
# The M6 EXIT GATE (design §M6-EXIT / issue #197) — the core-systems milestone-closing mirror of the
# M1/M2/M4/M5 gates: five blocking per-PR ctests on every OS leg (registered in
# docs/ci-fleet-manifest.json as the m6-exit-* gates; run by the "M6 exit gate" step in
# .github/workflows/ci.yml). They RIDE the landed M6 machinery — the EXIT-A sample games (#194),
# the X1 GC-pause profiler channel (#189), the X2 netsync harness (#191), and the X3 combined
# gameplay determinism scene (#193) — rather than rebuilding any of it:
#   m6-exit-1-games-run          — both games headless-step N ticks through the session surface with
#                                  every landed package visibly active (R-QA-005 simTick + queries)
#   m6-exit-2-gc-budget          — sustained movement.ts run: every JS-tier GC pause inside the
#                                  R-SIM-008 inter-tick budget via the X1 channel (R-LANG-012)
#   m6-exit-3-determinism-wedge  — the EXIT alias over the X3 combined-scene wedge gate (same
#                                  executable: cross-platform golden hash + determinism-diff triage)
#   m6-exit-4-netcode-harness    — the roll-3d moving-body scene replicated between two sessions on
#                                  the composed-id identity + dirty/delta metadata, converging
#   m6-exit-5-seam-checklist     — the executable M6 seam audit (one assertion per seam)

# Self-contained GCC runtime (mirrors m1/m2/m5-exit + src/cli): these executables use
# std::filesystem / stream IO, whose newer-libstdc++ symbols a stale PATH libstdc++-6.dll could
# shadow (STATUS_ENTRYPOINT_NOT_FOUND). Gated to GNU so the macOS/Clang + Windows/MSVC CI legs are
# untouched.
set(_m6_exit_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- m6-exit-1: both EXIT-A games run headless through the shipped session surface ---------------
add_executable(context_test_m6exit1_games_run test_m6exit1_games_run.cpp)
target_link_libraries(context_test_m6exit1_games_run PRIVATE context_sample_games context_warnings)
target_link_options(context_test_m6exit1_games_run PRIVATE ${_m6_exit_static_runtime})
target_include_directories(context_test_m6exit1_games_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_m6exit1_games_run PRIVATE
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
add_test(NAME m6-exit-1-games-run COMMAND context_test_m6exit1_games_run)

# --- m6-exit-2: the R-SIM-008 / R-LANG-012 GC-budget verdict over the authored movement.ts --------
# Links the X1 profiler channel (context_profile PUBLICly carries context_js — the REAL V8 backend
# on the CI legs, the stub locally) + the esbuild toolchain driver (context_ts). The staged esbuild
# path is exported by src/runtime/ts (CONTEXT_ESBUILD_BIN); the test branches at RUNTIME on
# v8BackendAvailable(), so no CONTEXT_JS_HAS_V8 define plumbing is needed here.
add_executable(context_test_m6exit2_gc_budget test_m6exit2_gc_budget.cpp)
target_link_libraries(context_test_m6exit2_gc_budget
    PRIVATE context_ts context_profile context_warnings)
target_link_options(context_test_m6exit2_gc_budget PRIVATE ${_m6_exit_static_runtime})
target_include_directories(context_test_m6exit2_gc_budget PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_m6exit2_gc_budget PRIVATE
    CONTEXT_ESBUILD_PATH="${CONTEXT_ESBUILD_BIN}"
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
# Same UBSan `vptr` false-positive carve-out as runtime/ts's V8-linking test TUs (issue #104): this
# TU's own std stream/file IO sits in a binary bundling the UNINSTRUMENTED rusty_v8 prebuilt (its
# Chromium libc++ RTTI confuses the vptr sub-check on valid libstdc++ streams). ASan + every other
# UBSan check stay ON; dev/CI release builds are byte-identical; non-MSVC only (a Clang/GCC flag).
if(CONTEXT_SANITIZE AND NOT MSVC)
    target_compile_options(context_test_m6exit2_gc_budget PRIVATE -fno-sanitize=vptr)
endif()
add_test(NAME m6-exit-2-gc-budget COMMAND context_test_m6exit2_gc_budget)

# --- m6-exit-3: the EXIT alias over the X3 combined-gameplay determinism wedge gate ---------------
# EXTENDS/RIDES the X3 gate (issue #192) instead of duplicating it: the SAME executable
# (context_session_determinism_gameplay — the physics-active + animated + particle combined scene,
# its cross-platform golden hierarchical state-hash, and the `context determinism diff` triage
# attribution) registered under the m6-exit- family, so the milestone exit formally blocks on the
# wedge law (L-54: byte-identical on Linux-x64 / Win-x64 / macOS-ARM64 — each build leg asserts the
# same golden). The F0a `deterministic:true` attestation from actually-applied strict-FP flags
# stays gated by the blocking `deterministic` job (determinism-attestation), whose hand-maintained
# --target list is deliberately UNCHANGED (this alias registers no new determinism-* ctest name).
add_test(NAME m6-exit-3-determinism-wedge COMMAND context_session_determinism_gameplay)

# --- m6-exit-4: the moving-body scene replicated between two sessions (L-48 / R-NET-001) ----------
add_executable(context_test_m6exit4_netcode_harness test_m6exit4_netcode_harness.cpp)
target_link_libraries(context_test_m6exit4_netcode_harness
    PRIVATE context_sample_games context_netsync context_warnings)
target_link_options(context_test_m6exit4_netcode_harness PRIVATE ${_m6_exit_static_runtime})
target_include_directories(context_test_m6exit4_netcode_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_m6exit4_netcode_harness PRIVATE
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
add_test(NAME m6-exit-4-netcode-harness COMMAND context_test_m6exit4_netcode_harness)

# --- m6-exit-5: the executable M6 seam audit (one assertion per seam) -----------------------------
add_executable(context_test_m6exit5_seam_checklist test_m6exit5_seam_checklist.cpp)
target_link_libraries(context_test_m6exit5_seam_checklist
    PRIVATE context_session context_physics3d context_physics2d context_animation
            context_particles context_spline context_input context_audio context_netsync
            context_determinism context_warnings)
target_link_options(context_test_m6exit5_seam_checklist PRIVATE ${_m6_exit_static_runtime})
target_include_directories(context_test_m6exit5_seam_checklist PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Seam 1 (the microkernel invariant) is a source audit: it scans the REAL src/kernel/ tree for
# package includes, so a reach-back can never land silently.
target_compile_definitions(context_test_m6exit5_seam_checklist PRIVATE
    CONTEXT_KERNEL_SRC_DIR="${CMAKE_SOURCE_DIR}/kernel")
add_test(NAME m6-exit-5-seam-checklist COMMAND context_test_m6exit5_seam_checklist)

# Gate 1 runs both 240-tick games; gate 2 boots V8 + 600 scheduled GC windows; the wedge alias
# re-runs the 5-fixture combined scene. Cap each so a regression never hangs the CI job.
set_tests_properties(
    m6-exit-1-games-run
    m6-exit-4-netcode-harness
    m6-exit-5-seam-checklist
    PROPERTIES TIMEOUT 240)
set_tests_properties(m6-exit-2-gc-budget m6-exit-3-determinism-wedge PROPERTIES TIMEOUT 300)

# Sanitizer-preset wiring for the ONE m6-exit gate that links the uninstrumented rusty_v8 archive
# (m6-exit-2 via context_profile -> context_js): the same narrow suppressions the sibling V8-linking
# tests wire — LSan's demangler/symbolizer scratch under ASan (can never mask a `context::` leak)
# and the benign V8-internal worker-pool races under TSan. The sanitizer presets are non-MSVC only,
# where the V8 backend links (the stub legs never configure them), so no _ctx_js_v8 re-derivation is
# needed; the env is inert if the stub is forced.
if(CONTEXT_SANITIZE)
    set_tests_properties(m6-exit-2-gc-budget PROPERTIES ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ts/lsan-suppressions.txt")
endif()
if(CONTEXT_TSAN)
    set_tests_properties(m6-exit-2-gc-budget PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/js/tsan-suppressions.txt")
    # TSan's per-access instrumentation inflates measured wall-clock GC-pause duration far beyond
    # the real R-SIM-008 budget (observed maxPauseMs=113.027 vs the true 4.167ms budget on an
    # otherwise-green CI run, while the SAME commit's ASan+UBSan leg measured maxPauseMs=0.991) —
    # a sanitizer-overhead artifact, not a regression. Widen the enforced budget on THIS preset
    # only; the blocking gate ("M6 exit gate" CI step, `ctest --preset dev`) never sets
    # CONTEXT_TSAN and always enforces the real, unwidened budget.
    target_compile_definitions(context_test_m6exit2_gc_budget PRIVATE CONTEXT_TSAN_BUILD=1)
endif()

# ===================================================================================================
# The M7 EXIT GATE (design 2026-07-13-m7-runtime-ui / a12-m7-exit) — the runtime-UI milestone-closing
# mirror of the M1/M2/M4/M5/M6 gates: five blocking per-PR ctests on every OS leg (registered in
# docs/ci-fleet-manifest.json as the m7-exit-* gates; run by the "M7 exit gate" step in
# .github/workflows/ci.yml, AFTER the M6 step). They RIDE the landed M7 runtime-UI machinery — the
# context_ui headless tree/layout/events (a1/a2/a3), the context.ui TS authoring surface (a4), the
# a5 `context ui` drive/assert verbs, the a6 GPU UI backend + a7/a8 text, the a9/a10 world-space
# (flat + curved) panels, and the a11 capability matrix — over REAL authored content
# (samples/platformer-2d/ui/hud.ui-hud.json + samples/roll-3d/ui/panel.ui-worldpanel.json):
#   m7-exit-1-hud-headless               — the authored platformer HUD data-binds (REAL a5 load path)
#                                          + renders headless (the a6 read-only extract, no GPU)
#   m7-exit-2-cli-drive                  — the REAL `context` binary drives the HUD via ui.* verbs
#                                          (cross-process; fail-closed on a wrong assert)
#   m7-exit-3-worldpanel                 — the roll-3d world-space panel LOGIC chain: raycast -> UV ->
#                                          panel coords -> the a3 single sink (a9 flat + a10 curved)
#   m7-exit-4-determinism-presentation   — hash_world bit-identical with UI absent / null / GPU (D6)
#   m7-exit-5-seam-checklist             — the executable M7 seam audit (incl. rulings c + d)
#
# The build job builds EVERY target via `--preset dev`, so each gate's executable is always built
# (never "Not Run") — NO `--target` list changes are needed (the M6 precedent); the strict-FP
# `deterministic` job's hand-maintained --target list is deliberately unchanged (m7-exit-4 registers
# no new determinism-* ctest name — the m6-exit-3 alias precedent). Green here CLOSES M7.

# Self-contained GCC runtime (mirrors m1/m2/m5/m6-exit + src/cli): these executables use
# std::filesystem / stream IO (+ spawn the real binary), whose newer-libstdc++ symbols a stale PATH
# libstdc++-6.dll could shadow (STATUS_ENTRYPOINT_NOT_FOUND). Gated to GNU so the macOS/Clang +
# Windows/MSVC CI legs are untouched.
set(_m7_exit_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- m7-exit-1: the authored platformer HUD data-binds (a5 in-process load path) + renders headless
#     (the a6 read-only extract). Links context_cli (the ui verbs' cli::run) + context_render_ui (the
#     GPU-free extract) + context_ui (the retained tree + null provider). ------------------------------
add_executable(context_test_m7exit1_hud_headless test_m7exit1_hud_headless.cpp)
target_link_libraries(context_test_m7exit1_hud_headless
    PRIVATE context_cli context_render_ui context_ui context_contract context_warnings)
target_link_options(context_test_m7exit1_hud_headless PRIVATE ${_m7_exit_static_runtime})
target_include_directories(context_test_m7exit1_hud_headless PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_m7exit1_hud_headless PRIVATE
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
add_test(NAME m7-exit-1-hud-headless COMMAND context_test_m7exit1_hud_headless)

# --- m7-exit-2: the REAL `context` binary drives the HUD via the a5 ui.* verbs, cross-process. Links
#     context_common (the hardened std::system runner + scratch temp file) + context_contract (parse
#     the captured R-CLI-008 envelope). Spawns the built `context` binary (CONTEXT_BINARY). -----------
add_executable(context_test_m7exit2_cli_drive test_m7exit2_cli_drive.cpp)
target_link_libraries(context_test_m7exit2_cli_drive
    PRIVATE context_common context_contract context_warnings)
target_link_options(context_test_m7exit2_cli_drive PRIVATE ${_m7_exit_static_runtime})
target_include_directories(context_test_m7exit2_cli_drive PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_m7exit2_cli_drive PRIVATE
    CONTEXT_BINARY="$<TARGET_FILE:context>"
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
add_dependencies(context_test_m7exit2_cli_drive context)
add_test(NAME m7-exit-2-cli-drive COMMAND context_test_m7exit2_cli_drive)

# --- m7-exit-3: the roll-3d world-space panel LOGIC chain. Links context_ui (curved_panel ray/UV
#     math + tree/layout) + context_ui_input (the a3 UiInputRouter single sink) + context_ui_raycast
#     (the a10 broad-phase-pruned picker) + context_input (the InputRouter) + context_session (the
#     sim sink + hash_world) + context_contract (parse the authored panel). ---------------------------
add_executable(context_test_m7exit3_worldpanel test_m7exit3_worldpanel.cpp)
target_link_libraries(context_test_m7exit3_worldpanel
    PRIVATE context_ui context_ui_input context_ui_raycast context_input context_session
            context_contract context_warnings)
target_link_options(context_test_m7exit3_worldpanel PRIVATE ${_m7_exit_static_runtime})
target_include_directories(context_test_m7exit3_worldpanel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_m7exit3_worldpanel PRIVATE
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
add_test(NAME m7-exit-3-worldpanel COMMAND context_test_m7exit3_worldpanel)

# --- m7-exit-4: hash_world bit-identical with UI absent / null / GPU (D6). Runs the platformer sample
#     game (context_sample_games) and presents a HUD through the null provider (context_ui) + the REAL
#     GpuUiProvider over the GPU-free fake RHI (context_render_ui + ../../render/tests/render_test_rhi.h,
#     the a11 capability-matrix precedent). ----------------------------------------------------------
add_executable(context_test_m7exit4_determinism_presentation test_m7exit4_determinism_presentation.cpp)
target_link_libraries(context_test_m7exit4_determinism_presentation
    PRIVATE context_sample_games context_render_ui context_ui context_session context_warnings)
target_link_options(context_test_m7exit4_determinism_presentation PRIVATE ${_m7_exit_static_runtime})
target_include_directories(context_test_m7exit4_determinism_presentation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../../render/tests)
target_compile_definitions(context_test_m7exit4_determinism_presentation PRIVATE
    CONTEXT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/../samples")
add_test(NAME m7-exit-4-determinism-presentation COMMAND context_test_m7exit4_determinism_presentation)

# --- m7-exit-5: the executable M7 seam audit (incl. rulings c + d). Links the union of the M7 UI
#     libraries the seams exercise: context_ui + context_ui_text (the a8 shaper) + context_ui_input
#     (the a3 glue) + context_ui_raycast (the a10 picker) + context_render_ui (the GPU provider, over
#     the fake RHI) + context_input + context_session (hash_world). -----------------------------------
add_executable(context_test_m7exit5_seam_checklist test_m7exit5_seam_checklist.cpp)
target_link_libraries(context_test_m7exit5_seam_checklist
    PRIVATE context_ui context_ui_text context_ui_input context_ui_raycast context_render_ui
            context_input context_session context_warnings)
target_link_options(context_test_m7exit5_seam_checklist PRIVATE ${_m7_exit_static_runtime})
target_include_directories(context_test_m7exit5_seam_checklist PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../../render/tests)
add_test(NAME m7-exit-5-seam-checklist COMMAND context_test_m7exit5_seam_checklist)

# m7-exit-2 spawns the real binary per verb (several short-lived processes); m7-exit-4 runs the sample
# game three times x 90 ticks presenting through both providers. Cap each so a regression never hangs CI.
set_tests_properties(
    m7-exit-1-hud-headless
    m7-exit-3-worldpanel
    m7-exit-5-seam-checklist
    PROPERTIES TIMEOUT 120)
set_tests_properties(m7-exit-2-cli-drive m7-exit-4-determinism-presentation PROPERTIES TIMEOUT 240)

# Sanitizer-preset wiring for the m7-exit gates that reach the uninstrumented rusty_v8 archive:
# m7-exit-1 links context_cli (-> context_js, the M6 X1 `context profile gc` path) and m7-exit-2
# SPAWNS the real `context` binary (which links it), so both need the same narrow LSan
# demangler/symbolizer suppressions the sibling V8-linking tests wire (they can never mask a
# `context::` leak); the spawned child inherits the env. The other three m7-exit gates link no V8.
if(CONTEXT_SANITIZE)
    set_tests_properties(
        m7-exit-1-hud-headless
        m7-exit-2-cli-drive
        PROPERTIES ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ts/lsan-suppressions.txt")
endif()

# The M8 EXIT GATE (ROADMAP §1-M8 exit / a14-m8-exit) — the build-pipeline milestone-closing mirror of
# the M1/M2/M4/M5/M6/M7 gates: five blocking per-PR ctests on every OS leg (registered in
# docs/ci-fleet-manifest.json as the m8-exit-* gates; run by the "M8 exit gate" step in
# .github/workflows/ci.yml, AFTER the M7 step). They RIDE the landed M8 build-pipeline machinery — the
# a05 build orchestrator (run_build), the a06/a10/a11/a13 export adapters, the a08 verify-before-use
# trust chain + the a10/a13 signing hooks, and the a07 packed-wedge determinism + replication gates:
#   m8-exit-1-platform-set        — the full v1 platform-set (linux desktop+server, windows, macos, web)
#                                   builds HEADLESS per-agent, each a supported runnable-artifact plan
#   m8-exit-2-desktop-signing     — a10 Authenticode + a13 Developer-ID/notarization hooks never-silent;
#                                   verify-before-use fails closed under the a08 pinned trust root
#   m8-exit-3-headless-smoke      — the R-BUILD-009 shipped-RuntimeKernel packed-boot smoke (a07 ALIAS)
#   m8-exit-4a-determinism-wedge  — the L-54 packed-wedge state-hash gate (a07 ALIAS)
#   m8-exit-4b-replication-wedge  — the L-48/R-NET-001 replication-against-packs gate (a07 ALIAS)
#   m8-exit-5-seam-checklist      — the executable M8 seam audit (incl. OPS1-honest advisory budget)
#
# The build job builds EVERY target via `--preset dev`, so each gate's executable is always built
# (never "Not Run") — NO `--target` list changes are needed (the M6/M7 precedent). The three -4/-3
# aliases RE-USE the a07 executables (the m6-exit-3 alias precedent — same executable, a milestone
# ctest name) so the milestone exit formally blocks on them WITHOUT registering a new determinism-*
# name: the strict-FP `deterministic` job's hand-maintained --target list is deliberately UNCHANGED
# (the aliases run only under the build job's "M8 exit gate" -R "^m8-exit-" step, which the
# `deterministic` job never runs). Green here CLOSES M8.

# Self-contained GCC runtime (mirrors m1/m2/m5/m6/m7-exit + src/cli): m8-exit-2/-5 use std::filesystem /
# stream IO (scratch verify artifact + reading the trust root / budget table), whose newer-libstdc++
# symbols a stale PATH libstdc++-6.dll could shadow (STATUS_ENTRYPOINT_NOT_FOUND). Gated to GNU so the
# macOS/Clang + Windows/MSVC CI legs are untouched.
set(_m8_exit_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# The committed pinned production trust root (a08) + the R-BUILD-006 build-time budget table (a12) — read
# by the signing + seam-checklist gates. CMAKE_SOURCE_DIR is the `-S src` root, so ../ is the repo root.
set(_m8_trust_root "${CMAKE_SOURCE_DIR}/../tools/trust-root/allowed_signers")
set(_m8_build_time_budget "${CMAKE_SOURCE_DIR}/../bench/build-time-budget.json")

# --- m8-exit-1: the full v1 platform-set builds headless per-agent (a05 run_build + a06/a10/a11/a13
#     adapters). Links context_build (the orchestrator + adapters; context_compose/pack/import come
#     transitively via its PUBLIC link). No filesystem — but keep the static-runtime idiom for parity. --
add_executable(context_test_m8exit1_platform_set test_m8exit1_platform_set.cpp)
target_link_libraries(context_test_m8exit1_platform_set PRIVATE context_build context_warnings)
target_link_options(context_test_m8exit1_platform_set PRIVATE ${_m8_exit_static_runtime})
target_include_directories(context_test_m8exit1_platform_set PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME m8-exit-1-platform-set COMMAND context_test_m8exit1_platform_set)

# --- m8-exit-2: the a10/a13 signing hooks + the a08 verify-before-use trust chain. Links context_build
#     (signing.h + build_errors.h) + context_common (verify_signature.h + subprocess.h). Reads the
#     pinned production trust root at test time. -------------------------------------------------------
add_executable(context_test_m8exit2_desktop_signing test_m8exit2_desktop_signing.cpp)
target_link_libraries(context_test_m8exit2_desktop_signing
    PRIVATE context_build context_common context_warnings)
target_link_options(context_test_m8exit2_desktop_signing PRIVATE ${_m8_exit_static_runtime})
target_include_directories(context_test_m8exit2_desktop_signing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_m8exit2_desktop_signing PRIVATE
    CONTEXT_TRUST_ROOT="${_m8_trust_root}")
add_test(NAME m8-exit-2-desktop-signing COMMAND context_test_m8exit2_desktop_signing)

# --- m8-exit-3 / m8-exit-4a / m8-exit-4b: the a07 packed-wedge gates re-registered under the m8-exit-
#     family (the m6-exit-3 ALIAS precedent — same executable, a milestone ctest name). No new build
#     targets, no --target list changes: the build job builds these via `--preset dev` regardless. -----
#   m8-exit-3  — R-BUILD-009 headless smoke: launch both SHIPPED wedge binaries over a real pack + step N.
add_test(NAME m8-exit-3-headless-smoke COMMAND context_runtime_host_test_smoke)
#   m8-exit-4a — L-54 packed-wedge determinism: the shipped RuntimeKernel's state hash == editor + golden.
add_test(NAME m8-exit-4a-determinism-wedge COMMAND context_runtime_host_test_packed_determinism)
#   m8-exit-4b — L-48/R-NET-001 replication metadata carried by the pack (M6 harness re-run vs packs).
add_test(NAME m8-exit-4b-replication-wedge COMMAND context_netsync_test_packed_replication)

# --- m8-exit-5: the executable M8 seam audit. Links context_build (orchestrator + adapters + signing) +
#     context_common (verify-before-use) + context_session (the wedge determinism law). Reads the trust
#     root + the build-time budget table. --------------------------------------------------------------
add_executable(context_test_m8exit5_seam_checklist test_m8exit5_seam_checklist.cpp)
target_link_libraries(context_test_m8exit5_seam_checklist
    PRIVATE context_build context_common context_session context_warnings)
target_link_options(context_test_m8exit5_seam_checklist PRIVATE ${_m8_exit_static_runtime})
target_include_directories(context_test_m8exit5_seam_checklist PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(context_test_m8exit5_seam_checklist PRIVATE
    CONTEXT_TRUST_ROOT="${_m8_trust_root}"
    CONTEXT_BUILD_TIME_BUDGET="${_m8_build_time_budget}")
add_test(NAME m8-exit-5-seam-checklist COMMAND context_test_m8exit5_seam_checklist)

# m8-exit-3/-4a launch shipped child processes over a real pack; -4b runs a 150-tick physics convergence;
# -1/-5 drive the orchestrator across the whole v1 target set. Cap each so a regression never hangs CI.
set_tests_properties(
    m8-exit-1-platform-set
    m8-exit-2-desktop-signing
    m8-exit-5-seam-checklist
    PROPERTIES TIMEOUT 120)
set_tests_properties(
    m8-exit-3-headless-smoke
    m8-exit-4a-determinism-wedge
    m8-exit-4b-replication-wedge
    PROPERTIES TIMEOUT 180)
