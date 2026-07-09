// Shader-variant enumeration: cartesian product, deterministic order, canonical keys, and the
// no-keyword + multi-value edges (R-REND-005; issue #121).

#include "context/render/material/material_ir.h"

#include "material_test.h"

#include <set>
#include <string>
#include <vector>

using namespace context::render::material;

namespace
{

ShaderIr make_ir(const std::vector<ShaderKeyword>& keywords)
{
    ShaderIr ir;
    ir.name = "probe";
    ir.keywords = keywords;
    ShaderStage vs;
    vs.kind = ShaderStageKind::Vertex;
    vs.entry_point = "main";
    vs.source = "void main() {}";
    ir.stages.push_back(vs);
    return ir;
}

// No keywords => exactly one empty variant with an empty canonical key.
void test_no_keywords()
{
    const std::vector<VariantKey> variants = enumerate_variants(make_ir({}));
    CHECK(variants.size() == 1);
    if (!variants.empty())
    {
        CHECK(variants.front().defines.empty());
        CHECK(variants.front().canonical().empty());
    }
}

// A single boolean keyword => two variants.
void test_single_boolean()
{
    const std::vector<VariantKey> variants = enumerate_variants(make_ir({{"FOG", {"off", "on"}}}));
    CHECK(variants.size() == 2);
    CHECK(variants[0].canonical() == "FOG=off");
    CHECK(variants[1].canonical() == "FOG=on");
}

// Cartesian product size = product of the value-set sizes; a multi-value axis is exercised.
void test_cartesian_product_and_uniqueness()
{
    const ShaderIr ir = make_ir({{"NORMAL_MAP", {"off", "on"}},
                                 {"QUALITY", {"low", "med", "high"}},
                                 {"SHADOWS", {"off", "on"}}});
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(variants.size() == 2 * 3 * 2); // 12

    // Every canonical key is unique.
    std::set<std::string> keys;
    for (const VariantKey& v : variants)
    {
        keys.insert(v.canonical());
    }
    CHECK(keys.size() == variants.size());

    // Each key's defines are sorted by keyword name (canonical identity), independent of authored
    // order, and there is one define per keyword axis.
    for (const VariantKey& v : variants)
    {
        CHECK(v.defines.size() == 3);
        for (std::size_t i = 1; i < v.defines.size(); ++i)
        {
            CHECK(v.defines[i - 1].first < v.defines[i].first);
        }
    }
}

// Determinism: authored keyword ORDER does not change the enumerated variant SET (keys are sorted by
// name), and the same input yields the same ordered output twice.
void test_order_independent_and_deterministic()
{
    const ShaderIr a = make_ir({{"B", {"0", "1"}}, {"A", {"x", "y"}}});
    const ShaderIr b = make_ir({{"A", {"x", "y"}}, {"B", {"0", "1"}}});

    const std::vector<VariantKey> va = enumerate_variants(a);
    const std::vector<VariantKey> vb = enumerate_variants(b);
    CHECK(va == vb);
    CHECK(enumerate_variants(a) == va); // repeatable

    // A varies slowest, B fastest (keywords sorted by name; last varies fastest).
    CHECK(va.size() == 4);
    if (va.size() == 4)
    {
        CHECK(va[0].canonical() == "A=x;B=0");
        CHECK(va[1].canonical() == "A=x;B=1");
        CHECK(va[2].canonical() == "A=y;B=0");
        CHECK(va[3].canonical() == "A=y;B=1");
    }
}

// Defensive: a keyword with an empty value set contributes no axis (parse() never emits one, but a
// programmatically-built IR might).
void test_empty_value_axis_ignored()
{
    const std::vector<VariantKey> variants =
        enumerate_variants(make_ir({{"EMPTY", {}}, {"REAL", {"a", "b"}}}));
    CHECK(variants.size() == 2);
    CHECK(variants[0].canonical() == "REAL=a");
    CHECK(variants[1].canonical() == "REAL=b");
}

} // namespace

int main()
{
    test_no_keywords();
    test_single_boolean();
    test_cartesian_product_and_uniqueness();
    test_order_independent_and_deterministic();
    test_empty_value_axis_ignored();
    MATERIAL_TEST_MAIN_END();
}
