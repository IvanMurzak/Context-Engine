# src/tests/concurrency/ — L-50 multi-client concurrency VALIDATION (issue #284, a16-l50): the
# end-to-end red-team of the shipped M1 concurrency + M2 structural-merge machinery. Two deterministic
# integration tests registered under the NON-gate `concurrency-` ctest prefix, so the `build` job's
# general ctest step (`ctest --preset dev ... -E "^m1-exit-|..."`, which builds every target via
# `--preset dev`) runs them on all THREE OS legs with NO ci.yml edit and ZERO "Not Run" risk — the CE
# CI-wiring discipline: a plain family outside the gate `-E` regex auto-runs in the general step, and
# `--preset dev` always builds its executable. NEW files only; the sole shared-file edit is the single
# add_subdirectory(tests/concurrency) line in src/CMakeLists.txt (append-only, sibling-conflict-safe).

# Self-contained GCC runtime (mirrors the m1/m2-exit gates + src/cli): both executables use
# std::filesystem / stream IO, whose newer-libstdc++ symbols a stale PATH libstdc++-6.dll could shadow
# (STATUS_ENTRYPOINT_NOT_FOUND). Gated to GNU so the macOS/Clang + Windows/MSVC CI legs are untouched.
set(_concurrency_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- concurrency-coedit: Scenario 1 (co-edit) -----------------------------------------------------
# The L-30 rebase-or-drop gesture engine under a concurrent second client + CAS --if-match + the
# R-QA-010 fault-seam composition check + the 10k-round no-lost-update seeded property (R-QA-011).
# Links context_gui_panel_inspector (the shared commit_override_write engine), context_testing (the
# SeededRng + fault-scenario drivers), context_filesync (content_hash), context_compose (WriteRequest).
add_executable(context_test_coedit_concurrency test_coedit_concurrency.cpp)
target_link_libraries(context_test_coedit_concurrency
    PRIVATE context_gui_panel_inspector context_testing context_filesync context_compose
            context_serializer context_warnings)
target_link_options(context_test_coedit_concurrency PRIVATE ${_concurrency_static_runtime})
target_include_directories(context_test_coedit_concurrency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME concurrency-coedit COMMAND context_test_coedit_concurrency)
set_tests_properties(concurrency-coedit PROPERTIES TIMEOUT 120)

# --- concurrency-worktree-merge: Scenario 2 (worktrees) -------------------------------------------
# Every documented R-FILE-012 conflict class exercised across the committed corpus, parallel-worktree
# divergence -> merge -> resolve-conflict, the post-merge `context validate` convergence gate (dup-id
# caught + re-key remedy), and the R-QA-005 sim-level pass. Links context_merge (the engine),
# context_cli (the real merge-file/validate/re-key verbs via cli::run), context_contract (the R-CLI-008
# envelope DOM), context_session (the deterministic headless sim).
add_executable(context_test_worktree_merge test_worktree_merge.cpp)
target_link_libraries(context_test_worktree_merge
    PRIVATE context_merge context_cli context_contract context_session context_serializer
            context_warnings)
target_link_options(context_test_worktree_merge PRIVATE ${_concurrency_static_runtime})
target_include_directories(context_test_worktree_merge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The committed R-FILE-012 conflict-class corpus (R-QA-011) lives with the merge module; CMAKE_SOURCE_DIR
# is the `-S src` root, so editor/merge/tests/corpus is the exact dir the merge module's test_corpus uses.
target_compile_definitions(context_test_worktree_merge
    PRIVATE CONTEXT_MERGE_CORPUS_DIR="${CMAKE_SOURCE_DIR}/editor/merge/tests/corpus")
add_test(NAME concurrency-worktree-merge COMMAND context_test_worktree_merge)
set_tests_properties(concurrency-worktree-merge PROPERTIES TIMEOUT 180)
# Under the sanitize leg (unfiltered `ctest --preset sanitize` runs this non-gate family too), this test
# drives context_cli in-process, which links the uninstrumented rusty_v8 prebuilt — suppress its
# documented LSan false-positive exactly as the sibling CLI-driving suites do (merge-test_git_driver in
# src/editor/merge/CMakeLists.txt; m1/m2/m7-exit + samples-corpus in src/tests/integration/CMakeLists.txt).
# The concurrency-coedit target links no CLI/V8, so it deliberately carries no such block.
if(CONTEXT_SANITIZE)
    set_tests_properties(concurrency-worktree-merge PROPERTIES ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ts/lsan-suppressions.txt")
endif()
