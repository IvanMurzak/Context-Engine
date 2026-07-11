// Tiny zero-dependency test harness + a shared in-memory gateway for the gui/session/undo ctest
// executables (mirrors the sibling src/editor/gui/panels/inspector/tests/inspector_test.h — the repo
// carries no C++ test framework, so each test is a plain executable that CHECK()s its invariants and
// returns non-zero on any failure).

#pragma once

#include "context/editor/gui/panels/inspector/inspector_panel.h"

#include "context/editor/serializer/json_tree.h"

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace undotest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace undotest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            undotest::fail(__FILE__, __LINE__, #cond);                                             \
    } while (false)

#define UNDO_TEST_MAIN_END() return undotest::g_failures == 0 ? 0 : 1

namespace undotest
{

namespace inspector = context::editor::gui::panels::inspector;
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

[[nodiscard]] inline JsonValue jstr(const std::string& s)
{
    JsonValue v;
    v.type = JsonValue::Type::string;
    v.string_value = s;
    return v;
}
[[nodiscard]] inline JsonValue jnum(double d)
{
    JsonValue v;
    v.type = JsonValue::Type::number;
    v.number_value = d;
    return v;
}

// An in-memory override gateway over a single mutable target file: a raw hash + the current composed
// value per pointer. A concurrent writer is simulated by `on_first_attempt` (fired once inside the
// FIRST attempt(), before its CAS check) OR by mutating field_values/file_hash directly between calls
// (the R-QA-010 fault-injection seam). Mirrors the inspector's FakeGateway.
class FakeGateway final : public inspector::OverrideWriteGateway
{
public:
    mutable std::uint64_t file_hash = 100;
    mutable std::map<std::string, JsonValue> field_values;
    mutable int attempts = 0;
    mutable int reads = 0;
    std::function<void()> on_first_attempt;
    mutable bool fired = false;

    inspector::WriteAttempt attempt(const context::editor::compose::WriteRequest& request,
                                    std::uint64_t expected_raw_hash) const override
    {
        ++attempts;
        if (on_first_attempt && !fired)
        {
            fired = true;
            on_first_attempt();
        }
        inspector::WriteAttempt a;
        if (expected_raw_hash == file_hash)
        {
            file_hash += 1; // the write advances the file's raw bytes
            field_values[request.pointer] = request.value;
            a.applied = true;
            a.file = request.root_scene;
            a.pointer = request.pointer;
            a.raw_hash = file_hash;
        }
        else
        {
            a.cas_mismatch = true;
            a.code = "cas.mismatch";
            a.raw_hash = file_hash;
        }
        return a;
    }

    inspector::FieldState read(const std::string&, const std::vector<std::string>&,
                               const std::string& pointer) const override
    {
        ++reads;
        inspector::FieldState s;
        s.present = true;
        s.raw_hash = file_hash;
        const auto it = field_values.find(pointer);
        if (it != field_values.end())
        {
            s.value = it->second;
        }
        return s;
    }
};

} // namespace undotest
