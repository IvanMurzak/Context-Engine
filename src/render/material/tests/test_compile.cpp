// Compile-via-seam: the deterministic fake/reference backend maps (IR + variant) -> a stub artifact
// that is a pure function of its inputs (R-REND-005; issue #121).

#include "context/render/material/material_ir.h"
#include "context/render/material/shader_compiler.h"

#include "material_test.h"

#include <string>
#include <vector>

using namespace context::render::material;

namespace
{

ShaderIr make_ir()
{
    ShaderIr ir;
    ir.name = "unlit_color";
    ir.keywords.push_back({"FOG", {"off", "on"}});
    ShaderStage vs;
    vs.kind = ShaderStageKind::Vertex;
    vs.entry_point = "vs_main";
    vs.source = "void vs_main() {}";
    ir.stages.push_back(vs);
    ShaderStage fs;
    fs.kind = ShaderStageKind::Fragment;
    fs.entry_point = "fs_main";
    fs.source = "void fs_main() {}";
    ir.stages.push_back(fs);
    return ir;
}

// A compiled artifact carries the backend id, the IR hash, the variant key, and a non-empty blob.
void test_artifact_fields()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(!variants.empty());

    FakeShaderCompiler compiler;
    const CompiledArtifact art = compiler.compile(ir, variants.front());

    CHECK(art.compiler_id == "fake-ref-v1");
    CHECK(art.ir_hash == ir_content_hash(ir));
    CHECK(art.variant_key == variants.front().canonical());
    CHECK(!art.artifact.empty());
    CHECK(compiler.compile_count() == 1);
}

// Determinism: the SAME (ir, variant) yields byte-identical artifacts, even across two backend
// instances — this is what makes the content-addressed cache sound.
void test_determinism()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);

    FakeShaderCompiler a;
    FakeShaderCompiler b;
    const CompiledArtifact art_a = a.compile(ir, variants.front());
    const CompiledArtifact art_b = b.compile(ir, variants.front());
    CHECK(art_a == art_b);
}

// Different variants of the same shader produce DIFFERENT artifacts (distinct variant_key + blob).
void test_variants_differ()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(variants.size() == 2);

    FakeShaderCompiler compiler;
    const CompiledArtifact off = compiler.compile(ir, variants[0]);
    const CompiledArtifact on = compiler.compile(ir, variants[1]);
    CHECK(off != on);
    CHECK(off.variant_key != on.variant_key);
    CHECK(off.artifact != on.artifact);
    CHECK(compiler.compile_count() == 2);
}

// The backend id is configurable and flows into the artifact (a cache-key component downstream).
void test_backend_id()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);

    FakeShaderCompiler custom("fake-ref-alt");
    const CompiledArtifact art = custom.compile(ir, variants.front());
    CHECK(custom.id() == "fake-ref-alt");
    CHECK(art.compiler_id == "fake-ref-alt");
}

} // namespace

int main()
{
    test_artifact_fields();
    test_determinism();
    test_variants_differ();
    test_backend_id();
    MATERIAL_TEST_MAIN_END();
}
