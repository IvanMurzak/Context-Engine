# src/render/ui/ — the M7 engine-integrated GPU UI backend (R-UI-005; M7 a6). Implements a1's
# backend-agnostic UiProvider contract (context_ui, src/packages/ui/) over the T1 RHI (context_render):
# UI extract -> UiRenderSnapshot on the L-39 double buffer, quad batching (reusing the sprite path's
# sort/coalesce discipline), damage-based repaint into a PERSISTENT offscreen UI-layer texture, and
# GPU-composited transforms/opacity at composite time. Capabilities advertised: gpu_driver +
# damage_repaint + composited_transforms.
#
#   * context_render_ui — the CPU + RHI-abstraction backend: extract, composite math, batch/draw-set
#                         selection, and the GpuUiProvider. GPU-free of any concrete backend (drives only
#                         rhi.h), so it builds + is unit-tested under EVERY toolchain (the local
#                         Ninja+Strawberry-GCC dev gate) AND compiles into the Emscripten web target.
#   * the ui-hud golden proof (hud_scene.h) — header-only, expressed entirely against the RHI, rendered
#                         + read back by the parent module's wgpu offscreen executable (native) and the
#                         web harness. Not built here; wired into context_render_wgpu_offscreen + the web
#                         target by their CMakeLists so the proof reuses the ONE wgpu / emdawnwebgpu path.

add_library(context_render_ui STATIC
    src/snapshot.cpp
    src/composite.cpp
    src/batch.cpp
    src/provider.cpp
    src/dynamic_texture.cpp  # M7 a9 (D4): the first dynamic-texture registry (per-panel RTT targets)
    src/glyph_atlas.cpp      # M7 a7 (issue #237): the GLYPH-ID-keyed R8 glyph atlas + LRU eviction
    src/text_quads.cpp)      # M7 a7: the offset-bearing atlas-textured quad emitter (a8-ready)

target_include_directories(context_render_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_ui: the extract + provider consume a1's UiTree/UiNode + the UiProvider contract header
# (public in this library's headers). PUBLIC context_render_sprite: reuse the proven ortho projection +
# quad WGSL (sprite/ortho.h, sprite_offscreen.h) — the geometry the CI GPU readback already SSIM-gates —
# and, transitively, context_render's rhi.h. The kernel is NEVER pulled in (D6 / R-HEAD-002): UI is
# presentation, lives outside the sim World.
target_link_libraries(context_render_ui PUBLIC context_ui context_render_sprite)
target_link_libraries(context_render_ui PRIVATE context_warnings)

# --- CPU unit tests (R-QA-013: behavior ships WITH its tests in the same PR) ----------------------
# Plain assertion executables registered with ctest, reusing the parent render module's render_test.h
# harness (../tests) + its GPU-free fake RHI backend (render_test_rhi.h). All build + run under the local
# GCC dev gate with no GPU. The `render-ui-*` family is not a gate family, so it auto-runs in the build
# job's general ctest step on all three OS legs (no ci.yml edit) and is built by --preset dev.
#
# hud_scene.h pulls golden.h (stream file IO for the PPM writer) + the offscreen test round-trips a PPM;
# static-link the GCC runtime (gated to GNU) so a stale libstdc++-6.dll on PATH can't shadow-crash the
# exe at launch on this MinGW host (setup.md § static-link note; the sibling packages do the same).
set(_context_render_ui_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_render_ui_tests
    test_ui_extract      # extract selects visible drawables; opacity/transform composited; double-buffer
    test_ui_composite    # composite math: transform (translate/scale-about-centre), opacity fold/compose
    test_ui_provider     # capabilities (shaping+bidi true); present() over the fake RHI: clear vs load
    test_ui_dynamic_texture # M7 a9 (D4): dynamic-texture registry + provider-into-target + extract chain
    test_ui_worldpanel   # M7 a9 (D4): compiles worldpanel_scene.h + pins the analytic ui-worldpanel golden
    test_glyph_atlas     # M7 a7: GLYPH-ID-keyed atlas build/hit/eviction/LRU-touch + blit + edges
    test_text_quads)     # M7 a7: offset-bearing atlas quad emitter — placement/UVs/GPOS offsets/skips
# These tests (and the atlas + emitter sources) are FreeType-FREE — the glyph atlas rasterizes via a
# caller-supplied callback, so context_render_ui carries no context_ui_text / FreeType dependency. The
# tests that include hud_scene.h (the ui-hud golden scene, which M7 a8 gave a shaped-text label) OR drive
# the real font path link context_ui_text — the separate _context_render_ui_text_tests loop below.
foreach(_t IN LISTS _context_render_ui_tests)
    add_executable(context_render_ui_${_t} tests/${_t}.cpp)
    target_link_libraries(context_render_ui_${_t} PRIVATE context_render_ui context_warnings)
    target_include_directories(context_render_ui_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
    target_link_options(context_render_ui_${_t} PRIVATE ${_context_render_ui_static_runtime})
    add_test(NAME render-ui-${_t} COMMAND context_render_ui_${_t})
endforeach()

# --- tests that link context_ui_text (the FreeType text package) ----------------------------------
# These either include hud_scene.h (the ui-hud golden scene, which M7 a8 gave a SHAPED-TEXT label drawn
# via measure -> FreeType glyph atlas -> textured-glyph cutout) or drive the real embedded font directly:
#   * test_ui_batch          — uses hud_scene.h build_reference_hud for its draw-set fixtures
#   * test_glyph_atlas_font  — measure -> glyph-id-keyed atlas -> emit over the embedded Noto Sans face
#   * test_ui_hud_offscreen  — the ui-hud golden proof (reference HUD extract + analytic rects + the 1-bit
#                              text mask + PPM round-trip); the CI render/render-web jobs SSIM-gate the same
#                              frame. `render-ui-*` auto-run in the build job's general step (no ci.yml edit).
set(_context_render_ui_text_tests
    test_ui_batch          # draw-order sort + adjacent coalesce + damage -> MINIMAL draw set (draw counts)
    test_glyph_atlas_font  # M7 a7: real-font measure -> atlas -> emit; repeated glyph ids share ONE cell
    test_ui_hud_offscreen) # M7 a6/a8: the ui-hud golden scene (rects + shaped text) analytic + fake-backend
foreach(_t IN LISTS _context_render_ui_text_tests)
    add_executable(context_render_ui_${_t} tests/${_t}.cpp)
    target_link_libraries(context_render_ui_${_t}
        PRIVATE context_render_ui context_ui_text context_warnings)
    target_include_directories(context_render_ui_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
    target_link_options(context_render_ui_${_t} PRIVATE ${_context_render_ui_static_runtime})
    add_test(NAME render-ui-${_t} COMMAND context_render_ui_${_t})
endforeach()
