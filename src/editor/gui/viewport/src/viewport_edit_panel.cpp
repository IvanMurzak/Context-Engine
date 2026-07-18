// The in-context viewport override-editing panel — see viewport_edit_panel.h. Projects the model into
// a headless uitree Panel: the selected composed entity, the gizmo + write-target affordances, the
// R-CLI-006 provenance chain at the point of edit, and the gesture-end commit — all keyboard-complete.

#include "context/editor/gui/viewport/viewport_edit_panel.h"

#include "context/editor/gui/uitree/node.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::viewport
{

namespace
{

using uitree::Role;
using uitree::UiNode;

// The grep-stable provenance source token (mirrors compose::provenance_json's source_name — the
// R-CLI-006 read-side vocabulary the `context query` provenance surface emits).
[[nodiscard]] const char* source_token(compose::ProvenanceEntry::Source source)
{
    switch (source)
    {
    case compose::ProvenanceEntry::Source::schema_default:
        return "schemaDefault";
    case compose::ProvenanceEntry::Source::template_value:
        return "template";
    case compose::ProvenanceEntry::Source::override_value:
        return "override";
    }
    return "template";
}

// One provenance-chain line: "<source> - <file> #<pointer> (level N)" — which template supplied the
// value and which instancing level overrode it (R-CLI-006, rendered at the point of edit).
[[nodiscard]] std::string provenance_line(const compose::ProvenanceEntry& entry)
{
    std::ostringstream out;
    out << source_token(entry.source) << " - " << entry.file;
    if (!entry.pointer.empty())
    {
        out << " #" << entry.pointer;
    }
    out << " (level " << entry.level << ")";
    return out.str();
}

// A focusable, command-bound affordance button. Marks the active choice in its accessible name so the
// current gizmo / target is legible without color alone (R-A11Y-001).
[[nodiscard]] UiNode affordance(const std::string& id, const std::string& title, const char* command,
                                bool active)
{
    const std::string label = active ? (title + " (active)") : title;
    return UiNode(Role::button, id)
        .set_label(label)
        .set_text(label)
        .set_focusable(true)
        .set_command(command);
}

} // namespace

uitree::Panel ViewportEditPanel::build_panel() const
{
    uitree::Panel panel("viewport-edit", "Viewport Edit");

    UiNode root(Role::region, "viewport-edit.panel");
    root.set_label("Viewport Edit");
    root.add_child(UiNode(Role::heading, "viewport-edit.heading")
                       .set_label("Viewport Edit")
                       .set_text("Viewport Edit"));
    root.add_child(UiNode(Role::status, "viewport-edit.status")
                       .set_label("Viewport edit status")
                       .set_text(model_.status_text()));

    // No selection: an a11y-clean placeholder with NO exposed command and no focusable widget (nothing
    // to manipulate until an entity is selected — the R-HUX-006 in-context editing surface is inert).
    if (!model_.has_selection())
    {
        UiNode hint(Role::region, "viewport-edit.empty");
        hint.set_label("No entity selected");
        hint.add_child(UiNode(Role::text, "viewport-edit.empty.desc")
                           .set_text("Select a composed entity in the viewport to edit its overrides."));
        root.add_child(std::move(hint));
        panel.set_root(std::move(root));
        return panel;
    }

    // --- the gizmo affordances (R-HUX-006 move/rotate/scale) -------------------------------------
    panel.add_command(kGizmoMoveCommand, "Move gizmo");
    panel.add_command(kGizmoRotateCommand, "Rotate gizmo");
    panel.add_command(kGizmoScaleCommand, "Scale gizmo");
    UiNode gizmos(Role::group, "viewport-edit.gizmos");
    gizmos.add_child(affordance("viewport-edit.gizmo-move", "Move", kGizmoMoveCommand,
                                model_.gizmo() == Gizmo::move));
    gizmos.add_child(affordance("viewport-edit.gizmo-rotate", "Rotate", kGizmoRotateCommand,
                                model_.gizmo() == Gizmo::rotate));
    gizmos.add_child(affordance("viewport-edit.gizmo-scale", "Scale", kGizmoScaleCommand,
                                model_.gizmo() == Gizmo::scale));
    root.add_child(std::move(gizmos));

    // --- the write-target affordances (R-CLI-006 retarget: outermost / template / mid-level) ------
    panel.add_command(kTargetOutermostCommand, "Write to outermost scene");
    panel.add_command(kTargetTemplateCommand, "Write to defining template");
    panel.add_command(kTargetAtInstanceCommand, "Write to instancing scene");
    UiNode targets(Role::group, "viewport-edit.targets");
    targets.add_child(affordance("viewport-edit.target-outermost", "Override (outermost)",
                                 kTargetOutermostCommand,
                                 model_.edit_target() == EditTarget::outermost));
    targets.add_child(affordance("viewport-edit.target-template", "Edit template",
                                 kTargetTemplateCommand,
                                 model_.edit_target() == EditTarget::edit_template));
    targets.add_child(affordance("viewport-edit.target-at-instance", "At instance",
                                 kTargetAtInstanceCommand,
                                 model_.edit_target() == EditTarget::at_instance));
    root.add_child(std::move(targets));

    // --- the edited field + its provenance chain, rendered AT THE POINT OF EDIT (R-CLI-006) -------
    const std::string pointer = model_.gizmo_pointer();
    UiNode field(Role::region, "viewport-edit.field");
    const bool overridden = model_.overridden_at(pointer);
    std::string field_label = std::string("Field ") + pointer + (overridden ? " (overridden)" : "");
    field.set_label(field_label);
    field.add_child(UiNode(Role::text, "viewport-edit.field.desc").set_text(field_label));

    UiNode provenance(Role::region, "viewport-edit.provenance");
    provenance.set_label("Provenance");
    const std::vector<compose::ProvenanceEntry> chain = model_.provenance(pointer);
    if (chain.empty())
    {
        provenance.add_child(UiNode(Role::text, "viewport-edit.provenance.none")
                                 .set_text("No composed value at this field."));
    }
    else
    {
        std::size_t i = 0;
        for (const compose::ProvenanceEntry& entry : chain)
        {
            provenance.add_child(
                UiNode(Role::text, "viewport-edit.provenance." + std::to_string(i))
                    .set_text(provenance_line(entry)));
            ++i;
        }
    }
    field.add_child(std::move(provenance));
    root.add_child(std::move(field));

    // --- the gesture-end commit affordance (L-20: commit at gesture end) --------------------------
    panel.add_command(kCommitCommand, "Commit edit");
    root.add_child(UiNode(Role::button, "viewport-edit.commit")
                       .set_label("Commit edit")
                       .set_text("Commit edit")
                       .set_focusable(true)
                       .set_command(kCommitCommand));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::viewport
