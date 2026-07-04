// RFC 6901 JSON-pointer resolution + override application over the serializer tree.
// (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/compose/json_pointer.h"

#include "context/editor/serializer/canonical.h"

#include "compose_test.h"

#include <string>
#include <vector>

using namespace context::editor::compose;
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

namespace
{

[[nodiscard]] JsonValue parse(const char* json)
{
    serializer::CanonicalizeResult r = serializer::canonicalize(json);
    CHECK(r.is_json);
    return r.root;
}

[[nodiscard]] std::string canonical(const JsonValue& v)
{
    std::string out;
    CHECK(serializer::serialize_canonical(v, out));
    return out;
}

} // namespace

int main()
{
    // --- parse_json_pointer ------------------------------------------------------------------------
    {
        std::vector<std::string> tokens;
        CHECK(parse_json_pointer("/a/b/c", tokens));
        CHECK(tokens.size() == 3 && tokens[0] == "a" && tokens[2] == "c");
        CHECK(parse_json_pointer("/a~0b/c~1d", tokens)); // ~0 -> '~', ~1 -> '/'
        CHECK(tokens.size() == 2 && tokens[0] == "a~b" && tokens[1] == "c/d");
        CHECK(parse_json_pointer("/", tokens)); // one empty token — valid per RFC 6901
        CHECK(tokens.size() == 1 && tokens[0].empty());

        CHECK(!parse_json_pointer("", tokens));     // whole-document override is a modeling error
        CHECK(!parse_json_pointer("a/b", tokens));  // must start with '/'
        CHECK(!parse_json_pointer("/a~2", tokens)); // invalid escape
        CHECK(!parse_json_pointer("/a~", tokens));  // truncated escape
    }

    const char* kDoc = R"({
      "name": "Light",
      "components": {
        "transform": {"position": [0, 1, 2]},
        "camera": {"fov": 1.0}
      }
    })";

    // --- resolve_json_pointer ----------------------------------------------------------------------
    {
        JsonValue doc = parse(kDoc);
        const JsonValue* name = resolve_json_pointer(doc, "/name");
        CHECK(name != nullptr && name->string_value == "Light");
        const JsonValue* y = resolve_json_pointer(doc, "/components/transform/position/1");
        CHECK(y != nullptr && y->type == JsonValue::Type::integer && y->int_value == 1);

        CHECK(resolve_json_pointer(doc, "/absent") == nullptr);
        CHECK(resolve_json_pointer(doc, "/components/transform/position/3") == nullptr);  // range
        CHECK(resolve_json_pointer(doc, "/components/transform/position/-") == nullptr);  // append
        CHECK(resolve_json_pointer(doc, "/components/transform/position/01") == nullptr); // 0-pad
        CHECK(resolve_json_pointer(doc, "/components/transform/position/1x") == nullptr);
        CHECK(resolve_json_pointer(doc, "/name/anything") == nullptr); // scalar mid-path
        CHECK(resolve_json_pointer(doc, "bad") == nullptr);            // malformed pointer
    }

    // --- set_json_pointer: replacement + creation --------------------------------------------------
    {
        JsonValue doc = parse(kDoc);
        JsonValue v = parse(R"({"v": [9, 9, 9]})");
        const JsonValue* nine = resolve_json_pointer(v, "/v");
        CHECK(nine != nullptr);

        CHECK(set_json_pointer(doc, "/components/transform/position", *nine)); // replace a leaf
        const JsonValue* pos = resolve_json_pointer(doc, "/components/transform/position/0");
        CHECK(pos != nullptr && pos->int_value == 9);

        JsonValue fov = parse(R"({"v": 2.5})");
        CHECK(set_json_pointer(doc, "/components/camera/fov",
                               *resolve_json_pointer(fov, "/v"))); // replace a scalar
        CHECK(resolve_json_pointer(doc, "/components/camera/fov")->number_value == 2.5);

        // Creation: an override may introduce a new field / component (intermediates are objects).
        JsonValue flag = parse(R"({"v": true})");
        CHECK(set_json_pointer(doc, "/components/light/enabled",
                               *resolve_json_pointer(flag, "/v")));
        const JsonValue* enabled = resolve_json_pointer(doc, "/components/light/enabled");
        CHECK(enabled != nullptr && enabled->boolean_value);

        // Array element replacement in place.
        JsonValue five = parse(R"({"v": 5})");
        CHECK(set_json_pointer(doc, "/components/transform/position/2",
                               *resolve_json_pointer(five, "/v")));
        CHECK(resolve_json_pointer(doc, "/components/transform/position/2")->int_value == 5);
    }

    // --- set_json_pointer: failure paths leave the tree untouched ---------------------------------
    {
        JsonValue doc = parse(kDoc);
        const std::string before = canonical(doc);
        JsonValue v = parse(R"({"v": 1})");
        const JsonValue& one = *resolve_json_pointer(v, "/v");

        CHECK(!set_json_pointer(doc, "/components/transform/position/9", one)); // array growth
        CHECK(!set_json_pointer(doc, "/components/transform/position/-", one)); // append token
        CHECK(!set_json_pointer(doc, "/name/deep", one));                       // scalar mid-path
        CHECK(!set_json_pointer(doc, "", one));                                 // whole-document
        CHECK(!set_json_pointer(doc, "no-slash", one));                         // malformed
        // A failing set must not have half-created intermediate members (the probe pass).
        CHECK(canonical(doc) == before);
    }

    COMPOSE_TEST_MAIN_END();
}
