# The D10 out-of-tree consumer smoke.
#
# This is NOT part of the engine build — it is configured STANDALONE by the `editor-boundary` CI job
# against a real `cmake --install` prefix, exactly as a third-party consumer would:
#
#   cmake -S src/editor/client/consumer -B <build> -DCMAKE_PREFIX_PATH=<install>/versions/<semver>
#   cmake --build <build>
#   <build>/context_client_consumer_smoke
#
# It exists to make the boundary claim FALSIFIABLE. If the SDK's public headers ever need something
# we do not install, or the package's link closure is incomplete, this fails to configure, compile,
# or link — in CI, on the PR that broke it, rather than months later inside e04/e05.
#
# The job's consumer target grows as e04/e05 land (e17 asserts the final shape); today it is the
# minimal thing that proves the seam is real.

cmake_minimum_required(VERSION 3.25)
project(context_client_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Deliberately NO reference to the engine source tree: find_package must supply everything.
find_package(ContextClient REQUIRED)

add_executable(context_client_consumer_smoke main.cpp)
target_link_libraries(context_client_consumer_smoke PRIVATE Context::context_client)

# A consumer compiles under its OWN warning policy, not the engine's. Keeping this strict is a real
# assertion: the installed public headers must be warning-clean for someone else's -Wall -Wextra
# build, since they are not ours to fix once shipped.
if(MSVC)
    target_compile_options(context_client_consumer_smoke PRIVATE /W4 /WX)
else()
    target_compile_options(context_client_consumer_smoke PRIVATE -Wall -Wextra -Werror)
endif()

# Self-contained GCC runtime, for the same reason every engine executable does it (see
# src/cli/CMakeLists.txt): the SDK pulls std::filesystem + stream IO, whose symbols live in a NEWER
# libstdc++-6.dll, and on a MinGW host a stale one earlier on PATH is loaded first — the binary then
# dies at launch (STATUS_ENTRYPOINT_NOT_FOUND, or a bare exit 127 under a shell) with a clean build
# and link. Gated to GNU: Apple Clang rejects -static-libstdc++ and MSVC has its own runtime model,
# so the macOS and Windows/MSVC CI legs are untouched.
target_link_options(context_client_consumer_smoke PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

enable_testing()
add_test(NAME consumer-smoke COMMAND context_client_consumer_smoke)
