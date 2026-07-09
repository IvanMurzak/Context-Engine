# src/render/sprite/ — the M4 first-class 2D sprite path (R-2D-001, L-55; issue #122).
#
# A 2D draw path in the SAME renderer (L-55: NOT a 3D engine with 2D bolted on), built OVER the T1
# RHI abstraction (context_render). Two halves, mirroring the parent render module's split:
#
#   * context_render_sprite — the pure-CPU 2D logic: orthographic projection (ortho.*), texture-atlas
#                             packing + UV lookup (atlas.*), and sprite draw-order sorting + batch
#                             coalescing (batch.*). GPU-free, so it builds + is unit-tested under
#                             EVERY toolchain including the local Ninja+Strawberry-GCC dev gate.
#   * the GPU sprite-draw offscreen proof (sprite_offscreen.h) — header-only, expressed entirely
#                             against the RHI, drawn + read back by the parent module's wgpu offscreen
#                             executable on the CI `render` job (Linux, like render-wgpu-offscreen).
#                             Not built here; wired into context_render_wgpu_offscreen by the parent
#                             CMakeLists so the sprite proof reuses the ONE wgpu prebuilt (no fork).

add_library(context_render_sprite STATIC
    src/ortho.cpp
    src/atlas.cpp
    src/batch.cpp)

target_include_directories(context_render_sprite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_render: sprite_offscreen.h (a public header) draws through the RHI (context/render/
# rhi.h), so consumers of this library that include it need context_render's headers. The CPU library
# TUs themselves pull in no GPU backend — a headless build never links one (R-HEAD-002).
target_link_libraries(context_render_sprite PUBLIC context_render)
target_link_libraries(context_render_sprite PRIVATE context_warnings)

# --- CPU unit tests (R-QA-013: behavior ships WITH its tests in the same PR) ---------------------
# Plain assertion executables registered with ctest, reusing the parent module's zero-dependency
# render_test.h harness (../tests). All build + run under the local GCC dev gate with no GPU.
set(_context_render_sprite_tests
    test_ortho            # ortho projection matrix + point/quad-corner projection
    test_atlas            # atlas add/find/uv + shelf packer (fit / overflow / determinism / padding)
    test_batch            # draw-order sort (layer/order/stable) + adjacent-same-atlas batch coalescing
    test_sprite_offscreen)  # compiles the GPU proof header + pins the scene draw order / quad geometry
foreach(_t IN LISTS _context_render_sprite_tests)
    add_executable(context_render_sprite_${_t} tests/${_t}.cpp)
    target_link_libraries(context_render_sprite_${_t} PRIVATE context_render_sprite context_warnings)
    target_include_directories(context_render_sprite_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
    add_test(NAME sprite-${_t} COMMAND context_render_sprite_${_t})
endforeach()
