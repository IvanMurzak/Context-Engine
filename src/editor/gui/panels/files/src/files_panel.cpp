// Files observer panel: project file-tree projection into a headless uitree Panel + selection +
// focus + settle/stability handling. Mirrors scene_tree_panel.cpp; see files_panel.h for the
// per-subject differences (no L-37 identity hash — a path is its own stable identity).

#include "context/editor/gui/panels/files/files_panel.h"

#include "context/editor/gui/panels/files/files_model.h"
#include "context/editor/gui/uitree/node.h"

#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::panels::files
{

namespace
{

// The accessible name + visible text base for one row: the display name plus a visible kind
// annotation for a directory, so the hierarchy is legible to sighted AND assistive-tech users.
[[nodiscard]] std::string row_label(const FileNode& node)
{
    std::string label = node.display_name.empty() ? node.identity : node.display_name;
    if (node.kind == FileNodeKind::directory)
    {
        label += " (folder)";
    }
    else if (!node.asset_kind.empty())
    {
        label += " (" + node.asset_kind + ")";
    }
    return label;
}

// Build one tree row (and its subtree). Every FILE row is a focusable, labelled treeitem bound to
// the select command (R-A11Y-001 / R-CLI-001); a DIRECTORY row is a grouping node only — it has no
// row identity to select (files/e2 is the write half that would let a human act on a folder).
// `expose_command` is false only when the panel exposes no command (an empty tree) — a bound-but-
// unexposed command would be an a11y orphan.
[[nodiscard]] uitree::UiNode build_row(const FileNode& node, const std::string& selected,
                                       bool expose_command)
{
    using uitree::Role;
    using uitree::UiNode;

    const bool selectable = node.kind == FileNodeKind::file;
    UiNode item(Role::treeitem, "files.item." + node.identity);
    const std::string label = row_label(node);
    item.set_label(label);
    item.set_focusable(true);
    if (selectable && expose_command)
    {
        item.set_command(kSelectCommand);
    }

    std::string text = label;
    if (selectable && node.identity == selected)
    {
        text += " (selected)";
    }
    item.set_text(text);

    if (!node.children.empty())
    {
        UiNode group(Role::group, "files.group." + node.identity);
        for (const FileNode& child : node.children)
        {
            group.add_child(build_row(child, selected, expose_command));
        }
        item.add_child(std::move(group));
    }
    return item;
}

} // namespace

void FilesPanel::set_model(FilesModel model)
{
    model_ = std::move(model);
    // Unlike the Scene tree, a file path needs no hash re-resolution on refresh — the path IS the
    // stable identity, so nothing here notifies listeners; a row simply renders "(selected)" or not
    // depending on whether the CURRENT model still has it, computed fresh at build_panel() time.
}

void FilesPanel::on_derivation_settled(std::uint64_t generation, bridge::Stability stability)
{
    generation_ = generation;
    stability_ = stability;
}

bool FilesPanel::select(const std::string& identity)
{
    // A row that is not in the rendered model (or is a directory) is a dead click — refuse locally
    // rather than asking the daemon to select something this panel cannot even name.
    const FileNode* node = find_node(model_, identity);
    if (node == nullptr || node->kind != FileNodeKind::file)
    {
        return false;
    }
    return write_selection(std::vector<std::string>{identity});
}

bool FilesPanel::clear_selection()
{
    return write_selection(std::vector<std::string>{});
}

bool FilesPanel::write_selection(const std::vector<std::string>& ids)
{
    if (gateway_ == nullptr)
    {
        return false;
    }
    const std::optional<std::vector<std::string>> applied = gateway_->request_selection(ids);
    if (!applied.has_value())
    {
        return false;
    }
    (void)apply_selection(*applied); // idempotent when the daemon reports no change
    return true;
}

bool FilesPanel::apply_selection(const std::vector<std::string>& ids)
{
    FileSelection next;
    if (!ids.empty())
    {
        next.identity = ids.front();
    }
    if (next.identity == selection_.identity)
    {
        return false; // the daemon restated what is already rendered — no listener churn
    }
    selection_ = std::move(next);
    notify();
    return true;
}

void FilesPanel::add_selection_listener(SelectionListener listener)
{
    listeners_.push_back(std::move(listener));
}

void FilesPanel::set_focused(bool focused)
{
    focused_ = focused;
}

void FilesPanel::notify() const
{
    for (const SelectionListener& listener : listeners_)
    {
        if (listener)
        {
            listener(selection_);
        }
    }
}

uitree::Panel FilesPanel::build_panel() const
{
    using uitree::Role;
    using uitree::UiNode;

    // `file_count` (not `!roots.empty()`): a tree of directories with no FILE row anywhere would
    // otherwise advertise a command bound to nothing — an a11y orphan (see build_row: only a FILE
    // row ever calls set_command). file_count already counts exactly the selectable rows.
    const bool has_selectable_rows = model_.file_count > 0;

    uitree::Panel panel("files", "Files");
    if (has_selectable_rows)
    {
        panel.add_command(kSelectCommand, "Select file");
    }

    UiNode root(Role::region, "files.panel");
    root.set_label("Files");

    root.add_child(UiNode(Role::heading, "files.heading").set_label("Files").set_text("Files"));

    std::ostringstream status;
    status << bridge::stability_name(stability_) << " - generation " << generation_ << " - "
           << model_.file_count << " files";
    if (!model_.ok)
    {
        status << " - incomplete";
    }
    if (focused_)
    {
        status << " - focused";
    }
    root.add_child(UiNode(Role::status, "files.status")
                       .set_label("File tree status")
                       .set_text(status.str()));

    UiNode tree(Role::tree, "files.tree");
    tree.set_label("Project files");
    for (const FileNode& node : model_.roots)
    {
        tree.add_child(build_row(node, selection_.identity, has_selectable_rows));
    }
    root.add_child(std::move(tree));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::panels::files
