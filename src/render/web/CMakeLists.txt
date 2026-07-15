# src/render/web/ — the T1 WEB RHI backend build (M4 T6, issue #137; R-REND-002, L-56, R-HEAD-002).
#
# The BROWSER's WebGPU backend: the SAME rhi.h implementation the native backend uses
# (../src/wgpu/wgpu_rhi.cpp), compiled to wasm+JS via Emscripten + the emdawnwebgpu port
# (webgpu.h -> navigator.gpu) — the L-56-locked web path, explicitly NOT a Dawn cross-compile. It is
# a NEW RHI backend, not a renderer fork: the sim->render extract + double-buffer + detachable
# renderer facade stay shared with native (already T1, in context_render).
#
# STANDALONE-configured under emcc, like the proven spikes/webgpu web leg — it is deliberately NOT
# add_subdirectory()'d from the main src/ tree, so the default 3-OS `dev` build matrix and the local
# GCC dev gate NEVER touch it (the rest of the engine — editor/runtime/cli daemon+IPC — does not
# compile under emscripten). The `render-web` CI job (.github/workflows/ci.yml) is its ONLY build
# signal (emcc is not on the local Windows executor):
#
#   emcmake cmake -S src/render/web -B build/render-web -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/render-web        # -> context-render-web.{html,js,wasm}
#
# The kernel-free triangle (3D pipeline) + sprite (2D) proofs run here; the lit/PBR (3D
# lighting/shadow) web proof pulls the kernel + extract and lands with the T7 golden gate (README.md).

cmake_minimum_required(VERSION 3.25)
project(context-render-web LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(CONTEXT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (baseline: ON)" ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/ContextWarnings.cmake)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "context-render-web is the Emscripten/emdawnwebgpu WEB backend build; "
                        "configure it with emcmake (the emscripten toolchain), e.g. "
                        "`emcmake cmake -S src/render/web -B build/render-web`. It is not part of "
                        "the native build (the native T1 backend is context_render_wgpu under "
                        "CONTEXT_BUILD_RENDER_WGPU).")
endif()

set(_render_root ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The web T1 RHI backend: the SAME rhi.h implementation as native (context_render_wgpu), compiled for
# the browser's WebGPU via emdawnwebgpu. NO CTX_RENDER_HAS_WGPU_EXTRAS — emdawnwebgpu has no
# wgpuInstanceEnumerateAdapters extra, so the R-HEAD-002 probe falls back to requestAdapter (the
# #ifdef fallback already in wgpu_rhi.cpp). --use-port=emdawnwebgpu at COMPILE resolves <webgpu/
# webgpu.h> onto navigator.gpu.
add_library(context_render_web STATIC ${_render_root}/src/wgpu/wgpu_rhi.cpp)
target_include_directories(context_render_web PUBLIC ${_render_root}/include)
target_compile_options(context_render_web PRIVATE "--use-port=emdawnwebgpu")
target_link_libraries(context_render_web PRIVATE context_warnings)

# The kernel-free 2D sprite CPU logic (ortho projection / atlas / draw-order sort) — pure math over
# the RHI, so the 2D sprite proof runs on the web with no kernel/extract dependency. Compiled from
# the SAME sources as the native context_render_sprite library (no fork).
add_library(context_render_web_sprite STATIC
    ${_render_root}/sprite/src/ortho.cpp
    ${_render_root}/sprite/src/atlas.cpp
    ${_render_root}/sprite/src/batch.cpp)
target_include_directories(context_render_web_sprite PUBLIC
    ${_render_root}/include
    ${_render_root}/sprite/include)
target_link_libraries(context_render_web_sprite PRIVATE context_warnings)

# The a1 runtime UI package (context_ui): the retained UiTree/UiNode + damage + the provider contract,
# compiled from the SAME sources as native (no fork). Pure stdlib (D6 presentation-only), so it builds
# under emscripten with no kernel/GPU dependency — the ui-hud golden's web leg (M7 a6, the web-target
# 4th edit beyond the ci.yml render drill).
add_library(context_ui_web STATIC
    ${_render_root}/../packages/ui/src/damage.cpp
    ${_render_root}/../packages/ui/src/layout.cpp
    ${_render_root}/../packages/ui/src/provider.cpp
    ${_render_root}/../packages/ui/src/ui_tree.cpp)
target_include_directories(context_ui_web PUBLIC ${_render_root}/../packages/ui/include)
target_link_libraries(context_ui_web PRIVATE context_warnings)

# The M7 a6 engine-integrated GPU UI backend (context_render_ui): extract/composite/batch/provider over
# the RHI, compiled from the SAME sources as native. The ui-hud golden's web render drives this
# (hud_scene.h); it reuses the sprite ortho/quad math (context_render_web_sprite) + a1's UiTree.
add_library(context_render_ui_web STATIC
    ${_render_root}/ui/src/snapshot.cpp
    ${_render_root}/ui/src/composite.cpp
    ${_render_root}/ui/src/batch.cpp
    ${_render_root}/ui/src/provider.cpp)
target_include_directories(context_render_ui_web PUBLIC
    ${_render_root}/include
    ${_render_root}/sprite/include
    ${_render_root}/ui/include
    ${_render_root}/../packages/ui/include)
target_link_libraries(context_render_ui_web PRIVATE context_warnings)

# The offscreen parity harness -> an .html shell (directly servable for the in-browser run).
add_executable(context-render-web web_main.cpp)
target_include_directories(context-render-web PRIVATE
    ${_render_root}/include
    ${_render_root}/sprite/include
    ${_render_root}/ui/include
    ${_render_root}/../packages/ui/include)
target_link_libraries(context-render-web
    PRIVATE context_render_web context_render_web_sprite context_render_ui_web context_ui_web
            context_warnings)
target_compile_options(context-render-web PRIVATE "--use-port=emdawnwebgpu")

# emdawnwebgpu build constraints (ROADMAP §5 risk table; spikes/webgpu/FINDINGS.md notes 3-5):
#   * --use-port=emdawnwebgpu           — Dawn's maintained webgpu.h binding (replaces the removed
#                                         -sUSE_WEBGPU); auto-downloads the pinned Dawn pkg at link.
#   * -sASYNCIFY                        — lets the synchronous pump loop (emscripten_sleep) yield to
#                                         the browser event loop so WebGPU promises resolve.
#   * -sEXIT_RUNTIME=1                  — main's return becomes the process exit code (harness result).
#   * -sINITIAL_MEMORY=67108864 (64 MiB), sized UP FRONT, and deliberately NO -sALLOW_MEMORY_GROWTH:
#     growth makes the wasm heap a *resizable* ArrayBuffer, and emdawnwebgpu's string glue
#     (TextDecoder.decode on a heap view) throws on resizable buffers -> device acquisition dies
#     (measured: emcc 6.x + emdawnwebgpu, Chrome). Fixed memory is plenty for the offscreen harness.
target_link_options(context-render-web PRIVATE
    "--use-port=emdawnwebgpu"
    "-sASYNCIFY"
    "-sEXIT_RUNTIME=1"
    "-sINITIAL_MEMORY=67108864")
set_target_properties(context-render-web PROPERTIES SUFFIX ".html")
