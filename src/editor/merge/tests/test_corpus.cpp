// The R-QA-011 three-way conflict-class corpus: enumerate every committed case on disk, merge it,
// and assert the merged tree + conflict classes/paths match the golden. A new conflict class that
// forgets its fixture still fails (the corpus is a versioned deliverable, not incidental fixtures).

#include "merge_test.h"

#include "context/editor/merge/three_way_merge.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace context::editor::merge;
using mergetest::canon;
using mergetest::parse;
using context::editor::serializer::JsonValue;

namespace
{
namespace fs = std::filesystem;

const JsonValue* member(const JsonValue& v, const char* key)
{
    if (v.type != JsonValue::Type::object)
        return nullptr;
    for (const context::editor::serializer::JsonMember& m : v.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::int64_t as_int(const JsonValue* v)
{
    if (v == nullptr)
        return 0;
    if (v->type == JsonValue::Type::integer)
        return v->int_value;
    if (v->type == JsonValue::Type::unsigned_integer)
        return static_cast<std::int64_t>(v->uint_value);
    return 0;
}

// Build the optional installed-schema floor from expected.json's "floor" object (kind_versions +
// component_versions maps). Absent => empty floor (newer-stamped detection off).
SchemaFloor read_floor(const JsonValue& expected)
{
    SchemaFloor floor;
    const JsonValue* f = member(expected, "floor");
    if (f == nullptr)
        return floor;
    if (const JsonValue* kv = member(*f, "kind_versions"); kv != nullptr)
        for (const auto& m : kv->members)
            floor.kind_versions[m.key] = as_int(&m.value);
    if (const JsonValue* cv = member(*f, "component_versions"); cv != nullptr)
        for (const auto& m : cv->members)
            floor.component_versions[m.key] = as_int(&m.value);
    return floor;
}

// Returns true when the case actually RAN a merge; false when the dir was skipped (no base.json —
// the binary_sidecar case). The caller counts executed cases so a silently-skipped JSON case fails.
bool run_case(const fs::path& dir)
{
    const std::string name = dir.filename().string();

    std::string base_text, ours_text, theirs_text, expected_text;
    // Non-JSON (binary_sidecar) cases carry base.bin and are exercised by the CLI merge test, not
    // here — the structural engine only sees parsed JSON. Skip a dir without base.json.
    if (!read_file(dir / "base.json", base_text))
        return false;
    if (!read_file(dir / "ours.json", ours_text) || !read_file(dir / "theirs.json", theirs_text) ||
        !read_file(dir / "expected.json", expected_text))
    {
        std::fprintf(stderr, "corpus case '%s' is missing an input\n", name.c_str());
        ++mergetest::g_failures;
        return false;
    }

    const JsonValue expected = parse(expected_text);
    MergeOptions opts;
    opts.floor = read_floor(expected);

    const MergeResult result =
        merge_documents(parse(base_text), parse(ours_text), parse(theirs_text), opts);

    // clean flag
    const JsonValue* clean = member(expected, "clean");
    if (clean != nullptr)
    {
        const bool want = clean->type == JsonValue::Type::boolean && clean->boolean_value;
        if (result.clean != want)
        {
            std::fprintf(stderr, "corpus '%s': clean=%d, expected %d\n", name.c_str(),
                         static_cast<int>(result.clean), static_cast<int>(want));
            ++mergetest::g_failures;
        }
    }

    // whole-file flag
    if (const JsonValue* whole = member(expected, "wholeFile"); whole != nullptr)
    {
        const bool want = whole->type == JsonValue::Type::boolean && whole->boolean_value;
        if (result.whole_file != want)
        {
            std::fprintf(stderr, "corpus '%s': wholeFile mismatch\n", name.c_str());
            ++mergetest::g_failures;
        }
    }

    // expected conflicts: every listed {path, class} must appear, and counts must match.
    if (const JsonValue* conflicts = member(expected, "conflicts");
        conflicts != nullptr && conflicts->type == JsonValue::Type::array)
    {
        if (conflicts->elements.size() != result.conflicts.size())
        {
            std::fprintf(stderr, "corpus '%s': %zu conflicts, expected %zu\n", name.c_str(),
                         result.conflicts.size(), conflicts->elements.size());
            ++mergetest::g_failures;
        }
        for (const JsonValue& want : conflicts->elements)
        {
            const JsonValue* wp = member(want, "path");
            const JsonValue* wc = member(want, "class");
            const std::string want_path = wp != nullptr ? wp->string_value : "";
            const std::string want_class = wc != nullptr ? wc->string_value : "";
            bool matched = false;
            for (const Conflict& got : result.conflicts)
                if (got.path == want_path && to_string(got.klass) == want_class)
                    matched = true;
            if (!matched)
            {
                std::fprintf(stderr, "corpus '%s': missing conflict {path='%s', class='%s'}\n",
                             name.c_str(), want_path.c_str(), want_class.c_str());
                ++mergetest::g_failures;
            }
        }
    }

    // golden merged tree (canonical-bytes compare), when the case pins one.
    if (const JsonValue* merged = member(expected, "merged"); merged != nullptr)
    {
        if (canon(result.merged) != canon(*merged))
        {
            std::fprintf(stderr, "corpus '%s': merged tree mismatch\n  got:  %s\n  want: %s\n",
                         name.c_str(), canon(result.merged).c_str(), canon(*merged).c_str());
            ++mergetest::g_failures;
        }
    }
    return true;
}

} // namespace

int main()
{
    const fs::path root(CONTEXT_MERGE_CORPUS_DIR);
    if (!fs::is_directory(root))
    {
        std::fprintf(stderr, "corpus dir not found: %s\n", CONTEXT_MERGE_CORPUS_DIR);
        return 1;
    }

    std::vector<fs::path> cases;
    for (const fs::directory_entry& entry : fs::directory_iterator(root))
        if (entry.is_directory())
            cases.push_back(entry.path());
    std::sort(cases.begin(), cases.end());

    // The corpus must be non-trivial (guards against an empty/renamed fixture dir silently passing).
    CHECK(cases.size() >= 5);

    std::size_t executed = 0;
    for (const fs::path& dir : cases)
        if (run_case(dir))
            ++executed;

    // Every JSON conflict-class case must actually RUN a merge. Counting discovered directories is
    // not enough: a case whose base.json is missing/mis-named skips like the (intentional) binary
    // case and would pass unseen. Assert the executed count against the 7 committed JSON classes
    // (the lone binary-sidecar dir is exercised by the CLI test); bump this floor when a new JSON
    // conflict class + fixture lands.
    CHECK(executed >= 7);

    MERGE_TEST_MAIN_END();
}
