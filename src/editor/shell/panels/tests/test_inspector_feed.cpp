// T1 for the LIVE inspector feed (M9 e05d3): the kernel-side builder+wire -> Shell-side parser
// ROUND-TRIP, the CAS-token adoption, the widget-id -> field-pointer mapping, the edit-staging
// dispatch through the provider, and the selection-driven fetch mechanics.
//
// Same split as test_scenetree_feed: the round-trip half drives the REAL builders (compose::flatten
// + the engine ctx:scene schema -> builders::build_inspector_model -> builders::inspector_to_wire)
// and asserts the feed's parsers reconstruct the model — value identity by CANONICAL bytes
// (R-FILE-001), the one equality the engine recognizes.

#include "context/editor/shell/panels/inspector_feed.h"

#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/panels/builders/inspector_builder.h"
#include "context/editor/gui/panels/builders/wire.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/canonical.h"
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
namespace inspector = context::editor::gui::panels::inspector;
namespace builders = context::editor::gui::panels::builders;
namespace compose = context::editor::compose;
namespace schema = context::editor::schema;
namespace gc = context::editor::gui::contract;
namespace serializer = context::editor::serializer;
using Json = context::editor::contract::Json;

namespace
{

constexpr const char* kPanelId = "builtin.inspector";

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

void seed(MapResolver& r)
{
    CHECK(r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Cam",
         "components": {
           "transform": {"position": [1, 2, 3]},
           "camera": {"fov": 1.0, "near": 0.1, "far": 500.0}
         }}
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

[[nodiscard]] std::vector<gc::Contribution> roster_with_inspector()
{
    gc::Contribution c;
    c.id = kPanelId;
    c.kind = gc::ContributionKind::panel;
    c.title = "Inspector";
    c.content.type = gc::ContentType::uitree;
    c.state.schema_version = 1;
    return {c};
}

[[nodiscard]] bool canonical_equal(const serializer::JsonValue& a, const serializer::JsonValue& b)
{
    std::string sa;
    std::string sb;
    const bool oka = serializer::serialize_canonical(a, sa);
    const bool okb = serializer::serialize_canonical(b, sb);
    return oka == okb && sa == sb;
}

// --- cases ----------------------------------------------------------------------------------------

void token_parsers_are_total()
{
    CHECK(panels::parse_widget_kind("text") == inspector::WidgetKind::text);
    CHECK(panels::parse_widget_kind("number") == inspector::WidgetKind::number);
    CHECK(panels::parse_widget_kind("toggle") == inspector::WidgetKind::toggle);
    CHECK(panels::parse_widget_kind("json") == inspector::WidgetKind::json);
    CHECK(panels::parse_widget_kind("readonly") == inspector::WidgetKind::readonly);
    // Fail-closed: an editing affordance we cannot understand must not be offered for editing.
    CHECK(panels::parse_widget_kind("holo-dial") == inspector::WidgetKind::readonly);
    CHECK(panels::parse_widget_kind("") == inspector::WidgetKind::readonly);

    CHECK(panels::parse_raw_hash("0") == 0u);
    CHECK(panels::parse_raw_hash("12345") == 12345u);
    CHECK(panels::parse_raw_hash("18446744073709551615") == static_cast<std::uint64_t>(-1));
    CHECK(panels::parse_raw_hash("") == 0u);
    CHECK(panels::parse_raw_hash("18446744073709551616") == 0u); // overflow -> refused
    CHECK(panels::parse_raw_hash("-3") == 0u);
    CHECK(panels::parse_raw_hash("0x10") == 0u); // decimal wire form only
}

void wire_round_trips_the_built_model()
{
    MapResolver r;
    seed(r);
    const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);

    const compose::ComposedEntity* entity = nullptr;
    for (const compose::ComposedEntity& e : scene.entities)
    {
        if (e.id_path == std::vector<std::string>{"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"})
        {
            entity = &e;
        }
    }
    CHECK(entity != nullptr);
    const schema::KindSchema* kind = schema::engine_schemas().latest("ctx:scene");
    CHECK(kind != nullptr);

    const inspector::InspectorModel built =
        builders::build_inspector_model(*entity, *kind, "root.scene.json");
    const Json wire = builders::inspector_to_wire(built);
    const std::optional<inspector::InspectorModel> parsed = panels::parse_inspector(wire);
    CHECK(parsed.has_value());
    CHECK(parsed->has_entity == built.has_entity);
    CHECK(parsed->root_scene == built.root_scene);
    CHECK(parsed->id_path == built.id_path);
    CHECK(parsed->identity == built.identity);
    CHECK(parsed->identity_hash == built.identity_hash);
    CHECK(parsed->kind_id == built.kind_id);
    CHECK(parsed->override_count == built.override_count);
    CHECK(parsed->fields.size() == built.fields.size());
    bool saw_fov = false;
    for (std::size_t i = 0; i < built.fields.size(); ++i)
    {
        const inspector::InspectorField& a = built.fields[i];
        const inspector::InspectorField& b = parsed->fields[i];
        CHECK(a.pointer == b.pointer);
        CHECK(a.label == b.label);
        CHECK(a.units == b.units);
        CHECK(a.kind == b.kind);
        CHECK(a.overridden == b.overridden);
        CHECK(a.editable == b.editable);
        CHECK(canonical_equal(a.value, b.value)); // R-FILE-001: the ONE value identity
        if (a.pointer == "/components/camera/fov")
        {
            saw_fov = true;
            CHECK(a.overridden); // the L-35 override is visible on BOTH sides
        }
    }
    CHECK(saw_fov);

    // The no-selection state round-trips as ENGAGED-but-empty (information, not absence of it).
    const std::optional<inspector::InspectorModel> none =
        panels::parse_inspector(builders::inspector_to_wire(inspector::InspectorModel{}));
    CHECK(none.has_value());
    CHECK(!none->has_entity);

    // A payload that says nothing about a selection is nullopt, NOT "no selection".
    CHECK(!panels::parse_inspector(Json::object()).has_value());
    CHECK(!panels::parse_inspector(Json(std::string("junk"))).has_value());
}

void apply_result_adopts_the_model_and_the_cas_token()
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    const std::uint64_t before = host.revision(kPanelId);

    Json model = Json::object();
    model.set("present", Json(true));
    model.set("rootScene", Json(std::string("root.scene.json")));
    model.set("identity", Json(std::string("e1")));
    Json id_path = Json::array();
    id_path.push_back(Json(std::string("e1")));
    model.set("idPath", std::move(id_path));
    Json fields = Json::array();
    Json field = Json::object();
    field.set("pointer", Json(std::string("/name")));
    field.set("label", Json(std::string("name")));
    field.set("kind", Json(std::string("text")));
    field.set("value", Json(std::string("\"Player\"")));
    field.set("editable", Json(true));
    fields.push_back(std::move(field));
    model.set("fields", std::move(fields));
    Json data = Json::object();
    data.set("inspector", std::move(model));
    data.set("rawHash", Json(std::string("4242")));
    Json envelope = Json::object();
    envelope.set("ok", Json(true));
    envelope.set("data", std::move(data));

    CHECK(feed.apply_result(envelope));
    CHECK(feed.results_applied() == 1u);
    CHECK(host.revision(kPanelId) > before);
    CHECK(feed.panel().has_selection());
    CHECK(feed.panel().base_raw_hash() == 4242u); // the CAS token rode along (e09 guards on it)

    // A corrupt VALUE degrades to a visible-but-readonly field, never editable-as-garbage.
    Json bad_field = Json::object();
    bad_field.set("pointer", Json(std::string("/broken")));
    bad_field.set("kind", Json(std::string("text")));
    bad_field.set("value", Json(std::string("{not json")));
    bad_field.set("editable", Json(true));
    Json bad_fields = Json::array();
    bad_fields.push_back(std::move(bad_field));
    Json bad_model = Json::object();
    bad_model.set("present", Json(true));
    bad_model.set("fields", std::move(bad_fields));
    const std::optional<inspector::InspectorModel> parsed = panels::parse_inspector(bad_model);
    CHECK(parsed.has_value());
    CHECK(parsed->fields.size() == 1u);
    CHECK(!parsed->fields[0].editable);
    CHECK(parsed->fields[0].kind == inspector::WidgetKind::readonly);
}

void edits_stage_through_the_provider()
{
    CHECK(panels::inspector_widget_pointer("inspector.widget./name") ==
          std::optional<std::string>("/name"));
    CHECK(!panels::inspector_widget_pointer("inspector.widget.").has_value());
    CHECK(!panels::inspector_widget_pointer("inspector.label./name").has_value());

    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    // Adopt a model with one editable + one readonly field.
    inspector::InspectorModel model;
    model.has_entity = true;
    model.root_scene = "root.scene.json";
    model.id_path = {"e1"};
    model.identity = "e1";
    inspector::InspectorField editable;
    editable.pointer = "/name";
    editable.label = "name";
    editable.kind = inspector::WidgetKind::text;
    editable.editable = true;
    model.fields.push_back(editable);
    inspector::InspectorField readonly_field;
    readonly_field.pointer = "/frozen";
    readonly_field.kind = inspector::WidgetKind::readonly;
    readonly_field.editable = false;
    model.fields.push_back(readonly_field);
    feed.panel().set_model(std::move(model), 100);

    bool dispatched = false;
    std::string error;

    // A dispatch carrying a parseable JSON literal STAGES the edit on the C++ model.
    Json params = Json::object();
    params.set("nodeId", Json(std::string("inspector.widget./name")));
    params.set("value", Json(std::string("\"Renamed\"")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, params, dispatched, error));
    CHECK(dispatched);
    CHECK(feed.panel().has_staged_edit());
    CHECK(feed.panel().staged_pointer() == "/name");

    feed.panel().discard_edit();

    // No value / unparseable value / readonly target / foreign node -> DECLINED, nothing staged.
    Json no_value = Json::object();
    no_value.set("nodeId", Json(std::string("inspector.widget./name")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, no_value, dispatched, error));
    CHECK(!dispatched);
    Json bad_value = Json::object();
    bad_value.set("nodeId", Json(std::string("inspector.widget./name")));
    bad_value.set("value", Json(std::string("{nope")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, bad_value, dispatched, error));
    CHECK(!dispatched);
    Json frozen = Json::object();
    frozen.set("nodeId", Json(std::string("inspector.widget./frozen")));
    frozen.set("value", Json(std::string("1")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, frozen, dispatched, error));
    CHECK(!dispatched);
    CHECK(!feed.panel().has_staged_edit());
}

void selection_fetch_mechanics()
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    CHECK(!feed.pending().has_value()); // nothing selected -> nothing to fetch

    feed.request("a/b");
    feed.request("a/c"); // a later selection REPLACES the pending one
    CHECK(feed.pending() == std::optional<std::string>("a/c"));
    feed.mark_fetched();
    CHECK(!feed.pending().has_value());

    // A cleared selection clears the panel NOW (no RPC) and drops any pending fetch.
    inspector::InspectorModel model;
    model.has_entity = true;
    feed.panel().set_model(std::move(model), 7);
    feed.request("a/d");
    const std::uint64_t before = host.revision(kPanelId);
    feed.request_clear();
    CHECK(!feed.pending().has_value());
    CHECK(!feed.panel().has_selection());
    CHECK(host.revision(kPanelId) > before);
}

} // namespace

int main()
{
    token_parsers_are_total();
    wire_round_trips_the_built_model();
    apply_result_adopts_the_model_and_the_cas_token();
    edits_stage_through_the_provider();
    selection_fetch_mechanics();
    PANELS_TEST_MAIN_END();
}
