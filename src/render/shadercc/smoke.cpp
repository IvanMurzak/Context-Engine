// context_shadercc_smoke — sub-task B de-risk proof (Part of #119; issue #125).
//
// Proves the native shader cross-compile toolchain — glslang + SPIRV-Tools + SPIRV-Cross, pulled
// from the vcpkg manifest's `shader-crosscompile` feature — RESOLVES, BUILDS, and LINKS, and that
// glslang actually lowers a trivial GLSL source to SPIR-V (the issue's required functional smoke).
// It additionally validates the produced module with SPIRV-Tools and reflects it back to GLSL with
// SPIRV-Cross, so all three newly-added native deps are exercised at run time — that is the whole
// point of the flag: de-risk the dependency path before sub-task C wires the real backend behind
// the material/ IShaderCompiler seam. This is throwaway proof, NEVER a shipped target: it builds
// only when CONTEXT_BUILD_SHADER_CROSSCOMPILE=ON (a CI-gated vcpkg dependency path like the wgpu
// backend), so the default local dev gate and the 3-OS build matrix never resolve the toolchain.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <spirv-tools/libspirv.hpp>

#include <spirv_cross/spirv_glsl.hpp>

namespace
{
constexpr const char* kVertexSource =
    "#version 450\n"
    "layout(location = 0) in vec3 inPosition;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vec4(inPosition, 1.0);\n"
    "}\n";

// glslang: lower a trivial GLSL vertex shader to SPIR-V. Returns the SPIR-V words, or an empty
// vector on any compile/link failure (the caller treats empty as failure).
std::vector<std::uint32_t> CompileToSpirv()
{
    glslang::TShader shader(EShLangVertex);
    const char* sources[] = {kVertexSource};
    shader.setStrings(sources, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangVertex, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    // GetDefaultResources() lives in the GLOBAL namespace (glslang/Public/ResourceLimits.h),
    // provided by the glslang::glslang-default-resource-limits target.
    if (!shader.parse(GetDefaultResources(), 100, false, messages))
    {
        std::cerr << "shadercc: glslang parse failed: " << shader.getInfoLog() << '\n';
        return {};
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages))
    {
        std::cerr << "shadercc: glslang link failed: " << program.getInfoLog() << '\n';
        return {};
    }

    std::vector<std::uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), spirv);
    return spirv;
}
} // namespace

int main()
{
    glslang::InitializeProcess();
    const std::vector<std::uint32_t> spirv = CompileToSpirv();
    glslang::FinalizeProcess();

    if (spirv.empty())
    {
        std::cerr << "shadercc: glslang produced no SPIR-V\n";
        return 1;
    }

    // SPIRV-Tools: validate the produced module against the Vulkan 1.0 environment (proves the
    // spirv-tools port links AND the glslang output is well-formed SPIR-V).
    const spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_0);
    if (!tools.Validate(spirv))
    {
        std::cerr << "shadercc: SPIRV-Tools rejected the glslang output\n";
        return 1;
    }

    // SPIRV-Cross: reflect the module back to GLSL (proves the spirv-cross port links AND runs).
    spirv_cross::CompilerGLSL glsl(spirv);
    spirv_cross::CompilerGLSL::Options options;
    options.version = 450;
    glsl.set_common_options(options);
    const std::string reflected = glsl.compile();
    if (reflected.empty())
    {
        std::cerr << "shadercc: SPIRV-Cross produced empty GLSL\n";
        return 1;
    }

    std::cout << "shadercc: OK — glslang produced " << spirv.size()
              << " SPIR-V words, SPIRV-Tools validated it, SPIRV-Cross reflected "
              << reflected.size() << " bytes of GLSL\n";
    return 0;
}
