# src/editor/merge/ — schema-aware structural three-way merge (M2 wave 4: R-FILE-012 / L-26/L-50/L-33,
# issue #59). The convergence primitive for worktree-per-agent parallelism: a field-path-granular,
# id-identity three-way merge over authored JSON, installable as a git merge driver (`context new`
# writes the .gitattributes mapping; L-27 keeps git OUT of the engine — the driver is invoked BY
# git, never the reverse). Depends on the canonical serializer (JsonValue + canonical bytes) and on
# compose for the L-35 pointer + L-33 stable-id primitives (never the reverse: compose does not
# consume merge). The CLI verbs (merge-file / resolve-conflict / re-key / validate) live in src/cli/.

add_library(context_merge STATIC
    src/three_way_merge.cpp
    src/rekey.cpp
    src/resolve.cpp)

target_include_directories(context_merge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: the public headers expose the serializer's JsonValue/Diagnostic types. compose is an
# implementation detail (stable-id minting + RFC 6901 resolution in rekey.cpp) — PRIVATE.
target_link_libraries(context_merge PUBLIC context_serializer)
target_link_libraries(context_merge PRIVATE context_compose context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern;
# tests/merge_test.h is the tiny CHECK() harness + JSON build/parse helpers). Coverage spans the
# happy path (disjoint-field auto-merge, id-keyed element field-merge, array reorders, one-sided
# adds/removes), edge cases (id add/add, delete/modify, whole-file meta-guid/newer-stamped classes,
# marker-free invariant), and the re-key / duplicate-id convergence gate.
set(_context_merge_tests
    test_three_way_merge
    test_rekey)

foreach(_t IN LISTS _context_merge_tests)
    add_executable(context_merge_${_t} tests/${_t}.cpp)
    target_link_libraries(context_merge_${_t} PRIVATE context_merge context_warnings)
    target_include_directories(context_merge_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME merge-${_t} COMMAND context_merge_${_t})
endforeach()

# The R-QA-011 three-way conflict-class corpus: a committed case per documented conflict class
# (tests/corpus/<case>/{base,ours,theirs}.json + expected.json). The test enumerates every case on
# disk, runs merge_documents, and asserts the merged tree + conflict classes/paths match the golden
# — so a new conflict class that forgets its fixture still fails. Uses std::filesystem, so it needs
# the self-contained GCC runtime (the setup.md libstdc++ DLL-shadowing quirk on MinGW hosts).
add_executable(context_merge_test_corpus tests/test_corpus.cpp)
target_link_libraries(context_merge_test_corpus PRIVATE context_merge context_warnings)
target_link_options(context_merge_test_corpus
    PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
target_include_directories(context_merge_test_corpus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_merge_test_corpus
    PRIVATE CONTEXT_MERGE_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
add_test(NAME merge-test_corpus COMMAND context_merge_test_corpus)

# --- real-git-merge integration (R-QA-013 / R-FILE-012): drive a REAL `git merge` through the
# auto-installed `context merge-file` driver end to end. The test `git init`s a temp repo, runs the
# built `context new` to scaffold + auto-install the driver, commits a base scene, makes divergent
# edits on two branches, `git merge`s them, and asserts the merged scene is valid JSON, carries BOTH
# disjoint edits, and contains NO text conflict markers. Needs the built `context` binary path
# (threaded in as a compile-time define, resolved per-config) and `git` on PATH (it SKIPs cleanly
# when git is absent). Module-local by design (the brief keeps it out of tests/integration/, whose
# index-zipped exit-test lists are reserved for the exit-gate task).
add_executable(context_merge_test_git_driver tests/test_git_driver.cpp)
# context_common: the shared std::system subprocess runner (issue #146) the driver test now uses.
target_link_libraries(context_merge_test_git_driver PRIVATE context_merge context_common context_warnings)
target_link_options(context_merge_test_git_driver
    PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
target_include_directories(context_merge_test_git_driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_merge_test_git_driver
    PRIVATE CONTEXT_BINARY="$<TARGET_FILE:context>")
add_dependencies(context_merge_test_git_driver context)
add_test(NAME merge-test_git_driver COMMAND context_merge_test_git_driver)
set_tests_properties(merge-test_git_driver PROPERTIES TIMEOUT 120)

# Sanitize preset only: the spawned `context` binary links context_js (M6 X1 `context profile gc`),
# so on the V8-capable legs the child carries the uninstrumented rusty_v8 archive with its duplicate
# C++-runtime typeinfo — UBSan vptr false-positives on valid iostream code, whose printed
# diagnostics drive demangler scratch allocations LSan reports as leaks, failing the child (and thus
# this test) with no Context defect (sanitize (ASan+UBSan, ubuntu) leg, CI run 29197875018). Same
# fix as src/cli / src/tests/integration: the narrow demangler/symbolizer LSan suppressions
# (runtime/ts/lsan-suppressions.txt), inherited by the spawned child; inert on the stub build.
if(CONTEXT_SANITIZE)
    set_tests_properties(merge-test_git_driver PROPERTIES
        ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ts/lsan-suppressions.txt")
endif()
