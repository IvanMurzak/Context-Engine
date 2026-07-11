// The a11y enforcement harness entry point (R-A11Y-001): scan every registered editor panel over the
// headless UI-logic tree, emit the JSON report, and FAIL (non-zero) if any panel has an accessibility
// violation or a command with no keyboard path. Registered as the `gui-a11y-scan` ctest, so the
// default 3-OS build matrix enforces per-panel a11y WITHOUT CEF; the editor-cef-smoke CI job also runs
// it with `--out <file>` and feeds the report to tools/a11y_scan.py for the axe-class DOM scan +
// coverage-manifest cross-check (Linux blocking).
//
// Usage:
//   context_gui_a11y_scan            scan + assert clean; print the JSON report to stdout
//   context_gui_a11y_scan --out F    also write the JSON report to file F

#include "context/editor/gui/a11y/harness.h"
#include "context/editor/gui/a11y/registry.h"
#include "context/editor/gui/uitree/panel.h"

#include <cstdio>
#include <fstream>
#include <ios>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
    using namespace context::editor::gui;

    std::string out_path;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc)
        {
            out_path = argv[++i];
        }
        else
        {
            std::fprintf(stderr, "a11y-scan: unexpected argument \"%s\"\n", arg.c_str());
            return 2;
        }
    }

    std::vector<a11y::PanelReport> reports;
    int failures = 0;
    for (const a11y::RegisteredPanel& rp : a11y::registered_panels())
    {
        a11y::PanelReport report = a11y::scan_panel(rp.id, rp.factory());
        if (!report.passed)
        {
            std::fprintf(stderr, "a11y-scan: panel \"%s\" FAILED the R-A11Y-001 audit:\n",
                         report.id.c_str());
            for (const uitree::A11yViolation& v : report.violations)
            {
                std::fprintf(stderr, "  [%s] %s: %s\n", v.code.c_str(), v.node_id.c_str(),
                             v.message.c_str());
            }
            ++failures;
        }
        reports.push_back(std::move(report));
    }

    const std::string json = a11y::reports_to_json(reports);
    if (out_path.empty())
    {
        std::printf("%s\n", json.c_str());
    }
    else
    {
        std::ofstream os(out_path, std::ios::binary);
        os << json;
        if (!os)
        {
            std::fprintf(stderr, "a11y-scan: failed writing report to \"%s\"\n", out_path.c_str());
            return 2;
        }
    }

    if (failures != 0)
    {
        std::fprintf(stderr, "a11y-scan: %d panel(s) failed the R-A11Y-001 gate\n", failures);
        return 1;
    }
    return 0;
}
