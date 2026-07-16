# src/runtime/host/ — the shipped RuntimeKernel host binary (M8 task a06, R-BUILD-009 / R-HEAD-001/002 /
# L-5). The FIRST standalone runtime executable in the engine: it boots a packed v1 artifact headlessly
# (PackContentSource → materialize units → boot a Session → step N fixed ticks) and prints the
# R-BUILD-009 boot/state signal. Two FLAVORS build from ONE source, differing only by whether the render
# subsystem is linked — the L-5 DCE'd minimal-build axis:
#   * context_runtime_desktop (OUTPUT_NAME context-runtime)        — render subsystem PRESENT (links
#       context_render, CONTEXT_HOST_RENDER on; the GPU-free extract runs headlessly, no device created);
#   * context_runtime_server  (OUTPUT_NAME context-runtime-server) — render subsystem ABSENT (never links
#       context_render, so the binary carries zero render/GUI payload — the footprint audit proves it).
# Both link only the minimal runtime set (session + content loader + kernel) — the ultra-light build L-5
# intends. The a06 export adapter (src/editor/build/adapter.*) plans these binaries into a tarball.

# The self-contained GCC runtime idiom (targets/context-engine/setup.md): the host uses std::ifstream /
# std::filesystem, whose libstdc++ symbols live in a newer libstdc++-6.dll a stale PATH copy can shadow
# on this MinGW host → a launch-time abort. Static-linking libgcc+libstdc++ (gated to GNU; MSVC/Clang CI
# legs untouched) makes the shipped binaries self-contained — which ALSO makes them true "no dev tree"
# artifacts (R-BUILD-009 clean-host smoke), carrying no external C++-runtime DLL dependency.
set(_host_static_runtime $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_host_core_libs context_session context_runtime_content context_kernel)

# --- server / headless flavor: render subsystem ABSENT (nothing links context_render) --------------
add_executable(context_runtime_server src/host.cpp src/main.cpp)
target_include_directories(context_runtime_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(context_runtime_server PRIVATE ${_host_core_libs} context_warnings)
target_link_options(context_runtime_server PRIVATE ${_host_static_runtime})
set_target_properties(context_runtime_server PROPERTIES OUTPUT_NAME context-runtime-server)

# --- desktop flavor: render subsystem PRESENT (links context_render; CONTEXT_HOST_RENDER on) --------
add_executable(context_runtime_desktop src/host.cpp src/main.cpp)
target_include_directories(context_runtime_desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(context_runtime_desktop PRIVATE CONTEXT_HOST_RENDER=1)
target_link_libraries(context_runtime_desktop PRIVATE ${_host_core_libs} context_render context_warnings)
target_link_options(context_runtime_desktop PRIVATE ${_host_static_runtime})
set_target_properties(context_runtime_desktop PROPERTIES OUTPUT_NAME context-runtime)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The `runtime-host-*` family is NOT a milestone gate prefix, so it auto-runs in the CI `build` job's
# general ctest step and is auto-built by `--preset dev` on all three OS legs — no ci.yml / --target
# edit needed. (The dedicated linux-export CI job additionally proves the ASSEMBLED tarball boots on a
# clean host with no dev tree — DoD 1/4.) The tests build a real v1 pack in-process via the a01 writer
# (context_pack) + the flatten/partition path (context_compose) — a TEST-ONLY link of the editor build
# side, so the host binaries themselves never depend on it.

# host.cpp is compiled per-flavor into the test exes too (the render half is a compile+link decision).
# Core (server-shaped): run_host over an in-memory pack, the failure path, determinism, the signal JSON.
add_executable(context_runtime_host_test_core tests/test_host.cpp src/host.cpp)
target_include_directories(context_runtime_host_test_core
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(context_runtime_host_test_core
    PRIVATE ${_host_core_libs} context_pack context_compose context_warnings)
target_link_options(context_runtime_host_test_core PRIVATE ${_host_static_runtime})
add_test(NAME runtime-host-core COMMAND context_runtime_host_test_core)

# Render (desktop-shaped): the SAME test source with CONTEXT_HOST_RENDER on + context_render linked,
# so it additionally asserts render_present + the headless GPU-free extract ran (no device created).
add_executable(context_runtime_host_test_render tests/test_host.cpp src/host.cpp)
target_compile_definitions(context_runtime_host_test_render PRIVATE CONTEXT_HOST_RENDER=1)
target_include_directories(context_runtime_host_test_render
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(context_runtime_host_test_render
    PRIVATE ${_host_core_libs} context_render context_pack context_compose context_warnings)
target_link_options(context_runtime_host_test_render PRIVATE ${_host_static_runtime})
add_test(NAME runtime-host-render COMMAND context_runtime_host_test_render)

# Cross-process smoke (R-BUILD-009): build a real pack to a scratch file, then LAUNCH each SHIPPED
# binary (server + desktop) with `--pack <file> --ticks N`, capture stdout, and assert the boot/state
# signal (ok + simTick==N + a named scene). Uses the hardened context_common subprocess runner (the
# cmd.exe outer-quote workaround) so it is robust on the Windows executor too. Built AFTER both host
# exes; their per-config paths are threaded in as compile-time defines.
add_executable(context_runtime_host_test_smoke tests/test_host_smoke.cpp)
target_include_directories(context_runtime_host_test_smoke
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(context_runtime_host_test_smoke
    PRIVATE context_common context_pack context_compose context_serializer context_warnings)
target_link_options(context_runtime_host_test_smoke PRIVATE ${_host_static_runtime})
target_compile_definitions(context_runtime_host_test_smoke PRIVATE
    CONTEXT_RUNTIME_SERVER_BIN="$<TARGET_FILE:context_runtime_server>"
    CONTEXT_RUNTIME_DESKTOP_BIN="$<TARGET_FILE:context_runtime_desktop>")
add_dependencies(context_runtime_host_test_smoke context_runtime_server context_runtime_desktop)
add_test(NAME runtime-host-smoke COMMAND context_runtime_host_test_smoke)
set_tests_properties(runtime-host-smoke PROPERTIES TIMEOUT 60)

# The render footprint / DCE audit (DoD 2): a toolchain-agnostic file(STRINGS)+file(SIZE) check that the
# server/headless binary carries NO render payload (marker absent + smaller) while the desktop binary
# does (marker present). Runs on every build leg (needs both exes built — `--preset dev` builds the tree
# before ctest). The markers are the sentinels host.cpp links per flavor (CONTEXT_HOST_RENDER gated).
add_test(NAME runtime-host-render-footprint
    COMMAND ${CMAKE_COMMAND}
        -DDESKTOP_BIN=$<TARGET_FILE:context_runtime_desktop>
        -DSERVER_BIN=$<TARGET_FILE:context_runtime_server>
        -DRENDER_MARKER=CTXHOST-FOOTPRINT:render-present
        -DCONTROL_MARKER=CTXHOST-FOOTPRINT:runtime-host
        -P ${CMAKE_CURRENT_SOURCE_DIR}/render_footprint_check.cmake)
