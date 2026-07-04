// The R-DATA-006 vocabulary: every engine semantic type accepts its pinned shape and rejects each
// invalid class; the units LAW admits SI+radians only; storage layouts and union tags follow their
// pinned grammars. (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/schema/vocabulary.h"
#include "context/editor/serializer/json_parse.h"

#include "schema_test.h"

#include <string>
#include <string_view>

using namespace context::editor::schema;
using context::editor::serializer::JsonValue;
using context::editor::serializer::parse_json;

namespace
{
JsonValue parsed(std::string_view text)
{
    auto result = parse_json(text);
    CHECK(result.ok);
    return result.root;
}

bool valid(SemanticType type, std::string_view text)
{
    return check_semantic(type, parsed(text)).empty();
}
} // namespace

int main()
{
    // --- ids -----------------------------------------------------------------------------------
    CHECK(is_semantic_type_id("quaternion"));
    CHECK(is_semantic_type_id("color"));
    CHECK(is_semantic_type_id("curve"));
    CHECK(is_semantic_type_id("gradient"));
    CHECK(is_semantic_type_id("bit-flags"));
    CHECK(!is_semantic_type_id("euler"));      // rotations are quaternions, not a second encoding
    CHECK(!is_semantic_type_id("bitflags"));   // the pinned id is hyphenated
    CHECK(!is_semantic_type_id(""));
    CHECK(semantic_type_from_id("gradient") == SemanticType::gradient);
    CHECK(semantic_type_id(SemanticType::bit_flags) == "bit-flags");

    // --- quaternion ------------------------------------------------------------------------------
    CHECK(valid(SemanticType::quaternion, "[0, 0, 0, 1]"));
    CHECK(valid(SemanticType::quaternion, "[0.5, -0.5, 0.5, 0.5]"));
    CHECK(!valid(SemanticType::quaternion, "[0, 0, 1]"));          // wrong arity
    CHECK(!valid(SemanticType::quaternion, "[0, 0, 0, 1, 0]"));    // wrong arity
    CHECK(!valid(SemanticType::quaternion, "[0, 0, 0, \"1\"]"));   // non-number component
    CHECK(!valid(SemanticType::quaternion, "{\"x\":0,\"y\":0,\"z\":0,\"w\":1}")); // wrong carrier
    {
        // The issue pointer names the offending component.
        auto issues = check_semantic(SemanticType::quaternion, parsed("[0, 0, 0, null]"));
        CHECK(issues.size() == 1);
        CHECK(issues[0].subpointer == "/3");
    }

    // --- color (declared color space is mandatory) ----------------------------------------------
    CHECK(valid(SemanticType::color, "{\"space\": \"srgb\", \"value\": [1, 0, 0]}"));
    CHECK(valid(SemanticType::color, "{\"space\": \"srgb-linear\", \"value\": [1, 0, 0, 0.5]}"));
    CHECK(!valid(SemanticType::color, "{\"value\": [1, 0, 0]}"));            // NO declared space
    CHECK(!valid(SemanticType::color, "{\"space\": \"bt601\", \"value\": [1, 0, 0]}")); // unknown
    CHECK(!valid(SemanticType::color, "{\"space\": \"srgb\", \"value\": [1, 0]}")); // 2 components
    CHECK(!valid(SemanticType::color, "{\"space\": \"srgb\"}"));             // no components
    CHECK(!valid(SemanticType::color, "[1, 0, 0]"));                          // wrong carrier
    CHECK(!valid(SemanticType::color, "{\"space\": \"srgb\", \"value\": [1, 0, \"0\"]}"));

    // --- curve -----------------------------------------------------------------------------------
    CHECK(valid(SemanticType::curve, "{\"keys\": [{\"t\": 0, \"v\": 1}]}"));
    CHECK(valid(SemanticType::curve, "{\"keys\": [{\"t\": 0, \"v\": 1}, {\"t\": 0.5, \"v\": 2}]}"));
    CHECK(!valid(SemanticType::curve, "{\"keys\": []}"));                    // empty
    CHECK(!valid(SemanticType::curve, "{}"));                                 // no keys
    CHECK(!valid(SemanticType::curve, "{\"keys\": [{\"t\": 0}]}"));          // missing v
    CHECK(!valid(SemanticType::curve, "{\"keys\": [{\"v\": 0}]}"));          // missing t
    CHECK(!valid(SemanticType::curve,
                 "{\"keys\": [{\"t\": 1, \"v\": 0}, {\"t\": 1, \"v\": 2}]}")); // duplicate t
    CHECK(!valid(SemanticType::curve,
                 "{\"keys\": [{\"t\": 2, \"v\": 0}, {\"t\": 1, \"v\": 0}]}")); // decreasing t

    // --- gradient --------------------------------------------------------------------------------
    const char* stop_ok = "{\"t\": 0, \"color\": {\"space\": \"srgb\", \"value\": [0, 0, 0]}}";
    CHECK(valid(SemanticType::gradient, std::string("{\"stops\": [") + stop_ok + "]}"));
    CHECK(valid(SemanticType::gradient,
                "{\"stops\": [{\"t\": 0, \"color\": {\"space\": \"srgb\", \"value\": [0,0,0]}},"
                " {\"t\": 0, \"color\": {\"space\": \"srgb\", \"value\": [1,1,1]}}]}")); // hard step
    CHECK(!valid(SemanticType::gradient, "{\"stops\": []}"));
    CHECK(!valid(SemanticType::gradient,
                 "{\"stops\": [{\"t\": 1.5, \"color\": {\"space\": \"srgb\", \"value\": [0,0,0]}}]}"));
    CHECK(!valid(SemanticType::gradient,
                 "{\"stops\": [{\"t\": 0.5, \"color\": {\"space\": \"srgb\", \"value\": [0,0,0]}},"
                 " {\"t\": 0.25, \"color\": {\"space\": \"srgb\", \"value\": [0,0,0]}}]}")); // order
    CHECK(!valid(SemanticType::gradient, "{\"stops\": [{\"t\": 0, \"color\": [0, 0, 0]}]}"));
    {
        // A bad stop color reports THROUGH the composite pointer.
        auto issues = check_semantic(
            SemanticType::gradient,
            parsed("{\"stops\": [{\"t\": 0, \"color\": {\"value\": [0,0,0]}}]}"));
        CHECK(issues.size() == 1);
        CHECK(issues[0].subpointer == "/stops/0/color");
    }

    // --- bit-flags -------------------------------------------------------------------------------
    CHECK(valid(SemanticType::bit_flags, "[]")); // the empty set is a set
    CHECK(valid(SemanticType::bit_flags, "[\"kinematic\", \"visible\"]"));
    CHECK(!valid(SemanticType::bit_flags, "[\"a\", \"a\"]"));   // duplicates
    CHECK(!valid(SemanticType::bit_flags, "[\"a\", 1]"));       // non-string
    CHECK(!valid(SemanticType::bit_flags, "[\"\"]"));           // empty name
    CHECK(!valid(SemanticType::bit_flags, "3"));                // wrong carrier

    // --- the units LAW ---------------------------------------------------------------------------
    CHECK(is_si_unit("1"));
    CHECK(is_si_unit("m"));
    CHECK(is_si_unit("rad"));
    CHECK(is_si_unit("m/s"));
    CHECK(is_si_unit("rad/s"));
    CHECK(is_si_unit("kg/m^3"));
    CHECK(!is_si_unit("deg"));       // the classic silent-corruption unit — outlawed
    CHECK(!is_si_unit("degrees"));
    CHECK(!is_si_unit("ft"));
    CHECK(!is_si_unit("ms"));        // seconds only — no scaled variants
    CHECK(!is_si_unit("M"));         // case-sensitive
    CHECK(!is_si_unit(""));

    // --- storage layouts -------------------------------------------------------------------------
    CHECK(is_storage_layout("f32"));
    CHECK(is_storage_layout("u8"));
    CHECK(is_storage_layout("i64"));
    CHECK(is_storage_layout("f32x3"));
    CHECK(is_storage_layout("f32x4"));
    CHECK(is_storage_layout("f64x16")); // 4x4 matrix
    CHECK(!is_storage_layout("f32x5")); // no 5-lane layout
    CHECK(!is_storage_layout("f16"));   // no half base in the pinned set
    CHECK(!is_storage_layout("x3"));
    CHECK(!is_storage_layout("f32x"));
    CHECK(!is_storage_layout(""));

    // --- the pinned tagged-union convention ------------------------------------------------------
    CHECK(is_union_tag("shape:circle"));
    CHECK(is_union_tag("physics2d:box-collider"));
    CHECK(is_union_tag("a:b_c"));
    CHECK(!is_union_tag("circle"));        // no namespace — the ad-hoc encoding the law forbids
    CHECK(!is_union_tag("Shape:circle"));  // lowercase grammar
    CHECK(!is_union_tag("shape:"));
    CHECK(!is_union_tag(":circle"));
    CHECK(!is_union_tag("a:b:c"));
    CHECK(!is_union_tag("9a:b"));

    // --- color spaces ----------------------------------------------------------------------------
    CHECK(is_color_space("srgb"));
    CHECK(is_color_space("rec2020"));
    CHECK(!is_color_space("SRGB"));
    CHECK(!is_color_space("yuv"));

    // --- notes -----------------------------------------------------------------------------------
    CHECK(is_valid_notes(parsed("\"a note\"")));
    CHECK(is_valid_notes(parsed("[\"a\", \"b\"]")));
    CHECK(is_valid_notes(parsed("[]")));
    CHECK(!is_valid_notes(parsed("42")));
    CHECK(!is_valid_notes(parsed("[\"a\", 42]")));
    CHECK(!is_valid_notes(parsed("{\"text\": \"a\"}")));

    SCHEMA_TEST_MAIN_END();
}
