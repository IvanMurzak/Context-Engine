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

#include <cstdint>
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
    c.dock.singleton = true;
    c.dock.min_width = 111;
    c.dock.min_height = 222;
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
    SHELL_TEST_MAIN_END();
}
