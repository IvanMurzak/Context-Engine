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
# vcpkg packages SPIRV-Cross as per-component CONFIG packages (spirv_cross_core, spirv_cross_glsl, …),
# and each component's Config (spirv_cross_glslConfig.cmake etc.) references the imported target of its
# base component WITHOUT find_dependency-ing it. So they MUST be found in dependency order — core, then
# glsl, then hlsl/msl (which derive from glsl) — otherwise the dependent's *_FOUND is set FALSE
# ("imported target spirv-cross-<base> is missing") and the REQUIRED find_package errors. sub-task C
# adds the hlsl + msl components (the real cross-compile backend targets HLSL/MSL/GLSL).
find_package(spirv_cross_core CONFIG REQUIRED)
find_package(spirv_cross_glsl CONFIG REQUIRED)
find_package(spirv_cross_hlsl CONFIG REQUIRED)
find_package(spirv_cross_msl CONFIG REQUIRED)
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

# The glslang GLSL->SPIR-V + SPIRV-Tools validate + SPIRV-Cross reflect smoke (ctest
# --preset shader-crosscompile), exit 0 on success. Sub-task B de-risk proof.
add_test(NAME shader-crosscompile-smoke COMMAND context_shadercc_smoke)

# ---------------------------------------------------------------------------------------------
# context_shadercc — the REAL cross-compile backend (sub-task C of #119, issue #130): a concrete
# context::render::material::IShaderCompiler that lowers each authored stage to SPIR-V via glslang and
# cross-compiles it to HLSL/MSL/GLSL via SPIRV-Cross. It plugs in BEHIND the backend-free material/
# seam (it links context_render_material for the interface + ShaderIr types) WITHOUT touching callers;
# the fake backend stays the default path. Built only under this flag, so the default dev gate + 3-OS
# build matrix never resolve the toolchain (like context_render_wgpu / the smoke above).
# ---------------------------------------------------------------------------------------------
add_library(context_shadercc STATIC src/cross_compiler.cpp)
target_include_directories(context_shadercc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(context_shadercc
    PUBLIC context_render_material   # the IShaderCompiler seam + authoring IR (propagates its include)
    PRIVATE
        glslang::glslang
        glslang::glslang-default-resource-limits
        glslang::SPIRV
        spirv-cross-glsl
        spirv-cross-hlsl
        spirv-cross-msl
        context_warnings)

if(MSVC)
    # Only cross_compiler.cpp pulls the third-party glslang/SPIRV-Cross headers; the public header is
    # first-party only. /external:W0 keeps /W4 /WX on our code while those headers stay quiet.
    target_compile_options(context_shadercc PRIVATE /external:W0)
endif()

# --- tests (R-QA-013: behaviour ships WITH its tests in the same PR) -----------------------------
# The author->SPIR-V->{HLSL,MSL,GLSL} round-trip over the REAL authored corpus (reused from
# material/corpus/ via an ABSOLUTE compile definition so the test never depends on the ctest working
# directory — the same pattern context_render_material_test_material_ir uses). Registered under the
# `shader-crosscompile` preset, so the EXISTING `shader-crosscompile` CI job (issue #128) runs it via
# `ctest` with NO .github/** change. The test TU includes no third-party headers (the backend hides
# them), so it needs no /external:W0.
add_executable(context_shadercc_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(context_shadercc_roundtrip PRIVATE context_shadercc context_warnings)
target_include_directories(context_shadercc_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_shadercc_roundtrip
    PRIVATE CONTEXT_MATERIAL_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../material/corpus")
# Self-contained GCC runtime (gated to GNU; inert on the Clang/MSVC/AppleClang CI legs) — the round-trip
# exe reads the corpus via std::ifstream/std::ostringstream (setup.md § static-link note).
target_link_options(context_shadercc_roundtrip
    PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

add_test(NAME shader-crosscompile-roundtrip COMMAND context_shadercc_roundtrip)
