// Tiny zero-dependency test harness for the migrate ctest executables (mirrors the sibling
// modules' tests/*_test.h — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure).
//
// Also home of the REFERENCE migration steps: the "test:sprite" v1→v2→v3 chain the unit tests
// exercise and the R-QA-011 fixture corpus (../fixtures/test-sprite/) pins byte-exactly, forever.
// The reference steps are test-registered on purpose — the engine-shipped production set
// (MigrationSet::engine_set()) is EMPTY until the first real engine schema bump.

#pragma once

#include "context/editor/migrate/migration_set.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace migratetest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

using context::editor::migrate::MigrationSet;
using context::editor::migrate::MigrationStep;
using context::editor::migrate::MigrationTier;
using context::editor::serializer::JsonMember;
using context::editor::serializer::JsonValue;

inline JsonValue* member_of(JsonValue& object, std::string_view key)
{
    for (JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// Reference step v1→v2: rename the "tint" member to "color" (a field rename — the classic bump).
// Path map: /tint moves to /color; everything else keeps its place.
inline bool sprite_v1_to_v2(JsonValue& payload)
{
    for (JsonMember& m : payload.members)
        if (m.key == "tint")
        {
            m.key = "color";
            return true;
        }
    return true; // a payload without the field is legal (schema-optional field)
}

// Reference step v2→v3: replace the scalar "size" with an "extent" object {"h": size, "w": size}
// and add an "opacity" default. Path map: /size has NO destination (the orphan-override case);
// everything else keeps its place.
inline bool sprite_v2_to_v3(JsonValue& payload)
{
    for (JsonMember& m : payload.members)
        if (m.key == "size")
        {
            JsonValue extent;
            extent.type = JsonValue::Type::object;
            JsonMember h;
            h.key = "h";
            h.value = m.value;
            JsonMember w;
            w.key = "w";
            w.value = m.value;
            extent.members.push_back(std::move(h));
            extent.members.push_back(std::move(w));
            m.key = "extent";
            m.value = std::move(extent);
            break;
        }
    for (JsonMember& m : payload.members)
        if (m.key == "opacity")
            return true;
    JsonMember opacity;
    opacity.key = "opacity";
    opacity.value.type = JsonValue::Type::integer;
    opacity.value.int_value = 1;
    payload.members.push_back(std::move(opacity));
    return true;
}

// The reference registration the fixture corpus pins: "test:sprite" current version 3, steps
// v1→v2 (revision 1) and v2→v3 (revision 1).
inline void register_reference_steps(MigrationSet& set)
{
    std::string problem;
    bool ok = set.register_component("test:sprite", 3, problem);

    MigrationStep v1;
    v1.component_type = "test:sprite";
    v1.from_version = 1;
    v1.revision = 1;
    v1.transform = sprite_v1_to_v2;
    v1.map_path = [](std::string_view p) -> std::optional<std::string> {
        if (p == "/tint")
            return std::string("/color");
        return std::string(p);
    };
    ok = set.register_step(std::move(v1), problem) && ok;

    MigrationStep v2;
    v2.component_type = "test:sprite";
    v2.from_version = 2;
    v2.revision = 1;
    v2.transform = sprite_v2_to_v3;
    v2.map_path = [](std::string_view p) -> std::optional<std::string> {
        if (p == "/size")
            return std::nullopt; // no destination: overrides addressing it orphan
        return std::string(p);
    };
    ok = set.register_step(std::move(v2), problem) && ok;

    if (!ok)
    {
        std::fprintf(stderr, "reference-step registration failed: %s\n", problem.c_str());
        ++g_failures;
    }
}

// Parse strict JSON into a tree (test inputs are authored valid).
inline JsonValue parse(std::string_view text)
{
    context::editor::serializer::ParseResult parsed = context::editor::serializer::parse_json(text);
    if (!parsed.ok)
    {
        std::fprintf(stderr, "test input did not parse as strict JSON\n");
        ++g_failures;
    }
    return std::move(parsed.root);
}

// Canonical bytes of a tree (total for test trees).
inline std::string canon(const JsonValue& root)
{
    std::string out;
    if (!context::editor::serializer::serialize_canonical(root, out))
    {
        std::fprintf(stderr, "test tree did not serialize canonically\n");
        ++g_failures;
    }
    return out;
}
} // namespace migratetest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            migratetest::fail(__FILE__, __LINE__, #cond);                                          \
    } while (false)

#define MIGRATE_TEST_MAIN_END() return migratetest::g_failures == 0 ? 0 : 1
