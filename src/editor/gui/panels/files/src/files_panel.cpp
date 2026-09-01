// Files panel: project file-tree projection into a headless uitree Panel + selection + focus +
// settle/stability handling, and (M9 e2) the D10 write half — rename / move / delete requested
// through the FileWriteGateway seam, with every outcome adopted and announced in one place.
// Mirrors scene_tree_panel.cpp; see files_panel.h for the per-subject differences (no L-37
// identity hash — a path is its own stable identity).

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

void FilesPanel::add_write_listener(WriteListener listener)
{
    write_listeners_.push_back(std::move(listener));
}

bool FilesPanel::settle_write(FileWriteVerb verb, FileWriteResult result)
{
    last_write_ = std::move(result);
    for (const WriteListener& listener : write_listeners_)
    {
        if (listener)
        {
            listener(verb, last_write_);
        }
    }
    return last_write_.ok();
}

bool FilesPanel::refuse_locally(FileWriteVerb verb, std::string code, std::string message,
                                std::string path)
{
    FileWriteResult refusal;
    refusal.status = FileWriteResult::Status::refused;
    refusal.code = std::move(code);
    refusal.message = std::move(message);
    refusal.path = std::move(path);
    return settle_write(verb, std::move(refusal));
}

bool FilesPanel::rename(const std::string& identity, const std::string& new_name)
{
    // A rename is a BASENAME change. A name carrying a separator is refused rather than
    // reinterpreted: silently treating "a/b" as a move would relocate the human's file into a
    // directory they never named — on the one panel whose other verb is a delete.
    if (new_name.empty() || new_name.find('/') != std::string::npos ||
        new_name.find('\\') != std::string::npos || new_name == "." || new_name == "..")
    {
        return refuse_locally(FileWriteVerb::move, kInvalidRequestCode,
                              "a rename takes a file NAME, not a path — use move to relocate a file",
                              identity);
    }
    const std::size_t slash = identity.rfind('/');
    const std::string destination =
        slash == std::string::npos ? new_name : identity.substr(0, slash + 1) + new_name;
    if (destination == identity)
    {
        return refuse_locally(FileWriteVerb::move, kInvalidRequestCode,
                              "the file already has that name", identity);
    }
    return move(identity, destination);
}

bool FilesPanel::move(const std::string& identity, const std::string& destination)
{
    if (const FileNode* node = find_node(model_, identity);
        node == nullptr || node->kind != FileNodeKind::file)
    {
        // The dead-click refusal `select()` already makes, for the same reason: asking the write
        // path to move a row this panel cannot even name is a request nobody can answer usefully.
        return refuse_locally(FileWriteVerb::move, kInvalidRequestCode,
                              "no file row with that identity is loaded in this panel", identity);
    }
    if (destination.empty())
    {
        return refuse_locally(FileWriteVerb::move, kInvalidRequestCode,
                              "a move needs a destination path", identity);
    }
    if (writes_ == nullptr)
    {
        return refuse_locally(FileWriteVerb::move, kNoWritePathCode,
                              "this build has no write path bound, so nothing was moved", identity);
    }
    return settle_write(FileWriteVerb::move, writes_->move_file(identity, destination));
}

bool FilesPanel::remove(const std::string& identity)
{
    if (const FileNode* node = find_node(model_, identity);
        node == nullptr || node->kind != FileNodeKind::file)
    {
        return refuse_locally(FileWriteVerb::remove, kInvalidRequestCode,
                              "no file row with that identity is loaded in this panel", identity);
    }
    if (writes_ == nullptr)
    {
        return refuse_locally(FileWriteVerb::remove, kNoWritePathCode,
                              "this build has no write path bound, so NOTHING was deleted",
                              identity);
    }
    return settle_write(FileWriteVerb::remove, writes_->delete_file(identity));
}

bool FilesPanel::restore(const std::string& restore_token)
{
    // Deliberately NOT gated on the model: a restore names a token, not a row, and the row it brings
    // back is by definition absent from the tree the panel is currently rendering.
    if (restore_token.empty())
    {
        return refuse_locally(FileWriteVerb::restore, kInvalidRequestCode,
                              "a restore needs the token the delete returned", "");
    }
    if (writes_ == nullptr)
    {
        return refuse_locally(FileWriteVerb::restore, kNoWritePathCode,
                              "this build has no write path bound, so nothing was restored", "");
    }
    return settle_write(FileWriteVerb::restore, writes_->restore_file(restore_token));
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

    // M9 e2: an authoring command is reachable only with a write path AND a selected file row to
    // act on. Both halves matter — a command exposed without a gateway would be an affordance that
    // silently does nothing, and one exposed with no selection would have no subject.
    const bool can_author =
        writes_ != nullptr && !selection_.identity.empty() &&
        find_node(model_, selection_.identity) != nullptr;

    uitree::Panel panel("files", "Files");
    if (has_selectable_rows)
    {
        panel.add_command(kSelectCommand, "Select file");
    }
    if (can_author)
    {
        panel.add_command(kRenameCommand, "Rename file");
        panel.add_command(kMoveCommand, "Move file");
        panel.add_command(kDeleteCommand, "Delete file");
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

    // THE LOUD HALF (design 10: destructive/lossy moments are never silent). The Shell also
    // broadcasts an `editor.ui.write-notice` for the same event, but a notice is transient and a
    // panel is where the human is already looking — so the last write outcome is rendered here as
    // its own live-region status node, and stays there. Absent before the first write, so a panel
    // that has authored nothing renders exactly what it did before e2.
    if (last_write_.status != FileWriteResult::Status::none)
    {
        std::ostringstream write_status;
        write_status << (last_write_.ok() ? "Last change: applied" : "Last change REFUSED");
        if (!last_write_.path.empty())
        {
            write_status << " - " << last_write_.path;
        }
        if (!last_write_.ok())
        {
            write_status << " - " << last_write_.code;
            if (!last_write_.message.empty())
            {
                write_status << ": " << last_write_.message;
            }
        }
        root.add_child(UiNode(Role::status, "files.write-status")
                           .set_label("Last file change")
                           .set_text(write_status.str()));
    }

    // The authoring affordances. Rendered as focusable buttons so every exposed command is backed by
    // a widget (audit_a11y reports an exposed-but-unreachable command as a violation).
    if (can_author)
    {
        UiNode actions(Role::group, "files.actions");
        actions.add_child(UiNode(Role::button, "files.button.rename")
                              .set_label("Rename file")
                              .set_text("Rename")
                              .set_focusable(true)
                              .set_command(kRenameCommand));
        actions.add_child(UiNode(Role::button, "files.button.move")
                              .set_label("Move file")
                              .set_text("Move")
                              .set_focusable(true)
                              .set_command(kMoveCommand));
        actions.add_child(UiNode(Role::button, "files.button.delete")
                              .set_label("Delete file")
                              .set_text("Delete")
                              .set_focusable(true)
                              .set_command(kDeleteCommand));
        root.add_child(std::move(actions));
    }

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
