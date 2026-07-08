// R-QA-013 tests for the derived TS accessor codegen (accessor_codegen.h). Toolchain-independent
// (pure string derivation — no V8, no esbuild), so it is a LOCAL gate on every leg including the
// Strawberry-GCC Windows dev build. Covers: the class-name derivation rule (happy + edge), the
// deterministic full emission for a mixed-type / multi-lane component, per-scalar-family DataView
// spelling (number vs bigint, little-endian flag), and the scalar-vs-lanes accessor signature.

#include "context/editor/component/component_type.h"
#include "context/editor/system/accessor_codegen.h"

#include "system_test.h"

#include <optional>
#include <string>
#include <vector>

namespace csys = context::editor::system;
namespace ccomp = context::editor::component;

namespace
{

// A mixed-type, multi-lane declarative component: a 3-lane f32 vector, a plain f32, a u32, and an i64
// (the BigInt path) — enough to exercise every codegen branch that matters.
[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

ccomp::ComponentTypeSchema compile_mover()
{
    const std::string def = R"({
        "$id": "demo:mover",
        "version": 2,
        "fields": [
            {"name": "pos", "x-ctx-storage": "f32x3"},
            {"name": "max_speed", "x-ctx-storage": "f32"},
            {"name": "hits", "x-ctx-storage": "u32"},
            {"name": "level", "x-ctx-storage": "i64"}
        ]
    })";
    std::vector<std::string> problems;
    std::optional<ccomp::ComponentTypeSchema> t = ccomp::compile_component_type(def, problems);
    CHECK(t.has_value());
    CHECK(problems.empty());
    return t.value_or(ccomp::ComponentTypeSchema{}); // degrade to empty on a failed compile (CHECK logs it)
}

void test_class_name_rule()
{
    CHECK(csys::accessor_class_name("demo:mover") == "demo_mover");
    CHECK(csys::accessor_class_name("a.b/c-d") == "a_b_c_d");
    CHECK(csys::accessor_class_name("already_ok") == "already_ok");
    CHECK(csys::accessor_class_name("9lives") == "_9lives"); // leading digit -> prefixed
    CHECK(csys::accessor_class_name("") == "_");              // empty -> valid identifier
}

void test_deterministic_and_shape()
{
    const ccomp::ComponentTypeSchema mover = compile_mover();
    const std::string a = csys::generate_component_accessor_ts(mover);
    const std::string b = csys::generate_component_accessor_ts(mover);
    CHECK(!a.empty());
    CHECK(a == b); // deterministic: same schema -> byte-identical TS

    CHECK(contains(a, "export class demo_mover"));
    CHECK(contains(a, "static readonly stride: number = " + std::to_string(mover.size) + ";"));
    CHECK(contains(a, "get count(): number"));
    CHECK(contains(a, "new DataView(view.buffer, view.byteOffset, view.byteLength)"));

    // A lanes>1 field takes (row, lane); a scalar field takes (row) only.
    CHECK(contains(a, "getPos(row: number, lane: number): number"));
    CHECK(contains(a, "setPos(row: number, lane: number, v: number): void"));
    CHECK(contains(a, "getMaxSpeed(row: number): number"));
    CHECK(contains(a, "setMaxSpeed(row: number, v: number): void"));

    // Per-family DataView spelling + TS type: f32 -> Float32/number, u32 -> Uint32/number,
    // i64 -> BigInt64/bigint.
    CHECK(contains(a, "this.dv.getFloat32("));
    CHECK(contains(a, "this.dv.getUint32("));
    CHECK(contains(a, "getHits(row: number): number"));
    CHECK(contains(a, "this.dv.getBigInt64("));
    CHECK(contains(a, "getLevel(row: number): bigint"));

    // Little-endian flag present on a multi-byte scalar; the field offset comes from the compiled
    // layout (never hard-coded), so codegen + layout can never drift.
    const ccomp::ComponentField* level = mover.field("level");
    CHECK(level != nullptr);
    CHECK(contains(a, "row * demo_mover.stride + " + std::to_string(level->offset) + ", true"));
}

} // namespace

int main()
{
    test_class_name_rule();
    test_deterministic_and_shape();
    if (systemtest::g_failures == 0)
    {
        std::printf("system accessor codegen: all checks passed\n");
    }
    SYSTEM_TEST_MAIN_END();
}
