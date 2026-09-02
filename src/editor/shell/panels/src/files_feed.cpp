// The live files feed implementation (see files_feed.h for the design + tolerance rationale). The
// parsers mirror builders::files_to_wire member-for-member; the feed tests link both halves and
// assert the round-trip.

#include "context/editor/shell/panels/files_feed.h"

#include "context/editor/shell/panels/builtin_panels.h" // kDerivationTopic
#include "context/editor/shell/panels/problems_feed.h"  // kDerivationSettledEvent
#include "wire_read.h" // read_string / read_bool / read_u64 / envelope_data

#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::shell::panels
{

namespace
{

// One wire node -> one model node. nullopt when the entry is not an object or carries no identity
// (a node the panel could neither select nor key by). Children recurse; unparseable children are
// skipped, never fatal — the ProblemsFeed / scenetree_feed tolerance discipline.
[[nodiscard]] std::optional<files::FileNode> parse_node(const contract::Json& wire)
{
    if (!wire.is_object())
    {
        return std::nullopt;
    }
    files::FileNode node;
    node.identity = read_string(wire, "identity");
    if (node.identity.empty())
    {
        return std::nullopt;
    }
    node.display_name = read_string(wire, "displayName");
    // Anything that is not the literal "directory" reads as a file: file is the safe default
    // (selectable), and an unknown future token must not invent a THIRD kind.
    node.kind = read_string(wire, "kind") == "directory" ? files::FileNodeKind::directory
                                                          : files::FileNodeKind::file;
    node.guid = read_string(wire, "guid");
    node.asset_kind = read_string(wire, "assetKind");
    const contract::Json& children = wire.at("children"); // at() is total: null when absent
    if (children.is_array())
    {
        for (std::size_t i = 0; i < children.size(); ++i)
        {
            if (std::optional<files::FileNode> child = parse_node(children.at(i)))
            {
                node.children.push_back(std::move(*child));
            }
        }
    }
    return node;
}

} // namespace

// --------------------------------------------------------------------------------- pure parsers

std::optional<files::FilesModel> parse_files(const contract::Json& wire)
{
    const contract::Json& roots = wire.at("roots"); // at() is total: null when absent / non-object
    if (!roots.is_array())
    {
        // NO RECOGNIZED CONTAINER = NO INFORMATION — not the same as "an empty project". The caller
        // keeps the current model rather than clearing it on a reply that said nothing.
        return std::nullopt;
    }
    files::FilesModel model;
    model.ok = !wire.contains("ok") || wire.at("ok").as_bool(); // ABSENT means ok — contains() matters
    for (std::size_t i = 0; i < roots.size(); ++i)
    {
        if (std::optional<files::FileNode> node = parse_node(roots.at(i)))
        {
            model.roots.push_back(std::move(*node));
        }
    }
    // fileCount is authoritative from the wire when present (it counts only FILE rows, which a local
    // recount over unparsed children could under/over-state); left at the model's zero default
    // otherwise, so the status line never claims a count nothing sent.
    model.file_count = static_cast<std::size_t>(read_u64(wire, "fileCount"));
    return model;
}

std::optional<std::string> files_row_identity(const std::string& node_id)
{
    constexpr std::string_view prefix = kFilesRowPrefix;
    if (node_id.size() <= prefix.size() || !std::string_view(node_id).starts_with(prefix))
    {
        return std::nullopt;
    }
    return node_id.substr(prefix.size());
}

// ------------------------------------------------------------------------------------ the feed

FilesFeed::FilesFeed(PanelHost& host, std::string panel_id, files::SelectionGateway* selection_gateway)
    : host_(host), panel_id_(std::move(panel_id)), panel_(selection_gateway)
{
    // The ONE write listener, installed here rather than by the composition root, so a FilesFeed is
    // never half-wired: every construction path (including the T1 suite's) gets the same fan-out,
    // and the sinks it fans out TO are optional (see on_panel_write).
    panel_.add_write_listener([this](files::FileWriteVerb verb, const files::FileWriteResult& result)
                              { on_panel_write(verb, result); });
}

void FilesFeed::bind_write_gateway(files::FileWriteGateway* gateway)
{
    panel_.set_write_gateway(gateway);
    // The authoring commands appear/disappear with the gateway, so the rendered surface CHANGED.
    host_.touch(panel_id_);
}

void FilesFeed::bind_checkpoint_sink(CheckpointSink sink)
{
    checkpoints_ = std::move(sink);
}

void FilesFeed::bind_notice_sink(NoticeSink sink)
{
    notices_ = std::move(sink);
}

void FilesFeed::on_panel_write(files::FileWriteVerb verb, const files::FileWriteResult& result)
{
    // The Shell's own prose for the human-facing `action` (write_notice.h: the action crosses as
    // prose, not as a pinned token, precisely so a new caller cannot introduce a silent drift).
    const char* action = "delete";
    switch (verb)
    {
    case files::FileWriteVerb::move:
        action = "rename";
        break;
    case files::FileWriteVerb::remove:
        action = "delete";
        break;
    case files::FileWriteVerb::restore:
        action = "restore";
        break;
    }

    if (!result.ok())
    {
        ++writes_refused_;
        if (notices_)
        {
            notices_(action, result);
        }
        // NO refetch: nothing was written, so the rendered tree is still current. The panel itself
        // re-renders anyway (its own status line now names the refusal), which is why the host is
        // touched on BOTH arms.
        host_.touch(panel_id_);
        return;
    }

    ++writes_landed_;
    if (checkpoints_)
    {
        undo::FileEdit edit;
        // Whether this operation becomes an undo STEP is a separate question from whether it
        // happened — the refetch below is unconditional either way, because EVERY landed operation
        // changes the tree this panel is rendering (a restore most of all: it brings back a row that
        // is by definition absent from the current model).
        bool journal = true;
        switch (verb)
        {
        case files::FileWriteVerb::move:
            edit.op = undo::FileEdit::Op::move;
            edit.from = result.path;
            edit.to = result.other_path;
            break;
        case files::FileWriteVerb::remove:
            edit.op = undo::FileEdit::Op::remove;
            edit.from = result.path;
            edit.restore_token = result.restore_token;
            // An applied DELETE with no restore token is not reversible (wire_file_gateway.h states
            // when that is reachable). Recording it would offer the human an undo guaranteed to
            // refuse; not recording it costs them one history entry. The second is the honest
            // failure.
            journal = !edit.restore_token.empty();
            break;
        case files::FileWriteVerb::restore:
            // A RESTORE is never journaled, and that is deliberate rather than an omission:
            // recording the inverse of an undo as a NEW step would make Ctrl+Z followed by Ctrl+Z
            // undo itself forever. Note the journal's own replay does NOT arrive here at all — it
            // calls the gateway directly (files_panel.h § restore) — so this arm covers a
            // panel-issued restore, which nothing exposes yet.
            journal = false;
            break;
        }
        if (journal)
        {
            checkpoints_(std::move(edit), std::string(action) + " " + result.path);
        }
    }
    // READ-YOUR-WRITES: the write went out over the gateway, not through anything that re-hydrates
    // this panel, so the tree it is rendering is stale by exactly the row the human acted on.
    fetch_due_ = true;
    host_.touch(panel_id_);
}

bool FilesFeed::apply_result(const contract::Json& reply)
{
    // Envelope tolerance: {result-envelope {data: {files}}} / {data:{files}} / {files} / the bare
    // tree. The FIRST recognized shape wins. The `data` hop is the shared one (wire_read.h); the
    // `files` hop is this feed's own, because which key to look for is policy.
    const contract::Json* wire = &envelope_data(reply);
    const contract::Json& nested_files = wire->at("files");
    if (nested_files.is_object())
    {
        wire = &nested_files;
    }
    std::optional<files::FilesModel> model = parse_files(*wire);
    if (!model.has_value())
    {
        return false;
    }
    panel_.set_model(std::move(*model));
    ++results_applied_;
    host_.touch(panel_id_);
    return true;
}

bool FilesFeed::apply_event(const std::string& topic, const contract::Json& payload,
                            std::uint64_t /*generation*/)
{
    if (topic != kDerivationTopic || read_string(payload, "event") != kDerivationSettledEvent)
    {
        return false;
    }
    // Unlike the Scene tree, the file tree has no generation/stability status line to advance —
    // `editor.files` is a plain list, not a composed-world read. The settle only schedules a
    // re-read: a write elsewhere (including this window's own) can create/move/delete a file, and
    // only the daemon knows the current shape.
    fetch_due_ = true;
    ++events_applied_;
    host_.touch(panel_id_);
    return true;
}

PanelProvider FilesFeed::make_provider()
{
    PanelProvider provider;
    provider.build = [this] { return panel_.build_panel(); };
    provider.invoke = [this](const std::string& command_id, const contract::Json& params)
    {
        if (command_id == files::kSelectCommand)
        {
            // The hydration runtime sends the ACTIVATED NODE's id — it knows nothing about file
            // identities. `files_row_identity` is the translation that keeps it that way.
            //
            // M9 e1, mirrors SceneTreeFeed::make_provider's invoke: select() is a WRITE to the daemon
            // (subject "file"), so what comes back is "the daemon applied it", not "the panel
            // decided".
            const std::optional<std::string> identity =
                files_row_identity(read_string(params, "nodeId"));
            return identity.has_value() && panel_.select(*identity);
        }

        // M9 e2: the authoring commands. Their SUBJECT is the panel's current selection rather than
        // a node id — a rename dialog's confirmation carries the new name, not the row it came from,
        // and the row the human is acting on is by definition the selected one (build_panel only
        // exposes these commands when a file row IS selected).
        const std::string& subject = panel_.selection().identity;
        if (command_id == files::kRenameCommand)
        {
            return panel_.rename(subject, read_string(params, "name"));
        }
        if (command_id == files::kMoveCommand)
        {
            return panel_.move(subject, read_string(params, "destination"));
        }
        if (command_id == files::kDeleteCommand)
        {
            // NO confirmation gate here, deliberately: confirming a destructive action is the
            // RENDERER's job (it owns the dialog, the copy and the a11y naming), and a second
            // C++-side prompt would be an un-dismissable one for a scripted or CLI caller that has
            // already decided. What this layer owes the human instead is reversibility and a loud
            // answer, which is exactly what it provides.
            return panel_.remove(subject);
        }
        return false;
    };
    // No gesture, no state pair: the undo STEP a file operation produces is persisted by the
    // journal, which owns that seam (see the header).
    return provider;
}

} // namespace context::editor::shell::panels
