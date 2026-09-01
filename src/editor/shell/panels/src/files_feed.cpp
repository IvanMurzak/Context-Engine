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
        if (command_id != files::kSelectCommand)
        {
            return false;
        }
        // The hydration runtime sends the ACTIVATED NODE's id — it knows nothing about file
        // identities. `files_row_identity` is the translation that keeps it that way.
        //
        // M9 e1, mirrors SceneTreeFeed::make_provider's invoke: select() is a WRITE to the daemon
        // (subject "file"), so what comes back is "the daemon applied it", not "the panel decided".
        const std::optional<std::string> identity = files_row_identity(read_string(params, "nodeId"));
        return identity.has_value() && panel_.select(*identity);
    };
    // No gesture, no state pair: a read-only observer with selection (see the header).
    return provider;
}

} // namespace context::editor::shell::panels
