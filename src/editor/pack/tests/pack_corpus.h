// Shared loader for the golden pack corpus (tests/corpus/): builds a v1 pack from a `<name>.scenes.json`
// case file. Consumed by both the corpus ctest (byte-compares against the committed `<name>.pack`) and
// the `context_pack_golden_gen` rebaseline tool (writes the goldens). Each consumer is its own
// executable, so the internal-linkage helpers here never clash.
//
// Case file shape:
//   {"root": "<root scene path>", "engineVersion": <u64, optional (default 1)>,
//    "scenes": {"<path>": {<scene doc>}, ...},
//    "sidecars": [{"relpath": "<owner-relative>", "hash": <u64>, "text": "<verbatim bytes>"}, ...]}

#pragma once

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/pack/pack_writer.h"
#include "context/editor/schema/json_access.h"
#include "context/editor/serializer/canonical.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace pack_corpus
{

namespace fs = std::filesystem;
namespace compose = context::editor::compose;
namespace pack = context::editor::pack;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

// A resolver over an in-memory scene map (the corpus case's `scenes` object).
class MapResolver final : public compose::SceneResolver
{
public:
    void add(std::string path, compose::SceneDoc doc) { docs_[std::move(path)] = std::move(doc); }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// The result of building one case: the pack bytes + whether every step succeeded.
struct CaseResult
{
    bool ok = false;
    std::string bytes;
};

[[nodiscard]] inline std::uint64_t read_u64_member(const JsonValue& obj, const char* key,
                                                   std::uint64_t fallback)
{
    const JsonValue* v = schema::find_member(obj, key);
    if (v == nullptr)
        return fallback;
    if (v->type == JsonValue::Type::unsigned_integer)
        return v->uint_value;
    if (v->type == JsonValue::Type::integer)
        return static_cast<std::uint64_t>(v->int_value);
    return fallback;
}

// Build the v1 pack for `scenes_file`. Total: returns {false, ""} on any malformed step so the
// caller (a CHECK-bearing test) reports it rather than crashing.
[[nodiscard]] inline CaseResult build_pack_from_case(const fs::path& scenes_file)
{
    CaseResult out;

    std::ifstream in(scenes_file, std::ios::binary);
    if (!in.good())
        return out;
    std::ostringstream ss;
    ss << in.rdbuf();

    serializer::CanonicalizeResult parsed = serializer::canonicalize(ss.str());
    if (!parsed.is_json)
        return out;
    const JsonValue& root = parsed.root;

    const JsonValue* root_path = schema::find_member(root, "root");
    if (root_path == nullptr || root_path->type != JsonValue::Type::string)
        return out;

    const JsonValue* scenes = schema::find_member(root, "scenes");
    if (scenes == nullptr || scenes->type != JsonValue::Type::object)
        return out;

    MapResolver resolver;
    for (const serializer::JsonMember& m : scenes->members)
    {
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(m.key, m.value);
        if (!doc.has_value())
            return out;
        resolver.add(m.key, std::move(*doc));
    }

    const compose::ComposedScene scene = compose::flatten(root_path->string_value, resolver);
    const compose::ContentUnitSet units = compose::partition_content_units(scene, resolver);

    std::vector<pack::PackSidecar> sidecars;
    if (const JsonValue* arr = schema::find_member(root, "sidecars");
        arr != nullptr && arr->type == JsonValue::Type::array)
    {
        for (const JsonValue& s : arr->elements)
        {
            pack::PackSidecar sc;
            if (const JsonValue* rp = schema::find_member(s, "relpath");
                rp != nullptr && rp->type == JsonValue::Type::string)
                sc.relpath = rp->string_value;
            sc.raw_hash = read_u64_member(s, "hash", 0);
            if (const JsonValue* tx = schema::find_member(s, "text");
                tx != nullptr && tx->type == JsonValue::Type::string)
                sc.bytes = tx->string_value;
            sidecars.push_back(std::move(sc));
        }
    }

    pack::PackWriteOptions options;
    options.engine_version = read_u64_member(root, "engineVersion", pack::kDefaultEngineVersion);

    pack::PackWriteResult written = pack::write_pack(units, scene, sidecars, options);
    if (!written.ok)
        return out;

    out.ok = true;
    out.bytes = std::move(written.bytes);
    return out;
}

// Every `<name>.scenes.json` case under `dir`, sorted for a stable order.
[[nodiscard]] inline std::vector<fs::path> list_cases(const fs::path& dir)
{
    std::vector<fs::path> cases;
    if (!fs::exists(dir))
        return cases;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir))
    {
        const fs::path& p = entry.path();
        if (entry.is_regular_file() && p.extension() == ".json"
            && p.string().find(".scenes.json") != std::string::npos)
            cases.push_back(p);
    }
    std::sort(cases.begin(), cases.end());
    return cases;
}

// The `<name>.pack` golden path for a `<name>.scenes.json` case.
[[nodiscard]] inline fs::path golden_path(const fs::path& scenes_file)
{
    std::string name = scenes_file.filename().string();
    const std::string suffix = ".scenes.json";
    if (name.size() >= suffix.size())
        name.erase(name.size() - suffix.size());
    return scenes_file.parent_path() / (name + ".pack");
}

} // namespace pack_corpus
