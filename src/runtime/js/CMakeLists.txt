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
    # PINNED-PREBUILT ABI CONTRACT (embedder build config MUST match the prebuilt, or Isolate
    # creation aborts with "Embedder-vs-V8 build configuration mismatch"). The pinned channel is
    # rusty_v8's DEFAULT `release` prebuilt: its file is `librusty_v8_release_<triple>.a` /
    # `rusty_v8_release_<triple>.lib` with NO `_ptrcomp` / `_sandbox` feature suffix
    # (tools/v8-prebuilt.json). rusty_v8's build.rs appends those suffixes ONLY when the
    # corresponding Cargo feature is on, and its default feature set is `["use_custom_libcxx"]`
    # (neither v8_enable_pointer_compression nor v8_enable_sandbox). So the archive is built with
    # v8_enable_pointer_compression=false AND v8_enable_sandbox=false — verified against
    # denoland/rusty_v8@v149.4.0 build.rs (prebuilt_features_suffix + the gn-args branch) and by
    # the Windows CI Isolate-creation check. The embedder must therefore define NONE of
    # V8_COMPRESS_POINTERS / V8_ENABLE_SANDBOX / V8_31BIT_SMIS_ON_64BIT_ARCH — V8's headers treat
    # each as absent-=-off, matching the prebuilt. (This is DIFFERENT from the js-engine spike,
    # which links the MSVC pmed/v8-nuget monolith — a standard sandbox-ENABLED Chromium build —
    # so the spike's V8_* defines do NOT transfer to this rusty_v8 host.) The from-source
    # migration seam (README.md) may re-enable these once it owns the gn args.
    # Platform system libs (from rusty_v8 build.rs `print_link_flags`, use_custom_libcxx=true:
    # libc++ is bundled STATICALLY inside the archive — do NOT link a system stdc++/c++).
    target_link_libraries(context_v8_prebuilt INTERFACE "${_ctx_v8_libfile}")
    if(WIN32)
        target_link_libraries(context_v8_prebuilt INTERFACE winmm dbghelp)
    elseif(APPLE)
        # V8's timezone/ICU path on Darwin calls into CoreFoundation (CFTimeZoneCopyDefault,
        # CFStringGet*, CFRelease) and Foundation; without these frameworks the arm64 link fails
        # with "Undefined symbols: _CFRelease, _CFTimeZoneCopyDefault, …".
        target_link_libraries(context_v8_prebuilt INTERFACE
            "-framework CoreFoundation" "-framework Foundation")
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(context_v8_prebuilt INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
    endif()

    # v8_rust_stubs.cpp satisfies the archive's Rust-provided extern-"C" references (the
    # rusty_v8 prebuilt is built to be consumed from Rust). Bounded, deterministic, and
    # unreachable at runtime in this minimal host — see that file's header. V8-backend only.
    #
    # LINK-ORDER SAFETY (why its own target + WHOLE_ARCHIVE): the stub TU only DEFINES symbols
    # that librusty_v8.a REFERENCES — it references nothing back. If the stub object sits in the
    # same archive as v8_engine.cpp (or in any archive that precedes librusty_v8.a on the link
    # line), a single-pass archive linker (GNU ld / lld on the Linux + sanitize legs) discards it
    # BEFORE the prebuilt introduces the temporal_rs_*/v8__*/… undefined refs, yielding the exact
    # "258 undefined reference to temporal_rs_*" failure. Isolating the stubs in their OWN target
    # and force-loading it (`$<LINK_LIBRARY:WHOLE_ARCHIVE,…>` → --whole-archive / -force_load /
    # /WHOLEARCHIVE) makes all 288 definitions unconditionally present regardless of link order,
    # on all 3 linkers. (MSVC linked without this; the fix is order-independent and harmless
    # there.) Reproduced + verified with a GNU-ld scratch link + the CMake genexp locally.
    add_library(context_v8_rust_stubs STATIC src/v8_rust_stubs.cpp)
    target_link_libraries(context_v8_rust_stubs PRIVATE context_warnings)

    # inspector.cpp = the R-OBS-005 interactive CDP inspector session (issue #94), built out from the
    # task-2a seam. V8-backend ONLY (links v8-inspector.h out of the rusty_v8 archive). It ALSO defines
    # the ten extern-"C" v8_inspector__{V8InspectorClient,V8Inspector__Channel}__BASE__* thunks that
    # librusty_v8.a(binding.o) references. Those thunks are pulled from an ORDINARY archive member only
    # when something references an inspector symbol — but the editor/system test executables link
    # context_js WITHOUT touching the inspector, so a single-pass linker (GNU ld / lld on the Linux +
    # sanitize legs) discards inspector.o and leaves binding.o's __BASE__* references undefined (the
    # issue-#104 link failure). Give inspector.cpp its OWN target and force-load it (WHOLE_ARCHIVE),
    # exactly like context_v8_rust_stubs above, so EVERY consumer of context_js retains the thunks
    # regardless of link order, on all 3 linkers. v8_engine.cpp's backward references to
    # describeException / compileAndRun / createInspectorSession (now defined in this lib, declared in
    # the shared inspector_session.h) resolve for the same force-load reason.
    add_library(context_js_inspector STATIC src/inspector.cpp)
    target_link_libraries(context_js_inspector PRIVATE context_v8_prebuilt context_warnings)
    target_compile_definitions(context_js_inspector PRIVATE CONTEXT_JS_HAS_V8)
    target_include_directories(context_js_inspector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(context_js_inspector PRIVATE /external:W0 /Zc:__cplusplus)
    endif()
    # UBSan `vptr` false-positive carve-out (issue #104), identical rationale to runtime/ts/CMakeLists.txt:
    # inspector.cpp's two ~SeamClient/~SeamChannel destructor calls dispatch virtually into archive-
    # allocated objects; the UNINSTRUMENTED rusty_v8 prebuilt (no-rtti/bundled libc++) carries no
    # matching RTTI, so UBSan's `vptr` sub-check reports "member call ... not of type ..." even though
    # the object IS valid — a FALSE POSITIVE of an instrumented embedder + uninstrumented prebuilt (it
    # also spawns demangler-driven LSan symbolizer allocations that read as leaks). Drop ONLY the vptr
    # sub-check for this TU under the sanitize preset (ASan + every other UBSan check stay ON); scoped
    # to CONTEXT_SANITIZE so dev/CI release builds are byte-identical, gated non-MSVC (a Clang/GCC flag).
    if(CONTEXT_SANITIZE AND NOT MSVC)
        target_compile_options(context_js_inspector PRIVATE -fno-sanitize=vptr)
    endif()

    add_library(context_js STATIC src/v8_engine.cpp src/view_element.cpp)
    target_link_libraries(context_js
        PRIVATE context_v8_prebuilt
        PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,context_v8_rust_stubs>"
               "$<LINK_LIBRARY:WHOLE_ARCHIVE,context_js_inspector>")
    target_compile_definitions(context_js PRIVATE CONTEXT_JS_HAS_V8)
    if(MSVC)
        # /external:W0 keeps /W4 /WX for our code while the SYSTEM V8 headers stay quiet;
        # /Zc:__cplusplus makes v8config.h see the real __cplusplus value (MSVC lies by default).
        target_compile_options(context_js PRIVATE /external:W0 /Zc:__cplusplus)
    endif()
    # Same UBSan `vptr` false-positive carve-out (issue #104): v8_engine.cpp's `delete
    # createParams_.array_buffer_allocator` is a virtual-destructor call on an archive-allocated
    # ArrayBuffer::Allocator, which trips the vptr sub-check against the uninstrumented prebuilt's
    # RTTI — a FALSE POSITIVE. Scoped/gated exactly as context_js_inspector above.
    if(CONTEXT_SANITIZE AND NOT MSVC)
        target_compile_options(context_js PRIVATE -fno-sanitize=vptr)
    endif()
else()
    add_library(context_js STATIC src/js_host_stub.cpp src/view_element.cpp)
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
# Same UBSan `vptr` false-positive carve-out as context_js_inspector/context_js (issue #104), mirroring
# ts-test_ts_debug: this test links the uninstrumented rusty_v8 prebuilt (via context_js), so any
# polymorphic-object member call the UBSan `vptr` sub-check inspects can false-positive against the
# prebuilt's absent RTTI. Drop ONLY the vptr sub-check for this TU under the sanitize preset.
if(_ctx_js_v8 AND CONTEXT_SANITIZE AND NOT MSVC)
    target_compile_options(context_js_test_js_engine PRIVATE -fno-sanitize=vptr)
endif()
add_test(NAME js-test_js_engine COMMAND context_js_test_js_engine)

# TSan preset only: the V8 engine links the rusty_v8 prebuilt, which is NOT TSan-instrumented
# (denoland/rusty_v8's default `release` static archive). Its multi-threaded worker pool
# (DefaultWorkerThreadsTaskRunner) therefore surfaces benign, un-actionable data races inside
# V8's own uninstrumented threading internals — never in Context-Engine code. We keep the
# multi-threaded platform on purpose (the single-threaded one null-deref-SEGVs during Isolate
# teardown), and silence ONLY these V8-internal races via a narrow suppressions file scoped to
# this one test. Guarded by CONTEXT_TSAN so ASan/UBSan + the normal dev build are untouched, and
# the path is absolute so it resolves regardless of the ctest working directory. See
# tsan-suppressions.txt for the full rationale.
if(_ctx_js_v8 AND CONTEXT_TSAN)
    set_tests_properties(js-test_js_engine PROPERTIES
        ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan-suppressions.txt")
endif()

# --- M6 X1 (R-SIM-008): the GC-discipline seam ------------------------------------------------
# Same split as js-test_js_engine: the full battery (inter-tick window force/trigger policies,
# pause-ring attribution, heap stats, invalid-options failures) on the V8 CI legs; a stub
# assertion locally. Same TSan suppression scoping (it links the same uninstrumented prebuilt).
add_executable(context_js_test_gc_discipline tests/test_gc_discipline.cpp)
target_link_libraries(context_js_test_gc_discipline PRIVATE context_js context_warnings)
if(_ctx_js_v8)
    target_compile_definitions(context_js_test_gc_discipline PRIVATE CONTEXT_JS_HAS_V8)
endif()
if(_ctx_js_v8 AND CONTEXT_SANITIZE AND NOT MSVC)
    target_compile_options(context_js_test_gc_discipline PRIVATE -fno-sanitize=vptr)
endif()
add_test(NAME js-test_gc_discipline COMMAND context_js_test_gc_discipline)
if(_ctx_js_v8 AND CONTEXT_TSAN)
    set_tests_properties(js-test_gc_discipline PROPERTIES
        ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan-suppressions.txt")
endif()

# --- M6 X1 determinism proof: GC in the inter-tick window leaves the state hash UNCHANGED ------
# Two identical seeded sessions (trace mode), one with mid-tick JS churn + a forced GC window at
# every tick boundary (session's inter-tick hook -> JsEngine::gcWindow, drained through the L-47
# channel): every per-tick/per-system/per-archetype hash must match byte-for-byte. Links
# context_session (the fixed-tick loop + hash trace) and context_profile (the channel). The local
# stub toolchain proves the hook mechanics + invariance with pure-C++ churn instead. Static-links
# the GCC runtime (context_session pulls std::filesystem symbols a stale PATH libstdc++-6.dll can
# shadow at ctest launch — the setup.md § Preconditions rule; gated to GNU).
add_executable(context_js_test_gc_state_hash tests/test_gc_state_hash.cpp)
target_link_libraries(context_js_test_gc_state_hash
    PRIVATE context_js context_profile context_session context_warnings)
target_link_options(context_js_test_gc_state_hash
    PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
if(_ctx_js_v8)
    target_compile_definitions(context_js_test_gc_state_hash PRIVATE CONTEXT_JS_HAS_V8)
endif()
if(_ctx_js_v8 AND CONTEXT_SANITIZE AND NOT MSVC)
    target_compile_options(context_js_test_gc_state_hash PRIVATE -fno-sanitize=vptr)
endif()
add_test(NAME js-test_gc_state_hash COMMAND context_js_test_gc_state_hash)
if(_ctx_js_v8 AND CONTEXT_TSAN)
    set_tests_properties(js-test_gc_state_hash PROPERTIES
        ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan-suppressions.txt")
endif()

# --- a17 TRUST-TIER RED-TEAM (M8.5; issue #283): the JS host exposes only INJECTED bindings --------
# R-SEC-001/002 (L-49): gameplay TS reaches only what the host injects — no ambient fs/net/process.
# Same V8-gated split as js-test_js_engine: on the V8 CI legs the probe evals from inside a guest that
# require/process/fetch/XHR/WebSocket/Deno/globalThis.fs are all undefined AND that an injected host
# binding IS reachable (the sole door in); on the stub toolchain it asserts the honest no-JS state.
# Named `redteam-js-ambient` — NOT a milestone-gate prefix, so the build job's general ctest step
# auto-picks it. Same rusty_v8 UBSan-vptr / TSan carve-outs as the sibling V8-linking test TUs.
add_executable(context_js_test_ambient_redteam tests/test_ambient_redteam.cpp)
target_link_libraries(context_js_test_ambient_redteam PRIVATE context_js context_warnings)
if(_ctx_js_v8)
    target_compile_definitions(context_js_test_ambient_redteam PRIVATE CONTEXT_JS_HAS_V8)
endif()
if(_ctx_js_v8 AND CONTEXT_SANITIZE AND NOT MSVC)
    target_compile_options(context_js_test_ambient_redteam PRIVATE -fno-sanitize=vptr)
endif()
add_test(NAME redteam-js-ambient COMMAND context_js_test_ambient_redteam)
if(_ctx_js_v8 AND CONTEXT_TSAN)
    set_tests_properties(redteam-js-ambient PROPERTIES
        ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan-suppressions.txt")
endif()
