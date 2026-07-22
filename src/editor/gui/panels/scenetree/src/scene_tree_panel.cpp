// Scene-tree observer panel: derived-world projection into a headless uitree Panel + selection +
// settle/stability handling.

#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"
#include "context/editor/gui/uitree/node.h"

#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::panels::scenetree
{

namespace
{

// The accessible name + visible text base for one row: the display name plus visible kind/override
// annotations, so instances and overrides (L-35) are legible to sighted AND assistive-tech users.
// Independent of selection, so the accessible name does not churn when the selection moves.
[[nodiscard]] std::string row_label(const SceneTreeNode& node)
{
    std::string label = node.display_name.empty() ? node.identity : node.display_name;
    if (node.kind == NodeKind::instance)
    {
        label += " (instance)";
    }
    if (node.overridden)
    {
        label += " (overridden)";
    }
    return label;
}

// Build one tree row (and its subtree). Every row is a focusable, labelled treeitem bound to the
// select command, so selection has a keyboard path (R-A11Y-001 / R-CLI-001). `expose_command` is
// false only when the panel exposes no command (an empty tree) — a bound-but-unexposed command would
// be an a11y orphan. The selected row carries a visible "(selected)" marker.
[[nodiscard]] uitree::UiNode build_row(const SceneTreeNode& node, const std::string& selected,
                                       bool expose_command)
{
    using uitree::Role;
    using uitree::UiNode;

    UiNode item(Role::treeitem, "scenetree.item." + node.identity);
    const std::string label = row_label(node);
    item.set_label(label);
    item.set_focusable(true);
    if (expose_command)
    {
        item.set_command(kSelectCommand);
    }

    std::string text = label;
    if (node.identity == selected)
    {
        text += " (selected)";
    }
    item.set_text(text);

    if (!node.children.empty())
    {
        UiNode group(Role::group, "scenetree.group." + node.identity);
        for (const SceneTreeNode& child : node.children)
        {
            group.add_child(build_row(child, selected, expose_command));
        }
        item.add_child(std::move(group));
    }
    return item;
}

} // namespace

void SceneTreePanel::set_model(SceneTreeModel model)
{
    model_ = std::move(model);
    if (selection_.identity.empty())
    {
        return;
    }
    // Selection is DAEMON state (e08b): a refresh re-resolves only the L-37 identity hash against the
    // new world. A selected identity with no row here keeps its place in the daemon's selection and
    // simply renders unmarked (hash 0 — the model's "no hash" value), because a node missing from
    // THIS panel's view is not the daemon deselecting it.
    const SceneTreeNode* node = find_node(model_, selection_.identity);
    const std::uint64_t resolved = node != nullptr ? node->identity_hash : 0;
    if (resolved != selection_.identity_hash)
    {
        selection_.identity_hash = resolved;
        notify();
    }
}

void SceneTreePanel::on_derivation_settled(std::uint64_t generation, bridge::Stability stability)
{
    generation_ = generation;
    stability_ = stability;
}

bool SceneTreePanel::select(const std::string& identity)
{
    // A row that is not in the rendered model is a dead click — refuse locally rather than asking the
    // daemon to select something this panel cannot even name. The "no gateway bound" case is
    // write_selection's, so it is not restated here.
    if (find_node(model_, identity) == nullptr)
    {
        return false;
    }
    return write_selection(std::vector<std::string>{identity});
}

bool SceneTreePanel::clear_selection()
{
    return write_selection(std::vector<std::string>{});
}

bool SceneTreePanel::write_selection(const std::vector<std::string>& ids)
{
    if (gateway_ == nullptr)
    {
        return false;
    }
    // The panel decides NOTHING: it asks, and renders what the daemon answers. A refusal renders
    // nothing at all. The echo of this write arrives later on the `session` topic stamped with our
    // own `origin` and is dropped one layer up — so this reply is the ONLY path by which our own
    // selection reaches the screen (see the header note).
    const std::optional<std::vector<std::string>> applied = gateway_->request_selection(ids);
    if (!applied.has_value())
    {
        return false;
    }
    (void)apply_selection(*applied); // idempotent when the daemon reports no change
    return true;
}

bool SceneTreePanel::apply_selection(const std::vector<std::string>& ids)
{
    // Single-select panel: the daemon's FIRST id is what renders. An empty list is a cleared
    // selection.
    SceneSelection next;
    if (!ids.empty())
    {
        next.identity = ids.front();
        if (const SceneTreeNode* node = find_node(model_, next.identity))
        {
            next.identity_hash = node->identity_hash;
        }
    }
    if (next.identity == selection_.identity && next.identity_hash == selection_.identity_hash)
    {
        return false; // the daemon restated what is already rendered — no listener churn
    }
    selection_ = std::move(next);
    notify();
    return true;
}

void SceneTreePanel::add_selection_listener(SelectionListener listener)
{
    listeners_.push_back(std::move(listener));
}

void SceneTreePanel::notify() const
{
    for (const SelectionListener& listener : listeners_)
    {
        if (listener)
        {
            listener(selection_);
        }
    }
}

uitree::Panel SceneTreePanel::build_panel() const
{
    using uitree::Role;
    using uitree::UiNode;

    const bool has_rows = !model_.roots.empty();

    uitree::Panel panel("scene-tree", "Scene Tree");
    if (has_rows)
    {
        panel.add_command(kSelectCommand, "Select node");
    }

    UiNode root(Role::region, "scenetree.panel");
    root.set_label("Scene Tree");

    root.add_child(UiNode(Role::heading, "scenetree.heading")
                       .set_label("Scene Tree")
                       .set_text("Scene Tree"));

    std::ostringstream status;
    status << bridge::stability_name(stability_) << " - generation " << generation_ << " - "
           << model_.entity_count << " entities";
    if (!model_.ok)
    {
        status << " - incomplete";
    }
    root.add_child(UiNode(Role::status, "scenetree.status")
                       .set_label("Derivation status")
                       .set_text(status.str()));

    UiNode tree(Role::tree, "scenetree.tree");
    tree.set_label("Scene hierarchy");
    for (const SceneTreeNode& node : model_.roots)
    {
        tree.add_child(build_row(node, selection_.identity, has_rows));
    }
    root.add_child(std::move(tree));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::panels::scenetree
