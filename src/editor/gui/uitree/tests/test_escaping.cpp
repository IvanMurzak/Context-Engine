// T1 ESCAPING CONTRACT suite (M9 e05b, C-F6 / design 04 §4): the XSS-from-project threat row.
//
// A uitree panel renders AUTHORED PROJECT STRINGS — entity names, the schema-blessed `notes`, any
// string field — into the TRUSTED editor-core zone. Escaping is the CONTROL; the strict CSP is only
// the backstop. This suite drives adversarial project strings through EVERY interpolation site
// (node id, aria-label, data-command, element text, the document title, and the CSP meta) and asserts
// that no attacker-controlled byte survives as markup.
//
// The assertion shape is deliberately structural rather than a golden string: after escaping, the
// ONLY '<' and '>' bytes in the output may be the ones this renderer itself emitted from its closed
// tag table — so counting tags is a complete test that no injected byte became markup.

#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"

#include "uitree_test.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using namespace context::editor::gui::uitree;

namespace
{

// The adversarial corpus: real XSS payload shapes an authored project file could legally carry.
std::vector<std::string> hostile_strings()
{
    return {
        "<script>alert(1)</script>",
        "</script><script>alert(1)//",
        "\" onmouseover=\"alert(1)",
        "' onfocus='alert(1)",
        "'><img src=x onerror=alert(1)>",
        "\"><svg/onload=alert(1)>",
        "</textarea><script>alert(1)</script>",
        "</section><script>alert(1)</script>",
        "javascript:alert(1)",
        "&lt;script&gt;alert(1)&lt;/script&gt;", // pre-escaped: must be DOUBLE-escaped, not passed
        "&#60;script&#62;",
        "&amp;",
        "]]><script>alert(1)</script>",
        "<!--<script>alert(1)</script>-->",
        "\\\"><script>alert(1)</script>",
        "\x01\x02\x1f control bytes \x7f",
        "mixed \"quotes\" & 'apostrophes' < > tags",
        "Ünïcödé — emoji 🎮 stays intact",
    };
}

std::size_t count(const std::string& haystack, char needle)
{
    return static_cast<std::size_t>(std::count(haystack.begin(), haystack.end(), needle));
}

std::size_t count(const std::string& haystack, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size()))
    {
        ++n;
    }
    return n;
}

} // namespace

int main()
{
    // --- escape_html_text: the five significant characters, at every occurrence -------------------
    {
        CHECK(escape_html_text("") == "");
        CHECK(escape_html_text("plain") == "plain");
        CHECK(escape_html_text("<") == "&lt;");
        CHECK(escape_html_text(">") == "&gt;");
        CHECK(escape_html_text("\"") == "&quot;");
        CHECK(escape_html_text("'") == "&#39;");
        CHECK(escape_html_text("&") == "&amp;");
        // ampersand-first ordering: an already-escaped entity is DOUBLE-escaped, never passed through
        // (passing it through would let a project author smuggle markup as "&lt;script&gt;").
        CHECK(escape_html_text("&lt;") == "&amp;lt;");
        CHECK(escape_html_text("<a href=\"x\">&'</a>") ==
              "&lt;a href=&quot;x&quot;&gt;&amp;&#39;&lt;/a&gt;");
        // every occurrence, not just the first
        CHECK(escape_html_text("<<<") == "&lt;&lt;&lt;");
    }

    // --- escape_html_text: C0 controls become U+FFFD; tab/LF/CR and UTF-8 survive ------------------
    {
        const std::string replacement = "\xEF\xBF\xBD";
        CHECK(escape_html_text(std::string(1, '\0')) == replacement);
        CHECK(escape_html_text("\x01") == replacement);
        CHECK(escape_html_text("\x1f") == replacement);
        CHECK(escape_html_text("\x7f") == replacement);
        CHECK(escape_html_text("\t\n\r") == "\t\n\r"); // legal HTML whitespace passes through
        CHECK(escape_html_text("Ünïcödé 🎮") == "Ünïcödé 🎮"); // byte-preserving for UTF-8
        // a NUL in the middle does not truncate the rest
        const std::string with_nul = std::string("a") + '\0' + "b";
        CHECK(escape_html_text(with_nul) == "a" + replacement + "b");
    }

    // --- the renderer: NO hostile byte becomes markup at ANY interpolation site --------------------
    for (const std::string& hostile : hostile_strings())
    {
        // One node exercising all four node-level interpolation sites at once.
        UiNode node(Role::button, hostile);
        node.set_label(hostile).set_text(hostile).set_command(hostile).set_focusable(true);
        const std::string html = render_html(node);

        // The renderer emitted exactly one <button ...> open tag and one </button> close tag, so the
        // output must contain exactly two '<' and two '>' bytes. Any third would be injected markup.
        CHECK(count(html, '<') == 2);
        CHECK(count(html, '>') == 2);
        CHECK(count(html, "<button") == 1);
        CHECK(count(html, "</button>") == 1);

        // No payload became an ELEMENT: '<' is always escaped, so no tag can be injected.
        CHECK(html.find("<script") == std::string::npos);
        CHECK(html.find("</script") == std::string::npos);
        CHECK(html.find("<img") == std::string::npos);
        CHECK(html.find("<svg") == std::string::npos);

        // No payload became an ATTRIBUTE either. The payload's WORDS (e.g. "onerror=alert(1)")
        // legitimately SURVIVE — inside an escaped attribute value and as escaped text — because
        // escaping neutralizes markup, it does not redact vocabulary. What must hold is that the
        // payload cannot BREAK OUT of the value it sits in, and that is exactly countable: a payload
        // can never contribute a '"' (always escaped), so every quote in the output is a delimiter the
        // renderer emitted. Five attributes => exactly ten quotes; an eleventh would mean a smuggled
        // attribute, a ninth an unterminated one.
        CHECK(count(html, '"') == 10);
        CHECK(count(html, '\'') == 0); // nor a single-quoted attribute (some parsers accept those)
        CHECK(count(html, " id=\"") == 1);
        CHECK(count(html, " role=\"") == 1);
        CHECK(count(html, " aria-label=\"") == 1);
        CHECK(count(html, " data-command=\"") == 1);
        CHECK(count(html, " tabindex=\"0\"") == 1);
    }

    // --- the same guarantee through a whole PANEL tree, including nested children ------------------
    {
        const std::string hostile = "</section><script>alert(1)</script>";
        Panel panel(hostile, hostile);
        UiNode root(Role::region, "root");
        root.set_label(hostile);
        root.add_child(UiNode(Role::text, "child.text").set_text(hostile));
        root.add_child(UiNode(Role::treeitem, hostile).set_label(hostile));
        panel.set_root(std::move(root));

        const std::string body = render_html(panel);
        CHECK(body.find("<script") == std::string::npos);
        CHECK(body.find("</section>") != std::string::npos); // ours (the region close tag)
        // exactly the tags the renderer emitted: section + span + li = 3 open + 3 close
        CHECK(count(body, '<') == 6);
        CHECK(count(body, '>') == 6);

        // --- the DOCUMENT composition site: title AND csp are escaped too -------------------------
        const std::string doc = render_document(panel, hostile);
        CHECK(doc.find("<script") == std::string::npos);
        CHECK(doc.find("alert(1)") != std::string::npos); // present, but as inert escaped TEXT
        CHECK(doc.find("</title>") != std::string::npos);
        CHECK(doc.find("<title>&lt;") != std::string::npos); // the title interpolation is escaped
        CHECK(doc.find("content=\"&lt;") != std::string::npos); // the CSP interpolation is escaped
        CHECK(count(doc, '"') % 2 == 0);

        // A REAL csp + title still compose to a sane document (the contract is not just paranoia).
        Panel plain("placeholder", "Context Editor");
        plain.set_root(UiNode(Role::region, "body").set_label("Context Editor"));
        const std::string ok_doc = render_document(plain, "default-src 'none'; script-src 'self'");
        CHECK(ok_doc.find("<title>Context Editor</title>") != std::string::npos);
        // the CSP's own apostrophes are escaped but remain a valid CSP after HTML unescaping
        CHECK(ok_doc.find("content=\"default-src &#39;none&#39;; script-src &#39;self&#39;\"") !=
              std::string::npos);
        CHECK(ok_doc.find("<!DOCTYPE html>") == 0);
        CHECK(ok_doc.find("</body></html>") != std::string::npos);
    }

    // --- an empty panel still composes a well-formed document (no root => empty body) --------------
    {
        const Panel empty("empty", "Empty");
        CHECK(render_html(empty).empty());
        const std::string doc = render_document(empty, "default-src 'none'");
        CHECK(doc.find("<body></body>") != std::string::npos);
    }

    UITREE_TEST_MAIN_END();
}
