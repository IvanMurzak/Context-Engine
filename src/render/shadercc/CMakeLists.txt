# src/render/shadercc/ — the M4 native shader cross-compile TOOLCHAIN de-risk (R-REND-005; issue
# #125, sub-task B of #119). This directory is entered ONLY when CONTEXT_BUILD_SHADER_CROSSCOMPILE=ON
# (gated from src/render/CMakeLists.txt), which is a vcpkg-manifest dependency path
# (VCPKG_MANIFEST_FEATURES=shader-crosscompile) — like the wgpu backend it is a CI-gated path the
# local default dev gate never resolves. It does NOT yet wire cross-compile logic into the engine
# (that is sub-task C, behind material/'s IShaderCompiler seam); it only proves the native toolchain
# resolves/builds/links and that glslang lowers GLSL -> SPIR-V.

# glslang / SPIRV-Cross / SPIRV-Tools come from the vcpkg `shader-crosscompile` manifest feature.
# Their imported CONFIG targets carry SYSTEM include dirs, so their own headers never trip our
# -Werror / /WX baseline.
find_package(glslang CONFIG REQUIRED)
find_package(spirv_cross_glsl CONFIG REQUIRED)
find_package(SPIRV-Tools CONFIG REQUIRED)

add_executable(context_shadercc_smoke smoke.cpp)
target_link_libraries(context_shadercc_smoke PRIVATE
    glslang::glslang
    glslang::glslang-default-resource-limits
    glslang::SPIRV
    spirv-cross-glsl
    SPIRV-Tools-static
    context_warnings)

if(MSVC)
    # /external:W0 — keep /W4 /WX for our code while the third-party headers stay quiet (mirrors
    # context_render_wgpu).
    target_compile_options(context_shadercc_smoke PRIVATE /external:W0)
endif()

# Self-contained GCC runtime (gated to GNU, a no-op on the Clang/MSVC/AppleClang CI toolchains):
# if a developer opts into this flag on the local MinGW host, a stale libstdc++-6.dll elsewhere on
# PATH can shadow the toolchain's at run time (setup.md § static-link note). Harmless everywhere
# else. CI builds this under Clang/MSVC/AppleClang, so the guard makes it inert on every CI leg.
target_link_options(context_shadercc_smoke
    PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# The single ctest the `shader-crosscompile` CI job runs (ctest --preset shader-crosscompile):
# glslang GLSL->SPIR-V + SPIRV-Tools validate + SPIRV-Cross reflect, exit 0 on success.
add_test(NAME shader-crosscompile-smoke COMMAND context_shadercc_smoke)
