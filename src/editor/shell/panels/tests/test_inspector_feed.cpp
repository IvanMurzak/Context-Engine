// T1 for the LIVE inspector feed (M9 e05d3): the kernel-side builder+wire -> Shell-side parser
// ROUND-TRIP, the CAS-token adoption, the widget-id -> field-pointer mapping, the edit-staging
// dispatch through the provider, and the selection-driven fetch mechanics.
//
// Same split as test_scenetree_feed: the round-trip half drives the REAL builders (compose::flatten
// + the engine ctx:scene schema -> builders::build_inspector_model -> builders::inspector_to_wire)
// and asserts the feed's parsers reconstruct the model — value identity by CANONICAL bytes
// (R-FILE-001), the one equality the engine recognizes.
//
// M9 e09e-2 adds the FAN-OUT half: `apply_event` on `derivation.settled`, and — the part worth having
// coverage for at all — the fact that it must NOT serve that refresh while a gesture is staged,
// because doing so silently re-bases the L-30 collision guard. The cross-process proof over a REAL
// daemon with a REAL second client lives in `src/tests/integration/test_e09b_concurrent_cas.cpp`;
// these cases pin the DECISION, which is pure over the feed's own state.

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
#include "context/editor/shell/panels/builtin_panels.h" // kDerivationTopic (the subscriber's spelling)
#include "context/editor/shell/panels/problems_feed.h"  // kDerivationSettledEvent
#include "context/editor/shell/panels/wire_override_gateway.h" // kNoDaemonCode (the refusal shape)

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
namespace undo = context::editor::gui::session::undo;
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

[[nodiscard]] std::string canonical_text(const serializer::JsonValue& value)
{
    std::string out;
    return serializer::serialize_canonical(value, out) ? out : std::string("<uncanonical>");
}

[[nodiscard]] serializer::JsonValue jstring(const char* text)
{
    serializer::JsonValue value;
    value.type = serializer::JsonValue::Type::string;
    value.string_value = text;
    return value;
}

// A gateway whose ONE outcome is scripted (M9 e09c): either the write lands, or it reports the L-30
// `cas.mismatch` trigger and the re-read answers a value a CO-WRITER left — which is what makes the
// engine drop rather than rebase.
class ScriptedGateway final : public inspector::OverrideWriteGateway
{
public:
    inspector::WriteAttempt attempt(const inspector::OverrideWriteRequest& request,
                                    std::uint64_t) const override
    {
        inspector::WriteAttempt out;
        if (refuse_with_cas_mismatch)
        {
            out.cas_mismatch = true;
            out.code = "cas.mismatch";
            out.message = "another writer advanced the file";
            out.raw_hash = 200;
            return out;
        }
        // The OTHER non-landing outcome (M9 e09b-3): the write PATH refused. Neither applied nor a
        // CAS mismatch, so the L-30 engine reports `Status::error` and keeps the staged gesture —
        // a different thing to tell the human than a concurrent-writer drop, and the reason the
        // notice sink carries both.
        if (refuse_with_error)
        {
            out.code = shell::panels::WireOverrideWriteGateway::kNoDaemonCode;
            out.message = "no daemon connection";
            return out;
        }
        out.applied = true;
        out.file = request.root_scene;
        out.pointer = request.pointer;
        out.raw_hash = 200;
        return out;
    }

    inspector::FieldState read(const std::string&, const std::vector<std::string>&,
                               const std::string&) const override
    {
        inspector::FieldState state;
        state.present = true;
        state.raw_hash = 200;
        state.value = moved_value;
        return state;
    }

    bool refuse_with_cas_mismatch = false;
    bool refuse_with_error = false;
    serializer::JsonValue moved_value;
};

// One inspector model with a single editable `/name` field holding `current`.
[[nodiscard]] inspector::InspectorModel one_field_model(const char* current)
{
    inspector::InspectorModel model;
    model.has_entity = true;
    model.root_scene = "root.scene.json";
    model.id_path = {"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"};
    model.identity = "aaaaaaaaaaaaaaa1/ccccccccccccccc1";
    inspector::InspectorField editable;
    editable.pointer = "/name";
    editable.label = "name";
    editable.kind = inspector::WidgetKind::text;
    editable.editable = true;
    editable.value = jstring(current);
    model.fields.push_back(editable);
    return model;
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

    // An UNKNOWN widget token fails closed across BOTH members: the kind reads readonly AND the
    // wire's independent `editable` bit is clamped off — stage_edit gates on `editable` alone, so
    // the token downgrade by itself would still leave the field editable through a widget this
    // build cannot render honestly.
    Json alien_field = Json::object();
    alien_field.set("pointer", Json(std::string("/alien")));
    alien_field.set("kind", Json(std::string("holo-dial")));
    alien_field.set("value", Json(std::string("1")));
    alien_field.set("editable", Json(true));
    Json alien_fields = Json::array();
    alien_fields.push_back(std::move(alien_field));
    Json alien_model = Json::object();
    alien_model.set("present", Json(true));
    alien_model.set("fields", std::move(alien_fields));
    const std::optional<inspector::InspectorModel> alien = panels::parse_inspector(alien_model);
    CHECK(alien.has_value());
    CHECK(alien->fields.size() == 1u);
    CHECK(alien->fields[0].kind == inspector::WidgetKind::readonly);
    CHECK(!alien->fields[0].editable);
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

// M9 e09b-2 — the OTHER half of the `gestures:false -> true` flip, pinned HERE so the capability
// cannot quietly become unconditional: a feed with NO write path bound exposes NO gesture at all, so
// the manifest reports `gestures:false`, `panel.gesture` refuses with `kErrPanelBadGesture`, and the
// hydration runtime never installs its pointer handlers. A stubbed gesture that only ever swallowed
// a commit is exactly what this task exists to retire — it must not come back as a default.
// (`test_builtin_panels.cpp` asserts the TRUE side, after the composition root binds the gateway.)
void a_feed_without_a_gateway_exposes_no_gesture()
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    CHECK(!feed.has_gateway());
    CHECK(host.provide(kPanelId, feed.make_provider()));

    const Json listing = host.list();
    CHECK(listing.at("panels").size() == 1);
    CHECK(!listing.at("panels").at(0).at("gestures").as_bool());

    bool dispatched = true;
    std::string error_code;
    CHECK(!host.gesture(kPanelId, shell::GestureVerb::commit, Json::object(), dispatched,
                        error_code));
    CHECK(!dispatched);
    CHECK(error_code == shell::kErrPanelBadGesture);
    // And no commit was invented behind the refusal: nothing observed, nothing re-armed.
    CHECK(feed.commits_observed() == 0);
    CHECK(feed.rereads_armed() == 0);
}

// M9 e09c — `request_refresh` is the ONE home for "the file changed under the panel, re-read it".
// Two callers route through it: this feed's own commit listener (read-your-writes) and, since e09c,
// `pump_panel_feeds` after a landed undo/redo replay (read-your-replays, which `InspectorPanel::commit`
// never runs for). Pinned here so the shared seam has coverage independent of either caller.
void request_refresh_rearms_the_inspected_identity()
{
    shell::PanelHost host;
    panels::InspectorFeed feed(host, kPanelId);

    // Nothing inspected -> an ordinary false, and nothing armed. A seam that armed an EMPTY identity
    // would send the pump fetching for no entity, once per frame, forever.
    CHECK(!feed.request_refresh());
    CHECK(!feed.pending().has_value());
    CHECK(feed.rereads_armed() == 0);

    feed.panel().set_model(one_field_model("Before"), 100);
    CHECK(feed.request_refresh());
    CHECK(feed.pending() == std::optional<std::string>("aaaaaaaaaaaaaaa1/ccccccccccccccc1"));
    CHECK(feed.rereads_armed() == 1);
}

// --- M9 e09c: a resolved gesture commit becomes an undo checkpoint -------------------------------
//
// The RECORD half of the session-undo wiring, pinned at the seam that produces it. The part that is
// easy to get wrong and impossible to notice at runtime: a checkpoint needs the gesture's BEFORE and
// AFTER values, and `InspectorPanel::commit` CONSUMES the staged edit — so a snapshot taken from the
// commit LISTENER (which fires inside that call) would see neither, record a null/null pair, and
// produce an undo that silently reverts nothing. These assertions are what catch that.
void a_resolved_commit_becomes_an_undo_checkpoint()
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    ScriptedGateway gateway;
    feed.bind_gateway(&gateway);

    std::vector<undo::FieldEdit> captured;
    CHECK(!feed.has_checkpoint_sink());
    feed.bind_checkpoint_sink([&captured](undo::FieldEdit edit)
                              { captured.push_back(std::move(edit)); });
    CHECK(feed.has_checkpoint_sink());
    CHECK(host.provide(kPanelId, feed.make_provider()));
    feed.panel().set_model(one_field_model("Before"), 100);

    bool dispatched = false;
    std::string error;
    Json params = Json::object();
    params.set("nodeId", Json(std::string("inspector.widget./name")));
    params.set("value", Json(std::string("\"After\"")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, params, dispatched, error));
    CHECK(dispatched);

    Json gesture = Json::object();
    gesture.set("nodeId", Json(std::string("inspector.widget./name")));
    CHECK(host.gesture(kPanelId, shell::GestureVerb::commit, gesture, dispatched, error));
    CHECK(dispatched);
    CHECK(feed.last_commit().status == inspector::CommitResult::Status::applied);

    CHECK(feed.checkpoints_sent() == 1u);
    CHECK(captured.size() == 1u);
    if (!captured.empty())
    {
        // The L-35 addressing a replay needs — without it the checkpoint reverts nothing.
        CHECK(captured[0].root_scene == "root.scene.json");
        CHECK(captured[0].id_path.size() == 2u);
        CHECK(captured[0].id_path.size() == 2u && captured[0].id_path[1] == "ccccccccccccccc1");
        CHECK(captured[0].pointer == "/name");
        // THE PAIR — both survived `commit()` consuming the gesture. Compared as CANONICAL bytes,
        // the one value identity the engine recognizes (R-FILE-001), against a canonicalization of
        // the expected value rather than a hand-written literal: `serialize_canonical` emits exactly
        // one trailing newline, so a literal would be asserting the writer's framing, not the value.
        CHECK(canonical_text(captured[0].before) == canonical_text(jstring("Before")));
        CHECK(canonical_text(captured[0].after) == canonical_text(jstring("After")));
        // …and they are genuinely DIFFERENT values: an undo that restored the value already there
        // would pass a same-string check while reverting nothing.
        CHECK(!canonical_equal(captured[0].before, captured[0].after));
    }
}

// The negative half, and the one that protects the human's data: a LOUD L-30 drop wrote nothing, so
// it must NOT become an undo step. A journal that recorded it would offer a revert of an edit that
// never landed — which, replayed, would overwrite the co-writer's value with our stale `before`.
void a_dropped_commit_records_no_checkpoint()
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    ScriptedGateway gateway;
    gateway.refuse_with_cas_mismatch = true;
    gateway.moved_value = jstring("Someone Else"); // the co-writer's value at the SAME field path
    feed.bind_gateway(&gateway);

    std::vector<undo::FieldEdit> captured;
    feed.bind_checkpoint_sink([&captured](undo::FieldEdit edit)
                              { captured.push_back(std::move(edit)); });
    CHECK(host.provide(kPanelId, feed.make_provider()));
    feed.panel().set_model(one_field_model("Before"), 100);

    bool dispatched = false;
    std::string error;
    Json params = Json::object();
    params.set("nodeId", Json(std::string("inspector.widget./name")));
    params.set("value", Json(std::string("\"After\"")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, params, dispatched, error));
    Json gesture = Json::object();
    gesture.set("nodeId", Json(std::string("inspector.widget./name")));
    CHECK(host.gesture(kPanelId, shell::GestureVerb::commit, gesture, dispatched, error));
    CHECK(dispatched); // the gesture RESOLVED — the drop is an outcome, not a protocol fault

    CHECK(feed.last_commit().status == inspector::CommitResult::Status::dropped);
    CHECK(feed.drops_observed() == 1u);
    CHECK(feed.checkpoints_sent() == 0u);
    CHECK(captured.empty());
}

// ------------------------------------------------- M9 e09b-3: the LOUD notice sink (design 05 §8)
//
// Drive ONE gesture commit through the real provider and report which outcomes reached the notice
// sink. Returns the sink's captures so each case asserts on the SAME driver — a per-case copy of this
// twelve-line dance is how two of them end up subtly different and one of them stops proving anything.
//
// It reports the COMMIT as well as the notices, and that is not padding: an assertion that only
// counts notices is satisfied just as well by a run in which the gesture never reached the gateway at
// all, which is precisely how the zero-notice CONTROL below could certify a write path that had
// quietly stopped writing.
struct CommitNoticeRun
{
    std::vector<inspector::CommitResult> notices;
    std::size_t notices_sent = 0;
    std::size_t commits_observed = 0;
    std::size_t drops_observed = 0;
    inspector::CommitResult last_commit;
};

// `bind_sink = false` drives the SAME gesture with no sink bound — the ordinary state of every T1 bag,
// and the one case that must show a refusal is still counted when nobody is listening. It is a
// parameter rather than a fourth hand-rolled copy of the dance precisely because it differs from the
// others in ONE line; a copy is how it would quietly stop driving the same path as its siblings.
[[nodiscard]] CommitNoticeRun commit_and_capture_notices(ScriptedGateway& gateway,
                                                          bool bind_sink = true)
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    feed.bind_gateway(&gateway);

    CommitNoticeRun run;
    if (bind_sink)
    {
        feed.bind_notice_sink([&run](const inspector::CommitResult& result)
                              { run.notices.push_back(result); });
    }
    CHECK(feed.has_notice_sink() == bind_sink);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    feed.panel().set_model(one_field_model("Before"), 100);

    bool dispatched = false;
    std::string error;
    Json params = Json::object();
    params.set("nodeId", Json(std::string("inspector.widget./name")));
    params.set("value", Json(std::string("\"After\"")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, params, dispatched, error));
    Json gesture = Json::object();
    gesture.set("nodeId", Json(std::string("inspector.widget./name")));
    CHECK(host.gesture(kPanelId, shell::GestureVerb::commit, gesture, dispatched, error));
    CHECK(dispatched);

    run.notices_sent = feed.notices_sent();
    run.commits_observed = feed.commits_observed();
    run.drops_observed = feed.drops_observed();
    run.last_commit = feed.last_commit();
    return run;
}

// A LOUD L-30 drop reaches the sink, carrying what the human has to be told: which field, and why.
void a_dropped_commit_is_handed_to_the_notice_sink()
{
    ScriptedGateway gateway;
    gateway.refuse_with_cas_mismatch = true;
    gateway.moved_value = jstring("Someone Else"); // the co-writer's value at the SAME field path

    const CommitNoticeRun run = commit_and_capture_notices(gateway);

    CHECK(run.notices_sent == 1u);
    // GUARDED: CHECK records a failure without aborting (panels_test.h), so indexing on the very run
    // the size assertion is there to catch would be UB — a heap-buffer-overflow on the ASan leg in
    // place of a legible failure. (The established shape here; see test_wire_override_gateway.cpp.)
    CHECK(run.notices.size() == 1u);
    if (run.notices.size() == 1u)
    {
        CHECK(run.notices[0].status == inspector::CommitResult::Status::dropped);
        CHECK(run.notices[0].code == "cas.mismatch");
        CHECK(run.notices[0].pointer == "/name");
        // The DIAGNOSTIC is non-empty. A notice whose message is "" renders as a toast with a headline
        // and no reason, which is the shape a "loud" surface degrades into when nobody checks.
        CHECK(!run.notices[0].message.empty());
    }
}

// The OTHER non-landing outcome: the write path refused. Also loud, and DISTINGUISHABLE — the
// composition root hues the two differently (06 §2: `wait` = awaiting-human, `bad` = error), so a
// sink that flattened them would silently mis-report a daemon outage as a co-writer collision.
void a_refused_commit_is_handed_to_the_notice_sink()
{
    ScriptedGateway gateway;
    gateway.refuse_with_error = true;

    const CommitNoticeRun run = commit_and_capture_notices(gateway);

    CHECK(run.notices_sent == 1u);
    CHECK(run.notices.size() == 1u); // guarded below — CHECK records, it does not abort
    if (run.notices.size() == 1u)
    {
        CHECK(run.notices[0].status == inspector::CommitResult::Status::error);
        CHECK(run.notices[0].status != inspector::CommitResult::Status::dropped);
        CHECK(run.notices[0].code == panels::WireOverrideWriteGateway::kNoDaemonCode);
    }
}

// THE ANTI-VACUITY CONTROL, and the case that makes the two above mean something. A gate that fired
// on EVERY commit would pass both of them while training the human to ignore the channel: a toast
// after every successful edit is noise, and noise is how a real drop goes unnoticed. So the SAME
// driver, with a gateway that APPLIES, must produce zero notices.
void an_applied_commit_produces_no_notice()
{
    ScriptedGateway gateway; // neither refusal flag set — the write lands

    const CommitNoticeRun run = commit_and_capture_notices(gateway);

    // FIRST prove a commit actually happened AND landed. Without these two the control is itself
    // vacuous in the other direction: a regression in STAGING — the gesture never reaching the
    // gateway, so `commit()` answers `none` and there is nothing to refuse — produces zero notices
    // just as faithfully, and this case would then certify the silence of a write path that had
    // stopped writing at all. (The undo twin already proves its replay ran; this is the same rung.)
    CHECK(run.commits_observed == 1u);
    CHECK(run.last_commit.status == inspector::CommitResult::Status::applied);

    CHECK(run.notices_sent == 0u);
    CHECK(run.notices.empty());
}

// An unbound sink is an ordinary state (every T1 bag builds one) and must not change the drop's own
// accounting: the refusal is still counted, still kept in `last_commit()`, still reported to stderr.
void an_unbound_notice_sink_leaves_the_drop_counted()
{
    ScriptedGateway gateway;
    gateway.refuse_with_cas_mismatch = true;
    gateway.moved_value = jstring("Someone Else");

    // THE SAME driver, with the sink left unbound — so this case cannot drift away from the path the
    // three above drive, which is the whole reason the driver exists.
    const CommitNoticeRun run = commit_and_capture_notices(gateway, /*bind_sink=*/false);

    CHECK(run.drops_observed == 1u);
    CHECK(run.notices_sent == 0u); // counted apart from the drop, so an unbound sink is VISIBLE
    CHECK(run.notices.empty());
}

// ------------------------------------------- M9 e09e-2: the FAN-OUT half (design 05 §8's tail)
//
// The daemon's `derivation.settled` payload, in the shape `EditorKernel::settle` publishes it
// (editor_kernel.cpp — `event` + `generation` on the `derivation` topic). Built from the real
// members rather than hand-spelled per case so no case can quietly assert against a shape the daemon
// does not send.
[[nodiscard]] Json settled_payload(std::uint64_t generation)
{
    Json payload = Json::object();
    payload.set("event", Json(std::string(panels::kDerivationSettledEvent)));
    payload.set("generation", Json(generation));
    return payload;
}

// A settle with NOTHING staged re-reads the inspected entity — the ordinary fan-out: another window,
// a CLI or an agent wrote, so what this panel is showing may be stale.
void a_settle_rearms_the_inspected_identity()
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    feed.panel().set_model(one_field_model("Before"), 100);
    CHECK(!feed.pending().has_value()); // adopting a model arms nothing — only a request does

    CHECK(feed.apply_event(panels::kDerivationTopic, settled_payload(7)));
    CHECK(feed.events_applied() == 1u);
    CHECK(feed.rereads_armed() == 1u);
    CHECK(feed.pending() == std::optional<std::string>("aaaaaaaaaaaaaaa1/ccccccccccccccc1"));
    CHECK(!feed.refresh_deferred()); // served immediately, so nothing is owed

    // Tolerance, and the anti-vacuity floor for every case below: a foreign topic and a DIFFERENT
    // `derivation` event are both ignored WITHOUT counting, so `events_applied()` is a truthful
    // "the settle was recognized" and not just "apply_event was called".
    feed.mark_fetched();
    CHECK(!feed.apply_event("files", settled_payload(8)));
    CHECK(!feed.apply_event("session", settled_payload(8)));
    Json other = Json::object();
    other.set("event", Json(std::string("derivation.started")));
    CHECK(!feed.apply_event(panels::kDerivationTopic, other));
    CHECK(!feed.apply_event(panels::kDerivationTopic, Json::object()));
    CHECK(feed.events_applied() == 1u);
    CHECK(feed.rereads_armed() == 1u);
    CHECK(!feed.pending().has_value());

    // Nothing INSPECTED: recognized, but there is no identity to re-read, so nothing is armed and
    // nothing is owed — a settle on an empty panel must not arm an empty-identity fetch forever.
    shell::PanelHost empty_host(roster_with_inspector());
    panels::InspectorFeed empty(empty_host, kPanelId);
    CHECK(!empty.apply_event(panels::kDerivationTopic, settled_payload(9)));
    CHECK(empty.events_applied() == 1u);
    CHECK(!empty.pending().has_value());
    CHECK(!empty.refresh_deferred());
}

// ⚠⚠ THE CASE THIS TASK EXISTS FOR. A settle arriving while a gesture is STAGED must be DEFERRED,
// because serving it would run `set_model` — which discards the in-flight edit AND adopts the fresh
// file as the gesture's CAS base, silently re-basing the L-30 collision guard. The failure is
// SILENT: no crash, no error status, just a compare-and-swap that can no longer detect the writer it
// is racing. So the assertions here are the NEGATIVE ones, and `events_applied()` is what stops them
// from passing vacuously on a feed that never saw the event at all.
void a_settle_during_a_gesture_is_deferred_not_served()
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    ScriptedGateway gateway;
    feed.bind_gateway(&gateway);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    feed.panel().set_model(one_field_model("Before"), 100);

    bool dispatched = false;
    std::string error;
    Json params = Json::object();
    params.set("nodeId", Json(std::string("inspector.widget./name")));
    params.set("value", Json(std::string("\"Mine\"")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, params, dispatched, error));
    CHECK(dispatched);
    CHECK(feed.panel().has_staged_edit());

    // The settle lands mid-gesture. RECOGNIZED (so this is not a topic-filter no-op) and WITHHELD.
    CHECK(!feed.apply_event(panels::kDerivationTopic, settled_payload(11)));
    CHECK(feed.events_applied() == 1u);
    CHECK(feed.refresh_deferred());
    CHECK(!feed.pending().has_value()); // NOT armed -> the pump cannot re-read behind the gesture
    CHECK(feed.rereads_armed() == 0u);
    // The two things a served refresh would have destroyed, asserted directly rather than implied.
    CHECK(feed.panel().has_staged_edit());
    CHECK(feed.panel().base_raw_hash() == 100u); // the collision guard was NOT re-based
    CHECK(feed.results_applied() == 0u);

    // Repeated settles collapse into the ONE owed re-read (a busy co-writer must not queue N fetches).
    CHECK(!feed.apply_event(panels::kDerivationTopic, settled_payload(12)));
    CHECK(feed.events_applied() == 2u);
    CHECK(feed.refresh_deferred());
    CHECK(!feed.pending().has_value());

    // THE GESTURE RESOLVES — a DROP, which is the case that matters: the co-writer moved this very
    // field, so the panel is rendering a value that is no longer on disk and the deferred re-read is
    // exactly what makes "re-make your edit against what is there now" actionable.
    gateway.refuse_with_cas_mismatch = true;
    gateway.moved_value = jstring("Someone Else");
    Json gesture = Json::object();
    gesture.set("nodeId", Json(std::string("inspector.widget./name")));
    CHECK(host.gesture(kPanelId, shell::GestureVerb::commit, gesture, dispatched, error));
    CHECK(dispatched);
    CHECK(feed.last_commit().status == inspector::CommitResult::Status::dropped);
    CHECK(feed.drops_observed() == 1u);
    // …and NOW it refreshes. ONE re-read, not two: the deferral is released, not replayed per settle.
    CHECK(!feed.refresh_deferred());
    CHECK(feed.rereads_armed() == 1u);
    CHECK(feed.pending() == std::optional<std::string>("aaaaaaaaaaaaaaa1/ccccccccccccccc1"));
}

// The OTHER resolution shape, and the one an outcome-based release gets wrong: a write-path ERROR
// deliberately KEEPS the staged gesture (inspector_panel.h), so the human's edit is STILL in flight
// and the deferral must SURVIVE that commit. Releasing on "a commit listener fired" would refresh
// here and destroy the very edit the error was careful not to discard.
void a_settle_deferred_across_an_error_stays_deferred()
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    ScriptedGateway gateway;
    gateway.refuse_with_error = true;
    feed.bind_gateway(&gateway);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    feed.panel().set_model(one_field_model("Before"), 100);

    bool dispatched = false;
    std::string error;
    Json params = Json::object();
    params.set("nodeId", Json(std::string("inspector.widget./name")));
    params.set("value", Json(std::string("\"Mine\"")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, params, dispatched, error));
    CHECK(!feed.apply_event(panels::kDerivationTopic, settled_payload(21)));
    CHECK(feed.refresh_deferred());

    Json gesture = Json::object();
    gesture.set("nodeId", Json(std::string("inspector.widget./name")));
    CHECK(host.gesture(kPanelId, shell::GestureVerb::commit, gesture, dispatched, error));
    CHECK(dispatched);
    CHECK(feed.last_commit().status == inspector::CommitResult::Status::error);
    CHECK(feed.panel().has_staged_edit()); // the edit survived the refusal — the premise of this case
    // STILL deferred, and still nothing armed: the gesture is in flight, so the guard still binds.
    CHECK(feed.refresh_deferred());
    CHECK(feed.rereads_armed() == 0u);
    CHECK(!feed.pending().has_value());
    CHECK(feed.panel().base_raw_hash() == 100u);

    // CANCEL is the other way a gesture ends, and `discard_edit` fires NO commit listener — so this
    // is the only path that can release the deferral. Without it the panel would render pre-write
    // state until the next selection change.
    CHECK(host.gesture(kPanelId, shell::GestureVerb::cancel, gesture, dispatched, error));
    CHECK(dispatched);
    CHECK(!feed.panel().has_staged_edit());
    CHECK(!feed.refresh_deferred());
    CHECK(feed.rereads_armed() == 1u);
    CHECK(feed.pending() == std::optional<std::string>("aaaaaaaaaaaaaaa1/ccccccccccccccc1"));
}

// An APPLIED commit already re-reads its own write (read-your-writes, 05 §7), so a settle deferred
// during it must be SUBSUMED rather than flushed on top — otherwise the same fetch is armed twice and
// `rereads_armed()` double-counts, which is the accounting a later task would read as two round trips.
void a_settle_deferred_across_an_applied_commit_is_subsumed()
{
    shell::PanelHost host(roster_with_inspector());
    panels::InspectorFeed feed(host, kPanelId);
    ScriptedGateway gateway; // neither refusal flag — the write lands
    feed.bind_gateway(&gateway);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    feed.panel().set_model(one_field_model("Before"), 100);

    bool dispatched = false;
    std::string error;
    Json params = Json::object();
    params.set("nodeId", Json(std::string("inspector.widget./name")));
    params.set("value", Json(std::string("\"Mine\"")));
    CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, params, dispatched, error));
    CHECK(!feed.apply_event(panels::kDerivationTopic, settled_payload(31)));
    CHECK(feed.refresh_deferred());

    Json gesture = Json::object();
    gesture.set("nodeId", Json(std::string("inspector.widget./name")));
    CHECK(host.gesture(kPanelId, shell::GestureVerb::commit, gesture, dispatched, error));
    CHECK(dispatched);
    CHECK(feed.last_commit().status == inspector::CommitResult::Status::applied);
    CHECK(!feed.refresh_deferred());
    CHECK(feed.rereads_armed() == 1u); // ONE, not two
    CHECK(feed.pending() == std::optional<std::string>("aaaaaaaaaaaaaaa1/ccccccccccccccc1"));
}

// A deferral is owed to the ENTITY that was inspected. Clearing the selection retires it (there is
// nothing left to re-read), and an adopted read discharges it (the panel just got fresh state) — so a
// stale deferral cannot outlive either and fire against whatever is selected next.
void a_deferral_does_not_outlive_its_selection_or_its_read()
{
    {
        shell::PanelHost host(roster_with_inspector());
        panels::InspectorFeed feed(host, kPanelId);
        ScriptedGateway gateway;
        feed.bind_gateway(&gateway);
        CHECK(host.provide(kPanelId, feed.make_provider()));
        feed.panel().set_model(one_field_model("Before"), 100);
        bool dispatched = false;
        std::string error;
        Json params = Json::object();
        params.set("nodeId", Json(std::string("inspector.widget./name")));
        params.set("value", Json(std::string("\"Mine\"")));
        CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, params, dispatched,
                          error));
        CHECK(!feed.apply_event(panels::kDerivationTopic, settled_payload(41)));
        CHECK(feed.refresh_deferred());

        feed.request_clear();
        CHECK(!feed.refresh_deferred());
        CHECK(!feed.pending().has_value());
    }
    {
        shell::PanelHost host(roster_with_inspector());
        panels::InspectorFeed feed(host, kPanelId);
        ScriptedGateway gateway;
        feed.bind_gateway(&gateway);
        CHECK(host.provide(kPanelId, feed.make_provider()));
        feed.panel().set_model(one_field_model("Before"), 100);
        bool dispatched = false;
        std::string error;
        Json params = Json::object();
        params.set("nodeId", Json(std::string("inspector.widget./name")));
        params.set("value", Json(std::string("\"Mine\"")));
        CHECK(host.invoke(kPanelId, inspector::InspectorPanel::kEditCommand, params, dispatched,
                          error));
        CHECK(!feed.apply_event(panels::kDerivationTopic, settled_payload(42)));
        CHECK(feed.refresh_deferred());

        // A fetch armed by something ELSE (a selection change) lands: the panel now holds fresh
        // state, which IS what the deferral was owed.
        Json data = Json::object();
        data.set("inspector", builders::inspector_to_wire(one_field_model("Fresh")));
        data.set("rawHash", Json(std::string("777")));
        Json envelope = Json::object();
        envelope.set("ok", Json(true));
        envelope.set("data", std::move(data));
        CHECK(feed.apply_result(envelope));
        CHECK(!feed.refresh_deferred());
        CHECK(feed.panel().base_raw_hash() == 777u);
    }
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
    request_refresh_rearms_the_inspected_identity();
    edits_stage_through_the_provider();
    a_feed_without_a_gateway_exposes_no_gesture();
    a_resolved_commit_becomes_an_undo_checkpoint();
    a_dropped_commit_records_no_checkpoint();
    a_dropped_commit_is_handed_to_the_notice_sink();
    a_refused_commit_is_handed_to_the_notice_sink();
    an_applied_commit_produces_no_notice();
    an_unbound_notice_sink_leaves_the_drop_counted();
    a_settle_rearms_the_inspected_identity();
    a_settle_during_a_gesture_is_deferred_not_served();
    a_settle_deferred_across_an_error_stays_deferred();
    a_settle_deferred_across_an_applied_commit_is_subsumed();
    a_deferral_does_not_outlive_its_selection_or_its_read();
    selection_fetch_mechanics();
    PANELS_TEST_MAIN_END();
}
