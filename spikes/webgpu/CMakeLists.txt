# spikes/webgpu — WebGPU-baseline rendering spike (L-11/L-56; R-REND-001/002/005, R-HEAD-002).
# THROWAWAY proof code. Built ONLY when CONTEXT_BUILD_SPIKES=ON via src/CMakeLists.txt — the
# default CI matrix never configures or builds this directory.
#
# Also configurable STANDALONE (cmake -S spikes/webgpu -B <dir>), which is how the two extra
# legs run:
#   * quick native iteration without the other spikes' vcpkg deps:
#       cmake -S spikes/webgpu -B build-native -DCMAKE_BUILD_TYPE=Release
#       cmake --build build-native && ctest --test-dir build-native
#   * the WEB leg — Emscripten binding the BROWSER's WebGPU (the locked web path,
#     ARCHITECTURE.md §4: `webgpu.h`→JS shim, NOT a Dawn cross-compile) via the emdawnwebgpu
#     port (bundled with Emscripten ≥ 4.0.10):
#       emcmake cmake -S spikes/webgpu -B build-web -DCMAKE_BUILD_TYPE=Release
#       cmake --build build-web        # -> context-spike-webgpu.{html,js,wasm}
#
# Native backend: wgpu-native PREBUILT release (pinned version + SHA256). Deliberate, recorded
# deviation from L-42 from-source, sanctioned for throwaway spikes (precedent: the js-engine
# spike's V8 NuGet monolith). Neither Dawn nor wgpu-native has a vcpkg port (verified
# 2026-07-02; microsoft/vcpkg#41847 still open) and Dawn's GN/depot_tools build is hours-class
# — the acquisition-cost comparison IS one of this spike's deliverables (FINDINGS.md).

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # ---- standalone configure (web leg / quick native iteration) ----
    cmake_minimum_required(VERSION 3.25)
    project(context-spike-webgpu LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    option(CONTEXT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (baseline: ON)" ON)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ContextWarnings.cmake)
    enable_testing()
endif()

add_executable(context-spike-webgpu src/main.cpp)
target_link_libraries(context-spike-webgpu PRIVATE context_warnings)

if(EMSCRIPTEN)
    # ---- web leg: bind the BROWSER's WebGPU through the emdawnwebgpu port ----
    # emdawnwebgpu is Dawn's maintained fork of Emscripten's retired -sUSE_WEBGPU bindings; it
    # maps webgpu.h onto the page's `navigator.gpu` at runtime — no native GPU code in the wasm.
    # -sASYNCIFY lets the spike's synchronous poll loop (emscripten_sleep) yield to the browser
    # event loop so WebGPU promises can resolve (emdawnwebgpu needs Asyncify or JSPI for waits).
    # NO -sALLOW_MEMORY_GROWTH: growth makes the wasm heap a *resizable* ArrayBuffer, and
    # emdawnwebgpu's string glue (TextDecoder.decode on a heap view) throws on resizable
    # buffers (observed: emcc 6.0.2 + emdawnwebgpu v20260423, Chrome). Fixed-size default
    # memory is plenty for the spike; FINDINGS.md records the gotcha.
    target_compile_options(context-spike-webgpu PRIVATE "--use-port=emdawnwebgpu")
    target_link_options(context-spike-webgpu PRIVATE
        "--use-port=emdawnwebgpu"
        "-sASYNCIFY"
        "-sEXIT_RUNTIME=1")
    # Emit an .html harness so the artifact is directly servable for the browser run.
    set_target_properties(context-spike-webgpu PROPERTIES SUFFIX ".html")
else()
    # ---- native leg: wgpu-native prebuilt (pinned + hash-verified) ----
    set(CTX_WGPU_NATIVE_VERSION "v29.0.1.1")
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        if(NOT MSVC)
            message(FATAL_ERROR "webgpu spike on Windows uses the -msvc prebuilt (MSVC import lib); "
                                "build with MSVC or add the -gnu asset pin.")
        endif()
        set(_wgpu_asset "wgpu-windows-x86_64-msvc-release.zip")
        set(_wgpu_sha256 "7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132")
    elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_wgpu_asset "wgpu-macos-aarch64-release.zip")
        set(_wgpu_sha256 "a5797a37b1adf720bcd5dcffb291edbbd5b7b14be0a3874c28e6393a655a7a3e")
    elseif(APPLE)
        set(_wgpu_asset "wgpu-macos-x86_64-release.zip")
        set(_wgpu_sha256 "8e2f7378548ddd0e2cf21e7d864dda46e953f0af724855a33778b85ead206d41")
    elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(_wgpu_asset "wgpu-linux-x86_64-release.zip")
        set(_wgpu_sha256 "95a4d90c071005a98d03eab348beaa6b07e16eb00d1dcdb9f8348f75eb97ec5a")
    else()
        message(FATAL_ERROR "webgpu spike: no wgpu-native prebuilt pinned for this platform "
                            "(${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}); add the asset + "
                            "SHA256 from the ${CTX_WGPU_NATIVE_VERSION} release.")
    endif()

    # Resilient downloader (retry/backoff + fail-closed SHA verify, Context-Engine#129). Guarded
    # include so it works both standalone (`cmake -S spikes/webgpu`) and via the src/ spikes preset.
    if(NOT COMMAND context_download)
        include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ContextDownload.cmake")
    endif()
    set(_wgpu_root "${CMAKE_BINARY_DIR}/_deps/wgpu-native")
    set(_wgpu_pkg "${_wgpu_root}/pkg")
    if(NOT EXISTS "${_wgpu_pkg}/include/webgpu/webgpu.h")
        message(STATUS "webgpu spike: downloading wgpu-native ${CTX_WGPU_NATIVE_VERSION} "
                       "prebuilt (${_wgpu_asset}, ~14-17 MB)")
        context_download(
            URL "https://github.com/gfx-rs/wgpu-native/releases/download/${CTX_WGPU_NATIVE_VERSION}/${_wgpu_asset}"
            PATH "${_wgpu_root}/${_wgpu_asset}"
            EXPECTED_SHA256 "${_wgpu_sha256}"
            DESCRIPTION "wgpu-native ${CTX_WGPU_NATIVE_VERSION} (${_wgpu_asset})")
        file(ARCHIVE_EXTRACT INPUT "${_wgpu_root}/${_wgpu_asset}" DESTINATION "${_wgpu_pkg}")
    endif()

    target_include_directories(context-spike-webgpu SYSTEM PRIVATE "${_wgpu_pkg}/include")
    # wgpu-native extras (webgpu/wgpu.h): instance-level adapter ENUMERATION for the probe mode
    # (emdawnwebgpu has no equivalent — the web probe falls back to requestAdapter).
    target_compile_definitions(context-spike-webgpu PRIVATE CTX_SPIKE_HAS_WGPU_EXTRAS)

    if(WIN32)
        # Dynamic linking: import lib + stage the DLL next to the executable.
        target_link_libraries(context-spike-webgpu PRIVATE "${_wgpu_pkg}/lib/wgpu_native.dll.lib")
        add_custom_command(TARGET context-spike-webgpu POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_wgpu_pkg}/lib/wgpu_native.dll"
                "$<TARGET_FILE_DIR:context-spike-webgpu>"
            COMMENT "webgpu spike: staging wgpu_native.dll next to the executable")
    elseif(APPLE)
        # Prebuilt dylib's install name is @rpath/libwgpu_native.dylib -> point rpath at the pkg.
        target_link_libraries(context-spike-webgpu PRIVATE "${_wgpu_pkg}/lib/libwgpu_native.dylib")
        set_target_properties(context-spike-webgpu PROPERTIES BUILD_RPATH "${_wgpu_pkg}/lib")
    else()
        # Prebuilt .so has no SONAME, so the link path is recorded verbatim; $ORIGIN is belt-and-
        # braces if the binary is moved alongside a copied .so.
        target_link_libraries(context-spike-webgpu PRIVATE "${_wgpu_pkg}/lib/libwgpu_native.so")
        set_target_properties(context-spike-webgpu PROPERTIES BUILD_RPATH "\$ORIGIN;${_wgpu_pkg}/lib")
    endif()

    if(MSVC)
        # /external:W0 — keep /W4 /WX for spike code while the prebuilt's headers stay quiet.
        target_compile_options(context-spike-webgpu PRIVATE /external:W0)
    endif()

    # R-HEAD-002 seam demo: enumerate adapters / report absence WITHOUT creating a device.
    # Must pass everywhere, including adapter-less headless boxes.
    add_test(NAME context-spike-webgpu-probe COMMAND context-spike-webgpu probe)

    # Offscreen render + pixel readback self-check: needs SOME adapter (a software one is fine —
    # lavapipe/llvmpipe on Linux, Metal on macOS). Exit 77 = clean SKIP when none exists.
    #
    # DELIBERATELY NOT REGISTERED ON WINDOWS. The self-hosted Windows runners are LocalSystem
    # services in Session 0. There, the native wgpu-native render path intermittently crashes with
    # 0xc0000409 (STATUS_STACK_BUFFER_OVERRUN) in device/instance teardown — NON-deterministically,
    # and REGARDLESS of adapter (it reproduced even forcing the WARP software adapter, on a single
    # non-concurrent runner). A flaky native crash is unacceptable for CI (tests must be
    # deterministic). Render+readback CORRECTNESS is platform-agnostic wgpu-native + WGSL logic and
    # is fully validated DETERMINISTICALLY on Linux (lavapipe) + the web (browser) legs; Windows
    # coverage comes from the `build` job (wgpu-native compiles + links under MSVC) and the `probe`
    # test (adapter enumeration). NOTE: the `probe` test itself hit this SAME 0xc0000409 teardown
    # race (issue #210, CI run 29233449101 — enumeration also spins up driver threads whose teardown
    # races), so it is NOT inherently teardown-safe. It is made DETERMINISTIC BY CONSTRUCTION in
    # src/main.cpp, which on Windows flushes stdio then std::_Exit()s past the racy
    # wgpuInstanceRelease call so it never runs (the repo's established idiom for this crash class —
    # see src/render/src/wgpu/offscreen_main.cpp finish()). The by-hand `render`/`window` Windows
    # modes benefit from that same instance-teardown bypass, but each still runs its own
    # wgpuDeviceRelease/wgpuAdapterRelease/wgpuSurfaceRelease calls beforehand (runRenderSelfCheck /
    # runWindowed), so this is not a full teardown bypass for those modes; `render` stays
    # unregistered here on M4-scope grounds (below), independent of this fix. This matches
    # ROADMAP §1 M4's own visual-equivalence
    # scope ("Linux-Vulkan + one browser blocking, other backends advisory"). Windows-specific D3D12
    # render validation, if wanted at M4, belongs on a real interactive-session GPU runner — not this
    # throwaway spike on a headless service. The `render` mode itself still works everywhere (it's
    # just not a CI gate on Windows); run it by hand there if debugging.
    if(NOT WIN32)
        add_test(NAME context-spike-webgpu-render COMMAND context-spike-webgpu render)
        set_tests_properties(context-spike-webgpu-render PROPERTIES
            SKIP_RETURN_CODE 77
            WORKING_DIRECTORY $<TARGET_FILE_DIR:context-spike-webgpu>)
    endif()
endif()
