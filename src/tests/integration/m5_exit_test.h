// Shared harness for the M5 EXIT-GATE integration tests (ROADMAP §1 M5 Exit / issue #168): the tiny
// CHECK() runner (mirrors m1_exit_test.h / m2_exit_test.h — the repo carries no C++ test framework, so
// each gate is a plain executable that CHECK()s its invariants and returns non-zero on any failure),
// plus the cross-cutting helpers the M5 observer-editor walkthrough needs: JSON-value constructors and
// an in-memory OverrideWriteGateway over a single mutable target file.
//
// Unlike M1 (a live daemon over the IPC wire) and M2 (file-level data-model determinism), the M5 exit
// gate drives the HEADLESS editor-GUI panels (uitree/scenetree/inspector/problems/viewport/playbar/
// undo — all CEF-free, testable-by-construction per R-EDIT-001) directly in-process: no CEF, no GPU, no
// daemon, so it runs on the default 3-OS `build` matrix. The per-OS CEF boot smoke + the golden-scene
// SSIM equivalence + the a11y DOM re-scan are the SIBLING CI-job gates (editor-cef-smoke / render /
// render-web) the M5 exit references; this in-process gate proves the observer user-journey seam.

#pragma once

#include "context/editor/gui/panels/inspector/inspector_panel.h" // OverrideWriteGateway seam

#include "context/editor/serializer/canonical.h" // serialize_canonical
#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace m5exit
{

inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

} // namespace m5exit

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            m5exit::fail(__FILE__, __LINE__, #cond);                                               \
    } while (false)

#define M5_EXIT_MAIN_END() return m5exit::g_failures == 0 ? 0 : 1

namespace m5exit
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

[[nodiscard]] inline JsonValue jbool(bool b)
{
    JsonValue v;
    v.type = JsonValue::Type::boolean;
    v.boolean_value = b;
    return v;
}

// Canonical string of a JSON value (for value-equality assertions across the write/undo path).
[[nodiscard]] inline std::string canonical(const JsonValue& v)
{
    std::string s;
    if (!serializer::serialize_canonical(v, s))
    {
        s.clear();
    }
    return s;
}

[[nodiscard]] inline bool value_equal(const JsonValue& a, const JsonValue& b)
{
    return canonical(a) == canonical(b);
}

// An in-memory override-write gateway over a single mutable target file — the `context set` write path
// the F3 inspector commit AND the F7 undo/redo replay both route through (the ONE L-30 engine). A raw
// hash + the current composed value per pointer; a concurrent writer is simulated by `on_first_attempt`
// (fired once inside the FIRST attempt(), before its CAS check). Mirrors the FakeGateway the inspector /
// undo unit tests use, so the walkthrough exercises the SAME seam production wires to the live daemon.
class WalkthroughGateway final : public inspector::OverrideWriteGateway
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
            file_hash += 1; // the write advances the file's raw bytes (a new CAS token)
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

} // namespace m5exit
