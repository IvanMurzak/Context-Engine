// Built-in placeholder panel factory.

#include "context/editor/gui/uitree/builtin.h"

#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"

namespace context::editor::gui::uitree
{

Panel make_placeholder_panel()
{
    // <section role=region aria-label="Context Editor">
    //   <h2>Context Editor</h2>
    //   <output role=status aria-label="Status">Editor host online</output>
    //   <button aria-label="Refresh" tabindex=0 data-command="placeholder.refresh">Refresh</button>
    // </section>
    UiNode root(Role::region, "placeholder.panel");
    root.set_label("Context Editor");
    root.add_child(UiNode(Role::heading, "placeholder.heading")
                       .set_label("Context Editor")
                       .set_text("Context Editor"));
    root.add_child(UiNode(Role::status, "placeholder.status")
                       .set_label("Status")
                       .set_text("Editor host online"));
    root.add_child(UiNode(Role::button, "placeholder.refresh")
                       .set_label("Refresh")
                       .set_text("Refresh")
                       .set_focusable(true)
                       .set_command("placeholder.refresh"));

    Panel panel("placeholder", "Context Editor");
    panel.set_root(std::move(root));
    panel.add_command("placeholder.refresh", "Refresh");
    return panel;
}

} // namespace context::editor::gui::uitree
