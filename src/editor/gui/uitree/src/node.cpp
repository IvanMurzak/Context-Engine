// UI-logic tree node implementation: the role vocabulary tables + semantic-HTML rendering.

#include "context/editor/gui/uitree/node.h"

#include <sstream>
#include <string>

namespace context::editor::gui::uitree
{

bool role_requires_name(Role role)
{
    switch (role)
    {
    case Role::region:
    case Role::treeitem:
    case Role::listitem:
    case Role::button:
    case Role::textbox:
    case Role::checkbox:
    case Role::heading:
    case Role::status:
        return true;
    case Role::group:
    case Role::tree:
    case Role::list:
    case Role::text:
        return false;
    }
    return false;
}

const char* role_token(Role role)
{
    switch (role)
    {
    case Role::region:
        return "region";
    case Role::group:
        return "group";
    case Role::tree:
        return "tree";
    case Role::treeitem:
        return "treeitem";
    case Role::list:
        return "list";
    case Role::listitem:
        return "listitem";
    case Role::button:
        return "button";
    case Role::textbox:
        return "textbox";
    case Role::checkbox:
        return "checkbox";
    case Role::heading:
        return "heading";
    case Role::status:
        return "status";
    case Role::text:
        return "text";
    }
    return "text";
}

const char* role_html_tag(Role role)
{
    // Semantic HTML tags (R-A11Y-001) — the element carries the semantics; the explicit role="" is
    // belt-and-braces for assistive tech and keeps the token grep-able in the DOM.
    switch (role)
    {
    case Role::region:
        return "section";
    case Role::group:
        return "div";
    case Role::tree:
        return "ul";
    case Role::treeitem:
        return "li";
    case Role::list:
        return "ul";
    case Role::listitem:
        return "li";
    case Role::button:
        return "button";
    case Role::textbox:
        return "input";
    case Role::checkbox:
        return "input";
    case Role::heading:
        return "h2";
    case Role::status:
        return "output";
    case Role::text:
        return "span";
    }
    return "span";
}

namespace
{

// Escape the five HTML-significant characters so a label/text value cannot break out of its
// attribute or element. Deterministic — the rendered HTML is byte-assertable.
std::string escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

void render_into(const UiNode& node, std::ostringstream& out)
{
    const char* tag = role_html_tag(node.role());
    out << '<' << tag << " id=\"" << escape(node.id()) << "\" role=\"" << role_token(node.role())
        << '"';
    if (!node.label().empty())
    {
        out << " aria-label=\"" << escape(node.label()) << '"';
    }
    if (node.focusable())
    {
        out << " tabindex=\"0\"";
    }
    if (!node.command().empty())
    {
        out << " data-command=\"" << escape(node.command()) << '"';
    }
    out << '>';
    if (!node.text().empty())
    {
        out << escape(node.text());
    }
    for (const UiNode& child : node.children())
    {
        render_into(child, out);
    }
    out << "</" << tag << '>';
}

} // namespace

std::string render_html(const UiNode& node)
{
    std::ostringstream out;
    render_into(node, out);
    return out.str();
}

} // namespace context::editor::gui::uitree
