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

# The Linux CPU present fallback (M9 e12a): XShmPutImage / XPutImage into the window, the exact
# counterpart of the gdi32 block above. libX11 + libXext are the platform's OWN windowing system —
# the direct analogue of gdi32/user32 — so nothing enters src/vcpkg.json or the license allowlist
# (the same reasoning src/editor/shell/ records for winhttp/ole32).
#
# PROBED, NOT REQUIRED, and this is load-bearing. Every ubuntu CI job configures this directory
# (`build`, `sanitize` x2, `render`, `linux-export`, `editor-boundary`, the spikes, the benches…),
# and only `editor-cef-smoke` and `build` install X11 development packages. A `find_package(X11
# REQUIRED)` would therefore fail CONFIGURE — that is, red the whole rollup — on every other one.
# Probing instead degrades to the honest "no X11 blitter in this build" diagnostic that
# make_present_blitter already had to carry for a missing platform. Non-vacuity is enforced where it
# matters instead: the editor-cef-smoke Linux leg runs the live windowed smoke with --require-x11,
# which fails if this block did not fire.
if(UNIX AND NOT APPLE)
    find_package(X11)
    if(X11_FOUND AND X11_XShm_FOUND AND TARGET X11::X11 AND TARGET X11::Xext)
        target_link_libraries(context_render_present PRIVATE X11::X11 X11::Xext)
        target_compile_definitions(context_render_present PRIVATE CONTEXT_PRESENT_HAS_X11=1)
        message(STATUS "context_render_present: X11 + MIT-SHM found — the Linux CPU present "
                       "fallback is REAL in this build")
    else()
        message(STATUS "context_render_present: no X11/MIT-SHM development headers — the Linux CPU "
                       "present fallback compiles to its honest missing-platform report "
                       "(install libx11-dev + libxext-dev for the real one)")
    endif()
endif()

# The macOS CPU present fallback (M9 e12b): a CGImage assigned to CALayer.contents, the exact
# counterpart of the gdi32 and libX11 blocks above. QuartzCore + CoreGraphics are the platform's OWN
# 2D/compositing frameworks — nothing enters src/vcpkg.json or the license allowlist, and the design
# 08 §3 dependency gate is not reached (the same reasoning both blocks above record).
#
# ⚠ THIS IS THE FIRST `.mm` IN A DEFAULT-BUILT TARGET, and it is added by target_sources under an
# APPLE guard rather than into the add_library() list above for a reason: CMake picks a compiler by
# file EXTENSION, and OBJCXX is enabled only under `if(APPLE)` in src/CMakeLists.txt. Listing it
# unconditionally would ask the Windows and Linux legs to compile Objective-C++ with no OBJCXX
# compiler enabled and fail at GENERATE time. UNCONDITIONAL on Apple, unlike the PROBED X11 block:
# QuartzCore is not an optional development package the way libx11-dev is on a headless runner. The
# non-Apple half of make_cocoa_layer_blitter stays in present_blit.cpp, so the off-platform refusal
# remains a VALUE the render-present-test_present_blit ctest asserts on all three legs.
if(APPLE)
    target_sources(context_render_present PRIVATE src/present_blit_mac.mm)
    target_link_libraries(context_render_present PRIVATE
        "-framework QuartzCore" "-framework CoreGraphics" "-framework Foundation")
    # ARC for the .mm TU: the blitter holds a CALayer reference by value in a C++ class, and ARC is
    # what makes that lifetime correct without a hand-written rule-of-five over an Objective-C
    # object (mirrors src/render/'s metal_interop.mm and src/editor/shell/'s cocoa_window.mm).
    set_source_files_properties(src/present_blit_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    message(STATUS "context_render_present: AppKit/QuartzCore found — the macOS CPU present "
                   "fallback (CALayer.contents) is REAL in this build")
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
