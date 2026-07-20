// Headless panel + a11y/keyboard-nav harness implementation.

#include "context/editor/gui/uitree/panel.h"

#include "context/editor/gui/uitree/node.h"

#include <set>
#include <string>
#include <vector>

namespace context::editor::gui::uitree
{

bool Panel::has_command(const std::string& command_id) const
{
    for (const Command& c : commands_)
    {
        if (c.id == command_id)
        {
            return true;
        }
    }
    return false;
}

namespace
{

void collect_focus_order(const UiNode& node, std::vector<std::string>& out)
{
    if (node.focusable())
    {
        out.push_back(node.id());
    }
    for (const UiNode& child : node.children())
    {
        collect_focus_order(child, out);
    }
}

// Walk every node once, applying `visit`. Depth-first, document order.
template <typename Visit>
void walk(const UiNode& node, Visit&& visit)
{
    visit(node);
    for (const UiNode& child : node.children())
    {
        walk(child, visit);
    }
}

} // namespace

std::vector<std::string> focus_order(const Panel& panel)
{
    std::vector<std::string> order;
    if (panel.has_root())
    {
        collect_focus_order(panel.root(), order);
    }
    return order;
}

std::vector<A11yViolation> audit_a11y(const Panel& panel)
{
    std::vector<A11yViolation> violations;
    if (!panel.has_root())
    {
        return violations;
    }

    std::set<std::string> seen_ids;
    std::set<std::string> commands_reached_by_keyboard;

    walk(panel.root(),
         [&](const UiNode& node)
         {
             // Unique ids within the panel.
             if (!seen_ids.insert(node.id()).second)
             {
                 violations.push_back(
                     A11yViolation{node.id(), "duplicate-id",
                                   "node id \"" + node.id() + "\" is not unique within the panel"});
             }

             // Accessible name required on focusable nodes + name-requiring roles.
             const bool needs_name = node.focusable() || role_requires_name(node.role());
             if (needs_name && node.label().empty())
             {
                 violations.push_back(A11yViolation{
                     node.id(), "missing-name",
                     "node \"" + node.id() + "\" (role " + role_token(node.role()) +
                         ") requires an accessible name (aria-label)"});
             }

             // Command bindings must reference a command the panel exposes.
             if (!node.command().empty())
             {
                 if (!panel.has_command(node.command()))
                 {
                     violations.push_back(A11yViolation{
                         node.id(), "orphan-command",
                         "node \"" + node.id() + "\" binds command \"" + node.command() +
                             "\" which the panel does not expose"});
                 }
                 else if (node.focusable())
                 {
                     commands_reached_by_keyboard.insert(node.command());
                 }
             }
         });

    // Every exposed command must be reachable from a focusable node (keyboard-only completeness).
    for (const Command& c : panel.commands())
    {
        if (commands_reached_by_keyboard.find(c.id) == commands_reached_by_keyboard.end())
        {
            violations.push_back(A11yViolation{
                c.id, "unreachable-command",
                "command \"" + c.id + "\" is not reachable from any focusable node (no keyboard path)"});
        }
    }

    return violations;
}

std::string render_html(const Panel& panel)
{
    if (!panel.has_root())
    {
        return std::string();
    }
    return render_html(panel.root());
}

std::string render_document(const Panel& panel, const std::string& csp)
{
    // Every interpolated value is escaped (C-F6, node.h) — the CSP and the title included. Only the
    // literal document scaffolding below is unescaped.
    return "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta http-equiv=\"Content-Security-Policy\" content=\"" +
           escape_html_text(csp) + "\"><title>" + escape_html_text(panel.title()) +
           "</title></head><body>" + render_html(panel) + "</body></html>";
}

} // namespace context::editor::gui::uitree
