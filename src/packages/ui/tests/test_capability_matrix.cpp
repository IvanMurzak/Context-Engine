// The rots-if-broken capability-matrix cross-check (M7 a11 DoD-2, L-53). The published matrix
// (docs/ui-capability-matrix.md) and the live Capabilities structs MUST agree: this ctest parses the
// machine-checked table between the `capability-matrix` markers and asserts every declared per-provider
// value equals what NullProvider and GpuUiProvider actually report — in BOTH directions. A doc/struct
// drift, a renamed/dropped capability, a new Capabilities field with no matrix column, or a
// desktop-vs-web divergence with no struct to back it all fail CI. (The struct-FIELD-COUNT half is pinned
// at compile time by the structured binding in provider_conformance.h::capability_entries; this test
// pins the NAMES + VALUES + doc completeness at run time.)
//
// The doc path is injected by CMake (CONTEXT_UI_CAPABILITY_MATRIX_PATH) — the coverage.manifest.jsonl
// cross-check precedent (src/editor/gui/a11y/tests/test_coverage.cpp). Reads via std::ifstream, so the
// target static-links the GCC runtime (see the ui CMakeLists).

#include "context/packages/ui/null_provider.h"
#include "context/render/ui/provider.h"

#include "provider_conformance.h"
#include "ui_test.h"

#include "render_test_rhi.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace context::packages::ui;

namespace
{

// Trim ASCII whitespace and surrounding markdown backticks from a table cell.
[[nodiscard]] std::string tidy(std::string s)
{
    const char* ws = " \t\r\n`";
    const std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
    {
        return {};
    }
    const std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Split a markdown table row on '|', dropping the empty leading/trailing cells the outer pipes produce.
[[nodiscard]] std::vector<std::string> split_row(const std::string& line)
{
    std::vector<std::string> cells;
    std::string cur;
    for (const char c : line)
    {
        if (c == '|')
        {
            cells.push_back(tidy(cur));
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    cells.push_back(tidy(cur));
    // Drop the empty cells produced by the leading and trailing pipes.
    while (!cells.empty() && cells.front().empty())
    {
        cells.erase(cells.begin());
    }
    while (!cells.empty() && cells.back().empty())
    {
        cells.pop_back();
    }
    return cells;
}

[[nodiscard]] bool is_separator_row(const std::vector<std::string>& cells)
{
    for (const std::string& c : cells)
    {
        if (c.find_first_not_of("-:") != std::string::npos || c.empty())
        {
            return false;
        }
    }
    return !cells.empty();
}

// A parsed capability value; false-on-error carries a message so a bad cell fails loudly.
struct ParsedBool
{
    bool ok = false;
    bool value = false;
};

[[nodiscard]] ParsedBool parse_yes_no(const std::string& cell)
{
    if (cell == "yes")
    {
        return {true, true};
    }
    if (cell == "no")
    {
        return {true, false};
    }
    return {false, false};
}

} // namespace

int main()
{
    // --- the live truth: each in-repo provider's actual capabilities, as name->bool maps -------------
    const Capabilities null_caps = NullProvider{}.capabilities();

    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<context::render::IDevice> device = rhi.create_device();
    CHECK(device != nullptr);
    const Capabilities gpu_caps =
        context::render::ui::GpuUiProvider(*device, context::render::Extent2D{128, 32},
                                           context::render::Color{0.06, 0.07, 0.10, 1.0})
            .capabilities();

    std::map<std::string, bool> live_null;
    std::map<std::string, bool> live_gpu;
    std::set<std::string> field_names; // the canonical Capabilities field set (single source of truth)
    for (const conformance::CapabilityEntry& e : conformance::capability_entries(null_caps))
    {
        live_null[std::string(e.name)] = e.value;
        field_names.insert(std::string(e.name));
    }
    for (const conformance::CapabilityEntry& e : conformance::capability_entries(gpu_caps))
    {
        live_gpu[std::string(e.name)] = e.value;
    }

    // --- parse the published machine-checked table ---------------------------------------------------
    std::ifstream f(CONTEXT_UI_CAPABILITY_MATRIX_PATH, std::ios::binary);
    CHECK(f.good()); // the doc must exist where CMake said it is

    std::string line;
    bool in_table = false;
    bool header_seen = false;
    bool separator_seen = false;
    // Per (platform) -> per (capability) -> declared value, for each provider column.
    std::map<std::string, std::map<std::string, bool>> doc_null;
    std::map<std::string, std::map<std::string, bool>> doc_gpu;
    std::set<std::string> platforms_seen;
    int parse_errors = 0;

    while (std::getline(f, line))
    {
        if (line.find("capability-matrix:begin") != std::string::npos)
        {
            in_table = true;
            continue;
        }
        if (line.find("capability-matrix:end") != std::string::npos)
        {
            in_table = false;
            break;
        }
        if (!in_table)
        {
            continue;
        }
        if (line.find('|') == std::string::npos)
        {
            continue;
        }

        const std::vector<std::string> cells = split_row(line);
        if (!header_seen)
        {
            // Header: Platform | Capability | <provider a> | <provider b>. Pin the column identities so a
            // reorder/rename is caught, not silently mismapped.
            CHECK(cells.size() == 4);
            CHECK(cells[0] == "Platform");
            CHECK(cells[1] == "Capability");
            CHECK(cells[2] == "null");
            CHECK(cells[3] == "engine-integrated");
            header_seen = true;
            continue;
        }
        if (!separator_seen && is_separator_row(cells))
        {
            separator_seen = true;
            continue;
        }

        // A data row: platform | capability | null value | engine-integrated value.
        if (cells.size() != 4)
        {
            std::fprintf(stderr, "ui-capability-matrix: malformed data row (expected 4 cells): %s\n",
                         line.c_str());
            ++parse_errors;
            continue;
        }
        const std::string platform = cells[0];
        const std::string cap = cells[1];
        const ParsedBool nv = parse_yes_no(cells[2]);
        const ParsedBool gv = parse_yes_no(cells[3]);
        if (!nv.ok || !gv.ok)
        {
            std::fprintf(stderr,
                         "ui-capability-matrix: cell for %s/%s is not yes/no (null=%s gpu=%s)\n",
                         platform.c_str(), cap.c_str(), cells[2].c_str(), cells[3].c_str());
            ++parse_errors;
            continue;
        }
        platforms_seen.insert(platform);
        doc_null[platform][cap] = nv.value;
        doc_gpu[platform][cap] = gv.value;
    }

    CHECK(header_seen);
    CHECK(separator_seen);
    CHECK(parse_errors == 0);
    // The v1 published platforms are desktop + web (L-53) — both must be present.
    CHECK(platforms_seen.count("desktop") == 1);
    CHECK(platforms_seen.count("web") == 1);

    // --- cross-check: every declared value equals the live struct, both directions -------------------
    for (const std::string& platform : platforms_seen)
    {
        const std::map<std::string, bool>& dn = doc_null[platform];
        const std::map<std::string, bool>& dg = doc_gpu[platform];

        // Completeness: the capabilities the doc covers for this platform == the canonical field set.
        // A missing row (or a new Capabilities field with no doc column) fails here; a doc-only
        // capability with no struct field fails too.
        std::set<std::string> doc_caps;
        for (const std::pair<const std::string, bool>& kv : dn)
        {
            doc_caps.insert(kv.first);
        }
        if (doc_caps != field_names)
        {
            for (const std::string& n : field_names)
            {
                if (doc_caps.count(n) == 0)
                {
                    std::fprintf(stderr,
                                 "ui-capability-matrix: Capabilities field '%s' has NO row for platform "
                                 "'%s' (add it to docs/ui-capability-matrix.md)\n",
                                 n.c_str(), platform.c_str());
                }
            }
            for (const std::string& n : doc_caps)
            {
                if (field_names.count(n) == 0)
                {
                    std::fprintf(stderr,
                                 "ui-capability-matrix: matrix row '%s' (platform '%s') names no "
                                 "Capabilities field (remove it or fix the struct)\n",
                                 n.c_str(), platform.c_str());
                }
            }
        }
        CHECK(doc_caps == field_names);

        // Value agreement per capability. desktop and web both cross-check against the SAME live struct
        // (v1: a provider reports identical capabilities on both platforms), so a doc-only per-platform
        // divergence with no struct to back it fails here.
        for (const std::pair<const std::string, bool>& kv : dn)
        {
            const std::string& cap = kv.first;
            if (live_null.count(cap) == 1 && kv.second != live_null[cap])
            {
                std::fprintf(stderr,
                             "ui-capability-matrix: null/%s declares %s but NullProvider reports %s\n",
                             cap.c_str(), kv.second ? "yes" : "no", live_null[cap] ? "yes" : "no");
            }
            CHECK(live_null.count(cap) == 1 && kv.second == live_null[cap]);
        }
        for (const std::pair<const std::string, bool>& kv : dg)
        {
            const std::string& cap = kv.first;
            if (live_gpu.count(cap) == 1 && kv.second != live_gpu[cap])
            {
                std::fprintf(
                    stderr,
                    "ui-capability-matrix: engine-integrated/%s declares %s but GpuUiProvider reports "
                    "%s\n",
                    cap.c_str(), kv.second ? "yes" : "no", live_gpu[cap] ? "yes" : "no");
            }
            CHECK(live_gpu.count(cap) == 1 && kv.second == live_gpu[cap]);
        }
    }

    UI_TEST_MAIN_END();
}
