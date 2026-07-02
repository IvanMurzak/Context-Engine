# parse-bench — M0 moat-perf spike (parse -> canonicalize -> hash + 3-way merge).
#
# OPT-IN: built only when CONTEXT_BUILD_SPIKES=ON (see the root CMakeLists). Requires
# the vcpkg toolchain for simdjson + blake3 (deps declared in vcpkg.json; licenses
# recorded in tools/license-allowlist.json).

find_package(simdjson CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(context-parse-bench
    src/main.cpp
    src/canonical.cpp
    src/merge.cpp)

target_link_libraries(context-parse-bench PRIVATE
    context_warnings
    simdjson::simdjson
    BLAKE3::blake3
    Threads::Threads)

# Self-check (R-QA-013): the binary's own canon-check mode on a tiny generated corpus —
# serialize∘parse must be the identity on canonical input. Registered under the spikes
# preset only (this directory is opt-in); needs a Python interpreter for gen_corpus.py.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(_pb_selfcheck_corpus ${CMAKE_CURRENT_BINARY_DIR}/selfcheck-corpus)
    add_test(NAME parse-bench-selfcheck-clean
             COMMAND ${CMAKE_COMMAND} -E rm -rf ${_pb_selfcheck_corpus})
    add_test(NAME parse-bench-selfcheck-gen-corpus
             COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../../bench/gen_corpus.py
                     --size 128 --out ${_pb_selfcheck_corpus} --jobs 1)
    add_test(NAME context-parse-bench-canon-check
             COMMAND context-parse-bench canon-check --corpus ${_pb_selfcheck_corpus} --sample 64)
    set_tests_properties(parse-bench-selfcheck-clean PROPERTIES FIXTURES_SETUP pb_corpus)
    set_tests_properties(parse-bench-selfcheck-gen-corpus PROPERTIES
        FIXTURES_SETUP pb_corpus
        DEPENDS parse-bench-selfcheck-clean)
    set_tests_properties(context-parse-bench-canon-check PROPERTIES FIXTURES_REQUIRED pb_corpus)
else()
    message(STATUS "parse-bench: no Python3 interpreter found — canon-check self-check not registered")
endif()
