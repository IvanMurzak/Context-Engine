# src/render/ — the M4 render subsystem foundation (R-REND-001/002, R-HEAD-002, L-11/L-39/L-56;
# issue #114). Two libraries by design:
#
#   * context_render      — the tiered RHI ABSTRACTION (rhi.h, WebGPU T1 semantics) + the L-39
#                           sim->render extract + double buffer + the DETACHABLE renderer facade.
#                           Dependency-free of any GPU backend, so it builds + tests under EVERY
#                           toolchain including the local Ninja+Strawberry-GCC Windows dev gate. This
#                           is where the R-HEAD-002 / L-39 coverage lives (a GPU-free fake backend in
#                           tests/ drives the full RHI object model).
#
#   * context_render_wgpu — the T1 NATIVE backend over the wgpu-native PREBUILT + the offscreen
#                           pixel-readback proof. Promotes the throwaway spikes/webgpu de-risk to a
#                           real subsystem under the R-SEC-009 signed-prebuilt exception (SHA-pinned
#                           + verify-before-use; acquisition channel owner-decided by the #76 V8
#                           Option-A precedent). Its only off-the-shelf Windows prebuilt is MSVC-ABI,
#                           so — like the V8 dependency path (src/runtime/js/) — the local Strawberry
#                           GCC dev gate CANNOT build it; it is GATED behind CONTEXT_BUILD_RENDER_WGPU
#                           (default OFF) and built + tested by the dedicated `render` CI job (the
#                           generalization of the spike-webgpu jobs). Render+readback correctness is
#                           validated deterministically on the Linux (lavapipe) leg; the Windows leg
#                           builds+links under MSVC (the /W4 /WX + C4190 signal) and runs `probe`.

# ---------------------------------------------------------------------------------------------
# context_render — RHI abstraction + extract + double buffer + detachable renderer (no GPU backend).
# ---------------------------------------------------------------------------------------------
add_library(context_render STATIC
    src/extract.cpp
    src/renderer.cpp)

target_include_directories(context_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel: the extract reads the World through the kernel's public headers (World,
# Entity, ComponentId). Render depends on the kernel, NEVER the reverse (R-HEAD-002 / R-KERNEL-002):
# the kernel links nothing render-related, so a headless build never pulls a GPU backend.
target_link_libraries(context_render PUBLIC context_kernel)
target_link_libraries(context_render PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). All three build + run under the local GCC dev gate.
set(_context_render_tests
    test_extract        # L-39 extract selects only drawables; double-buffer no-tearing semantics
    test_headless       # R-HEAD-002: probe creates no device; no-adapter is headless; engine runs absent
    test_rhi            # RHI object model driven end-to-end (offscreen readback) via the fake backend
    test_light_extract  # R-REND-004: light/material extraction incl. the malformed/absent edges
    test_ui_panel_extract) # M7 a9 (D4/L-39): world-space UiPanel extract incl. the no-Transform edge
foreach(_t IN LISTS _context_render_tests)
    add_executable(context_render_${_t} tests/${_t}.cpp)
    target_link_libraries(context_render_${_t} PRIVATE context_render context_warnings)
    target_include_directories(context_render_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME render-${_t} COMMAND context_render_${_t})
endforeach()

# ---------------------------------------------------------------------------------------------
# Feature packages layered on the RHI abstraction. Each is its own subdirectory building a CPU
# library over context_render (GPU-free, locally tested) plus a header-only GPU proof wired into the
# wgpu offscreen executable below. Each sub-package declares its own target, resolved at generate
# time regardless of add_subdirectory order (order below is cosmetic — an append-only catalog).
# ---------------------------------------------------------------------------------------------
# context_render_material — the backend-free material/shader AUTHORING layer (R-REND-005; issue #121,
# Part of #119): authoring IR + shader-variant enumeration + the IShaderCompiler seam (fake backend) +
# the R-FILE-010 content-addressed shader-compile cache.
add_subdirectory(material)
add_subdirectory(sprite)   # R-2D-001 / L-55 — the 2D sprite path (issue #122)
add_subdirectory(lit)      # R-REND-004/006 — PBR + lighting/shadows + lightmap hooks (issue #135)
add_subdirectory(ui)       # R-UI-005 — M7 a6 engine-integrated GPU UI backend (context_render_ui)

# --- WEB backend local parity guard (M4 T6, issue #137; R-QA-013) --------------------------------
# The emdawnwebgpu web backend (src/render/web/, a CI-only emscripten build) renders the SAME
# triangle (3D pipeline) + sprite (2D) proofs native renders. This native ctest is the
# locally-runnable guard on that exact proof set: the triangle asserted through the RHI on the fake
# backend + the sprite scene's draw order / baked quad geometry pinned (its GPU rasterization is the
# browser/Linux-CI gate). Links context_render_sprite (declared just above) for the 2D inputs.
add_executable(context_render_test_web_parity tests/test_web_parity.cpp)
target_link_libraries(context_render_test_web_parity
    PRIVATE context_render context_render_sprite context_warnings)
target_include_directories(context_render_test_web_parity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME render-web-parity COMMAND context_render_test_web_parity)

# --- M4 T7: golden-scene corpus + min-spec bench local coverage (issue #141; R-QA-013) -----------
# Self-contained GCC runtime on both (setup.md § static-link note): test_golden_cpu does stream
# file IO (the PPM round-trip) and golden_lit.h pulls std::string/chrono — a stale libstdc++-6.dll
# on PATH would otherwise crash the exe at launch on the MinGW host though the build was clean.
set(_context_render_golden_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# m4-exit-headless-no-render — the M4 EXIT gate's headless criterion (R-HEAD-002/004): run by the
# CI build job's blocking "M4 exit gate" named step on all three legs (wgpu toggle OFF — no GPU
# backend compiled), and by the local dev gate. The other two exit criteria are CI-job gates on the
# real backends (the goldens/ SSIM corpus + the min-spec bench in the render / render-web jobs).
add_executable(context_render_test_m4_exit tests/test_m4_exit.cpp)
target_link_libraries(context_render_test_m4_exit
    PRIVATE context_render context_render_sprite context_warnings)
target_include_directories(context_render_test_m4_exit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_options(context_render_test_m4_exit PRIVATE ${_context_render_golden_static_runtime})
add_test(NAME m4-exit-headless-no-render COMMAND context_render_test_m4_exit)

# render-golden-cpu — drives golden.h + golden_lit.h (corpus render, PPM writer, bench loop + its
# JSON contract) end-to-end on the fake backend, so the whole T7 surface compiles AND runs under
# the local GPU-less dev gate (the real rasterization signal is the CI render job + goldens/).
add_executable(context_render_test_golden_cpu tests/test_golden_cpu.cpp)
target_link_libraries(context_render_test_golden_cpu
    PRIVATE context_render context_render_sprite context_render_lit context_warnings)
target_include_directories(context_render_test_golden_cpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_options(context_render_test_golden_cpu PRIVATE ${_context_render_golden_static_runtime})
add_test(NAME render-golden-cpu COMMAND context_render_test_golden_cpu)

# ---------------------------------------------------------------------------------------------
# context_render_wgpu — the T1 native backend + offscreen pixel-readback proof (CI-only dep path).
# Opt-in: default OFF so the local GCC dev gate and the default 3-OS build matrix never fetch the
# wgpu-native prebuilt. The dedicated `render` CI job configures with -DCONTEXT_BUILD_RENDER_WGPU=ON.
# ---------------------------------------------------------------------------------------------
option(CONTEXT_BUILD_RENDER_WGPU
    "Build the wgpu-native T1 render backend + offscreen readback proof (downloads the SHA-pinned \
wgpu-native prebuilt; MSVC on Windows, clang/gcc elsewhere). Off by default — the local GCC dev gate \
cannot link the MSVC-ABI Windows prebuilt, so this is a CI-gated dependency path like V8." OFF)

if(CONTEXT_BUILD_RENDER_WGPU)
    # wgpu-native PREBUILT release, pinned version + per-platform SHA256 (R-SEC-009: SHA-pinned +
    # publisher-TLS + verify-before-use; from-source vcpkg port deferred pre-1.0, #76 precedent).
    # Same pins the proven spikes/webgpu spike uses.
    set(CTX_WGPU_NATIVE_VERSION "v29.0.1.1")
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        if(NOT MSVC)
            message(FATAL_ERROR "context_render_wgpu on Windows uses the -msvc prebuilt (MSVC import \
lib); build with MSVC (the CI render job does) or add the -gnu asset pin.")
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
        message(FATAL_ERROR "context_render_wgpu: no wgpu-native prebuilt pinned for this platform \
(${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}); add the asset + SHA256 from the \
${CTX_WGPU_NATIVE_VERSION} release.")
    endif()

    set(_wgpu_root "${CMAKE_BINARY_DIR}/_deps/wgpu-native-render")
    set(_wgpu_pkg "${_wgpu_root}/pkg")
    if(NOT EXISTS "${_wgpu_pkg}/include/webgpu/webgpu.h")
        message(STATUS "context_render_wgpu: downloading wgpu-native ${CTX_WGPU_NATIVE_VERSION} \
prebuilt (${_wgpu_asset}, ~14-17 MB) + verifying SHA256 before use (R-SEC-009)")
        # Retry/backoff + fail-closed SHA verify (Context-Engine#129): a transient GitHub-releases
        # 504/timeout no longer hard-fails this dependency-path configure. The pin is unchanged.
        context_download(
            URL "https://github.com/gfx-rs/wgpu-native/releases/download/${CTX_WGPU_NATIVE_VERSION}/${_wgpu_asset}"
            PATH "${_wgpu_root}/${_wgpu_asset}"
            EXPECTED_SHA256 "${_wgpu_sha256}"
            DESCRIPTION "wgpu-native ${CTX_WGPU_NATIVE_VERSION} (${_wgpu_asset})")
        file(ARCHIVE_EXTRACT INPUT "${_wgpu_root}/${_wgpu_asset}" DESTINATION "${_wgpu_pkg}")
    endif()

    add_library(context_render_wgpu STATIC src/wgpu/wgpu_rhi.cpp)
    target_include_directories(context_render_wgpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # wgpu-native headers are third-party: SYSTEM include so their own warnings never trip -Werror.
    target_include_directories(context_render_wgpu SYSTEM PRIVATE "${_wgpu_pkg}/include")
    # wgpu-native extras (webgpu/wgpu.h): instance-level adapter enumeration for the R-HEAD-002 probe.
    target_compile_definitions(context_render_wgpu PRIVATE CTX_RENDER_HAS_WGPU_EXTRAS)
    target_link_libraries(context_render_wgpu PUBLIC context_render)
    target_link_libraries(context_render_wgpu PRIVATE context_warnings)

    if(WIN32)
        target_link_libraries(context_render_wgpu PUBLIC "${_wgpu_pkg}/lib/wgpu_native.dll.lib")
    elseif(APPLE)
        target_link_libraries(context_render_wgpu PUBLIC "${_wgpu_pkg}/lib/libwgpu_native.dylib")
    else()
        target_link_libraries(context_render_wgpu PUBLIC "${_wgpu_pkg}/lib/libwgpu_native.so")
    endif()
    if(MSVC)
        # /external:W0 — keep /W4 /WX for our code while the prebuilt's headers stay quiet.
        target_compile_options(context_render_wgpu PRIVATE /external:W0)
    endif()

    # The offscreen proof executable: probe | render (the offscreen readback via the RHI abstraction).
    # context_render_sprite: the offscreen exe's `sprite` mode runs the R-2D-001 GPU sprite-draw proof
    # (sprite_offscreen.h) through the SAME wgpu backend — reusing the ONE prebuilt, not forking it.
    # context_render_lit: likewise, the `lit` mode runs the R-REND-004/006 GPU PBR/shadow/lightmap
    # proof (lit_offscreen.h) through the same backend.
    # context_render_ui: the offscreen exe's `ui` mode + `golden ui-hud` scene run the M7 a6 GPU UI
    # backend (hud_scene.h) through the SAME wgpu backend — reusing the ONE prebuilt, not forking it.
    # context_ui_text (M7 a8): hud_scene.h now draws a shaped-text label (measure -> FreeType glyph atlas
    # -> textured-glyph cutout draw), so the golden dump exe links the text package.
    add_executable(context_render_wgpu_offscreen src/wgpu/offscreen_main.cpp)
    target_link_libraries(context_render_wgpu_offscreen
        PRIVATE context_render_wgpu context_render_sprite context_render_lit context_render_ui
                context_ui_text context_warnings)
    if(WIN32)
        # Stage wgpu_native.dll next to the exe so the dynamic import resolves at run time.
        add_custom_command(TARGET context_render_wgpu_offscreen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_wgpu_pkg}/lib/wgpu_native.dll"
                "$<TARGET_FILE_DIR:context_render_wgpu_offscreen>"
            COMMENT "context_render_wgpu: staging wgpu_native.dll next to the offscreen exe")
    elseif(APPLE)
        # RPATH goes on the EXECUTABLE (the actual runtime loader of libwgpu_native.dylib), not the
        # static context_render_wgpu archive, which has no load step. BUILD_RPATH is additive to the
        # RPATH CMake auto-derives from the full-path dylib link, so this only ensures resolution.
        set_target_properties(context_render_wgpu_offscreen PROPERTIES BUILD_RPATH "${_wgpu_pkg}/lib")
    endif()

    # R-HEAD-002 probe: enumerate adapters / report absence WITHOUT a device — exit 0 everywhere,
    # incl. adapter-less headless boxes. Runs on Windows too: offscreen_main.cpp std::_Exit()s past
    # wgpu-native's flaky Session-0 teardown after the probe asserts (see its `finish()` note), so
    # the enumerate/R-HEAD-002 assertion still exercises on the Windows leg WITHOUT hitting the
    # 0xc0000409 teardown __fastfail that this test originally tripped intermittently.
    add_test(NAME render-wgpu-probe COMMAND context_render_wgpu_offscreen probe)

    # Offscreen render + pixel readback: needs SOME adapter (a software one — lavapipe/Metal — is
    # fine). Exit 77 = clean SKIP when none exists.
    #
    # DELIBERATELY NOT REGISTERED ON WINDOWS, matching the proven spikes/webgpu spike: the
    # self-hosted Windows runners are LocalSystem services in Session 0, where the native wgpu-native
    # render path intermittently crashes (0xc0000409) in device/instance teardown regardless of
    # adapter. offscreen_main.cpp's finish() std::_Exit skips that flaky TEARDOWN, but the render
    # path also does live device/queue/draw work whose Session-0 stability is not proven, so — unlike
    # the teardown-only probe — the offscreen readback stays Windows-unregistered. A flaky native
    # crash is unacceptable for CI. Render+readback correctness is validated deterministically on
    # Linux (lavapipe) + (via the same shared proof code) the fake-backend ctest; Windows stays
    # deterministic via the build+link (MSVC) and the `probe` test.
    if(NOT WIN32)
        add_test(NAME render-wgpu-offscreen COMMAND context_render_wgpu_offscreen render)
        set_tests_properties(render-wgpu-offscreen PROPERTIES
            SKIP_RETURN_CODE 77
            WORKING_DIRECTORY $<TARGET_FILE_DIR:context_render_wgpu_offscreen>)

        # The R-2D-001 GPU sprite-draw proof: ortho-projected quads + sorting-layer overdraw, read
        # back + asserted. Like render-wgpu-offscreen it needs a rasterizing adapter (software is
        # fine — lavapipe/Metal), exit 77 = clean SKIP when none exists, and is NOT registered on
        # Windows (the same Session-0 native-render carve-out the offscreen proof documents above).
        add_test(NAME render-wgpu-sprite COMMAND context_render_wgpu_offscreen sprite)
        set_tests_properties(render-wgpu-sprite PROPERTIES
            SKIP_RETURN_CODE 77
            WORKING_DIRECTORY $<TARGET_FILE_DIR:context_render_wgpu_offscreen>)

        # The R-REND-004/006 GPU lit/PBR proof: World -> extract -> shadow depth pass + PBR main
        # pass -> readback asserted against the CPU reference (lit-vs-unlit / parameter deltas,
        # shadowed-vs-unshadowed probes, lightmap-hook round-trip). Same adapter requirement, SKIP
        # convention, and Windows carve-out as the sibling proofs.
        add_test(NAME render-wgpu-lit COMMAND context_render_wgpu_offscreen lit)
        set_tests_properties(render-wgpu-lit PROPERTIES
            SKIP_RETURN_CODE 77
            WORKING_DIRECTORY $<TARGET_FILE_DIR:context_render_wgpu_offscreen>)

        # M5-F1 (issue #164): the observer-viewport composite proof — the 3D triangle base + the 2D
        # sprites overlaid, read back + asserted (clear + 3D layer + 2D-on-top). The frame the
        # "viewport" golden gates. Same adapter requirement, SKIP convention, and Windows carve-out as
        # the sibling proofs; auto-picked by the render CI job's `^render-wgpu-` regex.
        add_test(NAME render-wgpu-viewport COMMAND context_render_wgpu_offscreen viewport)
        set_tests_properties(render-wgpu-viewport PROPERTIES
            SKIP_RETURN_CODE 77
            WORKING_DIRECTORY $<TARGET_FILE_DIR:context_render_wgpu_offscreen>)

        # M7 a6 (R-UI-005): the engine-integrated GPU UI proof — the reference HUD extracted through the
        # GpuUiProvider, repainted into its persistent layer, read back + asserted (clear background,
        # solid panel, health fill over its background, composited badge). The frame the `ui-hud` golden
        # gates. Same adapter requirement, SKIP convention, and Windows carve-out as the sibling proofs;
        # auto-picked by the render CI job's `^render-wgpu-` regex (no ci.yml --target edit).
        add_test(NAME render-wgpu-ui COMMAND context_render_wgpu_offscreen ui)
        set_tests_properties(render-wgpu-ui PROPERTIES
            SKIP_RETURN_CODE 77
            WORKING_DIRECTORY $<TARGET_FILE_DIR:context_render_wgpu_offscreen>)

        # M7 a9 (R-UI-003, D4): the world-space RTT UI-panel proof — a UI tree rendered into a dynamic-
        # texture registry target, then sampled onto a rotated world quad over a lit-ground base, read
        # back + asserted (sky + lit ground + the panel's RTT texel on the quad). The frame the
        # `ui-worldpanel` golden gates. Same adapter requirement, SKIP convention, and Windows carve-out
        # as the sibling proofs; auto-picked by the render CI job's `^render-wgpu-` regex (no ci.yml
        # --target edit).
        add_test(NAME render-wgpu-worldpanel COMMAND context_render_wgpu_offscreen worldpanel)
        set_tests_properties(render-wgpu-worldpanel PROPERTIES
            SKIP_RETURN_CODE 77
            WORKING_DIRECTORY $<TARGET_FILE_DIR:context_render_wgpu_offscreen>)
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# shadercc — the native shader cross-compile TOOLCHAIN de-risk (R-REND-005; issue #125, sub-task B
# of #119). Like context_render_wgpu this is a CI-gated dependency path: the toolchain (glslang +
# SPIRV-Cross + SPIRV-Tools) comes from the vcpkg `shader-crosscompile` manifest feature, so it is
# NOT resolved by the default local dev gate or the 3-OS build matrix. Default OFF -> the normal
# build is unaffected. It only proves the toolchain resolves/builds/links + a glslang->SPIR-V smoke;
# the real backend wires behind material/'s IShaderCompiler seam in sub-task C. Declared here as its
# OWN tail stanza — deliberately apart from add_subdirectory(material) above — so the concurrent
# sibling task editing material/ merges cleanly (the shadercc subdir is added only under the flag).
# ---------------------------------------------------------------------------------------------
option(CONTEXT_BUILD_SHADER_CROSSCOMPILE
    "Build the native shader cross-compile toolchain proof (glslang + SPIRV-Cross + SPIRV-Tools from \
the vcpkg shader-crosscompile manifest feature) + a glslang->SPIR-V smoke test. Off by default — the \
toolchain is a vcpkg dependency path (VCPKG_MANIFEST_FEATURES=shader-crosscompile), CI-gated like the \
wgpu backend, that the local default dev gate never resolves. Sub-task B of #119 (issue #125)." OFF)

if(CONTEXT_BUILD_SHADER_CROSSCOMPILE)
    add_subdirectory(shadercc)
endif()
