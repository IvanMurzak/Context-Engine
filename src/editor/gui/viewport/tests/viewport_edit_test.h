// Shared harness for the a19 in-context viewport override-editing unit tests: the tiny CHECK() runner
// (the repo carries no C++ test framework — each test is a plain executable), JSON-value constructors,
// an in-memory compose::WriteResolver over a map of authored scene docs (so the model flattens a
// composed world with NO disk), and an in-memory inspector::OverrideWriteGateway (mirrors the M5
// WalkthroughGateway / inspector FakeGateway) so the L-30 drop/rebase paths are exercised headless.

#pragma once

#include "context/editor/gui/panels/inspector/inspector_panel.h" // OverrideWriteGateway

#include "context/editor/compose/compose_write.h" // WriteResolver
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vpedit
{

inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

} // namespace vpedit

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            vpedit::fail(__FILE__, __LINE__, #cond);                                               \
    } while (false)

#define VPEDIT_TEST_MAIN_END() return vpedit::g_failures == 0 ? 0 : 1

namespace vpedit
{

namespace compose = context::editor::compose;
namespace inspector = context::editor::gui::panels::inspector;
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

// Parse a JSON literal into a canonical value tree (the same path the CLI + files take).
[[nodiscard]] inline JsonValue jparse(const std::string& text)
{
    return serializer::canonicalize(text).root;
}

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

// An in-memory composed-write resolver over authored scene docs supplied as {path -> JSON text}. It
// canonicalizes + composition-parses each once, so compose::flatten / plan_write drive the exact same
// SceneDoc + raw tree the disk resolver would build — with no filesystem.
class MapResolver final : public compose::WriteResolver
{
public:
    void add(const std::string& path, const std::string& json_text)
    {
        serializer::CanonicalizeResult canon = serializer::canonicalize(json_text);
        Entry entry;
        if (canon.is_json)
        {
            if (std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, canon.root))
            {
                entry.has_doc = true;
                entry.doc = std::move(*doc);
            }
            entry.has_tree = true;
            entry.tree = std::move(canon.root);
        }
        docs_.emplace(path, std::move(entry));
    }

    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        const auto it = docs_.find(std::string(path));
        return (it != docs_.end() && it->second.has_doc) ? &it->second.doc : nullptr;
    }
    [[nodiscard]] const serializer::JsonValue* tree(std::string_view path) const override
    {
        const auto it = docs_.find(std::string(path));
        return (it != docs_.end() && it->second.has_tree) ? &it->second.tree : nullptr;
    }

private:
    struct Entry
    {
        bool has_tree = false;
        serializer::JsonValue tree;
        bool has_doc = false;
        compose::SceneDoc doc;
    };
    std::map<std::string, Entry, std::less<>> docs_;
};

// An in-memory override-write gateway over a single mutable target file — the `context set` write path
// the a19 viewport commit routes through (the ONE L-30 engine). A raw hash + the current composed value
// per pointer; a concurrent writer is simulated by `on_first_attempt` (fired once inside the FIRST
// attempt(), before its CAS check). `expected_raw_hash == 0` means "no CAS guard" (the retarget path,
// exactly like the disk gateway + `context set` without --if-match). Mirrors the M5 WalkthroughGateway.
class FakeGateway final : public inspector::OverrideWriteGateway
{
public:
    mutable std::uint64_t file_hash = 100;
    mutable std::map<std::string, JsonValue> field_values;
    mutable int attempts = 0;
    mutable int reads = 0;
    mutable compose::WriteRequest last_request; // the request of the LAST attempt (target assertions)
    std::function<void()> on_first_attempt;
    mutable bool fired = false;

    inspector::WriteAttempt attempt(const compose::WriteRequest& request,
                                    std::uint64_t expected_raw_hash) const override
    {
        ++attempts;
        last_request = request;
        if (on_first_attempt && !fired)
        {
            fired = true;
            on_first_attempt();
        }
        inspector::WriteAttempt a;
        if (expected_raw_hash == 0 || expected_raw_hash == file_hash)
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

// A minimal composed world: `root.scene.json` instances `tmpl.scene.json` (one Torch entity) and
// overrides the instanced Torch's position at the OUTERMOST level — the a19 subject (an instanced
// entity whose composed value has a template contributor AND an outermost override).
constexpr const char* kInst = "cccccccccccccc01";
constexpr const char* kTorch = "aaaaaaaaaaaaaa02";
constexpr const char* kRootScene = "root.scene.json";
constexpr const char* kTmplScene = "tmpl.scene.json";

[[nodiscard]] inline MapResolver make_world()
{
    MapResolver r;
    r.add(kTmplScene, R"({
        "$schema": "ctx:scene",
        "version": 1,
        "entities": [
            {"id": "aaaaaaaaaaaaaa01", "name": "Floor",
             "components": {"transform": {"position": [0, 0, 0]}}},
            {"id": "aaaaaaaaaaaaaa02", "name": "Torch",
             "components": {"transform": {"position": [1, 0, 0]}}}
        ]
    })");
    r.add(kRootScene, R"({
        "$schema": "ctx:scene",
        "version": 1,
        "entities": [
            {"id": "bbbbbbbbbbbbbb01", "name": "Camera",
             "components": {"transform": {"position": [0, 1, -5]}}}
        ],
        "instances": [{"id": "cccccccccccccc01", "scene": "tmpl.scene.json"}],
        "overrides": [
            {"path": ["cccccccccccccc01", "aaaaaaaaaaaaaa02"],
             "pointer": "/components/transform/position",
             "base": [1, 0, 0], "value": [5, 0, 0]}
        ]
    })");
    return r;
}

// The instanced Torch's composed identity (id-path joined with '/').
[[nodiscard]] inline std::string torch_identity()
{
    return std::string(kInst) + "/" + kTorch;
}

} // namespace vpedit
