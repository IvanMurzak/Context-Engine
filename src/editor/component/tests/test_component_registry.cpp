// Runtime component-type registration + data-driven archetype storage round-trip (R-LANG-010,
// R-QA-013): compile a declarative definition, register it, add it to real World entities with NO
// compile-time knowledge of the type, and round-trip payloads through the canonical serializer.

#include "context/editor/component/component_registry.h"
#include "context/editor/component/component_type.h"
#include "context/editor/serializer/json_parse.h"
#include "component_test.h"

#include <optional>
#include <string>
#include <vector>

using namespace context::editor::component;
namespace ser = context::editor::serializer;
using context::kernel::Entity;
using context::kernel::World;

namespace
{
ComponentTypeSchema must_compile(const std::string& json)
{
    std::vector<std::string> probs;
    std::optional<ComponentTypeSchema> t = compile_component_type(json, probs);
    CHECK(t.has_value());
    CHECK(probs.empty());
    return t.value_or(ComponentTypeSchema{});
}

ser::JsonValue parse(const std::string& json)
{
    const ser::ParseResult r = ser::parse_json(json);
    CHECK(r.ok);
    return r.root;
}

const ser::JsonValue* member(const ser::JsonValue& obj, const char* key)
{
    for (const ser::JsonMember& m : obj.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}
} // namespace

int main()
{
    const std::string mover_json = R"({
      "$id": "game:mover",
      "version": 1,
      "fields": [
        {"name": "pos", "x-ctx-storage": "f32x3", "x-ctx-units": "m"},
        {"name": "hp", "x-ctx-storage": "i32"},
        {"name": "flags", "x-ctx-storage": "u8"}
      ]
    })";

    // --- registration: stable id + component id + POD ops --------------------------------------
    {
        ComponentTypeRegistry reg;
        const RegisteredComponentType& r = reg.register_type(must_compile(mover_json));
        CHECK(r.schema.id == "game:mover");
        CHECK(r.ops.size == r.schema.size);
        CHECK(r.ops.align == r.schema.align);
        CHECK(r.ops.move_construct == nullptr); // POD: trivially relocatable
        CHECK(r.ops.destroy == nullptr);

        CHECK(reg.by_id("game:mover") == &r);
        CHECK(reg.by_component_id(r.component_id) == &r);
        CHECK(reg.by_id("nope") == nullptr);
        CHECK(reg.all().size() == 1);

        // Re-registering the same id REPLACES in place, keeping the ComponentId stable.
        const context::kernel::ComponentId before = r.component_id;
        const RegisteredComponentType& r2 = reg.register_type(must_compile(mover_json));
        CHECK(r2.component_id == before);
        CHECK(reg.all().size() == 1);
    }

    // --- data-driven storage round-trip through the real World ---------------------------------
    {
        ComponentTypeRegistry reg;
        const RegisteredComponentType& mover = reg.register_type(must_compile(mover_json));

        World w;
        const Entity e = w.create();
        void* rec = reg.add_default(w, e, mover);
        CHECK(rec != nullptr);
        CHECK(w.has_raw(e, mover.component_id));

        // A default record reads back all-zero.
        {
            const ser::JsonValue got = read_payload(mover, rec);
            const ser::JsonValue* hp = member(got, "hp");
            CHECK(hp != nullptr && hp->type == ser::JsonValue::Type::integer && hp->int_value == 0);
        }

        // Encode a full payload, then read it back.
        std::vector<std::string> probs;
        const ser::JsonValue payload = parse(R"({"pos":[1.5,2.0,-3.0],"hp":42,"flags":7})");
        CHECK(encode_payload(mover, payload, rec, probs));
        CHECK(probs.empty());

        const ser::JsonValue got = read_payload(mover, w.get_raw(e, mover.component_id));
        const ser::JsonValue* pos = member(got, "pos");
        const ser::JsonValue* hp = member(got, "hp");
        const ser::JsonValue* flags = member(got, "flags");
        CHECK(pos != nullptr && pos->type == ser::JsonValue::Type::array &&
              pos->elements.size() == 3);
        if (pos != nullptr && pos->elements.size() == 3)
        {
            CHECK(pos->elements[0].number_value == 1.5);
            CHECK(pos->elements[1].number_value == 2.0);
            CHECK(pos->elements[2].number_value == -3.0);
        }
        CHECK(hp != nullptr && hp->int_value == 42);
        CHECK(flags != nullptr && flags->int_value == 7);

        // serialize_payload produces canonical JSON (keys sorted: flags, hp, pos).
        const std::string canon = serialize_payload(mover, w.get_raw(e, mover.component_id));
        CHECK(canon.find("\"hp\": 42") != std::string::npos);
        CHECK(canon.find("\"flags\": 7") != std::string::npos);

        // Partial-patch: absent fields keep their bytes.
        std::vector<std::string> p2;
        CHECK(encode_payload(mover, parse(R"({"hp":100})"), w.get_raw(e, mover.component_id), p2));
        CHECK(p2.empty());
        const ser::JsonValue after = read_payload(mover, w.get_raw(e, mover.component_id));
        CHECK(member(after, "hp")->int_value == 100);
        CHECK(member(after, "pos")->elements[0].number_value == 1.5); // untouched
    }

    // --- payload failure paths -----------------------------------------------------------------
    {
        ComponentTypeRegistry reg;
        const RegisteredComponentType& mover = reg.register_type(must_compile(mover_json));
        World w;
        const Entity e = w.create();
        void* rec = reg.add_default(w, e, mover);

        std::vector<std::string> p;
        CHECK(!encode_payload(mover, parse(R"({"flags":300})"), rec, p)); // u8 overflow
        p.clear();
        CHECK(!encode_payload(mover, parse(R"({"hp":-1,"flags":-2})"), rec, p)); // u8 negative
        p.clear();
        CHECK(!encode_payload(mover, parse(R"({"nope":1})"), rec, p)); // unknown field
        p.clear();
        CHECK(!encode_payload(mover, parse(R"({"pos":[1.0,2.0]})"), rec, p)); // wrong lane count
        p.clear();
        CHECK(!encode_payload(mover, parse(R"({"hp":1.5})"), rec, p)); // non-integer for i32
        p.clear();
        CHECK(!encode_payload(mover, parse(R"({"pos":[1e300,0.0,0.0]})"), rec, p)); // f32 overflow → inf
        p.clear();
        CHECK(!encode_payload(mover, parse(R"([])"), rec, p)); // payload not an object
    }

    // --- two component types share the World's archetype storage -------------------------------
    {
        ComponentTypeRegistry reg;
        const RegisteredComponentType& mover = reg.register_type(must_compile(mover_json));
        const RegisteredComponentType& tag = reg.register_type(must_compile(
            R"({"$id":"game:tag","version":1,"fields":[{"name":"id","x-ctx-storage":"u32"}]})"));
        CHECK(mover.component_id != tag.component_id);

        World w;
        const Entity e = w.create();
        void* mrec = reg.add_default(w, e, mover);
        std::vector<std::string> p;
        CHECK(encode_payload(mover, parse(R"({"hp":9})"), mrec, p));
        // Adding the second type migrates the entity to a new archetype; the mover bytes survive.
        void* trec = reg.add_default(w, e, tag);
        CHECK(encode_payload(tag, parse(R"({"id":123})"), trec, p));

        CHECK(member(read_payload(mover, w.get_raw(e, mover.component_id)), "hp")->int_value == 9);
        CHECK(member(read_payload(tag, w.get_raw(e, tag.component_id)), "id")->int_value == 123);

        // Remove one type; the other remains.
        CHECK(w.remove_raw(e, tag.component_id));
        CHECK(!w.has_raw(e, tag.component_id));
        CHECK(w.has_raw(e, mover.component_id));
    }

    // --- engine_component_types(): live but empty in v1 ----------------------------------------
    {
        CHECK(engine_component_types().all().empty());
    }

    COMPONENT_TEST_MAIN_END();
}
