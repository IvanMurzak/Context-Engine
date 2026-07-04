// Cross-implementation test-vector corpus replay (R-QA-011, R-FILE-001): every committed vector's
// input must canonicalize to its expected bytes EXACTLY, and every expected file must be a
// canonicalization fixpoint. The corpus (tests/vectors/) is the versioned deliverable each writer
// implementation — this C++ one, the future TS one — must reproduce byte-for-byte.

#include "context/editor/serializer/canonical.h"

#include "serializer_test.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef SERIALIZER_VECTORS_DIR
#error "SERIALIZER_VECTORS_DIR (path to tests/vectors) must be defined by the build."
#endif

namespace fs = std::filesystem;
using context::editor::serializer::canonicalize;
using context::editor::serializer::CanonicalizeResult;

namespace
{

bool read_bytes(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// On mismatch, print the first differing offset so a corpus regression is debuggable from CI logs.
void report_mismatch(const std::string& name, const std::string& got, const std::string& expected)
{
    std::size_t at = 0;
    const std::size_t bound = std::min(got.size(), expected.size());
    while (at < bound && got[at] == expected[at])
        ++at;
    std::fprintf(stderr,
                 "vector '%s': canonical bytes differ at offset %zu (got %zu bytes, expected %zu)\n",
                 name.c_str(), at, got.size(), expected.size());
}

} // namespace

int main()
{
    const fs::path vectors_dir(SERIALIZER_VECTORS_DIR);
    CHECK(fs::exists(vectors_dir));

    std::vector<fs::path> inputs;
    for (const auto& entry : fs::directory_iterator(vectors_dir))
    {
        const std::string name = entry.path().filename().string();
        if (name.size() > 11 && name.rfind(".input.json") == name.size() - 11)
            inputs.push_back(entry.path());
    }
    std::sort(inputs.begin(), inputs.end());

    // The corpus is load-bearing, not a formality (R-FILE-001): an empty/renamed dir must fail.
    CHECK(inputs.size() >= 12);

    for (const fs::path& input_path : inputs)
    {
        const std::string name = input_path.filename().string();
        const std::string stem = name.substr(0, name.size() - 11); // strip ".input.json"

        std::string input;
        CHECK(read_bytes(input_path, input));

        const fs::path expected_path = vectors_dir / (stem + ".expected.json");
        std::string expected;
        if (!read_bytes(expected_path, expected))
        {
            std::fprintf(stderr, "vector '%s': missing %s\n", stem.c_str(),
                         expected_path.filename().string().c_str());
            CHECK(false);
            continue;
        }

        // 1) The input canonicalizes to the expected bytes, EXACTLY.
        const CanonicalizeResult result = canonicalize(input);
        CHECK(result.is_json);
        if (result.bytes != expected)
        {
            report_mismatch(stem, result.bytes, expected);
            CHECK(result.bytes == expected);
        }

        // 2) The expected bytes are a fixpoint: canonicalizing canonical output is a no-op.
        const CanonicalizeResult again = canonicalize(expected);
        CHECK(again.is_json);
        if (again.bytes != expected)
        {
            report_mismatch(stem + " (fixpoint)", again.bytes, expected);
            CHECK(again.bytes == expected);
        }
        CHECK(again.canonical_hash == result.canonical_hash);

        // 3) Optional expected-diagnostics sidecar: one stable code per line.
        const fs::path diags_path = vectors_dir / (stem + ".diags");
        std::string diags_text;
        if (read_bytes(diags_path, diags_text))
        {
            std::istringstream lines(diags_text);
            std::string code;
            while (std::getline(lines, code))
            {
                while (!code.empty() && (code.back() == '\r' || code.back() == ' '))
                    code.pop_back();
                if (code.empty())
                    continue;
                bool found = false;
                for (const auto& d : result.diagnostics)
                    found = found || d.code == code;
                if (!found)
                    std::fprintf(stderr, "vector '%s': expected diagnostic '%s' not emitted\n",
                                 stem.c_str(), code.c_str());
                CHECK(found);
            }
        }
    }

    SERIALIZER_TEST_MAIN_END();
}
