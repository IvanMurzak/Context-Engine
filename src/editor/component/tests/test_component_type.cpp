// Declarative component-type compiler tests: happy path (compile + layout derivation + publish),
// edge cases (multi-lane packing, alignment, layout-hash sensitivity), failure paths (every dialect +
// vocabulary-law rejection). R-QA-013 / R-LANG-010.

#include "context/editor/component/component_type.h"
#include "component_test.h"

#include <optional>
#include <string>
#include <vector>

using namespace context::editor::component;

namespace
{
// Compile helper: returns the compiled type (or nullopt) and stashes the problems out-of-band.
std::optional<ComponentTypeSchema> compile(const std::string& json, std::vector<std::string>& probs)
{
    probs.clear();
    return compile_component_type(json, probs);
}
} // namespace

int main()
{
    // --- storage-layout parsing (lockstep with schema::is_storage_layout) ----------------------
    {
        const std::optional<StorageLayout> s = parse_storage_layout("f32");
        CHECK(s.has_value());
        CHECK(s->base == ScalarKind::f32);
        CHECK(s->lanes == 1);
        CHECK(s->byte_size() == 4);
        CHECK(s->align() == 4);

        const std::optional<StorageLayout> v = parse_storage_layout("f64x3");
        CHECK(v.has_value());
        CHECK(v->base == ScalarKind::f64);
        CHECK(v->lanes == 3);
        CHECK(v->byte_size() == 24);
        CHECK(v->align() == 8);

        const std::optional<StorageLayout> m = parse_storage_layout("i32x16");
        CHECK(m.has_value());
        CHECK(m->byte_size() == 64);

        CHECK(!parse_storage_layout("f128").has_value());
        CHECK(!parse_storage_layout("f32x5").has_value()); // lane 5 not pinned
        CHECK(!parse_storage_layout("").has_value());
    }

    // --- happy path: compile + derived layout + offsets ----------------------------------------
    {
        // u8 flag, then f32 pos (needs 4-align → padded), then i16 hp. Proves per-field alignment.
        const std::string json = R"({
          "$id": "demo:widget",
          "version": 2,
          "notes": "a demo component",
          "fields": [
            {"name": "flag", "x-ctx-storage": "u8"},
            {"name": "pos", "x-ctx-storage": "f32x3", "x-ctx-units": "m"},
            {"name": "hp", "x-ctx-storage": "i16"}
          ]
        })";
        std::vector<std::string> probs;
        const std::optional<ComponentTypeSchema> t = compile(json, probs);
        CHECK(t.has_value());
        if (t.has_value())
        {
            CHECK(probs.empty());
            CHECK(t->id == "demo:widget");
            CHECK(t->version == 2);
            CHECK(t->field_count() == 3);

            const ComponentField* flag = t->field("flag");
            const ComponentField* pos = t->field("pos");
            const ComponentField* hp = t->field("hp");
            CHECK(flag != nullptr && pos != nullptr && hp != nullptr);
            CHECK(flag->offset == 0);              // u8 at 0
            CHECK(pos->offset == 4);               // padded from 1 → 4 (f32 align)
            CHECK(pos->storage.lanes == 3);
            CHECK(hp->offset == 16);               // 4 + 12
            CHECK(t->align == 4);                  // max field align (f32)
            CHECK(t->size == 20);                  // 16 + 2 (i16) = 18 → padded to 20 (align 4)

            // Published introspection carries id/version/size/layoutHash + the derived field index.
            const std::string intro = component_type_introspection_json(*t);
            CHECK(intro.find("\"demo:widget\"") != std::string::npos);
            CHECK(intro.find("layoutHash") != std::string::npos);
            CHECK(intro.find("\"offset\"") != std::string::npos);
            CHECK(intro.find("f32x3") != std::string::npos);
            CHECK(!t->canonical_doc.empty());
        }
    }

    // --- layout-hash: stability + sensitivity --------------------------------------------------
    {
        const std::string a = R"({"$id":"a:t","version":1,"fields":[
            {"name":"x","x-ctx-storage":"f32"},{"name":"y","x-ctx-storage":"f32"}]})";
        // Same fields, same order → same hash regardless of version / id.
        const std::string a2 = R"({"$id":"other:name","version":9,"fields":[
            {"name":"x","x-ctx-storage":"f32"},{"name":"y","x-ctx-storage":"f32"}]})";
        // Reordered fields → different hash.
        const std::string a_reordered = R"({"$id":"a:t","version":1,"fields":[
            {"name":"y","x-ctx-storage":"f32"},{"name":"x","x-ctx-storage":"f32"}]})";
        // Different storage width → different hash.
        const std::string a_wide = R"({"$id":"a:t","version":1,"fields":[
            {"name":"x","x-ctx-storage":"f64"},{"name":"y","x-ctx-storage":"f32"}]})";
        // Renamed field → different hash.
        const std::string a_renamed = R"({"$id":"a:t","version":1,"fields":[
            {"name":"x","x-ctx-storage":"f32"},{"name":"z","x-ctx-storage":"f32"}]})";

        std::vector<std::string> p;
        const auto ha = compile(a, p);
        const auto hb = compile(a2, p);
        const auto hr = compile(a_reordered, p);
        const auto hw = compile(a_wide, p);
        const auto hn = compile(a_renamed, p);
        CHECK(ha && hb && hr && hw && hn);
        CHECK(ha->layout_hash == hb->layout_hash);        // id/version do NOT affect layout
        CHECK(ha->layout_hash != hr->layout_hash);        // order matters
        CHECK(ha->layout_hash != hw->layout_hash);        // width matters
        CHECK(ha->layout_hash != hn->layout_hash);        // name matters
        CHECK(ha->layout_hash != 0);
    }

    // --- failure paths: dialect + vocabulary law -----------------------------------------------
    {
        std::vector<std::string> p;
        // not an object
        CHECK(!compile("[]", p).has_value());
        // missing $id
        CHECK(!compile(R"({"version":1,"fields":[{"name":"x","x-ctx-storage":"f32"}]})", p));
        // malformed $id (no namespace)
        CHECK(!compile(R"({"$id":"nope","version":1,"fields":[{"name":"x","x-ctx-storage":"f32"}]})",
                       p));
        // version < 1
        CHECK(!compile(R"({"$id":"a:b","version":0,"fields":[{"name":"x","x-ctx-storage":"f32"}]})",
                       p));
        // empty fields
        CHECK(!compile(R"({"$id":"a:b","version":1,"fields":[]})", p));
        // bad storage layout
        CHECK(!compile(
            R"({"$id":"a:b","version":1,"fields":[{"name":"x","x-ctx-storage":"f13"}]})", p));
        // non-SI units (the units LAW)
        CHECK(!compile(R"({"$id":"a:b","version":1,"fields":[
            {"name":"x","x-ctx-storage":"f32","x-ctx-units":"degrees"}]})", p));
        // unknown x-ctx-type
        CHECK(!compile(R"({"$id":"a:b","version":1,"fields":[
            {"name":"x","x-ctx-storage":"f32","x-ctx-type":"matrix"}]})", p));
        // duplicate field name
        CHECK(!compile(R"({"$id":"a:b","version":1,"fields":[
            {"name":"x","x-ctx-storage":"f32"},{"name":"x","x-ctx-storage":"f32"}]})", p));
        // malformed field name (uppercase)
        CHECK(!compile(
            R"({"$id":"a:b","version":1,"fields":[{"name":"X","x-ctx-storage":"f32"}]})", p));
        // unknown root key (closed dialect)
        CHECK(!compile(R"({"$id":"a:b","version":1,"bogus":true,"fields":[
            {"name":"x","x-ctx-storage":"f32"}]})", p));
        // unknown field key
        CHECK(!compile(R"({"$id":"a:b","version":1,"fields":[
            {"name":"x","x-ctx-storage":"f32","bogus":1}]})", p));
        // not JSON at all
        CHECK(!compile("{not json", p));
        // A rejection always reports at least one problem.
        CHECK(!p.empty());
    }

    // --- x-ctx-type acceptance (valid semantic type is allowed) --------------------------------
    {
        std::vector<std::string> p;
        const auto t = compile(R"({"$id":"a:b","version":1,"fields":[
            {"name":"tint","x-ctx-storage":"f32x4","x-ctx-type":"color"}]})", p);
        CHECK(t.has_value());
        CHECK(p.empty());
    }

    COMPONENT_TEST_MAIN_END();
}
