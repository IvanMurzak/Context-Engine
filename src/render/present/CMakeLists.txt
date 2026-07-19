# src/render/present/ — the M9 e03 PRESENTATION path (design 03 §2/§3/§4; R-HEAD-004, L-41).
#
# context_render_present holds everything that only a WINDOWED shell needs: the OSR import policy +
# dirty-rect upload driver, the premultiplied-alpha composite pass, and the C-F2 CPU present
# fallback. It is GPU-BACKEND-FREE — it links context_render (the rhi.h abstraction), no GPU backend
# and nothing from the editor tree (plus context_warnings, and gdi32 for the Windows GDI fallback
# below) — so it builds and is fully unit-tested under the local Ninja+Strawberry-GCC dev gate and on
# all three CI `build` legs, exactly like context_render itself. The wgpu-native ISurface/ISwapchain
# implementation lives in the CI-gated context_render_wgpu, not here.
#
# THE HEADLESS INVARIANT (03 §2): nothing in the daemon/runtime/CLI may link this target. That is not
# a convention — cmake/ContextPresentIsolation.cmake walks the transitive link closure of those
# targets at configure time and FAILS THE BUILD if presentation reaches them, on every OS leg.

add_library(context_render_present STATIC
    src/osr_import.cpp       # the per-platform accelerated/CPU-upload DECISION + the upload driver
    src/osr_composite.cpp    # the fullscreen-triangle premultiplied composite + its CPU oracle
    src/present_blit.cpp)    # C-F2: the GPU-less OS blit seam + the Windows GDI implementation

target_include_directories(context_render_present PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(context_render_present PUBLIC context_render)
target_link_libraries(context_render_present PRIVATE context_warnings)

if(WIN32)
    # StretchDIBits / GetDC / FillRect live in gdi32. Named for BOTH toolchains: CMake maps this to
    # -lgdi32 under MinGW and gdi32.lib under MSVC, so the local GCC gate compiles + links the real
    # Windows present fallback rather than skipping it.
    target_link_libraries(context_render_present PRIVATE gdi32)
endif()

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest, in the `render-present-*` family. That
# family is NOT a gate family, so it is auto-run by the build job's general ctest step on all three
# OS legs and needs no ci.yml --target bookkeeping (the "Not Run = RED" tripwire does not apply).
#
# ../tests is on the include path for render_test_rhi.h — the ONE GPU-free fake backend, extended by
# this task with a fake surface/swapchain/external-texture import so the present path's whole object
# model is driven with no GPU.
set(_context_render_present_tests
    test_osr_import      # the per-platform import policy (all 3 branches) + dirty-rect uploads
    test_osr_composite   # premultiplied blend + visible_rect/coded_size UVs + orientation
    test_present_blit    # C-F2 letterbox/pillarbox geometry + the honest missing-platform report
    test_swapchain)      # the acquire/present/resize/unconfigure loop incl. Outdated/Lost recovery
foreach(_t IN LISTS _context_render_present_tests)
    add_executable(context_render_present_${_t} tests/${_t}.cpp)
    target_link_libraries(context_render_present_${_t}
        PRIVATE context_render_present context_warnings)
    target_include_directories(context_render_present_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
    add_test(NAME render-present-${_t} COMMAND context_render_present_${_t})
endforeach()
