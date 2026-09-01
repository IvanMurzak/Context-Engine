// T1 for the Shell-side panel host (M9 e05d1) — the roster projection, the render payload, command
// and gesture dispatch, the D6 state round-trip and its degrade path, and the bridge binding.
//
// THE CENTRAL CLAIM THIS FILE EXISTS TO PROVE IS PANEL-AGNOSTICISM. Almost every case below drives
// SYNTHETIC panels — "test.alpha", "test.beta", "test.gestural" — built here out of raw uitree nodes.
// None of them exists in the e05b roster, none of them is Problems, and `panel_host.cpp` has never
// heard of any of them. If the host contained a single panel-id branch, a Problems-shaped envelope,
// or a hydration path assuming one panel kind, these cases could not pass. That is the property
// e05d3 depends on: it lands Scene tree + Inspector by binding two more providers, changing nothing
// here. A reviewer should read the synthetic roster below as the assertion, not as a convenience.
//
// The one case that DOES use the real roster (`hosts_the_real_roster`) asserts the opposite edge:
// that a rostered panel with NO provider is listed and honestly reports `hosted: false`, which is how
// the editor shows its whole panel set while two of them are still boundary-blocked.

#include "context/editor/shell/panel_host.h"

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/panel_state.h"
#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"
#include "context/editor/shell/ipc_bridge.h"

#include "shell_test.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace shell = context::editor::shell;
namespace gc = context::editor::gui::contract;
namespace ut = context::editor::gui::uitree;
using Json = context::editor::contract::Json;

namespace
{

// --- the synthetic roster -------------------------------------------------------------------------

gc::Contribution make_contribution(std::string id, std::string title, std::uint32_t schema_version)
{
    gc::Contribution c;
    c.id = std::move(id);
    c.kind = gc::ContributionKind::panel;
    c.title = std::move(title);
    c.icon = "test-icon";
    c.dock.default_zone = gc::DockZone::bottom;
    c.dock.min_width = 111;
    c.dock.min_height = 222;
    // Manifest v3 (04 §2). `limited` with a real ceiling on purpose: `singleton` is the DEFAULT, so a
    // fixture using it could not tell a projection that reads the manifest from one that emits a
    // constant.
    c.instances.mode = gc::InstanceMode::limited;
    c.instances.max = 3;
    c.path = "Test/Synthetic";
    c.content.type = gc::ContentType::uitree;
    c.state.schema_version = schema_version;
    c.capabilities = {gc::kCapabilityReadQuery};
    return c;
}

std::vector<gc::Contribution> synthetic_roster()
{
    return {make_contribution("test.alpha", "Alpha", 1),
            make_contribution("test.beta", "Beta", 7),
            make_contribution("test.gestural", "Gestural", 1)};
}

// One synthetic panel with an EXPLICIT instance mode (editor-UX c3). The shared roster above is
// `limited` max 3 on purpose (see make_contribution); the instance cases need each of the three
// modes side by side, and stating the mode at the call site is what keeps a case's expectation
// readable next to the fixture that produces it.
gc::Contribution with_mode(std::string id, gc::InstanceMode mode, int max)
{
    gc::Contribution c = make_contribution(std::move(id), "Modal", 1);
    c.instances.mode = mode;
    c.instances.max = max;
    return c;
}

// A model whose tree changes with its data, so a re-render is observably different — that is what
// makes the revision/patch assertions mean something instead of comparing a constant to itself.
struct FakeModel
{
    std::string label = "first";
    int activations = 0;
    std::vector<std::string> gestures;
    Json blob = Json("initial");

    [[nodiscard]] ut::Panel build() const
    {
        ut::UiNode root(ut::Role::region, "root");
        root.set_label("Synthetic");
        ut::UiNode row(ut::Role::listitem, "row-1");
        row.set_label(label).set_text(label).set_focusable(true).set_command("test.activate");
        root.add_child(std::move(row));

        ut::Panel panel("synthetic", "Synthetic");
        panel.set_root(std::move(root));
        panel.add_command("test.activate", "Activate");
        return panel;
    }
};

shell::PanelProvider make_provider(FakeModel& model, bool with_gestures, bool with_state)
{
    shell::PanelProvider provider;
    provider.build = [&model] { return model.build(); };
    provider.invoke = [&model](const std::string& command_id, const Json&)
    {
        if (command_id != "test.activate")
        {
            return false;
        }
        ++model.activations;
        model.label = "activated";
        return true;
    };
    if (with_gestures)
    {
        provider.gesture = [&model](shell::GestureVerb verb, const Json&)
        {
            model.gestures.emplace_back(shell::gesture_verb_token(verb));
            return true;
        };
    }
    if (with_state)
    {
        provider.get_state = [&model] { return model.blob; };
        provider.restore_state = [&model](const Json& data)
        {
            // A provider is entitled to REFUSE a payload it cannot use — the host reports that as
            // the same "you get defaults" degrade a version mismatch produces.
            if (!data.is_string())
            {
                return false;
            }
            model.blob = data;
            return true;
        };
    }
    return provider;
}

// A model bag that mints ONE FakeModel PER INSTANCE, keyed by instance id — the fixture the whole
// per-instance-state claim rests on.
//
// ⚠ IT DELIBERATELY DOES NOT SHARE. The sibling fixture (`provide`, a single FakeModel) is what
// proves the shared-model binding still shares, and running BOTH is what makes each meaningful: a
// factory test alone cannot tell "each instance got its own model" from "the host renders whatever
// the last caller touched".
struct InstanceModels
{
    // NODE-STABLE: a std::map never invalidates a reference on insert, and the providers below
    // capture `FakeModel&`. A vector would dangle the moment a second instance is created.
    std::map<std::string, FakeModel> models;

    [[nodiscard]] shell::PanelProviderFactory factory(bool with_gestures, bool with_state)
    {
        return [this, with_gestures, with_state](const std::string& instance_id)
        {
            FakeModel& model = models[instance_id];
            // Label the model by its instance so a render is OBSERVABLY per-copy: two instances that
            // shared a model would render the same label, and every "distinct state" assertion below
            // would be comparing a constant to itself.
            model.label = instance_id;
            model.blob = Json(instance_id);
            return make_provider(model, with_gestures, with_state);
        };
    }
};

Json instance_params(const std::string& panel_id, const std::string& instance_id)
{
    Json params = Json::object();
    params.set("panelId", Json(panel_id));
    params.set("instanceId", Json(instance_id));
    return params;
}

// --- helpers over the JSON projections --------------------------------------------------------

const Json* find_panel(const Json& listing, const std::string& id)
{
    const Json& panels = listing.at("panels");
    for (std::size_t i = 0; i < panels.size(); ++i)
    {
        if (panels.at(i).at("id").as_string() == id)
        {
            return &panels.at(i);
        }
    }
    return nullptr;
}

// Dispatch one JSON-RPC call through the router and return the parsed response envelope.
Json call(shell::BridgeRouter& router, const std::string& method, const Json& params)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(1));
    request.set("method", Json(method));
    request.set("params", params);
    const shell::BridgeDispatch dispatch = router.dispatch(request.dump());
    return Json::parse(dispatch.response);
}

Json panel_params(const std::string& panel_id)
{
    Json params = Json::object();
    params.set("panelId", Json(panel_id));
    return params;
}

// --- cases ----------------------------------------------------------------------------------------

void lists_every_rostered_panel_hosted_or_not()
{
    shell::PanelHost host(synthetic_roster());
    FakeModel alpha;
    CHECK(host.provide("test.alpha", make_provider(alpha, false, false)));

    const Json listing = host.list();
    CHECK(listing.at("panels").size() == 3);
    CHECK(listing.at("contractMajor").as_int() == static_cast<std::int64_t>(gc::kContractMajor));

    const Json* hosted = find_panel(listing, "test.alpha");
    CHECK(hosted != nullptr);
    if (hosted != nullptr)
    {
        CHECK(hosted->at("hosted").as_bool());
        CHECK(hosted->at("title").as_string() == "Alpha");
        CHECK(hosted->at("dock").at("zone").as_string() == "bottom");
        CHECK(hosted->at("dock").at("minWidth").as_int() == 111);
        // Manifest v3 on the WIRE. `dock.singleton` is GONE from the projection — asserted as an
        // absence because a stale renderer reading it would otherwise see `false` for every panel and
        // silently treat the whole roster as multi-instance.
        CHECK(!hosted->at("dock").contains("singleton"));
        CHECK(hosted->at("instances").at("mode").as_string() == "limited");
        CHECK(hosted->at("instances").at("max").as_int() == 3);
        CHECK(hosted->at("path").as_string() == "Test/Synthetic");
        CHECK(hosted->at("selection").at("subjects").size() == 0);
        CHECK(hosted->at("events").at("publishes").size() == 0);
        CHECK(hosted->at("events").at("subscribes").size() == 0);
        CHECK(hosted->at("content").at("type").as_string() == "uitree");
        CHECK(hosted->at("state").at(gc::kStateSchemaVersionKey).as_int() == 1);
        CHECK(hosted->at("capabilities").size() == 1);
        // No provider.gesture / get_state were supplied, so both optional capabilities are false.
        CHECK(!hosted->at("gestures").as_bool());
        CHECK(!hosted->at("persists").as_bool());
    }

    // The UNHOSTED half: listed in full, honestly flagged. This is the shape `builtin.inspector`
    // takes in a real build today, and the reason `panel.unknown` and `panel.not_hosted` are
    // different codes.
    const Json* unhosted = find_panel(listing, "test.beta");
    CHECK(unhosted != nullptr);
    if (unhosted != nullptr)
    {
        CHECK(!unhosted->at("hosted").as_bool());
        CHECK(unhosted->at("title").as_string() == "Beta");
        CHECK(unhosted->at("state").at(gc::kStateSchemaVersionKey).as_int() == 7);
    }

    CHECK(host.hosted_count() == 1);
    CHECK(host.knows("test.beta"));
    CHECK(!host.hosts("test.beta"));
    CHECK(!host.knows("test.nonexistent"));
}

void refuses_a_provider_for_an_unrostered_or_bound_panel()
{
    shell::PanelHost host(synthetic_roster());
    FakeModel model;

    // Deny-by-default: a provider cannot smuggle in a panel the roster never declared.
    CHECK(!host.provide("test.not-on-roster", make_provider(model, false, false)));
    CHECK(host.hosted_count() == 0);

    CHECK(host.provide("test.alpha", make_provider(model, false, false)));
    // Double-binding is a wiring bug, not a silent replacement.
    CHECK(!host.provide("test.alpha", make_provider(model, false, false)));

    // A provider that cannot render is not a provider.
    shell::PanelProvider empty;
    CHECK(!host.provide("test.beta", empty));
    CHECK(host.hosted_count() == 1);
}

void renders_html_focus_order_and_commands()
{
    shell::PanelHost host(synthetic_roster());
    FakeModel model;
    CHECK(host.provide("test.alpha", make_provider(model, false, false)));

    std::string error_code;
    const std::optional<shell::PanelRender> rendered = host.render("test.alpha", error_code);
    CHECK(rendered.has_value());
    if (!rendered.has_value())
    {
        return;
    }
    CHECK(error_code.empty());
    CHECK(rendered->panel_id == "test.alpha");
    // The HTML is uitree::render_html's, verbatim — the node ids and the bound command are what the
    // hydration runtime keys its DOM patches and its activation binding on.
    CHECK(shelltest::mentions(rendered->html, "id=\"root\""));
    CHECK(shelltest::mentions(rendered->html, "id=\"row-1\""));
    CHECK(shelltest::mentions(rendered->html, "data-command=\"test.activate\""));
    CHECK(shelltest::mentions(rendered->html, "tabindex=\"0\""));
    // The model's declared focus order, not one re-derived from the markup.
    CHECK(rendered->focus_order.size() == 1);
    CHECK(!rendered->focus_order.empty() && rendered->focus_order.front() == "row-1");
    CHECK(rendered->commands.size() == 1);
    CHECK(!rendered->commands.empty() && rendered->commands.front().id == "test.activate");

    // An unknown panel and an unhosted one are DIFFERENT refusals.
    std::string unknown_code;
    CHECK(!host.render("test.nope", unknown_code).has_value());
    CHECK(unknown_code == shell::kErrPanelUnknown);
    std::string unhosted_code;
    CHECK(!host.render("test.beta", unhosted_code).has_value());
    CHECK(unhosted_code == shell::kErrPanelNotHosted);
}

void renders_a_rootless_panel_as_empty_rather_than_failing()
{
    shell::PanelHost host(synthetic_roster());
    shell::PanelProvider provider;
    // A model built before its first data arrived: a legitimate state, not an error.
    provider.build = [] { return ut::Panel("empty", "Empty"); };
    CHECK(host.provide("test.alpha", std::move(provider)));

    std::string error_code;
    const std::optional<shell::PanelRender> rendered = host.render("test.alpha", error_code);
    CHECK(rendered.has_value());
    if (rendered.has_value())
    {
        CHECK(rendered->html.empty());
        CHECK(rendered->focus_order.empty());
    }
}

void dispatches_commands_and_advances_the_revision()
{
    shell::PanelHost host(synthetic_roster());
    FakeModel model;
    CHECK(host.provide("test.alpha", make_provider(model, false, false)));

    const std::uint64_t before = host.revision("test.alpha");
    bool dispatched = false;
    std::string error_code;
    CHECK(host.invoke("test.alpha", "test.activate", Json::object(), dispatched, error_code));
    CHECK(dispatched);
    CHECK(model.activations == 1);
    CHECK(host.revision("test.alpha") > before);

    // The re-render reflects the model change — the reason the runtime re-renders on a revision bump.
    std::string render_code;
    const std::optional<shell::PanelRender> rendered = host.render("test.alpha", render_code);
    CHECK(rendered.has_value());
    CHECK(rendered.has_value() && shelltest::mentions(rendered->html, "activated"));

    // A command the panel model does not expose is REFUSED at the seam, not forwarded. This is the
    // stale-mounted-DOM case: the renderer holds a `data-command` the model has since dropped.
    bool ghost_dispatched = false;
    std::string ghost_code;
    const std::uint64_t steady = host.revision("test.alpha");
    CHECK(!host.invoke("test.alpha", "test.ghost", Json::object(), ghost_dispatched, ghost_code));
    CHECK(!ghost_dispatched);
    CHECK(ghost_code == shell::kErrPanelUnknownCommand);
    CHECK(model.activations == 1);
    CHECK(host.revision("test.alpha") == steady);
}

void maps_the_four_gesture_verbs_and_refuses_a_fifth()
{
    CHECK(shell::parse_gesture_verb("begin").has_value());
    CHECK(shell::parse_gesture_verb("extend").has_value());
    CHECK(shell::parse_gesture_verb("commit").has_value());
    CHECK(shell::parse_gesture_verb("cancel").has_value());
    // The closed vocabulary: a verb invented in the renderer cannot reach a model.
    CHECK(!shell::parse_gesture_verb("paint").has_value());
    CHECK(!shell::parse_gesture_verb("").has_value());
    CHECK(!shell::parse_gesture_verb("BEGIN").has_value());
    CHECK(std::string(shell::gesture_verb_token(shell::GestureVerb::commit)) == "commit");

    shell::PanelHost host(synthetic_roster());
    FakeModel gestural;
    FakeModel inert;
    CHECK(host.provide("test.gestural", make_provider(gestural, true, false)));
    CHECK(host.provide("test.alpha", make_provider(inert, false, false)));

    bool dispatched = false;
    std::string error_code;
    CHECK(host.gesture("test.gestural", shell::GestureVerb::begin, Json::object(), dispatched,
                       error_code));
    CHECK(dispatched);
    CHECK(host.gesture("test.gestural", shell::GestureVerb::extend, Json::object(), dispatched,
                       error_code));
    CHECK(host.gesture("test.gestural", shell::GestureVerb::commit, Json::object(), dispatched,
                       error_code));
    CHECK(gestural.gestures.size() == 3);
    CHECK(gestural.gestures.size() == 3 && gestural.gestures[0] == "begin" &&
          gestural.gestures[1] == "extend" && gestural.gestures[2] == "commit");

    // A panel that declares no gestures REFUSES them rather than silently succeeding — which is why
    // `panel.list` publishes `gestures` at all: the runtime never has to guess.
    bool inert_dispatched = false;
    std::string inert_code;
    CHECK(!host.gesture("test.alpha", shell::GestureVerb::begin, Json::object(), inert_dispatched,
                        inert_code));
    CHECK(inert_code == shell::kErrPanelBadGesture);
    CHECK(inert.gestures.empty());

    const Json listing = host.list();
    const Json* gestural_entry = find_panel(listing, "test.gestural");
    CHECK(gestural_entry != nullptr && gestural_entry->at("gestures").as_bool());
    const Json* inert_entry = find_panel(listing, "test.alpha");
    CHECK(inert_entry != nullptr && !inert_entry->at("gestures").as_bool());
}

void round_trips_d6_state_and_degrades_on_a_schema_mismatch()
{
    shell::PanelHost host(synthetic_roster());
    FakeModel model;
    model.blob = Json("persisted-payload");
    CHECK(host.provide("test.alpha", make_provider(model, false, true)));

    // --- the round trip. The blob's version comes from the MANIFEST, not the provider.
    std::string error_code;
    const std::optional<Json> persisted = host.get_state("test.alpha", error_code);
    CHECK(persisted.has_value());
    if (!persisted.has_value())
    {
        return;
    }
    CHECK(persisted->at(gc::kStateSchemaVersionKey).as_int() == 1);
    CHECK(persisted->at(gc::kStateDataKey).as_string() == "persisted-payload");

    model.blob = Json("clobbered");
    bool restored = false;
    std::string code;
    std::string diagnostic;
    CHECK(host.restore_state("test.alpha", *persisted, restored, code, diagnostic, error_code));
    CHECK(restored);
    CHECK(code.empty());
    CHECK(diagnostic.empty());
    CHECK(model.blob.as_string() == "persisted-payload");

    // --- THE D6 DEGRADE PATH (04 §3): a blob written against another schema version is NOT migrated
    // and NOT partially applied. The call SUCCEEDS, the panel keeps its defaults, and the caller
    // gets a diagnostic. Never a crash and never an error — e05d2 relies on exactly this so one
    // stale panel blob cannot discard a whole layout.
    Json stale = Json::object();
    stale.set(gc::kStateSchemaVersionKey, Json(99));
    stale.set(gc::kStateDataKey, Json("from-a-future-version"));
    model.blob = Json("untouched");
    bool stale_restored = true;
    std::string stale_code;
    std::string stale_diagnostic;
    CHECK(host.restore_state("test.alpha", stale, stale_restored, stale_code, stale_diagnostic,
                             error_code));
    CHECK(!stale_restored);
    CHECK(stale_code == gc::kErrStateSchemaMismatch);
    CHECK(!stale_diagnostic.empty());
    CHECK(model.blob.as_string() == "untouched");

    // A hostile / truncated blob degrades identically (malformed rather than mismatched).
    bool junk_restored = true;
    std::string junk_code;
    std::string junk_diagnostic;
    CHECK(host.restore_state("test.alpha", Json("not-an-object"), junk_restored, junk_code,
                             junk_diagnostic, error_code));
    CHECK(!junk_restored);
    CHECK(junk_code == gc::kErrStateMalformed);
    CHECK(model.blob.as_string() == "untouched");

    // A well-formed, correctly-versioned blob the PROVIDER refuses degrades the same way.
    Json refused = Json::object();
    refused.set(gc::kStateSchemaVersionKey, Json(1));
    refused.set(gc::kStateDataKey, Json::array());
    bool refused_restored = true;
    std::string refused_code;
    std::string refused_diagnostic;
    CHECK(host.restore_state("test.alpha", refused, refused_restored, refused_code,
                             refused_diagnostic, error_code));
    CHECK(!refused_restored);
    CHECK(refused_code == gc::kErrStateMalformed);
    CHECK(model.blob.as_string() == "untouched");

    // A panel that persists nothing says so, rather than inventing an empty blob.
    FakeModel stateless;
    CHECK(host.provide("test.beta", make_provider(stateless, false, false)));
    std::string no_state_code;
    CHECK(!host.get_state("test.beta", no_state_code).has_value());
    CHECK(no_state_code == shell::kErrPanelNoState);
}

void binds_every_panel_method_on_the_router()
{
    shell::PanelHost host(synthetic_roster());
    FakeModel model;
    CHECK(host.provide("test.alpha", make_provider(model, true, true)));

    shell::BridgeRouter router;
    CHECK(host.install(router));
    CHECK(router.has_method(shell::kPanelListMethod));
    CHECK(router.has_method(shell::kPanelRenderMethod));
    CHECK(router.has_method(shell::kPanelCommandMethod));
    CHECK(router.has_method(shell::kPanelGestureMethod));
    CHECK(router.has_method(shell::kPanelStateGetMethod));
    CHECK(router.has_method(shell::kPanelStateSetMethod));

    // Installing twice is a name collision, reported rather than silently ignored.
    shell::PanelHost twice(synthetic_roster());
    CHECK(!twice.install(router));

    // --- panel.list over the wire
    const Json listed = call(router, shell::kPanelListMethod, Json::object());
    CHECK(listed.contains("result"));
    CHECK(listed.at("result").at("panels").size() == 3);
    CHECK(host.lists_served() == 1);

    // --- panel.render over the wire
    const Json rendered = call(router, shell::kPanelRenderMethod, panel_params("test.alpha"));
    CHECK(rendered.contains("result"));
    CHECK(shelltest::mentions(rendered.at("result").at("html").as_string(), "id=\"row-1\""));
    CHECK(rendered.at("result").at("focusOrder").size() == 1);
    CHECK(rendered.at("result").at("commands").size() == 1);
    CHECK(host.renders_served() == 1);

    // --- panel.command over the wire
    Json command_params = panel_params("test.alpha");
    command_params.set("commandId", Json("test.activate"));
    const Json commanded = call(router, shell::kPanelCommandMethod, command_params);
    CHECK(commanded.contains("result"));
    CHECK(commanded.at("result").at("dispatched").as_bool());
    CHECK(model.activations == 1);
    CHECK(host.commands_dispatched() == 1);

    // --- panel.gesture over the wire
    Json gesture_params = panel_params("test.alpha");
    gesture_params.set("verb", Json("begin"));
    const Json gestured = call(router, shell::kPanelGestureMethod, gesture_params);
    CHECK(gestured.contains("result"));
    CHECK(gestured.at("result").at("dispatched").as_bool());
    CHECK(model.gestures.size() == 1);

    // --- panel.state.get / .set over the wire
    const Json got = call(router, shell::kPanelStateGetMethod, panel_params("test.alpha"));
    CHECK(got.contains("result"));
    Json set_params = panel_params("test.alpha");
    set_params.set("state", got.at("result").at("state"));
    const Json set = call(router, shell::kPanelStateSetMethod, set_params);
    CHECK(set.contains("result"));
    CHECK(set.at("result").at("restored").as_bool());
}

// Every panel method is reachable by a RENDERER, which means every one of them is reachable with
// arbitrary params. None may throw, crash, or answer anything but a well-formed refusal.
void refuses_hostile_params_without_crashing()
{
    shell::PanelHost host(synthetic_roster());
    FakeModel model;
    CHECK(host.provide("test.alpha", make_provider(model, true, true)));
    shell::BridgeRouter router;
    CHECK(host.install(router));

    // Missing / wrong-typed panelId on every method that takes one.
    const char* methods[] = {shell::kPanelRenderMethod, shell::kPanelCommandMethod,
                             shell::kPanelGestureMethod, shell::kPanelStateGetMethod,
                             shell::kPanelStateSetMethod};
    for (const char* method : methods)
    {
        const Json missing = call(router, method, Json::object());
        CHECK(missing.contains("error"));
        CHECK(missing.at("error").at("data").at("reason").as_string() == shell::kErrPanelBadParams);

        Json wrong_type = Json::object();
        wrong_type.set("panelId", Json(42));
        const Json typed = call(router, method, wrong_type);
        CHECK(typed.contains("error"));
        CHECK(typed.at("error").at("data").at("reason").as_string() == shell::kErrPanelBadParams);
    }

    // An unknown panel id.
    const Json unknown = call(router, shell::kPanelRenderMethod, panel_params("../../etc/passwd"));
    CHECK(unknown.contains("error"));
    CHECK(unknown.at("error").at("data").at("reason").as_string() == shell::kErrPanelUnknown);

    // A gesture verb outside the closed vocabulary.
    Json bad_verb = panel_params("test.alpha");
    bad_verb.set("verb", Json("obliterate"));
    const Json refused_verb = call(router, shell::kPanelGestureMethod, bad_verb);
    CHECK(refused_verb.contains("error"));
    CHECK(refused_verb.at("error").at("data").at("reason").as_string() == shell::kErrPanelBadGesture);

    // panel.state.set with NO state member: the documented "restore from nothing" degrade, which is
    // a SUCCESS carrying a diagnostic rather than a refusal.
    const Json no_state = call(router, shell::kPanelStateSetMethod, panel_params("test.alpha"));
    CHECK(no_state.contains("result"));
    CHECK(!no_state.at("result").at("restored").as_bool());
    CHECK(!no_state.at("result").at("diagnostic").as_string().empty());

    // Nothing above may have been contained as a thrown handler — every refusal is classified.
    CHECK(router.last_reject() == shell::BridgeReject::none);
}

// The one case over the REAL roster: the whole panel set is listed, and the two panels e05d3 must
// unblock are present-but-unhosted rather than missing.
void hosts_the_real_roster()
{
    shell::PanelHost host;
    CHECK(host.roster_size() == gc::builtin_contributions().size());
    CHECK(host.hosted_count() == 0); // nothing bound yet — providers come from the composition root

    const Json listing = host.list();
    CHECK(listing.at("panels").size() == gc::builtin_contributions().size());
    for (const char* id : {"builtin.problems", "builtin.scene-tree", "builtin.inspector"})
    {
        const Json* entry = find_panel(listing, id);
        CHECK(entry != nullptr);
        CHECK(entry != nullptr && !entry->at("hosted").as_bool());
    }
    CHECK(host.knows("builtin.problems"));
    CHECK(host.knows("builtin.inspector"));
}

// e07b: a panel's manifest `commands` (04 §3) are projected INTO panel.list, so the editor-core
// command registry's source (c) reads them from the promoted roster. A contribution that declares
// commands carries them here — id, title, and the `when` clause verbatim (empty = always); a panel
// that declares none still carries the field as an empty array, so the JS parser never sees it absent.
void projects_manifest_commands_into_the_roster()
{
    gc::Contribution declares = make_contribution("test.declares", "Declares", 1);
    declares.commands = {
        gc::CommandContribution{"declares.alpha", "Alpha Command", "panelFocus == test.declares"},
        gc::CommandContribution{"declares.beta", "Beta Command", ""},
    };
    gc::Contribution silent = make_contribution("test.silent", "Silent", 1); // declares no commands

    std::vector<gc::Contribution> roster;
    roster.push_back(std::move(declares));
    roster.push_back(std::move(silent));
    shell::PanelHost host(std::move(roster));

    const Json listing = host.list();

    const Json* with_commands = find_panel(listing, "test.declares");
    CHECK(with_commands != nullptr);
    if (with_commands != nullptr)
    {
        const Json& commands = with_commands->at("commands");
        CHECK(commands.size() == 2);
        CHECK(commands.at(0).at("id").as_string() == "declares.alpha");
        CHECK(commands.at(0).at("title").as_string() == "Alpha Command");
        CHECK(commands.at(0).at("when").as_string() == "panelFocus == test.declares");
        CHECK(commands.at(1).at("id").as_string() == "declares.beta");
        // An always-active command round-trips its `when` as the empty string, not a dropped field.
        CHECK(commands.at(1).at("when").as_string().empty());
    }

    const Json* without = find_panel(listing, "test.silent");
    CHECK(without != nullptr);
    if (without != nullptr)
    {
        CHECK(without->at("commands").size() == 0);
    }
}


// --- editor-UX c3: the instance runtime -------------------------------------------------------

// The id COMPOSITION rule, both directions. It is a wire contract (`kPanelInstanceSeparator` is
// byte-compared against the TS mirror by `webui-panel-contract`), and the decomposition is what a
// layout restore depends on: Dockview re-creates a persisted panel BY ID before anything has
// registered it, so the kind must be recoverable from the id alone.
void composes_and_decomposes_instance_ids()
{
    CHECK(shell::make_panel_instance_id("builtin.problems", 1) == "builtin.problems#1");
    CHECK(shell::make_panel_instance_id("builtin.problems", 42) == "builtin.problems#42");
    CHECK(shell::panel_id_of_instance("builtin.problems#1") == "builtin.problems");
    // A bare id — what a persisted arrangement written before instances existed carries — reads as
    // the KIND itself, which is the honest restore rather than a lookup that can only fail.
    CHECK(shell::panel_id_of_instance("builtin.problems") == "builtin.problems");
    // Splits on the LAST separator: a panel id containing one must resolve to the panel that
    // exists, not to a prefix that does not.
    CHECK(shell::panel_id_of_instance("odd#name#7") == "odd#name");
}

// SINGLETON: the second open FOCUSES the live copy and reports it, which is the behaviour
// `dock.singleton` never had — `open` used to refuse a second open of every panel whatever its mode
// said, so the flag could not change any outcome.
void singleton_focuses_instead_of_refusing()
{
    std::vector<gc::Contribution> roster;
    roster.push_back(with_mode("test.single", gc::InstanceMode::singleton, 0));
    shell::PanelHost host(std::move(roster));
    InstanceModels models;
    CHECK(host.provide_factory("test.single", models.factory(false, false)));

    const shell::InstanceOpen first = host.open_instance("test.single");
    CHECK(first.outcome == shell::InstanceOutcome::opened);
    CHECK(first.instance_id == "test.single#1");

    const shell::InstanceOpen second = host.open_instance("test.single");
    CHECK(second.outcome == shell::InstanceOutcome::focused);
    // THE SAME copy, and no diagnostic: "already open" is an answer, not a failure.
    CHECK(second.instance_id == first.instance_id);
    CHECK(second.code.empty());
    CHECK(host.instances("test.single").size() == 1);

    // A DIFFERENT id, though, is genuinely refused — a singleton cannot hold two copies, and
    // silently answering with the live one would attach the caller's state to the wrong copy.
    const shell::InstanceOpen other = host.open_instance("test.single", "test.single#2");
    CHECK(other.outcome == shell::InstanceOutcome::refused);
    CHECK(other.code == shell::kErrPanelInstanceLimit);
    CHECK(shelltest::mentions(other.diagnostic, "singleton"));
    CHECK(host.instances("test.single").size() == 1);
}

// LIMITED: opens up to `max`, and the max+1 is refused WITH THE LIMIT NAMED. Both halves matter —
// a refusal alone would pass with the ceiling set to zero, so the three successful opens are the
// non-vacuity sibling.
void limited_opens_to_max_then_refuses_naming_the_limit()
{
    std::vector<gc::Contribution> roster;
    roster.push_back(with_mode("test.limited", gc::InstanceMode::limited, 2));
    shell::PanelHost host(std::move(roster));
    InstanceModels models;
    CHECK(host.provide_factory("test.limited", models.factory(false, false)));

    CHECK(host.open_instance("test.limited").outcome == shell::InstanceOutcome::opened);
    CHECK(host.open_instance("test.limited").outcome == shell::InstanceOutcome::opened);
    CHECK(host.instances("test.limited").size() == 2);

    const shell::InstanceOpen third = host.open_instance("test.limited");
    CHECK(third.outcome == shell::InstanceOutcome::refused);
    CHECK(third.code == shell::kErrPanelInstanceLimit);
    // NAMES the limit (design 04 section 3). A diagnostic that only said "refused" would send the
    // human to the manifest to find out what they hit.
    CHECK(shelltest::mentions(third.diagnostic, "max 2"));
    CHECK(host.instances("test.limited").size() == 2);

    // CLOSING FREES A SLOT — the half a ceiling counted over the SESSION rather than over the LIVE
    // set would fail. Without `close_instance` a `limited` panel would refuse forever after `max`
    // opens, however many the user had since closed.
    CHECK(host.close_instance("test.limited", "test.limited#1"));
    CHECK(host.instances("test.limited").size() == 1);
    const shell::InstanceOpen reopened = host.open_instance("test.limited");
    CHECK(reopened.outcome == shell::InstanceOutcome::opened);
    // A FRESH ordinal, never a reused one: the counter is monotonic so a stale caller still holding
    // `#1` cannot be handed the model of the copy that replaced it.
    CHECK(reopened.instance_id == "test.limited#3");
    CHECK(!host.close_instance("test.limited", "test.limited#1")); // idempotent, not an error
}

// UNLIMITED: every open mints a DISTINCT copy, and each copy holds its OWN state. This is the
// factory binding's whole reason to exist.
void unlimited_mints_distinct_instances_with_distinct_state()
{
    std::vector<gc::Contribution> roster;
    roster.push_back(with_mode("test.many", gc::InstanceMode::unlimited, 0));
    shell::PanelHost host(std::move(roster));
    InstanceModels models;
    CHECK(host.provide_factory("test.many", models.factory(false, true)));

    const shell::InstanceOpen a = host.open_instance("test.many");
    const shell::InstanceOpen b = host.open_instance("test.many");
    CHECK(a.outcome == shell::InstanceOutcome::opened);
    CHECK(b.outcome == shell::InstanceOutcome::opened);
    CHECK(a.instance_id != b.instance_id);
    CHECK(host.instances("test.many").size() == 2);

    // DISTINCT RENDERS. The factory labels each model with its own instance id, so identical HTML
    // here would mean the two copies share one model — the exact failure the pair exists to rule out.
    std::string code;
    const std::optional<shell::PanelRender> render_a = host.render("test.many", code, a.instance_id);
    const std::optional<shell::PanelRender> render_b = host.render("test.many", code, b.instance_id);
    CHECK(render_a.has_value() && render_b.has_value());
    if (render_a.has_value() && render_b.has_value())
    {
        CHECK(render_a->instance_id == a.instance_id);
        CHECK(render_b->instance_id == b.instance_id);
        CHECK(render_a->html != render_b->html);
        CHECK(shelltest::mentions(render_a->html, a.instance_id.c_str()));
        CHECK(shelltest::mentions(render_b->html, b.instance_id.c_str()));
    }

    // DISTINCT STATE, and a write to one does NOT leak to the other.
    Json persisted = Json::object();
    persisted.set(gc::kStateSchemaVersionKey, Json(static_cast<std::uint64_t>(1)));
    persisted.set(gc::kStateDataKey, Json("only-a"));
    bool restored = false;
    std::string state_code;
    std::string diagnostic;
    CHECK(host.restore_state("test.many", persisted, restored, state_code, diagnostic, code,
                             a.instance_id));
    CHECK(restored);

    const std::optional<Json> state_a = host.get_state("test.many", code, a.instance_id);
    const std::optional<Json> state_b = host.get_state("test.many", code, b.instance_id);
    CHECK(state_a.has_value() && state_b.has_value());
    if (state_a.has_value() && state_b.has_value())
    {
        CHECK(state_a->at(gc::kStateDataKey).as_string() == "only-a");
        // B still holds what its own factory gave it. Equality here would be the leak.
        CHECK(state_b->at(gc::kStateDataKey).as_string() == b.instance_id);
    }
}

// THE SIBLING that keeps the case above honest: a `provide()` binding SHARES one model across every
// copy, exactly as panel_host.h documents. Without this, "each factory instance has its own state"
// could not be told from "the host always gives every instance its own state", and the factory
// binding would look like an elaborate no-op.
void a_shared_provider_binding_shares_one_model_across_instances()
{
    std::vector<gc::Contribution> roster;
    roster.push_back(with_mode("test.shared", gc::InstanceMode::unlimited, 0));
    shell::PanelHost host(std::move(roster));
    FakeModel one;
    CHECK(host.provide("test.shared", make_provider(one, false, true)));

    const shell::InstanceOpen a = host.open_instance("test.shared");
    const shell::InstanceOpen b = host.open_instance("test.shared");
    CHECK(a.instance_id != b.instance_id);

    // A command dispatched to A changes the ONE model, so B renders the change too.
    bool dispatched = false;
    std::string code;
    CHECK(host.invoke("test.shared", "test.activate", Json::object(), dispatched, code,
                      a.instance_id));
    CHECK(dispatched);
    CHECK(one.activations == 1);
    const std::optional<shell::PanelRender> render_b = host.render("test.shared", code, b.instance_id);
    CHECK(render_b.has_value());
    if (render_b.has_value())
    {
        CHECK(shelltest::mentions(render_b->html, "activated"));
    }
}

// The DEFAULT instance: a call that names no id addresses the kind's first live copy, and
// MATERIALISES one when there is none. That is what keeps every pre-c3 caller — 55 of them —
// meaning exactly what it meant.
void an_unnamed_instance_resolves_to_the_default_copy()
{
    shell::PanelHost host(synthetic_roster());
    FakeModel alpha;
    CHECK(host.provide("test.alpha", make_provider(alpha, false, false)));
    CHECK(host.instances("test.alpha").empty());

    std::string code;
    const std::optional<shell::PanelRender> first = host.render("test.alpha", code);
    CHECK(first.has_value());
    if (first.has_value())
    {
        // MATERIALISED on first use, with the ordinary first ordinal.
        CHECK(first->instance_id == "test.alpha#1");
    }
    CHECK(host.instances("test.alpha").size() == 1);

    // And a second unnamed call reuses it rather than minting a second copy — otherwise every poll
    // of a singleton panel would allocate a model.
    const std::optional<shell::PanelRender> second = host.render("test.alpha", code);
    CHECK(second.has_value() && host.instances("test.alpha").size() == 1);

    // A NAMED id the renderer minted is materialised too (it owns lifecycle; the ceiling is the
    // backstop), and the mint counter moves past it so the next default cannot collide.
    const std::optional<shell::PanelRender> named = host.render("test.alpha", code, "test.alpha#9");
    CHECK(named.has_value());
    CHECK(host.instances("test.alpha").size() == 2);
    CHECK(host.open_instance("test.alpha").instance_id == "test.alpha#10");
}

// The RESOURCE ceiling and the id-length bound — the two limits that are about an untrusted renderer
// rather than about the manifest. `unlimited` is a statement about the panel, not a licence for an
// unbounded allocation driven from the wire.
void bounds_instance_count_and_id_length_against_a_hostile_renderer()
{
    std::vector<gc::Contribution> roster;
    roster.push_back(with_mode("test.many", gc::InstanceMode::unlimited, 0));
    shell::PanelHost host(std::move(roster));
    InstanceModels models;
    CHECK(host.provide_factory("test.many", models.factory(false, false)));

    for (std::size_t i = 0; i < shell::kMaxPanelInstances; ++i)
    {
        CHECK(host.open_instance("test.many").outcome == shell::InstanceOutcome::opened);
    }
    const shell::InstanceOpen past = host.open_instance("test.many");
    CHECK(past.outcome == shell::InstanceOutcome::refused);
    CHECK(past.code == shell::kErrPanelInstanceLimit);
    CHECK(shelltest::mentions(past.diagnostic, "host ceiling"));

    // A 4 KiB instance id is refused as BAD PARAMS, not as a limit: it is a malformed address, and
    // classifying it as a ceiling would tell the caller to close a panel that would not help.
    std::vector<gc::Contribution> other;
    other.push_back(with_mode("test.wide", gc::InstanceMode::unlimited, 0));
    shell::PanelHost wide(std::move(other));
    InstanceModels wide_models;
    CHECK(wide.provide_factory("test.wide", wide_models.factory(false, false)));
    const shell::InstanceOpen huge = wide.open_instance("test.wide", std::string(4096, 'x'));
    CHECK(huge.outcome == shell::InstanceOutcome::refused);
    CHECK(huge.code == shell::kErrPanelBadParams);
    CHECK(wide.instances("test.wide").empty());
}

// A factory whose probe cannot render is refused AT BIND TIME, exactly as a build-less provider is —
// and a factory that CAN is bound with its capability shape read off that same probe, so
// `panel.list` answers `gestures`/`persists` honestly for a kind with no live copy yet (which is
// every kind at boot, when the renderer reads the roster).
void a_factory_is_probed_at_bind_time_for_renderability_and_capabilities()
{
    std::vector<gc::Contribution> roster;
    roster.push_back(with_mode("test.many", gc::InstanceMode::unlimited, 0));
    roster.push_back(with_mode("test.broken", gc::InstanceMode::unlimited, 0));
    shell::PanelHost host(std::move(roster));

    CHECK(!host.provide_factory("test.broken",
                                [](const std::string&) { return shell::PanelProvider{}; }));
    CHECK(!host.hosts("test.broken"));
    CHECK(!host.provide_factory("test.many", nullptr));

    InstanceModels models;
    CHECK(host.provide_factory("test.many", models.factory(true, true)));
    const Json listing = host.list();
    const Json* many = find_panel(listing, "test.many");
    CHECK(many != nullptr);
    if (many != nullptr)
    {
        CHECK(many->at("hosted").as_bool());
        // Asserted with NO instance open — the state the renderer actually reads the roster in.
        CHECK(many->at("gestures").as_bool());
        CHECK(many->at("persists").as_bool());
    }
    CHECK(host.instances("test.many").empty());
}

// The WIRE half: `instanceId` is optional on all five panel methods, echoed on every reply, and a
// non-string one is refused rather than silently defaulted to the first copy — which would route one
// instance's command into another's model.
void carries_the_instance_id_on_every_panel_method()
{
    std::vector<gc::Contribution> roster;
    roster.push_back(with_mode("test.many", gc::InstanceMode::unlimited, 0));
    shell::PanelHost host(std::move(roster));
    InstanceModels models;
    CHECK(host.provide_factory("test.many", models.factory(true, true)));
    shell::BridgeRouter router;
    CHECK(host.install(router));

    const Json addressed = call(router, shell::kPanelRenderMethod,
                                instance_params("test.many", "test.many#2"));
    CHECK(addressed.contains("result"));
    if (addressed.contains("result"))
    {
        CHECK(addressed.at("result").at("instanceId").as_string() == "test.many#2");
        CHECK(shelltest::mentions(addressed.at("result").at("html").as_string(), "test.many#2"));
    }

    // UNNAMED still answers, and the reply NAMES the copy it addressed rather than echoing the
    // caller's empty string back — a caller that sent no id still learns which copy it reached.
    const Json unnamed = call(router, shell::kPanelStateGetMethod, panel_params("test.many"));
    CHECK(unnamed.contains("result"));
    if (unnamed.contains("result"))
    {
        CHECK(unnamed.at("result").at("instanceId").as_string() == "test.many#2");
    }

    // A NUMBER where the id belongs is a refusal. Defaulting it would be indistinguishable from a
    // caller that meant the default copy, which is precisely the confusion that must not be silent.
    Json bad = Json::object();
    bad.set("panelId", Json("test.many"));
    bad.set("instanceId", Json(static_cast<std::int64_t>(7)));
    const Json refused = call(router, shell::kPanelRenderMethod, bad);
    CHECK(refused.contains("error"));
    if (refused.contains("error"))
    {
        CHECK(refused.at("error").at("data").at("reason").as_string() == shell::kErrPanelBadParams);
    }

    // And the RELEASE verb frees the model, so a ceiling counts LIVE copies rather than opens.
    const Json closed = call(router, shell::kPanelInstanceCloseMethod,
                             instance_params("test.many", "test.many#2"));
    CHECK(closed.contains("result"));
    if (closed.contains("result"))
    {
        CHECK(closed.at("result").at("closed").as_bool());
    }
    CHECK(host.instances("test.many").empty());
    // A second close is `closed:false` and NOT an error — a double close is ordinary.
    const Json again = call(router, shell::kPanelInstanceCloseMethod,
                            instance_params("test.many", "test.many#2"));
    CHECK(again.contains("result") && !again.at("result").at("closed").as_bool());
}

// A CEILING REFUSAL NAMES THE LIMIT ON THE WIRE, not only on the in-process `open_instance` path.
//
// `resolve_instance` has nowhere to put the diagnostic `may_open` produced, so every `panel.*`
// handler used to answer `panel.instance_limit` with its own generic wording — "cannot be rendered
// by this build", which is FALSE (the build hosts the panel fine) and contradicts
// `kErrPanelInstanceLimit`'s own contract that the diagnostic NAMES the limit (design 04 section 3).
// The cost is a debugging round: a human who hit a ceiling went looking for a missing feature
// instead of closing a panel. `open_instance` was the ONLY path that surfaced it, and it is not on
// the wire — so the in-process tests above all passed while the renderer saw the wrong cause.
void the_wire_names_the_limit_when_a_ceiling_refuses()
{
    std::vector<gc::Contribution> roster;
    roster.push_back(with_mode("test.limited", gc::InstanceMode::limited, 2));
    roster.push_back(with_mode("test.capped", gc::InstanceMode::limited, 1));
    shell::PanelHost host(std::move(roster));
    InstanceModels models;
    CHECK(host.provide_factory("test.limited", models.factory(true, true)));
    // The sibling subject below: a KNOWN panel that persists nothing AND sits at its own ceiling —
    // both properties are load-bearing, see there.
    CHECK(host.provide_factory("test.capped", models.factory(false, false)));
    shell::BridgeRouter router;
    CHECK(host.install(router));

    CHECK(host.open_instance("test.limited").outcome == shell::InstanceOutcome::opened);
    CHECK(host.open_instance("test.limited").outcome == shell::InstanceOutcome::opened);

    // A THIRD copy, addressed from the wire — the ceiling refuses it on every method that would
    // otherwise materialise one. Two methods rather than one because the wording is per-handler.
    const Json rendered =
        call(router, shell::kPanelRenderMethod, instance_params("test.limited", "test.limited#9"));
    CHECK(rendered.contains("error"));
    if (rendered.contains("error"))
    {
        CHECK(rendered.at("error").at("data").at("reason").as_string()
              == shell::kErrPanelInstanceLimit);
        const std::string message = rendered.at("error").at("message").as_string();
        CHECK(shelltest::mentions(message, "max 2"));
        // And the old wording, which asserted a cause this build contradicts, is gone.
        CHECK(!shelltest::mentions(message, "cannot be rendered by this build"));
    }

    const Json state = call(router, shell::kPanelStateGetMethod,
                            instance_params("test.limited", "test.limited#9"));
    CHECK(state.contains("error"));
    if (state.contains("error"))
    {
        CHECK(state.at("error").at("data").at("reason").as_string()
              == shell::kErrPanelInstanceLimit);
        CHECK(shelltest::mentions(state.at("error").at("message").as_string(), "max 2"));
    }

    // NON-VACUITY SIBLING, ON A PANEL THIS HOST KNOWS. A refusal that is not a ceiling keeps its
    // handler's own wording, so the recomputation stays narrow rather than becoming a blanket
    // rewrite of every `panel.*` error message.
    //
    // ⚠ THE SUBJECT NEEDS BOTH PROPERTIES, and MEASUREMENT is why — two weaker siblings were tried
    // first and BOTH stayed green against a plant that deleted the `error_code` guard:
    //   * an UNKNOWN panel is refused before the code is ever consulted (`find` misses, so the
    //     fallback is returned whatever the code says), and
    //   * a known panel BELOW its ceiling takes the defensive `may_open() == true` branch, which
    //     also returns the fallback.
    // Only a known panel sitting AT its ceiling reaches the recomputation with a non-ceiling code.
    // `test.capped` is `limited` with max 1 and persists nothing, so once its single copy is open a
    // `panel.state.get` refuses with `panel.no_state` from a panel `may_open` would refuse — the one
    // combination that can tell a narrow recomputation from a blanket one.
    CHECK(host.open_instance("test.capped").outcome == shell::InstanceOutcome::opened);
    const Json no_state = call(router, shell::kPanelStateGetMethod, panel_params("test.capped"));
    CHECK(no_state.contains("error"));
    if (no_state.contains("error"))
    {
        const std::string message = no_state.at("error").at("message").as_string();
        CHECK(no_state.at("error").at("data").at("reason").as_string()
              != shell::kErrPanelInstanceLimit);
        CHECK(shelltest::mentions(message, "panel.state.get refused for 'test.capped'"));
        CHECK(!shelltest::mentions(message, "max 1"));
    }

    // And an UNKNOWN panel still keeps the render handler's own wording too.
    const Json unknown = call(router, shell::kPanelRenderMethod,
                              instance_params("test.nonexistent", "test.nonexistent#1"));
    CHECK(unknown.contains("error"));
    if (unknown.contains("error"))
    {
        CHECK(unknown.at("error").at("data").at("reason").as_string()
              != shell::kErrPanelInstanceLimit);
        CHECK(shelltest::mentions(unknown.at("error").at("message").as_string(),
                                  "cannot be rendered by this build"));
    }
}

} // namespace

int main()
{
    lists_every_rostered_panel_hosted_or_not();
    refuses_a_provider_for_an_unrostered_or_bound_panel();
    renders_html_focus_order_and_commands();
    renders_a_rootless_panel_as_empty_rather_than_failing();
    dispatches_commands_and_advances_the_revision();
    maps_the_four_gesture_verbs_and_refuses_a_fifth();
    round_trips_d6_state_and_degrades_on_a_schema_mismatch();
    binds_every_panel_method_on_the_router();
    refuses_hostile_params_without_crashing();
    hosts_the_real_roster();
    projects_manifest_commands_into_the_roster();
    composes_and_decomposes_instance_ids();
    singleton_focuses_instead_of_refusing();
    limited_opens_to_max_then_refuses_naming_the_limit();
    unlimited_mints_distinct_instances_with_distinct_state();
    a_shared_provider_binding_shares_one_model_across_instances();
    an_unnamed_instance_resolves_to_the_default_copy();
    bounds_instance_count_and_id_length_against_a_hostile_renderer();
    a_factory_is_probed_at_bind_time_for_renderability_and_capabilities();
    carries_the_instance_id_on_every_panel_method();
    the_wire_names_the_limit_when_a_ceiling_refuses();
    SHELL_TEST_MAIN_END();
}
