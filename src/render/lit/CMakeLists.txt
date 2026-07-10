# src/render/lit/ — the M4 PBR + real-time lighting/shadows path (R-REND-004 MUST) with the
# R-REND-006 lightmap INPUT hooks (issue #135). Two halves, mirroring the sprite module's split:
#
#   * context_render_lit — the pure-CPU half: 3D math (lit_math.*), the CPU reference of the WGSL's
#                          metallic-roughness shading (pbr.* — the analytic oracle the GPU proof
#                          asserts against), and the reference scene (lit_scene.*: authored World
#                          components -> extract -> uniform packing, WGSL generation, analytic
#                          sample points). GPU-free, so it builds + is unit-tested under EVERY
#                          toolchain including the local Ninja+Strawberry-GCC dev gate.
#   * the GPU lit offscreen proof (lit_offscreen.h) — header-only, expressed entirely against the
#                          RHI, drawn + read back by the parent module's wgpu offscreen executable
#                          (`lit` mode) on the CI `render` job (Linux/lavapipe, like the triangle
#                          and sprite proofs). Not built here; wired by the parent CMakeLists.

add_library(context_render_lit STATIC
    src/lit_math.cpp
    src/pbr.cpp
    src/lit_scene.cpp)

target_include_directories(context_render_lit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_render: the scene consumes the render components + extract (render_world.h /
# extract.h) and lit_offscreen.h (a public header) draws through the RHI. The CPU library TUs pull
# in no GPU backend — a headless build never links one (R-HEAD-002).
target_link_libraries(context_render_lit PUBLIC context_render)
target_link_libraries(context_render_lit PRIVATE context_warnings)

# --- CPU unit tests (R-QA-013: behavior ships WITH its tests in the same PR) ---------------------
# Plain assertion executables registered with ctest, reusing the parent module's zero-dependency
# render_test.h harness (../tests). All build + run under the local GCC dev gate with no GPU.
# Self-contained GCC runtime: lit_scene.cpp emits WGSL via std::ostringstream + std::locale, which
# pull newer libstdc++ symbols; on this MinGW host a stale libstdc++-6.dll elsewhere on PATH can
# shadow the toolchain's and crash the exe at launch even though the build was clean (setup.md §
# static-link note). Gated to GNU so the macOS/Clang + Windows/MSVC CI legs are untouched.
set(_context_render_lit_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_render_lit_tests
    test_lit_math   # mat/vec ops; camera + light view-projections map known points correctly
    test_pbr        # CPU BRDF reference: horizon/energy/roughness/metallic/attenuation properties
    test_lit_scene) # scene tables, sample-point projection, shadow analytics, packing, WGSL shape
foreach(_t IN LISTS _context_render_lit_tests)
    add_executable(context_render_lit_${_t} tests/${_t}.cpp)
    target_link_libraries(context_render_lit_${_t} PRIVATE context_render_lit context_warnings)
    target_link_options(context_render_lit_${_t} PRIVATE ${_context_render_lit_static_runtime})
    target_include_directories(context_render_lit_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
    add_test(NAME lit-${_t} COMMAND context_render_lit_${_t})
endforeach()
