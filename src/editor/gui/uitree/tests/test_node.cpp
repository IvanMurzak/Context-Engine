// UiNode tests: the role vocabulary tables + deterministic semantic-HTML rendering with ARIA
// role/label/tabindex + HTML escaping.

#include "context/editor/gui/uitree/node.h"

#include "uitree_test.h"

#include <string>

using namespace context::editor::gui::uitree;

namespace
{

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    // --- role tables ----------------------------------------------------------------------------
    {
        // Name-requiring roles.
        CHECK(role_requires_name(Role::region));
        CHECK(role_requires_name(Role::button));
        CHECK(role_requires_name(Role::textbox));
        CHECK(role_requires_name(Role::heading));
        CHECK(role_requires_name(Role::status));
        CHECK(role_requires_name(Role::treeitem));
        // Roles that do not require a name.
        CHECK(!role_requires_name(Role::group));
        CHECK(!role_requires_name(Role::tree));
        CHECK(!role_requires_name(Role::list));
        CHECK(!role_requires_name(Role::text));

        // Stable role tokens + semantic tags.
        CHECK(std::string(role_token(Role::button)) == "button");
        CHECK(std::string(role_token(Role::treeitem)) == "treeitem");
        CHECK(std::string(role_html_tag(Role::region)) == "section");
        CHECK(std::string(role_html_tag(Role::heading)) == "h2");
        CHECK(std::string(role_html_tag(Role::button)) == "button");
        CHECK(std::string(role_html_tag(Role::tree)) == "ul");
        CHECK(std::string(role_html_tag(Role::treeitem)) == "li");
    }

    // --- render_html: semantic tag + ARIA attrs + tabindex + command binding --------------------
    {
        UiNode button(Role::button, "b1");
        button.set_label("Refresh").set_text("Refresh").set_focusable(true).set_command("do.refresh");
        const std::string html = render_html(button);
        CHECK(contains(html, "<button "));
        CHECK(contains(html, "id=\"b1\""));
        CHECK(contains(html, "role=\"button\""));
        CHECK(contains(html, "aria-label=\"Refresh\""));
        CHECK(contains(html, "tabindex=\"0\""));
        CHECK(contains(html, "data-command=\"do.refresh\""));
        CHECK(contains(html, ">Refresh</button>"));
    }

    // --- render_html: a non-focusable node carries NO tabindex, no command ----------------------
    {
        UiNode label(Role::text, "t1");
        label.set_text("hello");
        const std::string html = render_html(label);
        CHECK(!contains(html, "tabindex"));
        CHECK(!contains(html, "data-command"));
        CHECK(!contains(html, "aria-label")); // no label set
        CHECK(contains(html, "<span id=\"t1\" role=\"text\">hello</span>"));
    }

    // --- render_html: nesting is preserved (parent wraps child) ---------------------------------
    {
        UiNode region(Role::region, "r1");
        region.set_label("Panel").add_child(UiNode(Role::text, "c1").set_text("child"));
        const std::string html = render_html(region);
        CHECK(contains(html, "<section id=\"r1\" role=\"region\" aria-label=\"Panel\">"));
        CHECK(contains(html, "<span id=\"c1\" role=\"text\">child</span>"));
        CHECK(contains(html, "</section>"));
        // child element appears before the closing section tag
        CHECK(html.find("c1") < html.find("</section>"));
    }

    // --- render_html: HTML escaping (a label cannot break out of its attribute) ------------------
    {
        UiNode n(Role::button, "x");
        n.set_label("a\"<b>&'").set_focusable(true);
        const std::string html = render_html(n);
        CHECK(contains(html, "aria-label=\"a&quot;&lt;b&gt;&amp;&#39;\""));
        CHECK(!contains(html, "<b>")); // the literal <b> must not survive unescaped
    }

    // --- render_html: <input> is a VOID element (M9 e05d3 inherited fix) ------------------------
    // A textbox/checkbox emitted as `<input>text</input>` makes a conforming parser drop the end tag
    // and hoist the text OUT of the input, corrupting the mounted DOM's correspondence with the
    // model — latent until a Shell-hostable panel used those roles; the Inspector is built from them.
    {
        // Textbox: the node text rides `value="…"`; no content, no end tag.
        UiNode box(Role::textbox, "f1");
        box.set_label("/name").set_text("Player One").set_focusable(true).set_command("inspector.edit");
        const std::string html = render_html(box);
        CHECK(contains(html, "<input id=\"f1\" role=\"textbox\""));
        CHECK(contains(html, "value=\"Player One\""));
        CHECK(!contains(html, "</input>"));
        CHECK(!contains(html, ">Player One")); // never as element CONTENT
    }
    {
        // Checkbox: `type="checkbox"` always; `checked` present iff the text is "true".
        UiNode on(Role::checkbox, "c-on");
        on.set_label("/visible").set_text("true").set_focusable(true);
        const std::string on_html = render_html(on);
        CHECK(contains(on_html, "type=\"checkbox\""));
        CHECK(contains(on_html, " checked"));
        CHECK(!contains(on_html, "</input>"));
        CHECK(!contains(on_html, "value=")); // checkbox state is `checked`, not a value attribute

        UiNode off(Role::checkbox, "c-off");
        off.set_label("/visible").set_text("false").set_focusable(true);
        const std::string off_html = render_html(off);
        CHECK(contains(off_html, "type=\"checkbox\""));
        CHECK(!contains(off_html, "checked"));
        CHECK(!contains(off_html, "</input>"));
    }
    {
        // ADVERSARIAL T1 (the C-F6 escaping-contract lesson): a hostile field VALUE cannot break out
        // of the value attribute, close the void tag early, or smuggle an element/handler in.
        UiNode evil(Role::textbox, "f2");
        evil.set_label("/name").set_text("\"><script>alert(1)</script><input onfocus=\"x()\" value=\"");
        const std::string html = render_html(evil);
        CHECK(contains(html,
                       "value=\"&quot;&gt;&lt;script&gt;alert(1)&lt;/script&gt;&lt;input "
                       "onfocus=&quot;x()&quot; value=&quot;\""));
        CHECK(!contains(html, "<script>"));  // the literal script tag must not survive
        CHECK(!contains(html, "onfocus=\"x")); // nor a live handler attribute
        CHECK(!contains(html, "</input>"));
    }
    {
        // Out-of-contract nesting under a void element: children render as FOLLOWING SIBLINGS,
        // deterministically on OUR side — never inside the input, never dropped.
        UiNode box(Role::textbox, "f3");
        box.set_label("/name").set_text("v");
        box.add_child(UiNode(Role::text, "f3.note").set_text("note"));
        const std::string html = render_html(box);
        const std::size_t input_end = html.find('>');
        CHECK(input_end != std::string::npos);
        CHECK(contains(html, "<span id=\"f3.note\" role=\"text\">note</span>"));
        CHECK(html.find("f3.note") > input_end); // the child follows the closed void tag
    }

    UITREE_TEST_MAIN_END();
}
