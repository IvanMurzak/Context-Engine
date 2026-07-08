// R-QA-013 tests for the Source Map v3 parser + resolver (source_map.h). Zero-dependency harness
// (the repo carries no C++ test framework — each test is a plain executable that CHECK()s its
// invariants and returns non-zero on any failure), mirroring runtime/js/tests/test_js_engine.cpp.
//
// A LOCAL gate: source_map links no V8 and no esbuild, so it builds + runs under the local
// Strawberry-GCC Windows dev gate. Covers VLQ decoding, the nearest-preceding-segment resolve rule,
// multi-line cumulative deltas, name resolution, and the malformed-input failure paths.
//
// The `mappings` strings below are hand-encoded base64-VLQ so the expected original positions are
// known exactly (each segment's fields are commented). base64 alphabet index:
// A=0 B=1 C=2 D=3 E=4 ... K=10 ... U=20 ... a=26 ... e=30 ...; a VLQ value is zig-zag encoded
// (value*2, sign in the LSB) then base64-split low-5-bits-first with 0x20 as the continuation bit.

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>

#include "context/runtime/ts/source_map.h"

namespace smtest
{
int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace smtest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            smtest::fail(__FILE__, __LINE__, #cond);                                               \
    } while (false)

namespace cts = context::runtime::ts;

namespace
{

// sources=["game.ts"], names=["boom"].
// line 0: seg "AAAAA" = genCol 0, src 0, origLine 0, origCol 0, name 0("boom")
//         seg "IAAU"  = genCol +4 -> 4, src +0, origLine +0 -> 0, origCol +10 -> 10, (no name)
// line 1: seg "EAKI"  = genCol 2 (reset), src +0 -> 0, origLine +5 -> 5, origCol +4 -> 4, (no name)
const char* kMap = R"({"version":3,"sources":["game.ts"],"names":["boom"],)"
                   R"("mappings":"AAAAA,IAAU;EAKI"})";

} // namespace

static void test_parse_and_resolve()
{
    std::string err;
    std::optional<cts::SourceMap> map = cts::SourceMap::parse(kMap, &err);
    CHECK(map.has_value());
    if (!map.has_value())
    {
        std::fprintf(stderr, "parse error: %s\n", err.c_str());
        return;
    }
    CHECK(map->sources().size() == 1);
    CHECK(map->sources().at(0) == "game.ts");
    CHECK(map->names().size() == 1);
    CHECK(map->names().at(0) == "boom");

    // line 0, col 0 -> first segment (origLine 0, origCol 0, name "boom").
    std::optional<cts::OriginalPosition> p = map->resolve(0, 0);
    CHECK(p.has_value());
    if (p.has_value())
    {
        CHECK(p->source == "game.ts");
        CHECK(p->line == 0);
        CHECK(p->column == 0);
        CHECK(p->name == "boom");
    }

    // line 0, col 3 -> STILL the first segment (nearest genCol <= 3 is genCol 0; genCol 4 > 3).
    p = map->resolve(0, 3);
    CHECK(p.has_value());
    if (p.has_value())
    {
        CHECK(p->column == 0);
        CHECK(p->name == "boom");
    }

    // line 0, col 4 -> the second segment (genCol 4 -> origCol 10, no name).
    p = map->resolve(0, 4);
    CHECK(p.has_value());
    if (p.has_value())
    {
        CHECK(p->line == 0);
        CHECK(p->column == 10);
        CHECK(p->name.empty());
    }

    // line 0, col 100 -> beyond the last segment still resolves to the nearest preceding (genCol 4).
    p = map->resolve(0, 100);
    CHECK(p.has_value());
    if (p.has_value())
    {
        CHECK(p->column == 10);
    }

    // line 1, col 2 -> the line-1 segment. origLine/origColumn are cumulative across the WHOLE
    // mappings string (only the generated column resets per line): origLine 0+0+5 = 5; origColumn
    // 0 (seg0) +10 (seg1 on line 0) +4 (this seg) = 14.
    p = map->resolve(1, 2);
    CHECK(p.has_value());
    if (p.has_value())
    {
        CHECK(p->source == "game.ts");
        CHECK(p->line == 5);
        CHECK(p->column == 14);
        CHECK(p->name.empty());
    }
}

static void test_unmapped_positions()
{
    std::optional<cts::SourceMap> map = cts::SourceMap::parse(kMap);
    CHECK(map.has_value());
    if (!map.has_value())
    {
        return;
    }
    // A column before the first segment on the line is unmapped (line 1's first genCol is 2).
    CHECK(!map->resolve(1, 0).has_value());
    CHECK(!map->resolve(1, 1).has_value());
    // A generated line with no mappings row at all.
    CHECK(!map->resolve(2, 0).has_value());
    CHECK(!map->resolve(99, 99).has_value());
}

static void test_empty_mappings()
{
    // An empty mappings string is valid (a map that resolves nothing), not a parse error.
    std::optional<cts::SourceMap> map =
        cts::SourceMap::parse(R"({"version":3,"sources":[],"names":[],"mappings":""})");
    CHECK(map.has_value());
    if (map.has_value())
    {
        CHECK(!map->resolve(0, 0).has_value());
    }
}

static void test_malformed_inputs()
{
    std::string err;
    // Not a JSON object.
    CHECK(!cts::SourceMap::parse("[1,2,3]", &err).has_value());
    CHECK(!err.empty());
    // Unsupported version.
    CHECK(!cts::SourceMap::parse(R"({"version":2,"sources":[],"names":[],"mappings":""})")
               .has_value());
    // Missing the required mappings field.
    CHECK(!cts::SourceMap::parse(R"({"version":3,"sources":[],"names":[]})").has_value());
    // Missing the required version field.
    CHECK(!cts::SourceMap::parse(R"({"sources":[],"names":[],"mappings":""})").has_value());
    // Malformed VLQ in mappings ('!' is not a base64 alphabet char).
    CHECK(!cts::SourceMap::parse(R"({"version":3,"sources":["a"],"names":[],"mappings":"!!!"})")
               .has_value());
    // Truncated JSON.
    CHECK(!cts::SourceMap::parse(R"({"version":3,"sources":["a")").has_value());
    // Non-ascending generated column on a line: seg "U" = genCol +10 -> 10, then seg "N" = genCol
    // -6 -> 4 (still >= 0 but BACKWARD). resolve()'s binary search assumes genCol-ascending, so parse
    // must fail closed on this malformed ordering rather than accept a broken precondition.
    CHECK(!cts::SourceMap::parse(R"({"version":3,"sources":[],"names":[],"mappings":"U,N"})", &err)
               .has_value());
}

static void test_ignores_extra_fields()
{
    // sourcesContent / sourceRoot / file are structurally skipped, not a parse error.
    const char* m = R"({"version":3,"file":"out.js","sourceRoot":"","sources":["a.ts"],)"
                    R"("sourcesContent":["const x = 1;\n"],"names":[],"mappings":"AAAA"})";
    std::optional<cts::SourceMap> map = cts::SourceMap::parse(m);
    CHECK(map.has_value());
    if (map.has_value())
    {
        CHECK(map->sources().size() == 1);
        std::optional<cts::OriginalPosition> p = map->resolve(0, 0);
        CHECK(p.has_value());
        if (p.has_value())
        {
            CHECK(p->source == "a.ts");
            CHECK(p->line == 0);
            CHECK(p->column == 0);
        }
    }
}

static void test_deeply_nested_extra_field_fails_closed()
{
    // A pathologically deep EXTRANEOUS field ("x") must FAIL CLOSED (parse returns nullopt) via the
    // reader's skip-depth guard rather than driving the mutually-recursive skipValue/skipContainer
    // into a native-stack overflow. The four required fields parse first; then "x"'s deeply nested
    // array trips the guard. Without the guard this input would crash the process (stack overflow).
    std::string deep = R"({"version":3,"sources":[],"names":[],"mappings":"","x":)";
    const int depth = 512; // well past kMaxSkipDepth (64)
    deep.append(static_cast<std::size_t>(depth), '[');
    deep.append(static_cast<std::size_t>(depth), ']');
    deep.push_back('}');
    std::string err;
    std::optional<cts::SourceMap> map = cts::SourceMap::parse(deep, &err);
    CHECK(!map.has_value());
    CHECK(!err.empty());
}

static void test_overlong_vlq_fails_closed()
{
    // A VLQ with too many continuation sextets would, at shift 60, left-shift a non-zero 5-bit chunk
    // into the int64 sign bit — signed-overflow UB. The decoder must FAIL CLOSED (parse nullopt).
    // "ggggggggggggI" = 12 continuation sextets (char 'g' = base64 32: continuation bit set, payload
    // 0) followed by a terminal 'I' (base64 8) at shift 60, so the old code's `8 << 60` overflowed.
    std::string err;
    std::optional<cts::SourceMap> map = cts::SourceMap::parse(
        R"({"version":3,"sources":[],"names":[],"mappings":"ggggggggggggI"})", &err);
    CHECK(!map.has_value());
    CHECK(!err.empty());
}

static void test_overlong_int_fails_closed()
{
    // A JSON integer with more digits than int64 can hold would overflow `value * 10 + digit`
    // (signed-overflow UB) in the reader's parseInt. The reader must FAIL CLOSED (parse nullopt),
    // mirroring the sibling VLQ overflow guard. parseInt is reachable both on `version` AND — via
    // skipValue's default branch — on any extraneous numeric field, so exercise both paths.
    const std::string bigNum(40, '9'); // 40 digits — far past int64's ~19-digit range
    std::string err;
    // (a) oversized `version` value.
    std::optional<cts::SourceMap> vmap = cts::SourceMap::parse(
        R"({"version":)" + bigNum + R"(,"sources":[],"names":[],"mappings":""})", &err);
    CHECK(!vmap.has_value());
    CHECK(!err.empty());
    // (b) oversized value on an EXTRANEOUS numeric field (skipped via skipValue -> parseInt). Without
    // the guard this parse would SUCCEED (the overflow is silent UB during the skip); with it, the
    // whole map fails closed.
    err.clear();
    std::optional<cts::SourceMap> xmap = cts::SourceMap::parse(
        R"({"version":3,"x":)" + bigNum + R"(,"sources":[],"names":[],"mappings":""})", &err);
    CHECK(!xmap.has_value());
    CHECK(!err.empty());
}

int main()
{
    test_parse_and_resolve();
    test_unmapped_positions();
    test_empty_mappings();
    test_malformed_inputs();
    test_ignores_extra_fields();
    test_deeply_nested_extra_field_fails_closed();
    test_overlong_vlq_fails_closed();
    test_overlong_int_fails_closed();
    if (smtest::g_failures != 0)
    {
        std::fprintf(stderr, "test_source_map: %d CHECK(s) failed\n", smtest::g_failures);
        return 1;
    }
    std::printf("test_source_map: OK\n");
    return 0;
}
