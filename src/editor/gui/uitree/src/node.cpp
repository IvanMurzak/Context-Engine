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

void render_into(const UiNode& node, std::ostringstream& out)
{
    const char* tag = role_html_tag(node.role());
    // `<input>` is an HTML VOID element: it takes no end tag and can hold no content. Emitting
    // `<input>text</input>` (the pre-e05d3 bug) makes a conforming parser DROP the end tag and hoist
    // the text OUTSIDE the input — silently corrupting the mounted DOM's correspondence with the
    // model the moment a textbox/checkbox renders (exactly the roles the Inspector is built from).
    // So for the void roles the node's TEXT rides the value channel instead: `value="…"` for a
    // textbox (its current value), `checked` presence for a checkbox (text == "true"). A checkbox
    // also carries `type="checkbox"` — without it the element IS a text input to the browser
    // (isTextEntry in the hydration runtime keys off `type`, not `role`).
    const bool is_void = (node.role() == Role::textbox || node.role() == Role::checkbox);
    // EVERY interpolation below is escaped — see the C-F6 escaping contract in node.h. `tag` and the
    // role token are the only unescaped insertions, and both come from a CLOSED enum table above (not
    // from project data), which is why they need none.
    out << '<' << tag << " id=\"" << escape_html_text(node.id()) << "\" role=\""
        << role_token(node.role()) << '"';
    if (node.role() == Role::checkbox)
    {
        out << " type=\"checkbox\"";
    }
    if (!node.label().empty())
    {
        out << " aria-label=\"" << escape_html_text(node.label()) << '"';
    }
    if (node.focusable())
    {
        out << " tabindex=\"0\"";
    }
    if (!node.command().empty())
    {
        out << " data-command=\"" << escape_html_text(node.command()) << '"';
    }
    if (is_void)
    {
        if (node.role() == Role::checkbox)
        {
            if (node.text() == "true")
            {
                out << " checked";
            }
        }
        else if (!node.text().empty())
        {
            out << " value=\"" << escape_html_text(node.text()) << '"';
        }
        out << '>';
        // A void element cannot contain children. A model that nests under a textbox/checkbox is
        // out of contract, but render_html is TOTAL — so any children are rendered as FOLLOWING
        // SIBLINGS (deterministically, on OUR side) rather than silently dropped or left for the
        // browser's parser to hoist unpredictably.
        for (const UiNode& child : node.children())
        {
            render_into(child, out);
        }
        return;
    }
    out << '>';
    if (!node.text().empty())
    {
        out << escape_html_text(node.text());
    }
    for (const UiNode& child : node.children())
    {
        render_into(child, out);
    }
    out << "</" << tag << '>';
}

} // namespace

std::string escape_html_text(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char raw : text)
    {
        const unsigned char c = static_cast<unsigned char>(raw);
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
            // A C0 control other than tab/LF/CR (and a bare DEL) has no HTML text representation; a
            // conforming parser substitutes U+FFFD, so do it here to keep the output deterministic.
            if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F)
            {
                out += "\xEF\xBF\xBD"; // U+FFFD REPLACEMENT CHARACTER, UTF-8
            }
            else
            {
                out += raw; // byte-preserving: UTF-8 sequences pass through untouched
            }
            break;
        }
    }
    return out;
}

std::string render_html(const UiNode& node)
{
    std::ostringstream out;
    render_into(node, out);
    return out.str();
}

} // namespace context::editor::gui::uitree
