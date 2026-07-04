// The committed composition edge-case corpus (R-QA-011: test corpora are versioned deliverables —
// this task STARTS the composition corpus): deep nesting, dense fan-in, cycles, orphan overrides,
// structural ops, id-space violations, and the entity budget. Each case is one JSON file under
// tests/corpus/: an in-memory scene set + the exact expected composed id-paths and diagnostic
// codes. Add a case file and this runner picks it up — no code change.

#include "context/editor/compose/flatten.h"

#include "context/editor/schema/json_access.h"
#include "context/editor/serializer/canonical.h"

#include "compose_test.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace context::editor::compose;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;
using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

class CaseResolver final : public SceneResolver
{
public:
    void add(std::string path, SceneDoc doc) { docs_[std::move(path)] = std::move(doc); }
    [[nodiscard]] const SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, SceneDoc> docs_;
};

[[nodiscard]] std::string joined(const std::vector<std::string>& segments)
{
    std::string out;
    for (const std::string& seg : segments)
    {
        if (!out.empty())
            out.push_back('/');
        out += seg;
    }
    return out;
}

void run_case(const fs::path& file)
{
    const int failures_before = cmptest::g_failures;

    std::ifstream in(file, std::ios::binary);
    CHECK(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    serializer::CanonicalizeResult parsed = serializer::canonicalize(ss.str());
    CHECK(parsed.is_json);
    const JsonValue& root = parsed.root;

    const JsonValue* root_path = schema::find_member(root, "root");
    CHECK(root_path != nullptr && root_path->type == JsonValue::Type::string);

    ComposeLimits limits;
    if (const JsonValue* l = schema::find_member(root, "limits"); l != nullptr)
    {
        if (const JsonValue* v = schema::find_member(*l, "maxDepth"); v != nullptr)
            limits.max_depth = static_cast<std::size_t>(v->int_value);
        if (const JsonValue* v = schema::find_member(*l, "fanInThreshold"); v != nullptr)
            limits.fan_in_threshold = static_cast<std::size_t>(v->int_value);
        if (const JsonValue* v = schema::find_member(*l, "maxEntities"); v != nullptr)
            limits.max_entities = static_cast<std::size_t>(v->int_value);
    }

    CaseResolver resolver;
    const JsonValue* scenes = schema::find_member(root, "scenes");
    CHECK(scenes != nullptr && scenes->type == JsonValue::Type::object);
    for (const JsonMember& m : scenes->members)
    {
        std::optional<SceneDoc> doc = build_scene_doc(m.key, m.value);
        CHECK(doc.has_value());
        if (doc.has_value())
            resolver.add(m.key, std::move(*doc));
    }

    const ComposedScene out = flatten(root_path->string_value, resolver, limits);

    const JsonValue* expect = schema::find_member(root, "expect");
    CHECK(expect != nullptr);
    const JsonValue* expect_ok = schema::find_member(*expect, "ok");
    CHECK(expect_ok != nullptr && expect_ok->type == JsonValue::Type::boolean);
    CHECK(out.ok == expect_ok->boolean_value);

    // Composed id-paths: exact set match (order-insensitive — expansion order is pinned by the
    // unit tests; the corpus pins WHAT composed).
    std::vector<std::string> actual_paths;
    actual_paths.reserve(out.entities.size());
    for (const ComposedEntity& e : out.entities)
        actual_paths.push_back(joined(e.id_path));
    std::vector<std::string> expected_paths;
    const JsonValue* id_paths = schema::find_member(*expect, "idPaths");
    CHECK(id_paths != nullptr && id_paths->type == JsonValue::Type::array);
    for (const JsonValue& p : id_paths->elements)
    {
        CHECK(p.type == JsonValue::Type::array);
        std::vector<std::string> segments;
        for (const JsonValue& seg : p.elements)
            segments.push_back(seg.string_value);
        expected_paths.push_back(joined(segments));
    }
    std::sort(actual_paths.begin(), actual_paths.end());
    std::sort(expected_paths.begin(), expected_paths.end());
    CHECK(actual_paths == expected_paths);

    // Diagnostic codes: exact multiset match — a case lists EVERY code its flatten emits.
    std::vector<std::string> actual_codes;
    actual_codes.reserve(out.diagnostics.size());
    for (const ComposeDiagnostic& d : out.diagnostics)
        actual_codes.push_back(d.code);
    std::vector<std::string> expected_codes;
    const JsonValue* diagnostics = schema::find_member(*expect, "diagnostics");
    CHECK(diagnostics != nullptr && diagnostics->type == JsonValue::Type::array);
    for (const JsonValue& c : diagnostics->elements)
        expected_codes.push_back(c.string_value);
    std::sort(actual_codes.begin(), actual_codes.end());
    std::sort(expected_codes.begin(), expected_codes.end());
    CHECK(actual_codes == expected_codes);

    if (cmptest::g_failures != failures_before)
    {
        std::fprintf(stderr, "corpus case FAILED: %s\n", file.filename().string().c_str());
        std::fprintf(stderr, "  composed:");
        for (const std::string& p : actual_paths)
            std::fprintf(stderr, " [%s]", p.c_str());
        std::fprintf(stderr, "\n  diagnostics:");
        for (const std::string& c : actual_codes)
            std::fprintf(stderr, " %s", c.c_str());
        std::fprintf(stderr, "\n");
    }
}

} // namespace

int main()
{
    const fs::path corpus_dir = COMPOSE_CORPUS_DIR;
    CHECK(fs::exists(corpus_dir));

    std::vector<fs::path> cases;
    for (const fs::directory_entry& entry : fs::directory_iterator(corpus_dir))
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            cases.push_back(entry.path());
    std::sort(cases.begin(), cases.end());

    // The corpus is a versioned deliverable — a silently-empty directory must fail, not pass.
    CHECK(cases.size() >= 11);
    for (const fs::path& file : cases)
        run_case(file);

    COMPOSE_TEST_MAIN_END();
}
