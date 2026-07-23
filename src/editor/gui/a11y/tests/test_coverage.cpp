// Standing a11y coverage-contract guard (R-A11Y-001): the two append-only shared anchors —
// registered_panels() (registry.cpp, the set the harness SCANS) and coverage.manifest.jsonl (the set
// tools/a11y_scan.py DECLARES) — must always name the SAME panel set. A panel added to one without the
// other is a coverage gap (its own reason F4 Problems once shipped uncovered, #168). Registered as the
// `gui-a11y-coverage` ctest, so the CEF-free default 3-OS build matrix enforces the invariant in the
// panel's OWN PR — not a trailing wave, and not only the CEF-gated tools/a11y_scan.py leg.
//
// Deliberately a STANDING contract, not a milestone snapshot: unlike m5-exit-2-a11y-coverage (which
// additionally asserts a hardcoded M5 observer-panel list), this guard hardcodes NO panel names, so it
// never goes stale as panels land in later milestones — it purely asserts the anchors agree.
//
// M9 e05b widened it from TWO anchors to FOUR, because registered_panels() is no longer hand-written
// but DERIVED from the built-in roster (gui/contract/builtin_roster.h):
//
//   roster (builtin_contributions)  ==  factories (panel_factory_ids)
//         ==  scanned (registered_panels)  ==  declared (coverage.manifest.jsonl)
//
// The factory direction is the one the derivation cannot self-enforce: a roster entry with NO factory
// simply does not appear in registered_panels(), so it would be silently unscanned — the exact
// vacuous-coverage shape that let the Problems panel ship uncovered (#168). Asserting all four sets
// equal makes every single-anchor hand-edit a named, actionable failure on the default 3-OS matrix.
//
// M9 e06d PARTITIONS the roster, because a `ContentType::local` panel (Settings — editor-core renders
// it from the e06c kit; there is no C++ model at all) can never have a factory or be scanned here:
//
//   roster  ==  declared (coverage.manifest.jsonl)          <- EVERY shipped panel is declared
//   roster MINUS local  ==  factories  ==  scanned          <- the C++-modelled half, as before
//   every local entry declares `ts-a11y` in its `requires`  <- its gate is the webui-ts-* browser tier
//
// That last line is what keeps the partition from being an escape hatch. Dropping a panel out of the
// C++ scan is otherwise a one-word manifest edit (`"type": "local"`) with no visible consequence —
// exactly the "a gate that checks nothing reads as passed" shape. Requiring the marker means a local
// panel MUST claim the other tier's coverage in the same PR, and the marker is what tools/a11y_scan.py
// keys its own exemption off, so the two tools cannot disagree about which panels it scans.

#include "context/editor/gui/a11y/registry.h"

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/extension.h"

#include "a11y_test.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace context::editor::gui;

namespace
{

// Extract the value of a flat `"<key>": "<value>"` field from one already-trimmed manifest line, or
// an empty string when the line does not carry it. Minimal parser (one flat JSON object per line) —
// enough to cross-check the manifest against the harness registry and the roster without pulling a
// JSON dependency into a11y.
[[nodiscard]] std::string line_field(const std::string& line, const std::string& key)
{
    const std::string quoted = "\"" + key + "\"";
    const std::size_t kpos = line.find(quoted);
    if (kpos == std::string::npos)
    {
        return {};
    }
    const std::size_t colon = line.find(':', kpos + quoted.size());
    if (colon == std::string::npos)
    {
        return {};
    }
    const std::size_t open = line.find('"', colon);
    if (open == std::string::npos)
    {
        return {};
    }
    const std::size_t close = line.find('"', open + 1);
    if (close == std::string::npos)
    {
        return {};
    }
    return line.substr(open + 1, close - open - 1);
}

// id -> title for every non-comment, non-blank line of the JSON-Lines coverage manifest.
[[nodiscard]] std::map<std::string, std::string> manifest_entries(const std::string& path)
{
    std::map<std::string, std::string> entries;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line))
    {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        if (trimmed.empty() || trimmed[0] == '#')
        {
            continue;
        }
        const std::string id = line_field(trimmed, "id");
        if (id.empty())
        {
            continue;
        }
        entries.emplace(id, line_field(trimmed, "title"));
    }
    return entries;
}

// id -> the whole (trimmed) manifest line. Used for membership questions the flat `line_field` reader
// cannot answer — `requires` is an ARRAY, and a substring probe over its own line is both sufficient
// and honest here (the alternative is a JSON dependency in a11y for one containment test).
[[nodiscard]] std::map<std::string, std::string> manifest_lines(const std::string& path)
{
    std::map<std::string, std::string> lines;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line))
    {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        if (trimmed.empty() || trimmed[0] == '#')
        {
            continue;
        }
        const std::string id = line_field(trimmed, "id");
        if (!id.empty())
        {
            lines.emplace(id, trimmed);
        }
    }
    return lines;
}

[[nodiscard]] std::set<std::string> manifest_ids(const std::string& path)
{
    std::set<std::string> ids;
    for (const std::pair<const std::string, std::string>& entry : manifest_entries(path))
    {
        ids.insert(entry.first);
    }
    return ids;
}

} // namespace

int main()
{
    // --- the registry set: every panel registered_panels() exposes, no duplicate ids ---------------
    const std::vector<a11y::RegisteredPanel> registered = a11y::registered_panels();
    std::set<std::string> registered_ids;
    for (const a11y::RegisteredPanel& rp : registered)
    {
        CHECK(registered_ids.insert(rp.id).second); // a duplicate registry id is itself a defect
    }
    CHECK(!registered_ids.empty());

    // --- the manifest set: every panel coverage.manifest.jsonl declares --------------------------
    const std::set<std::string> declared = manifest_ids(CONTEXT_GUI_A11Y_MANIFEST);
    CHECK(!declared.empty());

    // --- the roster set: every contribution the single built-in roster declares (M9 e05b) --------
    std::set<std::string> roster_ids;
    for (const contract::Contribution& c : contract::builtin_contributions())
    {
        CHECK(roster_ids.insert(c.id).second); // a duplicate roster id is itself a defect
    }
    CHECK(!roster_ids.empty());

    // --- the partition: which roster entries CAN have a C++ factory (M9 e06d) --------------------
    // A `local` panel is rendered by editor-core, so it has no C++ model to instantiate or scan. Every
    // other panel must.
    std::set<std::string> local_ids;
    std::set<std::string> cpp_modelled_ids;
    for (const contract::Contribution& c : contract::builtin_contributions())
    {
        if (c.content.type == contract::ContentType::local)
        {
            local_ids.insert(c.id);
        }
        else
        {
            cpp_modelled_ids.insert(c.id);
        }
    }
    CHECK(!cpp_modelled_ids.empty()); // the C++-modelled half is never empty — this gate is not vacuous

    // --- the factory set: every id gui/a11y/registry.cpp can actually instantiate ----------------
    const std::vector<std::string> factory_id_list = a11y::panel_factory_ids();
    std::set<std::string> factory_ids;
    for (const std::string& id : factory_id_list)
    {
        CHECK(factory_ids.insert(id).second); // a duplicate factory binding is itself a defect
    }

    // --- the derivation: registered_panels() IS the roster, in roster order ----------------------
    // (not merely the same set — the scan order is the roster order, so a report diff stays stable)
    {
        std::vector<std::string> roster_order;
        for (const contract::Contribution& c : contract::builtin_contributions())
        {
            if (factory_ids.count(c.id) != 0)
            {
                roster_order.push_back(c.id);
            }
        }
        std::vector<std::string> scanned_order;
        for (const a11y::RegisteredPanel& rp : registered)
        {
            scanned_order.push_back(rp.id);
        }
        CHECK(scanned_order == roster_order);
    }

    // --- the roster <-> factory contract: a roster entry with NO factory is silently unscanned ----
    // This is the direction the derivation cannot enforce by itself (see the file header). Compared
    // against the C++-MODELLED half of the roster since M9 e06d: a `local` panel legitimately has no
    // factory, and demanding one would forbid the content type outright.
    if (cpp_modelled_ids != factory_ids)
    {
        std::vector<std::string> roster_without_factory;
        std::set_difference(cpp_modelled_ids.begin(), cpp_modelled_ids.end(), factory_ids.begin(),
                            factory_ids.end(), std::back_inserter(roster_without_factory));
        std::vector<std::string> factory_without_roster;
        std::set_difference(factory_ids.begin(), factory_ids.end(), cpp_modelled_ids.begin(),
                            cpp_modelled_ids.end(), std::back_inserter(factory_without_roster));
        for (const std::string& id : roster_without_factory)
        {
            std::fprintf(stderr,
                         "gui-a11y-coverage: panel %s is on the built-in roster "
                         "(gui/contract/src/builtin_roster.cpp) but has NO headless factory in "
                         "gui/a11y/src/registry.cpp, so the a11y scan SKIPS it (bind it + link its "
                         "library in gui/a11y/CMakeLists.txt, in the same PR)\n",
                         id.c_str());
        }
        for (const std::string& id : factory_without_roster)
        {
            std::fprintf(stderr,
                         "gui-a11y-coverage: panel %s has a factory in gui/a11y/src/registry.cpp but "
                         "is NOT on the built-in roster (add its Contribution to "
                         "gui/contract/src/builtin_roster.cpp in the same PR)\n",
                         id.c_str());
        }
    }
    CHECK(cpp_modelled_ids == factory_ids);

    // --- the contract: the DECLARED set is every shipped panel -----------------------------------
    // A roster entry with no manifest line (or a manifest line naming no shipped panel) is a coverage
    // gap — surface each side of the difference with an actionable message before the CHECK fails.
    //
    // Compared against the ROSTER since M9 e06d (it was registered_panels()): a local panel is shipped
    // and MUST be declared, but can never be scanned, so the scan stopped being the right yardstick
    // for "declared" the moment the third content type existed.
    if (declared != roster_ids)
    {
        std::vector<std::string> only_roster;
        std::set_difference(roster_ids.begin(), roster_ids.end(), declared.begin(), declared.end(),
                            std::back_inserter(only_roster));
        std::vector<std::string> only_declared;
        std::set_difference(declared.begin(), declared.end(), roster_ids.begin(), roster_ids.end(),
                            std::back_inserter(only_declared));
        for (const std::string& id : only_roster)
        {
            std::fprintf(stderr,
                         "gui-a11y-coverage: panel %s is on the built-in roster but has NO line in "
                         "coverage.manifest.jsonl (add it in the same PR)\n",
                         id.c_str());
        }
        for (const std::string& id : only_declared)
        {
            std::fprintf(stderr,
                         "gui-a11y-coverage: panel %s is declared in coverage.manifest.jsonl but is "
                         "NOT on the built-in roster (add its Contribution, or drop the line)\n",
                         id.c_str());
        }
    }
    CHECK(declared == roster_ids);

    // --- and therefore the anchors are one set, partitioned by WHO RENDERS -----------------------
    // Every shipped panel is declared (including the local ones the scan cannot reach), and the
    // scanned set is exactly the C++-modelled half.
    CHECK(registered_ids == cpp_modelled_ids);

    // --- a local panel must claim the OTHER tier's coverage (M9 e06d) ----------------------------
    // Without this, "drop a panel out of the a11y scan" is a one-word manifest edit with no visible
    // consequence. The marker is also what tools/a11y_scan.py exempts on, so the two tools agree by
    // construction about which panels the C++ harness is expected to scan.
    {
        const std::map<std::string, std::string> lines = manifest_lines(CONTEXT_GUI_A11Y_MANIFEST);
        bool local_coverage_declared = true;
        for (const std::string& id : local_ids)
        {
            const std::map<std::string, std::string>::const_iterator it = lines.find(id);
            if (it == lines.end() || it->second.find("ts-a11y") == std::string::npos)
            {
                local_coverage_declared = false;
                std::fprintf(stderr,
                             "gui-a11y-coverage: panel %s is a content.type=\"local\" panel "
                             "(editor-core renders it), so the C++ harness cannot scan it — its "
                             "coverage.manifest.jsonl line MUST declare \"ts-a11y\" in `requires` "
                             "and its a11y MUST be asserted in the webui-ts-* browser tier "
                             "(core/src/test/settings.test.ts is the worked example)\n",
                             id.c_str());
            }
        }
        CHECK(local_coverage_declared);
    }

    // --- the manifest's TITLE column tracks the roster -------------------------------------------
    // The anchors agree on IDs by construction, but `title` is hand-typed in each of them and nothing
    // compared the copies — so a panel rename left the a11y report and the manifest disagreeing with
    // no gate saying so (M9 e05b hit exactly this and had to hand-sync it). The roster is the source
    // of truth; this ctest already holds both sides, so assert them here rather than adding an anchor.
    {
        const std::map<std::string, std::string> entries =
            manifest_entries(CONTEXT_GUI_A11Y_MANIFEST);
        bool titles_agree = true;
        for (const contract::Contribution& c : contract::builtin_contributions())
        {
            const std::map<std::string, std::string>::const_iterator it = entries.find(c.id);
            if (it == entries.end())
            {
                continue; // a missing line is already reported as an id mismatch above
            }
            if (it->second != c.title)
            {
                titles_agree = false;
                std::fprintf(stderr,
                             "gui-a11y-coverage: panel %s is titled \"%s\" on the built-in roster "
                             "(gui/contract/src/builtin_roster.cpp) but \"%s\" in "
                             "coverage.manifest.jsonl — the roster is the source of truth, update "
                             "the manifest (and the panel's own build_panel() title) to match\n",
                             c.id.c_str(), c.title.c_str(), it->second.c_str());
            }
        }
        CHECK(titles_agree);
    }

    A11Y_TEST_MAIN_END();
}
