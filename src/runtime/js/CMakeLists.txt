# src/runtime/js/ — the in-process V8 JavaScript host (issue #76 / L-61 / R-LANG-002/008 /
# R-SEC-009). The foundation of the M3 TypeScript scripting tier: a minimal Isolate+Context
# that evaluates JS from C++, a host<->JS boundary each way, the shape-B VM-allocated
# (sandbox-interior) backing-store seam task 3's zero-copy views build on, and a stubbed CDP
# inspector seam (R-OBS-005). V8 comes from the SHA-pinned rusty_v8 prebuilt channel
# (tools/v8-prebuilt.json + tools/fetch_v8.py — TLS + SHA-pin + verify-before-use, fail-closed;
# the wgpu-native/CEF third-party-prebuilt precedent in docs/signing.md — NOT the first-party
# R-SEC-009 trust root, so NOT via tools/verify_artifact.py).
#
# HYBRID host (TD ruling, issue #76): every rusty_v8 prebuilt is use_custom_libcxx=true
# (Chromium libc++, __Cr ABI), so STL-typed v8:: signatures cannot link against a system-STL
# embedder. The STL-free majority uses the real v8:: API; the two STL-crossing ops (platform
# boot, shape-B backing store) go through rusty_v8's extern-"C" v8__* shims (src/v8_shims.h).
#
# The rusty_v8 prebuilt is an MSVC/Clang-ABI static lib, so it is CI-only for its dependency
# path: the local Strawberry-GCC Windows `dev` gate CANNOT link it (profile setup.md/test.md
# carve-out) and builds a STUB backend (src/js_host_stub.cpp), keeping the local non-dependency
# build + ctest green. The 3-OS CI build legs (Linux-x64 clang / macOS-ARM64 clang / Win-x64
# MSVC) are the AUTHORITATIVE gate. Wired into the DEFAULT build graph so those legs build it.

# --- resolve the host triple + whether this toolchain can link the rusty_v8 prebuilt --------
set(_ctx_js_v8 OFF)
set(_ctx_js_triple "")
if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_js_v8 ON)
    set(_ctx_js_triple "x86_64-pc-windows-msvc")
elseif(APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_ctx_js_v8 ON)
    set(_ctx_js_triple "aarch64-apple-darwin")
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_js_v8 ON)
    set(_ctx_js_triple "x86_64-unknown-linux-gnu")
endif()

# Escape hatch: force the stub even where V8 could link (e.g. to smoke the non-dependency build).
option(CONTEXT_JS_FORCE_STUB "Build the JS host stub backend even where V8 could link" OFF)
if(CONTEXT_JS_FORCE_STUB)
    set(_ctx_js_v8 OFF)
endif()

if(_ctx_js_v8)
    # --- acquire + verify the pinned V8 prebuilt (verify-before-use, fail-closed) ------------
    # FindPython3 resolves the real interpreter path (incl. the `py` launcher on Windows), so
    # this is robust to the python3-vs-py alias difference on the self-hosted Windows runner.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_ctx_v8_manifest "${CMAKE_SOURCE_DIR}/../tools/v8-prebuilt.json")
    set(_ctx_v8_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_v8.py")
    set(_ctx_v8_dest "${CMAKE_BINARY_DIR}/_v8/${_ctx_js_triple}")
    message(STATUS "context_js: fetching + verifying pinned V8 prebuilt for ${_ctx_js_triple}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_ctx_v8_fetch}"
                --manifest "${_ctx_v8_manifest}"
                --triple "${_ctx_js_triple}"
                --dest "${_ctx_v8_dest}"
        RESULT_VARIABLE _ctx_v8_rc
        OUTPUT_VARIABLE _ctx_v8_out
        ERROR_VARIABLE _ctx_v8_err)
    if(NOT _ctx_v8_rc EQUAL 0)
        message(FATAL_ERROR
            "context_js: V8 prebuilt fetch/verify FAILED (rc=${_ctx_v8_rc}) — refusing to build "
            "against an unverified V8 (R-SEC-009 fail-closed).\nstdout: ${_ctx_v8_out}\nstderr: "
            "${_ctx_v8_err}")
    endif()
    string(STRIP "${_ctx_v8_out}" _ctx_v8_out)
    message(STATUS "context_js: ${_ctx_v8_out}")

    # Extracted layout (tools/fetch_v8.py): <dest>/lib/<extracted> + <dest>/include/*
    if(WIN32)
        set(_ctx_v8_libfile "${_ctx_v8_dest}/lib/rusty_v8.lib")
    else()
        set(_ctx_v8_libfile "${_ctx_v8_dest}/lib/librusty_v8.a")
    endif()

    add_library(context_v8_prebuilt INTERFACE)
    target_include_directories(context_v8_prebuilt SYSTEM INTERFACE "${_ctx_v8_dest}/include")
    # Embedder defines MUST match how rusty_v8's default `release` prebuilt was built:
    # v8_enable_sandbox=true (=> pointer compression + external code space + 31-bit SMIs).
    # Mirrors the triple verified by the js-engine spike (spikes/js-engine).
    target_compile_definitions(context_v8_prebuilt INTERFACE
        V8_COMPRESS_POINTERS
        V8_ENABLE_SANDBOX
        V8_31BIT_SMIS_ON_64BIT_ARCH)
    # Platform system libs (from rusty_v8 build.rs `print_link_flags`, use_custom_libcxx=true:
    # libc++ is bundled STATICALLY inside the archive — do NOT link a system stdc++/c++).
    target_link_libraries(context_v8_prebuilt INTERFACE "${_ctx_v8_libfile}")
    if(WIN32)
        target_link_libraries(context_v8_prebuilt INTERFACE winmm dbghelp)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(context_v8_prebuilt INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
    endif()

    # v8_rust_stubs.cpp satisfies the archive's Rust-provided extern-"C" references (the
    # rusty_v8 prebuilt is built to be consumed from Rust). Bounded, deterministic, and
    # unreachable at runtime in this minimal host — see that file's header. V8-backend only.
    add_library(context_js STATIC src/v8_engine.cpp src/v8_rust_stubs.cpp)
    target_link_libraries(context_js PRIVATE context_v8_prebuilt)
    target_compile_definitions(context_js PRIVATE CONTEXT_JS_HAS_V8)
    if(MSVC)
        # /external:W0 keeps /W4 /WX for our code while the SYSTEM V8 headers stay quiet;
        # /Zc:__cplusplus makes v8config.h see the real __cplusplus value (MSVC lies by default).
        target_compile_options(context_js PRIVATE /external:W0 /Zc:__cplusplus)
    endif()
else()
    add_library(context_js STATIC src/js_host_stub.cpp)
endif()

target_include_directories(context_js PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_js PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) --------------------------
# Full battery when the V8 backend is built (CI legs); a stub-availability assertion otherwise
# (the local Strawberry-GCC Windows dev gate), so `ctest --preset dev` stays green either way.
add_executable(context_js_test_js_engine tests/test_js_engine.cpp)
target_link_libraries(context_js_test_js_engine PRIVATE context_js context_warnings)
if(_ctx_js_v8)
    target_compile_definitions(context_js_test_js_engine PRIVATE CONTEXT_JS_HAS_V8)
endif()
add_test(NAME js-test_js_engine COMMAND context_js_test_js_engine)
