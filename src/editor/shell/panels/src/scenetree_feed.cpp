// The live scene-tree feed implementation (see scenetree_feed.h for the design + tolerance
// rationale). The parsers mirror builders::scene_tree_to_wire member-for-member; the feed tests
// link both halves and assert the round-trip.

#include "context/editor/shell/panels/scenetree_feed.h"

#include "context/editor/shell/panels/builtin_panels.h" // kDerivationTopic
#include "context/editor/shell/panels/problems_feed.h"  // kDerivationSettledEvent + parse_stability
#include "wire_read.h"                                  // read_string / read_bool / read_u64 / envelope_data

#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::shell::panels
{

namespace
{

// One wire node -> one model node. nullopt when the entry is not an object or carries no identity
// (a node the panel could neither select nor key a patch by). Children recurse; unparseable
// children are skipped, never fatal.
[[nodiscard]] std::optional<scenetree::SceneTreeNode> parse_node(const contract::Json& wire)
{
    if (!wire.is_object())
    {
        return std::nullopt;
    }
    scenetree::SceneTreeNode node;
    node.identity = read_string(wire, "identity");
    if (node.identity.empty())
    {
        return std::nullopt;
    }
    node.identity_hash = parse_hex_u64(read_string(wire, "identityHash"));
    node.display_name = read_string(wire, "displayName");
    // Anything that is not the literal "instance" reads as an entity: the entity kind is the safe
    // default (selectable, hash-bearing), and an unknown future token must not invent a THIRD kind.
    node.kind = read_string(wire, "kind") == "instance" ? scenetree::NodeKind::instance
                                                        : scenetree::NodeKind::entity;
    node.overridden = read_bool(wire, "overridden");
    const contract::Json& children = wire.at("children"); // at() is total: null when absent
    if (children.is_array())
    {
        for (std::size_t i = 0; i < children.size(); ++i)
        {
            if (std::optional<scenetree::SceneTreeNode> child = parse_node(children.at(i)))
            {
                node.children.push_back(std::move(*child));
            }
        }
    }
    return node;
}

} // namespace

// --------------------------------------------------------------------------------- pure parsers

std::uint64_t parse_hex_u64(const std::string& text)
{
    if (text.empty() || text.size() > 16)
    {
        return 0; // more than 16 nibbles cannot be a u64; refuse rather than wrap
    }
    std::uint64_t out = 0;
    for (const char c : text)
    {
        std::uint64_t nibble = 0;
        if (c >= '0' && c <= '9')
        {
            nibble = static_cast<std::uint64_t>(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            nibble = static_cast<std::uint64_t>(c - 'a') + 10;
        }
        else
        {
            return 0; // not lowercase hex -> not ours
        }
        out = (out << 4) | nibble;
    }
    return out;
}

std::optional<scenetree::SceneTreeModel> parse_scene_tree(const contract::Json& wire)
{
    const contract::Json& roots = wire.at("roots"); // at() is total: null when absent / non-object
    if (!roots.is_array())
    {
        // NO RECOGNIZED CONTAINER = NO INFORMATION — not the same as "an empty tree". The caller
        // keeps the current model rather than clearing it on a reply that said nothing.
        return std::nullopt;
    }
    scenetree::SceneTreeModel model;
    model.root_scene = read_string(wire, "rootScene");
    model.ok = !wire.contains("ok") || wire.at("ok").as_bool(); // ABSENT means ok — contains() matters
    for (std::size_t i = 0; i < roots.size(); ++i)
    {
        if (std::optional<scenetree::SceneTreeNode> node = parse_node(roots.at(i)))
        {
            model.roots.push_back(std::move(*node));
        }
    }
    // entityCount is authoritative from the wire when present (the daemon counted the COMPOSED
    // entities, which synthetic instance boundaries under-count locally); left at the model's zero
    // default otherwise, so the status line never claims a count nothing sent.
    // read_u64 carries the range check (an out-of-range double cast is UBSan `float-cast-overflow`
    // on an untrusted wire — wire_read.h) and degrades to the model's zero default, which is exactly
    // this field's "nothing sent" value.
    model.entity_count = static_cast<std::size_t>(read_u64(wire, "entityCount"));
    return model;
}

std::optional<std::string> scenetree_row_identity(const std::string& node_id)
{
    constexpr std::string_view prefix = kSceneTreeRowPrefix;
    if (node_id.size() <= prefix.size() || !std::string_view(node_id).starts_with(prefix))
    {
        return std::nullopt;
    }
    return node_id.substr(prefix.size());
}

// ------------------------------------------------------------------------------------ the feed

SceneTreeFeed::SceneTreeFeed(PanelHost& host, std::string panel_id,
                             scenetree::SelectionGateway* selection_gateway)
    : host_(host), panel_id_(std::move(panel_id)), panel_(selection_gateway)
{
}

bool SceneTreeFeed::apply_result(const contract::Json& reply)
{
    // Envelope tolerance: {result-envelope {data: {sceneTree}}} / {data:{sceneTree}} / {sceneTree}
    // / the bare tree. The FIRST recognized shape wins. The `data` hop is the shared one
    // (wire_read.h); the `sceneTree` hop is this feed's own, because which key to look for is policy.
    const contract::Json* wire = &envelope_data(reply);
    const contract::Json& nested_tree = wire->at("sceneTree");
    if (nested_tree.is_object())
    {
        wire = &nested_tree;
    }
    std::optional<scenetree::SceneTreeModel> model = parse_scene_tree(*wire);
    if (!model.has_value())
    {
        return false;
    }
    panel_.set_model(std::move(*model));
    ++results_applied_;
    host_.touch(panel_id_);
    return true;
}

bool SceneTreeFeed::apply_event(const std::string& topic, const contract::Json& payload,
                                std::uint64_t generation)
{
    if (topic != kDerivationTopic || read_string(payload, "event") != kDerivationSettledEvent)
    {
        return false;
    }
    // The settle advances the status line NOW (cheap, local) and schedules the re-read (the shape
    // may have changed; only the daemon knows). parse_stability is the problems feed's — the ONE
    // stability-token table.
    panel_.on_derivation_settled(generation, parse_stability(read_string(payload, "stability")));
    fetch_due_ = true;
    ++events_applied_;
    host_.touch(panel_id_);
    return true;
}

PanelProvider SceneTreeFeed::make_provider()
{
    PanelProvider provider;
    provider.build = [this] { return panel_.build_panel(); };
    provider.invoke = [this](const std::string& command_id, const contract::Json& params)
    {
        if (command_id != scenetree::kSelectCommand)
        {
            return false;
        }
        // The hydration runtime sends the ACTIVATED NODE's id — it knows nothing about scene
        // identities. `scenetree_row_identity` is the translation that keeps it that way.
        //
        // e08b: select() is now a WRITE to the daemon, so what comes back is "the daemon applied it",
        // not "the panel decided". The panel then renders the write's REPLY — NOT the
        // `selection-changed` fact it publishes, which carries our own `origin` and is dropped by
        // SessionFeed's echo suppression (scene_tree_panel.h: reading it the other way round yields a
        // panel that shows every OTHER client's selection and never its own). Applying that reply is
        // what fires the selection listeners the Inspector feed hydrates from (R-HUX-011), so that
        // loop now runs on daemon truth instead of a local decision.
        const std::optional<std::string> identity =
            scenetree_row_identity(read_string(params, "nodeId"));
        return identity.has_value() && panel_.select(*identity);
    };
    // No gesture, no state pair: a read-only observer with selection (see the header).
    return provider;
}

} // namespace context::editor::shell::panels
