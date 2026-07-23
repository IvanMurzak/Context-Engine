// M5 exit criterion 2 — the A11Y + KEYBOARD-ONLY-NAV + COVERAGE-MANIFEST gate (ROADMAP §1 M5 Exit /
// issue #168; R-A11Y-001 / R-EDIT-001), registered as the blocking `m5-exit-2-a11y-coverage` ctest and
// run by the CI build job's "M5 exit gate" named step on all three build-matrix legs (headless, no CEF).
//
// The M5 exit requires "a11y scan + keyboard-only nav green on all 7 panels (coverage manifest
// complete)". This gate asserts, at milestone-exit strength and in-process:
//   1. COVERAGE COMPLETE — the a11y registry (registered_panels(), which the harness scans) and the
//      coverage.manifest.jsonl (the DATA half tools/a11y_scan.py cross-checks) declare the SAME panel
//      set, and that set covers every M5 shipped OBSERVER panel: the placeholder + F1 viewport + F2
//      scene-tree + F3 inspector + F4 Problems + F5 playbar. (Historically F4 Problems was landed but
//      NOT registered — this exit closes that gap; a regression that drops a panel from EITHER side
//      turns this gate red, the C++ belt to tools/a11y_scan.py's DOM-side braces.)
//   2. EVERY registered panel is a11y-CLEAN (uitree::audit_a11y returns no violations — semantic/ARIA
//      names, unique ids) AND KEYBOARD-NAVIGABLE (audit_a11y also proves every exposed command is
//      reachable from a focusable node, i.e. complete keyboard-only navigation — no pointer required).
//   3. The two F1-F7 surfaces that are NOT observer panels are covered too: F6 is the a11y harness
//      ITSELF (it scans the others; asserted by scanning through it here) and F7 is GUI session
//      undo/redo, whose command panel is a11y-clean + keyboard-navigable when an action is available.
//
// The per-OS CEF-rendered DOM re-scan (tools/a11y_scan.py over the axe-class HTML + the manifest
// cross-check) is the SIBLING editor-cef-smoke CI-job gate; this in-process gate is the headless,
// 3-OS-matrix half that needs no CEF.

#include "context/editor/gui/a11y/harness.h"
#include "context/editor/gui/a11y/registry.h"

#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/gui/panels/inspector/inspector_model.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/session/undo/undo_journal.h"
#include "context/editor/gui/uitree/panel.h"

#include "m5_exit_test.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace a11y = context::editor::gui::a11y;
namespace inspector = context::editor::gui::panels::inspector;
namespace problems = context::editor::gui::panels::problems;
namespace undo = context::editor::gui::session::undo;
namespace uitree = context::editor::gui::uitree;

using m5exit::jstr;
using m5exit::WalkthroughGateway;

namespace
{

// Extract the `"id": "<value>"` field from every non-comment, non-blank line of the JSON-Lines
// coverage manifest. Minimal parser (the manifest is one flat JSON object per line) — enough to
// cross-check the DECLARED panel set against the harness registry without a JSON dependency here.
// `renderer_covered` collects the ids whose line declares `ts-a11y` -- the M9 e06d
// `ContentType::local` panels (editor-core renders them, so this harness has no model to instantiate
// and their a11y is gated in the webui-ts-* browser tier instead). They are DECLARED but never
// SCANNED, exactly as tools/a11y_scan.py treats them, so the set comparison below must exclude them or
// this gate would fail on a panel it is structurally unable to cover. The M5 intent is untouched:
// every M5 observer panel is C++-modelled and is still required to be scanned AND declared.
[[nodiscard]] std::set<std::string> manifest_ids(const std::string& path,
                                                 std::set<std::string>* renderer_covered = nullptr)
{
    std::set<std::string> ids;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    const std::string key = "\"id\"";
    while (std::getline(f, line))
    {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        const std::size_t kpos = trimmed.find(key);
        if (kpos == std::string::npos)
            continue;
        const std::size_t colon = trimmed.find(':', kpos + key.size());
        if (colon == std::string::npos)
            continue;
        const std::size_t open = trimmed.find('"', colon);
        if (open == std::string::npos)
            continue;
        const std::size_t close = trimmed.find('"', open + 1);
        if (close == std::string::npos)
            continue;
        const std::string id = trimmed.substr(open + 1, close - open - 1);
        ids.insert(id);
        if (renderer_covered != nullptr && trimmed.find("ts-a11y") != std::string::npos)
            renderer_covered->insert(id);
    }
    return ids;
}

// A panel is a11y-clean AND keyboard-navigable iff the headless audit finds no violation — audit_a11y
// enforces accessible names, unique ids, AND that every exposed command is reachable from a focusable
// node (complete keyboard-only navigation). Returns true on a clean panel.
[[nodiscard]] bool a11y_and_keyboard_clean(const uitree::Panel& panel)
{
    const std::vector<uitree::A11yViolation> violations = uitree::audit_a11y(panel);
    for (const uitree::A11yViolation& v : violations)
    {
        std::fprintf(stderr, "  a11y violation on panel %s: [%s] %s: %s\n", panel.id().c_str(),
                     v.code.c_str(), v.node_id.c_str(), v.message.c_str());
    }
    // A panel exposing commands must have a non-empty keyboard focus order (a reachable path).
    if (!panel.commands().empty() && uitree::focus_order(panel).empty())
    {
        std::fprintf(stderr, "  panel %s exposes commands but has an empty focus order\n",
                     panel.id().c_str());
        return false;
    }
    return violations.empty();
}

} // namespace

int main()
{
    // === 1. COVERAGE COMPLETE — registry set == manifest set, covering every M5 observer panel ========
    const std::vector<a11y::RegisteredPanel> registered = a11y::registered_panels();
    std::set<std::string> registered_ids;
    for (const a11y::RegisteredPanel& rp : registered)
    {
        CHECK(registered_ids.insert(rp.id).second); // no duplicate registry ids
    }

    // Every M5 shipped OBSERVER panel must be registered (F4 Problems was the historically-missing one —
    // this exit registers it, completing the coverage manifest).
    for (const char* id : {"placeholder", "builtin.scene-tree", "builtin.inspector", "builtin.viewport",
                           "builtin.playbar", "builtin.problems"})
    {
        if (registered_ids.count(id) == 0)
        {
            std::fprintf(stderr, "M5-exit: observer panel %s is NOT in registered_panels()\n", id);
        }
        CHECK(registered_ids.count(id) == 1);
    }

    // The registry (what the harness SCANS) must exactly match coverage.manifest.jsonl (what the CI DOM
    // gate DECLARES) — a panel in one but not the other is a coverage failure (mirrors tools/a11y_scan.py).
    std::set<std::string> renderer_covered;
    const std::set<std::string> all_declared =
        manifest_ids(CONTEXT_GUI_A11Y_MANIFEST, &renderer_covered);
    CHECK(!all_declared.empty());
    // The SCANNABLE declared set: everything except the `ts-a11y` (ContentType::local) panels, which
    // this harness structurally cannot instantiate (see manifest_ids). Mirrors tools/a11y_scan.py.
    std::set<std::string> declared;
    std::set_difference(all_declared.begin(), all_declared.end(), renderer_covered.begin(),
                        renderer_covered.end(), std::inserter(declared, declared.end()));
    if (declared != registered_ids)
    {
        std::vector<std::string> only_declared;
        std::set_difference(declared.begin(), declared.end(), registered_ids.begin(),
                            registered_ids.end(), std::back_inserter(only_declared));
        std::vector<std::string> only_registered;
        std::set_difference(registered_ids.begin(), registered_ids.end(), declared.begin(),
                            declared.end(), std::back_inserter(only_registered));
        for (const std::string& id : only_declared)
            std::fprintf(stderr, "M5-exit: %s declared in manifest but NOT registered\n", id.c_str());
        for (const std::string& id : only_registered)
            std::fprintf(stderr, "M5-exit: %s registered but NOT declared in manifest\n", id.c_str());
    }
    CHECK(declared == registered_ids);

    // === 2. EVERY registered panel is a11y-clean + keyboard-navigable (scanned through the F6 harness) =
    for (const a11y::RegisteredPanel& rp : registered)
    {
        const a11y::PanelReport report = a11y::scan_panel(rp.id, rp.factory());
        if (!report.passed)
        {
            std::fprintf(stderr, "M5-exit: panel %s failed the a11y scan\n", rp.id.c_str());
            for (const uitree::A11yViolation& v : report.violations)
                std::fprintf(stderr, "  [%s] %s: %s\n", v.code.c_str(), v.node_id.c_str(),
                             v.message.c_str());
        }
        CHECK(report.passed); // no violations => a11y-clean AND every command keyboard-reachable
        // A panel exposing commands has a real keyboard focus order (complete keyboard-only nav).
        if (!report.commands.empty())
        {
            CHECK(!report.focus_order.empty());
        }
    }

    // === 3. The non-observer F1-F7 surfaces: F6 harness (used above) + F7 undo/redo ===================
    // F7 GUI session undo/redo: its command panel exposes undo/redo ONLY when an action is available,
    // so it is a11y-clean + keyboard-navigable in BOTH the empty and the action-available states.
    {
        undo::UndoJournal empty_journal;
        CHECK(a11y_and_keyboard_clean(empty_journal.build_panel())); // no actions -> no unreachable command

        WalkthroughGateway gw;
        gw.field_values["/name"] = jstr("New");
        undo::UndoJournal journal(&gw);
        undo::FieldEdit e;
        e.root_scene = "root.scene.json";
        e.id_path = {"ccccccccccccccc1"};
        e.pointer = "/name";
        e.before = jstr("Old");
        e.after = jstr("New");
        journal.capture(e);
        CHECK(journal.can_undo());
        CHECK(a11y_and_keyboard_clean(journal.build_panel())); // the undo command has a keyboard path
    }

    // Belt-and-braces: the Problems panel (F4) is a11y-clean + keyboard-navigable with real diagnostics
    // (the registry scans only its default/empty state; here we scan a populated, navigable set).
    {
        problems::ProblemsPanel panel;
        std::vector<problems::ProblemDiagnostic> diags;
        problems::ProblemDiagnostic d;
        d.code = "schema.unknown_field";
        d.message = "unknown field 'foo'";
        d.severity = problems::Severity::error;
        d.nav.file = "root.scene.json";
        d.nav.line = 3;
        diags.push_back(d);
        panel.set_diagnostics(std::move(diags));
        CHECK(a11y_and_keyboard_clean(panel.build_panel()));
    }

    M5_EXIT_MAIN_END();
}
