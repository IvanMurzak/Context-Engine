// Accessibility enforcement harness implementation: per-panel scan + deterministic JSON report.

#include "context/editor/gui/a11y/harness.h"

#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace context::editor::gui::a11y
{

PanelReport scan_panel(const std::string& id, const uitree::Panel& panel)
{
    PanelReport report;
    report.id = id;
    report.title = panel.title();
    report.violations = uitree::audit_a11y(panel);
    report.focus_order = uitree::focus_order(panel);
    for (const uitree::Command& c : panel.commands())
    {
        report.commands.push_back(c.id);
    }
    report.html = uitree::render_html(panel);
    // audit_a11y already emits an "unreachable-command" violation for any command with no keyboard
    // path, so an empty violation list is the complete pass condition.
    report.passed = report.violations.empty();
    return report;
}

namespace
{

// Escape a string for embedding inside a JSON string literal (RFC 8259). Deterministic — the report
// is byte-assertable and re-parsable by tools/a11y_scan.py.
std::string json_escape(const std::string& in)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                const unsigned char uc = static_cast<unsigned char>(c);
                out += "\\u00";
                out += kHex[(uc >> 4) & 0xF];
                out += kHex[uc & 0xF];
            }
            else
            {
                out += c;
            }
            break;
        }
    }
    return out;
}

void append_string_array(std::ostringstream& out, const std::vector<std::string>& items)
{
    out << '[';
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        out << '"' << json_escape(items[i]) << '"';
    }
    out << ']';
}

} // namespace

std::string reports_to_json(const std::vector<PanelReport>& reports)
{
    std::ostringstream out;
    out << "{\"panels\":[";
    for (std::size_t i = 0; i < reports.size(); ++i)
    {
        const PanelReport& r = reports[i];
        if (i != 0)
        {
            out << ',';
        }
        out << "{\"id\":\"" << json_escape(r.id) << "\",\"title\":\"" << json_escape(r.title)
            << "\",\"passed\":" << (r.passed ? "true" : "false") << ",\"violations\":[";
        for (std::size_t v = 0; v < r.violations.size(); ++v)
        {
            const uitree::A11yViolation& viol = r.violations[v];
            if (v != 0)
            {
                out << ',';
            }
            out << "{\"node_id\":\"" << json_escape(viol.node_id) << "\",\"code\":\""
                << json_escape(viol.code) << "\",\"message\":\"" << json_escape(viol.message)
                << "\"}";
        }
        out << "],\"focus_order\":";
        append_string_array(out, r.focus_order);
        out << ",\"commands\":";
        append_string_array(out, r.commands);
        out << ",\"html\":\"" << json_escape(r.html) << "\"}";
    }
    out << "]}";
    return out.str();
}

} // namespace context::editor::gui::a11y
