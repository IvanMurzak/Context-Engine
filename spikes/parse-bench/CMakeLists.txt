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
