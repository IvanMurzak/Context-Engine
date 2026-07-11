// Unit tests for the a11y enforcement harness (R-QA-013: happy / edge / failure): scan_panel over the
// conformant placeholder, the missing-name + unreachable-command failure paths, deterministic JSON
// report + string escaping, and the standing gate that EVERY registered built-in panel is a11y-clean.

#include "context/editor/gui/a11y/harness.h"
#include "context/editor/gui/a11y/registry.h"

#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"

#include "a11y_test.h"

#include <string>
#include <utility>
#include <vector>

using namespace context::editor::gui;
using a11y::PanelReport;

namespace
{

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

bool has_code(const std::vector<uitree::A11yViolation>& vs, const std::string& code)
{
    for (const uitree::A11yViolation& v : vs)
    {
        if (v.code == code)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    // --- scan_panel on the conformant F0b placeholder: passes, captures focus order + commands ----
    {
        PanelReport r = a11y::scan_panel("placeholder", uitree::make_placeholder_panel());
        CHECK(r.passed);
        CHECK(r.violations.empty());
        CHECK(r.id == "placeholder");
        CHECK(r.title == "Context Editor");
        CHECK(r.focus_order.size() == 1);
        CHECK(r.focus_order.front() == "placeholder.refresh");
        CHECK(r.commands.size() == 1);
        CHECK(r.commands.front() == "placeholder.refresh");
        // the rendered semantic HTML carries the ARIA role + accessible name + a tab stop
        CHECK(contains(r.html, "role=\"button\""));
        CHECK(contains(r.html, "aria-label=\"Refresh\""));
        CHECK(contains(r.html, "tabindex=\"0\""));
    }

    // --- scan_panel surfaces a missing accessible name on a focusable node -----------------------
    {
        uitree::UiNode root(uitree::Role::region, "r");
        root.set_label("Panel");
        root.add_child(uitree::UiNode(uitree::Role::button, "b").set_focusable(true)); // no label
        uitree::Panel p("p", "P");
        p.set_root(std::move(root));
        PanelReport r = a11y::scan_panel("p", p);
        CHECK(!r.passed);
        CHECK(has_code(r.violations, "missing-name"));
    }

    // --- scan_panel surfaces an unreachable command (no keyboard path) ---------------------------
    {
        uitree::UiNode root(uitree::Role::region, "r");
        root.set_label("Panel");
        root.add_child(uitree::UiNode(uitree::Role::button, "b")
                           .set_label("B")
                           .set_command("do.thing")
                           .set_focusable(false)); // bound but not focusable
        uitree::Panel p("p", "P");
        p.set_root(std::move(root));
        p.add_command("do.thing", "Do the thing");
        PanelReport r = a11y::scan_panel("p", p);
        CHECK(!r.passed);
        CHECK(has_code(r.violations, "unreachable-command"));
    }

    // --- reports_to_json: deterministic shape + JSON string escaping ------------------------------
    {
        std::vector<PanelReport> reports;
        reports.push_back(a11y::scan_panel("placeholder", uitree::make_placeholder_panel()));
        const std::string json = a11y::reports_to_json(reports);
        CHECK(contains(json, "\"id\":\"placeholder\""));
        CHECK(contains(json, "\"passed\":true"));
        CHECK(contains(json, "\"focus_order\":[\"placeholder.refresh\"]"));
        CHECK(contains(json, "\"commands\":[\"placeholder.refresh\"]"));
        // the html value is JSON-escaped (its embedded double-quotes become \")
        CHECK(contains(json, "\\\"button\\\""));
    }

    // --- escaping: a value emitted verbatim (the title) is JSON-escaped in the report ------------
    // (a node label is HTML-escaped by render_html before the JSON layer, so it can't exercise the
    // JSON string escaper — the title reaches reports_to_json unescaped and must be escaped there.)
    {
        uitree::UiNode root(uitree::Role::region, "r");
        root.set_label("Panel");
        uitree::Panel p("p", "A \"quoted\" title"); // the title carries a double-quote
        p.set_root(std::move(root));
        std::vector<PanelReport> reports;
        reports.push_back(a11y::scan_panel("q", p));
        const std::string json = a11y::reports_to_json(reports);
        CHECK(contains(json, "\"title\":\"A \\\"quoted\\\" title\""));
    }

    // --- the standing gate: EVERY registered built-in panel is a11y-clean -------------------------
    {
        const std::vector<a11y::RegisteredPanel> panels = a11y::registered_panels();
        CHECK(!panels.empty());
        bool saw_placeholder = false;
        for (const a11y::RegisteredPanel& rp : panels)
        {
            if (rp.id == "placeholder")
            {
                saw_placeholder = true;
            }
            PanelReport r = a11y::scan_panel(rp.id, rp.factory());
            CHECK(r.passed);
        }
        CHECK(saw_placeholder);
    }

    A11Y_TEST_MAIN_END();
}
