# src/render/shadercc/ — the M4 native shader cross-compile TOOLCHAIN de-risk (R-REND-005; issue
# #125, sub-task B of #119) PLUS the REAL cross-compile backend (issue #130, sub-task C of #119). This
# directory is entered ONLY when CONTEXT_BUILD_SHADER_CROSSCOMPILE=ON (gated from
# src/render/CMakeLists.txt), which is a vcpkg-manifest dependency path
# (VCPKG_MANIFEST_FEATURES=shader-crosscompile) — like the wgpu backend it is a CI-gated path the
# local default dev gate never resolves. Sub-task B proves the native toolchain resolves/builds/links
# and that glslang lowers GLSL -> SPIR-V (the smoke below); sub-task C hosts the real context_shadercc
# cross-compile backend behind material/'s IShaderCompiler seam (the context_shadercc library +
# round-trip test below).

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
# context_shadercc — the REAL cross-compile backend (sub-tasks C+D of #119, issues #130/#133): a
# concrete context::render::material::IShaderCompiler that lowers each authored stage to SPIR-V via
# glslang and cross-compiles it to HLSL/MSL/GLSL via SPIRV-Cross plus WGSL via the pinned tint
# subprocess (the measured M4 tool ruling — docs/wgsl-tool-decision.md). It plugs in BEHIND the
# backend-free material/ seam (it links context_render_material for the interface + ShaderIr types)
# WITHOUT touching callers; the fake backend stays the default path. Built only under this flag, so
# the default dev gate + 3-OS build matrix never resolve the toolchain (like context_render_wgpu /
# the smoke above).
# ---------------------------------------------------------------------------------------------

# The WGSL leg's tools are staged by the fetch scripts (CI: the `shader-crosscompile` job stages both
# and prepends them to PATH before this configure):
#   * tint — the WIRED SPIR-V->WGSL tool, built from Dawn source at the tag pinned in
#     tools/tint-toolchain.json (tools/fetch_tint.py; commit-verified, fail-closed). tint has no
#     --version self-report, so the pin string is baked into the backend and folded into every
#     artifact (`wgsltool=` line) for R-FILE-010 cache-key hygiene.
#   * naga — the round-trip test's NATIVE-CONSUMER cross-validator (the in-tree wgpu-native parses
#     WGSL with naga), pinned in tools/naga-toolchain.json (tools/fetch_naga.py).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/tint-toolchain.json" _tint_manifest)
string(JSON _tint_pin_tag GET "${_tint_manifest}" tag)
find_program(CONTEXT_SHADERCC_TINT_EXECUTABLE NAMES tint
    DOC "tint executable for the SPIR-V->WGSL leg (staged by tools/fetch_tint.py)")
if(NOT CONTEXT_SHADERCC_TINT_EXECUTABLE)
    message(FATAL_ERROR "CONTEXT_BUILD_SHADER_CROSSCOMPILE needs the pinned `tint` on PATH — "
        "run `python3 tools/fetch_tint.py --dest <dir>` and add <dir>/bin to PATH "
        "(or pass -DCONTEXT_SHADERCC_TINT_EXECUTABLE=<path>).")
endif()
find_program(CONTEXT_SHADERCC_NAGA_EXECUTABLE NAMES naga
    DOC "naga executable for the round-trip test's native-consumer WGSL cross-validation (staged by tools/fetch_naga.py)")
if(NOT CONTEXT_SHADERCC_NAGA_EXECUTABLE)
    message(FATAL_ERROR "CONTEXT_BUILD_SHADER_CROSSCOMPILE needs the pinned `naga` on PATH — "
        "run `python3 tools/fetch_naga.py --dest <dir>` and add <dir>/bin to PATH "
        "(or pass -DCONTEXT_SHADERCC_NAGA_EXECUTABLE=<path>).")
endif()

add_library(context_shadercc STATIC src/cross_compiler.cpp)
target_include_directories(context_shadercc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(context_shadercc PRIVATE
    CONTEXT_SHADERCC_WGSL_TOOL_EXECUTABLE="${CONTEXT_SHADERCC_TINT_EXECUTABLE}"
    CONTEXT_SHADERCC_WGSL_TOOL_PIN="tint dawn-${_tint_pin_tag}")
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
# The author->SPIR-V->{HLSL,MSL,GLSL,WGSL} round-trip over the REAL authored corpus (reused from
# material/corpus/ via an ABSOLUTE compile definition so the test never depends on the ctest working
# directory — the same pattern context_render_material_test_material_ir uses). Registered under the
# `shader-crosscompile` preset, so the EXISTING `shader-crosscompile` CI job (issue #128) runs it via
# `ctest` with NO new job. The test TU includes no third-party headers (the backend hides them), so
# it needs no /external:W0. The naga definition feeds the WGSL cross-validation: every emitted WGSL
# module must ALSO parse under the native consumer's naga generation (spikes/webgpu/FINDINGS.md —
# emitted WGSL is consumed by BOTH Tint on web and naga inside wgpu-native on native).
add_executable(context_shadercc_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(context_shadercc_roundtrip PRIVATE context_shadercc context_warnings)
target_include_directories(context_shadercc_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_shadercc_roundtrip
    PRIVATE CONTEXT_MATERIAL_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../material/corpus"
            CONTEXT_SHADERCC_NAGA_EXECUTABLE="${CONTEXT_SHADERCC_NAGA_EXECUTABLE}")
# Self-contained GCC runtime (gated to GNU; inert on the Clang/MSVC/AppleClang CI legs) — the round-trip
# exe reads the corpus via std::ifstream/std::ostringstream (setup.md § static-link note).
target_link_options(context_shadercc_roundtrip
    PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

add_test(NAME shader-crosscompile-roundtrip COMMAND context_shadercc_roundtrip)
