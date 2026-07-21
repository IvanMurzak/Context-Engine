// T1 for the LIVE scene-tree feed (M9 e05d3): the kernel-side builder+wire -> Shell-side parser
// ROUND-TRIP (the two halves that must not drift), the envelope tolerance, the settle->refetch
// cadence, the node-id -> identity mapping, and the selection dispatch through the provider.
//
// This is the test that makes "the Scene tree hydrates from the LIVE daemon" assertable WITHOUT a
// daemon: the round-trip half drives the REAL builders (compose::flatten over an in-memory resolver
// -> builders::build_scene_tree -> builders::scene_tree_to_wire) and asserts the feed's parsers
// reconstruct the model, on all three default `build` legs. What is left for the live smoke is only
// that the wiring is connected — the ProblemsFeed split, applied to the e05d3 pair.

#include "context/editor/shell/panels/scenetree_feed.h"

#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/panels/builders/scene_tree_builder.h"
#include "context/editor/gui/panels/builders/wire.h"
#include "context/editor/serializer/json_parse.h"
#include "context/editor/shell/panel_host.h"

#include "panels_test.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace scenetree = context::editor::gui::panels::scenetree;
namespace builders = context::editor::gui::panels::builders;
namespace compose = context::editor::compose;
namespace gc = context::editor::gui::contract;
namespace bridge = context::editor::bridge;
namespace serializer = context::editor::serializer;
using Json = context::editor::contract::Json;

namespace
{

constexpr const char* kPanelId = "builtin.scene-tree";

// A lean in-memory SceneResolver over authored scene JSON strings (the m5-exit MapResolver shape).
class MapResolver final : public compose::SceneResolver
{
public:
    bool add(const std::string& path, const std::string& json)
    {
        serializer::ParseResult parsed = serializer::parse_json(json);
        if (!parsed.ok)
        {
            return false;
        }
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, parsed.root);
        if (!doc.has_value())
        {
            return false;
        }
        docs_.emplace(path, std::move(*doc));
        return true;
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        const auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// Root instances child + OVERRIDES the child camera's fov — instance boundary AND override marker
// must survive the wire (L-35 visibility).
void seed(MapResolver& r)
{
    CHECK(r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Cam",
         "components": {"camera": {"fov": 1.0}}}
      ]})"));
    CHECK(r.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [],
      "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"}],
      "overrides": [
        {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"],
         "pointer": "/components/camera/fov", "value": 1.4, "base": 1.0}
      ]})"));
}

[[nodiscard]] std::vector<gc::Contribution> roster_with_scenetree()
{
    gc::Contribution c;
    c.id = kPanelId;
    c.kind = gc::ContributionKind::panel;
    c.title = "Scene Tree";
    c.content.type = gc::ContentType::uitree;
    c.state.schema_version = 1;
    return {c};
}

[[nodiscard]] bool models_equal(const scenetree::SceneTreeNode& a, const scenetree::SceneTreeNode& b)
{
    if (a.identity != b.identity || a.identity_hash != b.identity_hash ||
        a.display_name != b.display_name || a.kind != b.kind || a.overridden != b.overridden ||
        a.children.size() != b.children.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.children.size(); ++i)
    {
        if (!models_equal(a.children[i], b.children[i]))
        {
            return false;
        }
    }
    return true;
}

// --- cases ----------------------------------------------------------------------------------------

void hex_parser_is_total()
{
    CHECK(panels::parse_hex_u64("0") == 0u);
    CHECK(panels::parse_hex_u64("1a2b") == 0x1a2bu);
    CHECK(panels::parse_hex_u64("ffffffffffffffff") == static_cast<std::uint64_t>(-1));
    CHECK(panels::parse_hex_u64("") == 0u);                  // absent -> the model's "no hash"
    CHECK(panels::parse_hex_u64("XYZ") == 0u);               // not hex
    CHECK(panels::parse_hex_u64("1A2B") == 0u);              // uppercase is not OUR wire form
    CHECK(panels::parse_hex_u64("10000000000000000") == 0u); // 17 nibbles cannot be a u64
}

void wire_round_trips_the_built_model()
{
    MapResolver r;
    seed(r);
    const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);

    const scenetree::SceneTreeModel built = builders::build_scene_tree(scene);
    const Json wire = builders::scene_tree_to_wire(built);
    const std::optional<scenetree::SceneTreeModel> parsed = panels::parse_scene_tree(wire);
    CHECK(parsed.has_value());
    CHECK(parsed->root_scene == built.root_scene);
    CHECK(parsed->ok == built.ok);
    CHECK(parsed->entity_count == built.entity_count);
    CHECK(parsed->roots.size() == built.roots.size());
    for (std::size_t i = 0; i < built.roots.size(); ++i)
    {
        CHECK(models_equal(parsed->roots[i], built.roots[i]));
    }

    // The specific L-35 shape this fixture pins: a synthetic instance boundary wrapping an
    // OVERRIDDEN entity — visible on both sides of the wire.
    CHECK(parsed->roots.size() == 1u);
    CHECK(parsed->roots[0].kind == scenetree::NodeKind::instance);
    CHECK(parsed->roots[0].children.size() == 1u);
    CHECK(parsed->roots[0].children[0].display_name == "Cam");
    CHECK(parsed->roots[0].children[0].overridden);
}

void unrecognized_payloads_say_nothing()
{
    CHECK(!panels::parse_scene_tree(Json(std::string("nope"))).has_value());
    CHECK(!panels::parse_scene_tree(Json::object()).has_value()); // no roots container
    // Unparseable NODES inside a recognized container are skipped, never fatal.
    Json wire = Json::object();
    Json roots = Json::array();
    roots.push_back(Json(std::string("junk")));
    Json good = Json::object();
    good.set("identity", Json(std::string("e1")));
    roots.push_back(std::move(good));
    wire.set("roots", std::move(roots));
    const std::optional<scenetree::SceneTreeModel> parsed = panels::parse_scene_tree(wire);
    CHECK(parsed.has_value());
    CHECK(parsed->roots.size() == 1u);
    CHECK(parsed->roots[0].identity == "e1");
}

void apply_result_tolerates_the_envelope_and_touches_the_host()
{
    shell::PanelHost host(roster_with_scenetree());
    panels::SceneTreeFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    const std::uint64_t before = host.revision(kPanelId);

    // The full envelope shape the pump hands over: {ok, data: {sceneTree: {...}}}.
    Json tree = Json::object();
    Json roots = Json::array();
    Json node = Json::object();
    node.set("identity", Json(std::string("e1")));
    node.set("displayName", Json(std::string("Player")));
    node.set("kind", Json(std::string("entity")));
    roots.push_back(std::move(node));
    tree.set("roots", std::move(roots));
    tree.set("rootScene", Json(std::string("root.scene.json")));
    Json data = Json::object();
    data.set("sceneTree", std::move(tree));
    Json envelope = Json::object();
    envelope.set("ok", Json(true));
    envelope.set("data", std::move(data));

    CHECK(feed.apply_result(envelope));
    CHECK(feed.results_applied() == 1u);
    CHECK(host.revision(kPanelId) > before); // the renderer will see a fresh render
    CHECK(feed.panel().model().roots.size() == 1u);

    // A reply that says nothing leaves the adopted model alone.
    CHECK(!feed.apply_result(Json::object()));
    CHECK(feed.panel().model().roots.size() == 1u);
}

void settle_marks_a_refetch_due_and_advances_the_status()
{
    shell::PanelHost host(roster_with_scenetree());
    panels::SceneTreeFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    CHECK(feed.fetch_due()); // born due: the first pump performs the initial hydration
    feed.mark_fetched();
    CHECK(!feed.fetch_due());

    Json payload = Json::object();
    payload.set("event", Json(std::string("derivation.settled")));
    payload.set("stability", Json(std::string("settling")));
    CHECK(feed.apply_event("derivation", payload, 7));
    CHECK(feed.fetch_due()); // the settle scheduled the re-read
    CHECK(feed.panel().generation() == 7u);
    CHECK(feed.panel().stability() == bridge::Stability::settling);

    // Unknown topics / other derivation events are ignored.
    CHECK(!feed.apply_event("files", payload, 8));
    Json other = Json::object();
    other.set("event", Json(std::string("derivation.started")));
    CHECK(!feed.apply_event("derivation", other, 8));
}

void selection_dispatches_through_the_provider()
{
    CHECK(panels::scenetree_row_identity("scenetree.item.a/b") == std::optional<std::string>("a/b"));
    CHECK(!panels::scenetree_row_identity("scenetree.item.").has_value());
    CHECK(!panels::scenetree_row_identity("problems.row.0").has_value());

    shell::PanelHost host(roster_with_scenetree());
    panels::SceneTreeFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    scenetree::SceneTreeModel model;
    scenetree::SceneTreeNode node;
    node.identity = "e1";
    node.display_name = "Player";
    model.roots.push_back(std::move(node));
    feed.panel().set_model(std::move(model));

    // The runtime dispatches the ACTIVATED NODE id; the provider translates + selects.
    bool dispatched = false;
    std::string error;
    Json params = Json::object();
    params.set("nodeId", Json(std::string("scenetree.item.e1")));
    CHECK(host.invoke(kPanelId, scenetree::kSelectCommand, params, dispatched, error));
    CHECK(dispatched);
    CHECK(feed.panel().selection().identity == "e1");

    // An unknown row / a foreign node id is DECLINED, not an error.
    Json bad = Json::object();
    bad.set("nodeId", Json(std::string("scenetree.item.ghost")));
    CHECK(host.invoke(kPanelId, scenetree::kSelectCommand, bad, dispatched, error));
    CHECK(!dispatched);
}

} // namespace

int main()
{
    hex_parser_is_total();
    wire_round_trips_the_built_model();
    unrecognized_payloads_say_nothing();
    apply_result_tolerates_the_envelope_and_touches_the_host();
    settle_marks_a_refetch_due_and_advances_the_status();
    selection_dispatches_through_the_provider();
    PANELS_TEST_MAIN_END();
}
