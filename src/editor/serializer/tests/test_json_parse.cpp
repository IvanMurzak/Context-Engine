// Strict-JSON parser tests: grammar happy paths, canonical-contract strictness, encoding
// diagnostics, and every failure class (R-QA-013: happy + edge + failure per rule).

#include "context/editor/serializer/json_parse.h"

#include "serializer_test.h"

#include <string>

using context::editor::serializer::JsonValue;
using context::editor::serializer::kMaxParseDepth;
using context::editor::serializer::parse_json;
using context::editor::serializer::ParseResult;

namespace
{

bool has_diag(const ParseResult& r, const std::string& code)
{
    for (const auto& d : r.diagnostics)
    {
        if (d.code == code)
            return true;
    }
    return false;
}

} // namespace

int main()
{
    // Happy path: every value kind, nested.
    {
        const ParseResult r = parse_json(
            R"({"s": "hi", "i": 42, "neg": -7, "f": 1.5, "b": true, "n": null, "a": [1, 2, {}]})");
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
        CHECK(r.root.type == JsonValue::Type::object);
        CHECK(r.root.members.size() == 7);
        CHECK(r.root.members[0].key == "s");
        CHECK(r.root.members[0].value.type == JsonValue::Type::string);
        CHECK(r.root.members[0].value.string_value == "hi");
        CHECK(r.root.members[1].value.type == JsonValue::Type::integer);
        CHECK(r.root.members[1].value.int_value == 42);
        CHECK(r.root.members[2].value.int_value == -7);
        CHECK(r.root.members[3].value.type == JsonValue::Type::number);
        CHECK(r.root.members[3].value.number_value == 1.5);
        CHECK(r.root.members[4].value.type == JsonValue::Type::boolean);
        CHECK(r.root.members[4].value.boolean_value);
        CHECK(r.root.members[5].value.type == JsonValue::Type::null_value);
        CHECK(r.root.members[6].value.type == JsonValue::Type::array);
        CHECK(r.root.members[6].value.elements.size() == 3);
    }

    // Number typing: integer literals are lossless i64/u64; fraction/exponent forms are doubles;
    // integer literals beyond u64 fall through to the ECMAScript double domain (json_parse.h).
    {
        const ParseResult r = parse_json(
            R"([0, -0, 9007199254740993, 9223372036854775807, -9223372036854775808,)"
            R"( 18446744073709551615, 18446744073709551616, 1.0, 1e2])");
        CHECK(r.ok);
        const auto& a = r.root.elements;
        CHECK(a[0].type == JsonValue::Type::integer);
        CHECK(a[0].int_value == 0);
        CHECK(a[1].type == JsonValue::Type::integer); // "-0" is integer zero (-0 -> 0, R-FILE-001)
        CHECK(a[1].int_value == 0);
        CHECK(a[2].type == JsonValue::Type::integer); // 2^53+1 survives losslessly as i64
        CHECK(a[2].int_value == 9007199254740993LL);
        CHECK(a[3].type == JsonValue::Type::integer); // i64 max
        CHECK(a[4].type == JsonValue::Type::integer); // i64 min
        CHECK(a[5].type == JsonValue::Type::unsigned_integer); // u64 max
        CHECK(a[5].uint_value == 18446744073709551615ULL);
        CHECK(a[6].type == JsonValue::Type::number); // 2^64: beyond u64 -> ECMAScript double
        CHECK(a[6].number_value == 18446744073709551616.0);
        CHECK(a[7].type == JsonValue::Type::number); // "1.0" is a double (canonicalizes to "1")
        CHECK(a[7].number_value == 1.0);
        CHECK(a[8].type == JsonValue::Type::number); // exponent form is a double
        CHECK(a[8].number_value == 100.0);
    }

    // Escapes: standard short escapes, \u BMP, and a surrogate PAIR decoding to an astral char.
    // (Non-ASCII bytes are spelled as \x escapes so the test is independent of
    // source-file encoding across the three CI toolchains.)
    {
        const ParseResult r = parse_json(
            "[\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\", "
            "\"\\u00e9\", \"\\ud83d\\ude00\", \"\xC3\xA9\"]");
        CHECK(r.ok);
        CHECK(r.root.elements[0].string_value == "a\"b\\c/d\b\f\n\r\t");
        CHECK(r.root.elements[1].string_value == "\xC3\xA9");         // \u00e9 -> U+00E9 as UTF-8
        CHECK(r.root.elements[2].string_value == "\xF0\x9F\x98\x80"); // surrogate pair -> U+1F600
        CHECK(r.root.elements[3].string_value == "\xC3\xA9");         // raw UTF-8 pass-through
    }

    // NFC at parse (R-FILE-001): a DECOMPOSED sequence (e + combining acute, raw or
    // \u-escaped) lands COMPOSED in the tree.
    {
        const ParseResult raw = parse_json("[\"e\xCC\x81\"]");
        CHECK(raw.ok);
        CHECK(raw.root.elements[0].string_value == "\xC3\xA9"); // U+00E9, not e + U+0301
        const ParseResult escaped = parse_json("[\"e\\u0301\"]");
        CHECK(escaped.ok);
        CHECK(escaped.root.elements[0].string_value == "\xC3\xA9");
    }

    // Encoding diagnostics are NON-FATAL: BOM + CRLF parse fine but are flagged for healing.
    {
        const std::string bom_crlf = "\xEF\xBB\xBF{\r\n  \"a\": 1\r\n}\r\n";
        const ParseResult r = parse_json(bom_crlf);
        CHECK(r.ok);
        CHECK(has_diag(r, "encoding.bom"));
        CHECK(has_diag(r, "encoding.crlf"));
        CHECK(r.root.members.size() == 1);
    }

    // Failure paths, one per strictness rule.
    CHECK(!parse_json("").ok);                            // empty document
    CHECK(!parse_json("   \n\t ").ok);                    // whitespace-only
    CHECK(!parse_json("{\"a\": 1} x").ok);                // trailing content
    CHECK(has_diag(parse_json("{\"a\": 1} x"), "json.trailing_content"));
    CHECK(!parse_json("{\"a\": 1, \"a\": 2}").ok);        // duplicate key
    CHECK(has_diag(parse_json("{\"a\": 1, \"a\": 2}"), "json.duplicate_key"));
    CHECK(!parse_json("{a: 1}").ok);                      // unquoted key
    CHECK(!parse_json("[1, ]").ok);                       // trailing comma
    CHECK(!parse_json("[1 2]").ok);                       // missing comma
    CHECK(!parse_json("\"abc").ok);                       // unterminated string
    CHECK(!parse_json("\"a\nb\"").ok);                    // raw control char in string
    CHECK(has_diag(parse_json("\"a\nb\""), "json.invalid_char"));
    CHECK(!parse_json("\"\\q\"").ok);                     // unknown escape
    CHECK(!parse_json("\"\\ud83d\"").ok);                 // unpaired high surrogate
    CHECK(has_diag(parse_json("\"\\ud83d\""), "json.invalid_escape"));
    CHECK(!parse_json("\"\\ude00\"").ok);                 // unpaired low surrogate
    CHECK(!parse_json("01").ok);                          // leading zero
    CHECK(has_diag(parse_json("01"), "json.invalid_number"));
    CHECK(!parse_json("+1").ok);                          // leading plus is not JSON
    CHECK(!parse_json(".5").ok);                          // missing integer part
    CHECK(!parse_json("5.").ok);                          // missing fraction digits
    CHECK(!parse_json("1e").ok);                          // missing exponent digits
    CHECK(!parse_json("NaN").ok);                         // NaN has no literal (R-FILE-001)
    CHECK(!parse_json("Infinity").ok);                    // Infinity has no literal (R-FILE-001)
    CHECK(!parse_json("-Infinity").ok);
    CHECK(!parse_json("1e999").ok);                       // overflows a double -> would be Infinity
    CHECK(has_diag(parse_json("1e999"), "json.number_out_of_range"));
    CHECK(parse_json("1e-999").ok);                       // underflow rounds to zero: fine
    CHECK(!parse_json("\"\x80\"").ok);                    // invalid UTF-8 lead byte
    CHECK(has_diag(parse_json("\"\x80\""), "encoding.invalid_utf8"));
    CHECK(!parse_json("\"\xC3\"").ok);                    // truncated UTF-8 sequence
    CHECK(!parse_json("\"\xC0\xAF\"").ok);                // overlong encoding
    CHECK(!parse_json("\"\xED\xA0\x80\"").ok);            // encoded surrogate (CESU-8)

    // Depth cap: kMaxParseDepth nests parse; one deeper fails with json.depth_exceeded.
    {
        std::string ok_doc(kMaxParseDepth, '[');
        ok_doc += "1";
        ok_doc.append(kMaxParseDepth, ']');
        CHECK(parse_json(ok_doc).ok);

        std::string deep(kMaxParseDepth + 1, '[');
        deep += "1";
        deep.append(kMaxParseDepth + 1, ']');
        const ParseResult r = parse_json(deep);
        CHECK(!r.ok);
        CHECK(has_diag(r, "json.depth_exceeded"));
    }

    // Diagnostics carry 1-based positions.
    {
        const ParseResult r = parse_json("{\n  \"a\": ?\n}");
        CHECK(!r.ok);
        CHECK(!r.diagnostics.empty());
        CHECK(r.diagnostics[0].line == 2);
        CHECK(r.diagnostics[0].column == 8);
    }

    SERIALIZER_TEST_MAIN_END();
}
