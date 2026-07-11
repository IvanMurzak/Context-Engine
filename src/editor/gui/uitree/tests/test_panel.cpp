// Panel tests: command registry, focus order, and panel-level HTML rendering.

#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"

#include "uitree_test.h"

#include <string>
#include <vector>

using namespace context::editor::gui::uitree;

int main()
{
    // --- command registry -----------------------------------------------------------------------
    {
        Panel p("scene-tree", "Scene Tree");
        p.add_command("scene.reveal", "Reveal in tree");
        CHECK(p.has_command("scene.reveal"));
        CHECK(!p.has_command("nope"));
        CHECK(p.commands().size() == 1);
        CHECK(p.id() == "scene-tree");
        CHECK(p.title() == "Scene Tree");
        CHECK(!p.has_root());
    }

    // --- focus order = depth-first over focusable nodes -----------------------------------------
    {
        UiNode root(Role::region, "root");
        root.set_label("Panel");
        root.add_child(UiNode(Role::button, "a").set_label("A").set_focusable(true));
        UiNode group(Role::group, "g");
        group.add_child(UiNode(Role::text, "inert").set_text("x")); // not focusable — skipped
        group.add_child(UiNode(Role::button, "b").set_label("B").set_focusable(true));
        root.add_child(std::move(group));
        root.add_child(UiNode(Role::button, "c").set_label("C").set_focusable(true));

        Panel p("p", "P");
        p.set_root(std::move(root));
        CHECK(p.has_root());

        const std::vector<std::string> order = focus_order(p);
        CHECK(order.size() == 3);
        CHECK(order[0] == "a");
        CHECK(order[1] == "b");
        CHECK(order[2] == "c");
    }

    // --- panel render delegates to the root; empty panel renders empty --------------------------
    {
        Panel empty("e", "E");
        CHECK(render_html(empty).empty());

        Panel placeholder = make_placeholder_panel();
        const std::string html = render_html(placeholder);
        CHECK(html.find("placeholder.panel") != std::string::npos);
        CHECK(html.find("<button ") != std::string::npos);
        CHECK(html.find("</section>") != std::string::npos);
    }

    // --- the built-in placeholder panel is wired sanely -----------------------------------------
    {
        Panel placeholder = make_placeholder_panel();
        CHECK(placeholder.id() == "placeholder");
        CHECK(placeholder.has_command("placeholder.refresh"));
        const std::vector<std::string> order = focus_order(placeholder);
        CHECK(order.size() == 1);
        CHECK(order[0] == "placeholder.refresh");
    }

    UITREE_TEST_MAIN_END();
}
