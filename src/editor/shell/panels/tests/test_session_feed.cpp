// The M9 e08b DAEMON SESSION feed (R-QA-013): the `session` topic subscriber, `origin` echo
// suppression, and the two write seams (`editor.select`, `editor.play|pause|stop|step`) — driven
// through a REAL `client::Client` over the scripted `clientmock::MockChannel`.
//
// ⚠ WHY THE MOCK IS AT THE **WIRE**, NOT AT THE CLIENT. A double standing in for `client::Client`
// would let this suite pass over request/reply shapes the daemon never produces — the exact failure
// mode that hid an empty `granted_scopes()` for six tasks (mock_channel.h records it). Mocking the
// CHANNEL means the frames here are the frames dispatcher.cpp emits, the real Client parses them,
// and what is asserted is the feed's behaviour over the real SDK. The cross-process
// `editor-session-panels-t2` drill against a live `context daemon` is what proves the two agree.
//
// THE STRUCTURAL CLAIM (the e08b DoD's "no private ownership left behind") is the last block: a
// SECOND client's selection and play state become visible in the panels with ZERO writes issued by
// this Shell — asserted against the feed's own write counter, not by reading the code.

#include "context/editor/shell/panels/session_feed.h"

#include "context/editor/client/client.h"
#include "context/editor/gui/panels/scenetree/scene_tree_model.h"
#include "context/editor/gui/playbar/playbar_panel.h"
#include "context/editor/gui/uitree/panel.h"

#include "mock_channel.h"
#include "panels_test.h"

#include <memory>
#include <string>
#include <vector>

using context::editor::shell::PanelHost;
using context::editor::shell::panels::SessionFeed;
using context::editor::shell::panels::kCameraChangedEvent;
using context::editor::shell::panels::kPlayStateEvent;
using context::editor::shell::panels::kSelectionChangedEvent;
using context::editor::shell::panels::kSessionTopicName;
namespace client = context::editor::client;
namespace playbar = context::editor::gui::playbar;
namespace scenetree = context::editor::gui::panels::scenetree;
namespace uitree = context::editor::gui::uitree;
using context::editor::contract::Json;

namespace
{

// A `session` topic payload exactly as kernel_server.cpp's `session_fact()` builds it.
[[nodiscard]] Json selection_fact(std::uint64_t origin, const std::vector<std::string>& ids)
{
    Json fact = Json::object();
    fact.set("event", Json(std::string(kSelectionChangedEvent)));
    fact.set("origin", Json(origin));
    Json wire = Json::array();
    for (const std::string& id : ids)
    {
        wire.push_back(Json(id));
    }
    fact.set("ids", std::move(wire));
    fact.set("mode", Json(std::string("replace")));
    return fact;
}

[[nodiscard]] Json play_fact(std::uint64_t origin, const char* state, std::uint64_t sim_tick)
{
    Json fact = Json::object();
    fact.set("event", Json(std::string(kPlayStateEvent)));
    fact.set("origin", Json(origin));
    fact.set("state", Json(std::string(state)));
    fact.set("simTick", Json(sim_tick));
    return fact;
}

// The daemon's `editor.play|pause|stop|step` success data.
[[nodiscard]] Json play_reply(const char* state, std::uint64_t sim_tick, bool changed)
{
    Json data = Json::object();
    data.set("state", Json(std::string(state)));
    data.set("simTick", Json(sim_tick));
    data.set("changed", Json(changed));
    return clientmock::MockChannel::ok_envelope(std::move(data));
}

// A two-node scene-tree model, so a selected identity has a row to resolve its L-37 hash against.
[[nodiscard]] scenetree::SceneTreeModel two_node_model()
{
    scenetree::SceneTreeModel model;
    scenetree::SceneTreeNode a;
    a.identity = "e1";
    a.identity_hash = 0x11;
    a.display_name = "Hero";
    scenetree::SceneTreeNode b;
    b.identity = "e2";
    b.identity_hash = 0x22;
    b.display_name = "Light";
    model.roots.push_back(std::move(a));
    model.roots.push_back(std::move(b));
    model.entity_count = 2;
    return model;
}

// A real Client over the scripted channel, attached (so it has a client id) and holding `channel`.
struct Wired
{
    clientmock::MockChannel* channel = nullptr;
    std::unique_ptr<client::Client> client;
};

[[nodiscard]] Wired make_client(std::uint64_t client_id)
{
    auto channel = std::make_unique<clientmock::MockChannel>();
    clientmock::MockChannel* raw = channel.get();
    // The attach reply is FLAT in `result` — NOT an envelope (mock_channel.h's standing warning).
    raw->on("attach",
            [client_id](const clientmock::Request&)
            {
                Json result = Json::object();
                result.set("protocolMajor",
                           Json(static_cast<std::uint64_t>(context::editor::contract::kProtocolMajor)));
                result.set("clientId", Json(client_id));
                Json caps = Json::array();
                caps.push_back(Json(std::string("describe")));
                result.set("capabilities", std::move(caps));
                Json scopes = Json::array();
                scopes.push_back(Json(std::string("read")));
                scopes.push_back(Json(std::string("session_control")));
                result.set("scopes", std::move(scopes));
                return result;
            });

    Wired out;
    out.channel = raw;
    out.client = std::make_unique<client::Client>(std::move(channel));
    client::AttachOptions options;
    options.scope = "read,session";
    options.token = "t";
    std::string error;
    CHECK(out.client->attach(options, error));
    CHECK(out.client->client_id() == client_id);
    return out;
}

} // namespace

int main()
{
    constexpr std::uint64_t kShellId = 4;
    constexpr std::uint64_t kOtherClientId = 9;

    // --- the subscriber: another client's selection reaches the panel -----------------------------
    {
        PanelHost host;
        SessionFeed feed(host, playbar::PlaybarModel::kContributionId);
        scenetree::SceneTreePanel tree(&feed);
        tree.set_model(two_node_model());
        feed.bind_scene_tree(&tree, scenetree::SceneTreePanel::kContributionId);
        feed.bind_client(nullptr, kShellId); // attached identity, no live connection needed here

        CHECK(feed.apply_event(kSessionTopicName, selection_fact(kOtherClientId, {"e2"})));
        CHECK(tree.selection().identity == "e2");
        CHECK(tree.selection().identity_hash == 0x22); // resolved against the rendered model
        CHECK(feed.facts_applied() == 1);
        CHECK(feed.writes_issued() == 0); // NOTHING was written to reach the rendered state

        // ...and its play state reaches the L-51 indicator, still with no write.
        CHECK(feed.apply_event(kSessionTopicName, play_fact(kOtherClientId, "playing", 5)));
        CHECK(feed.playbar_model().state() == playbar::PlayState::playing);
        CHECK(feed.playbar_model().sim_tick() == 5);
        CHECK(feed.writes_issued() == 0);
        CHECK(panelstest::mentions(uitree::render_html(playbar::build_playbar_panel(
                                       feed.playbar_model())),
                                   "PLAY MODE"));

        // A fact stamped with OUR OWN origin is the echo of a write we made — DROPPED, never applied
        // a second time. This is what makes "no flicker / no double-apply" structural.
        CHECK(!feed.apply_event(kSessionTopicName, selection_fact(kShellId, {"e1"})));
        CHECK(tree.selection().identity == "e2"); // unmoved
        CHECK(feed.echoes_dropped() == 1);
        CHECK(feed.facts_applied() == 2);

        // A restated fact from another client changes nothing and counts nothing.
        CHECK(!feed.apply_event(kSessionTopicName, selection_fact(kOtherClientId, {"e2"})));
        CHECK(feed.facts_applied() == 2);

        // A recognised-but-not-ours fact (cameras are e11) and a foreign topic are both ignored.
        Json camera = Json::object();
        camera.set("event", Json(std::string(kCameraChangedEvent)));
        camera.set("origin", Json(kOtherClientId));
        CHECK(!feed.apply_event(kSessionTopicName, camera));
        CHECK(!feed.apply_event("derivation", selection_fact(kOtherClientId, {"e1"})));
        CHECK(feed.facts_applied() == 2);
    }

    // --- an UNATTACHED feed (client id 0) is a plain subscriber, not an echo swallower ------------
    // 0 is also the DAEMON's own origin, so a naive `origin == client_id_` test would make an
    // unattached Shell drop every daemon-originated fact in silence.
    {
        PanelHost host;
        SessionFeed feed(host, playbar::PlaybarModel::kContributionId);
        scenetree::SceneTreePanel tree(&feed);
        tree.set_model(two_node_model());
        feed.bind_scene_tree(&tree, scenetree::SceneTreePanel::kContributionId);

        CHECK(feed.client_id() == 0);
        CHECK(feed.apply_event(kSessionTopicName, selection_fact(0, {"e1"})));
        CHECK(tree.selection().identity == "e1");
        CHECK(feed.echoes_dropped() == 0);
    }

    // --- an UNREADABLE play-state token leaves the rendered state ALONE ----------------------------
    // `edit` is a POSITIVE L-51 claim ("authored truth, no live session"), so a token from a NEWER
    // daemon — or a fact with no `state` member at all — must NOT be read as one: the daemon
    // publishes `play-state` only on a CHANGE, so a fabricated `edit` would never be corrected. This
    // is the same rule the TS side applies (`toPlayState` in when.ts) — the two sources of the one
    // contract must not disagree.
    {
        PanelHost host;
        SessionFeed feed(host, playbar::PlaybarModel::kContributionId);
        feed.bind_client(nullptr, kShellId);

        CHECK(feed.apply_event(kSessionTopicName, play_fact(kOtherClientId, "playing", 7)));
        CHECK(feed.playbar_model().state() == playbar::PlayState::playing);
        const std::uint64_t generation = feed.playbar_model().control_generation();

        // A token a newer daemon might mint: not applied, not counted, nothing notified.
        CHECK(!feed.apply_event(kSessionTopicName, play_fact(kOtherClientId, "recording", 8)));
        CHECK(feed.playbar_model().state() == playbar::PlayState::playing);
        CHECK(feed.playbar_model().sim_tick() == 7);
        CHECK(feed.playbar_model().control_generation() == generation);

        // A `play-state` fact with no `state` member reads the same way — absent is not `edit`.
        Json bare = Json::object();
        bare.set("event", Json(std::string(kPlayStateEvent)));
        bare.set("origin", Json(kOtherClientId));
        bare.set("simTick", Json(std::uint64_t{9}));
        CHECK(!feed.apply_event(kSessionTopicName, bare));
        CHECK(feed.playbar_model().state() == playbar::PlayState::playing);

        // ...and the daemon's own `edit` token still lands, so the rule costs no real transition.
        CHECK(feed.apply_event(kSessionTopicName, play_fact(kOtherClientId, "edit", 0)));
        CHECK(feed.playbar_model().state() == playbar::PlayState::edit);
    }

    // --- the same rule on the REPLY path: an unreadable reply state keeps the rendered one ---------
    {
        PanelHost host;
        SessionFeed feed(host, playbar::PlaybarModel::kContributionId);

        Wired wired = make_client(kShellId);
        wired.channel->on("editor.play",
                          [](const clientmock::Request&) { return play_reply("playing", 3, true); });
        // A reply whose `state` this build cannot name. `PlayCommandResult::state` DEFAULTS to
        // `edit`, so a reader that left it defaulted would render "no live session" on a step that
        // demonstrably advanced the tick.
        wired.channel->on(
            "editor.step",
            [](const clientmock::Request&) { return play_reply("recording", 4, true); });
        feed.bind_client(wired.client.get(), wired.client->client_id());

        CHECK(feed.playbar_model().play().ok);
        CHECK(feed.playbar_model().state() == playbar::PlayState::playing);
        CHECK(feed.playbar_model().step(1).ok);
        CHECK(feed.playbar_model().state() == playbar::PlayState::playing); // unmoved, not `edit`
        CHECK(feed.playbar_model().sim_tick() == 4);
    }

    // --- the WRITE seam: `editor.select` over the real Client -------------------------------------
    {
        PanelHost host;
        SessionFeed feed(host, playbar::PlaybarModel::kContributionId);
        scenetree::SceneTreePanel tree(&feed);
        tree.set_model(two_node_model());
        feed.bind_scene_tree(&tree, scenetree::SceneTreePanel::kContributionId);

        Wired wired = make_client(kShellId);
        wired.channel->on("editor.select",
                          [](const clientmock::Request& request)
                          {
                              Json data = Json::object();
                              data.set("ids", request.params.at("ids"));
                              data.set("mode", Json(std::string("replace")));
                              data.set("changed", Json(true));
                              return clientmock::MockChannel::ok_envelope(std::move(data));
                          });
        feed.bind_client(wired.client.get(), wired.client->client_id());

        CHECK(tree.select("e1"));
        CHECK(feed.writes_issued() == 1);
        const std::vector<clientmock::Request> sent =
            wired.channel->requests_for("editor.select");
        CHECK(sent.size() == 1);
        if (sent.size() == 1)
        {
            CHECK(sent[0].params.at("ids").size() == 1);
            CHECK(sent[0].params.at("ids").at(0).as_string() == "e1");
            // `mode` is the daemon's default; sending it would pin a choice that is not ours.
            CHECK(!sent[0].params.contains("mode"));
        }
        // The panel renders the DAEMON'S reply — which is its ONLY path to seeing its own selection,
        // because the fact this write publishes carries our origin and is dropped below.
        CHECK(tree.selection().identity == "e1");
        CHECK(tree.selection().identity_hash == 0x11);

        // The echo arrives and IS dropped — no double-apply, and (the reason this matters) no
        // regression to a panel whose own selections never appear.
        CHECK(!feed.apply_event(kSessionTopicName, selection_fact(kShellId, {"e1"})));
        CHECK(feed.echoes_dropped() == 1);
        CHECK(tree.selection().identity == "e1");

        // Clearing sends an EMPTY id list.
        CHECK(tree.clear_selection());
        const std::vector<clientmock::Request> after =
            wired.channel->requests_for("editor.select");
        CHECK(after.size() == 2);
        if (after.size() == 2)
        {
            CHECK(after[1].params.at("ids").size() == 0);
        }
    }

    // --- a daemon no-op still renders the daemon's selection; a refusal renders nothing ------------
    {
        PanelHost host;
        SessionFeed feed(host, playbar::PlaybarModel::kContributionId);
        scenetree::SceneTreePanel tree(&feed);
        tree.set_model(two_node_model());
        feed.bind_scene_tree(&tree, scenetree::SceneTreePanel::kContributionId);

        Wired wired = make_client(kShellId);
        // The daemon's no-op reply: `changed:false`, but `ids` still carries what IS selected. A
        // reader that keyed off `changed` alone would leave the panel blank after a re-click.
        wired.channel->on("editor.select",
                          [](const clientmock::Request& request)
                          {
                              Json data = Json::object();
                              data.set("ids", request.params.at("ids"));
                              data.set("changed", Json(false));
                              return clientmock::MockChannel::ok_envelope(std::move(data));
                          });
        feed.bind_client(wired.client.get(), wired.client->client_id());
        CHECK(tree.select("e1"));
        CHECK(tree.selection().identity == "e1");

        // A REFUSAL renders nothing at all — the rendered selection stays exactly where it was.
        wired.channel->fail_method("editor.select", "scope denied", "scope.denied");
        CHECK(!tree.select("e2"));
        CHECK(tree.selection().identity == "e1");
    }

    // --- the WRITE seam: play control, and the ok<-changed / catalog-code mapping ------------------
    {
        PanelHost host;
        SessionFeed feed(host, playbar::PlaybarModel::kContributionId);

        Wired wired = make_client(kShellId);
        wired.channel->on("editor.play",
                          [](const clientmock::Request&) { return play_reply("playing", 0, true); });
        wired.channel->on("editor.step",
                          [](const clientmock::Request&) { return play_reply("playing", 2, true); });
        wired.channel->on("editor.stop",
                          [](const clientmock::Request&) { return play_reply("edit", 0, true); });
        feed.bind_client(wired.client.get(), wired.client->client_id());

        CHECK(feed.playbar_model().play().ok);
        CHECK(feed.playbar_model().state() == playbar::PlayState::playing);

        CHECK(feed.playbar_model().step(2).ok);
        CHECK(feed.playbar_model().sim_tick() == 2);
        const std::vector<clientmock::Request> steps = wired.channel->requests_for("editor.step");
        CHECK(steps.size() == 1);
        if (steps.size() == 1)
        {
            // A NUMBER, not the CLI projection's string — the daemon accepts both.
            CHECK(steps[0].params.at("ticks").as_int() == 2);
        }

        CHECK(feed.playbar_model().stop().ok);
        CHECK(feed.playbar_model().state() == playbar::PlayState::edit);

        // A refusal carries the daemon's OWN catalog code, verbatim (each maps to an exit class).
        wired.channel->fail_method("editor.pause", "no live play session",
                                   playbar::kPlayNotRunningCode);
        const playbar::PlayAction refused = feed.playbar_model().pause();
        CHECK(!refused.ok);
        CHECK(refused.error_code == std::string(playbar::kPlayNotRunningCode));
        CHECK(feed.playbar_model().state() == playbar::PlayState::edit);
    }

    // --- the provider: the four transport commands dispatch, an unknown one does not ---------------
    {
        PanelHost host;
        SessionFeed feed(host, playbar::PlaybarModel::kContributionId);
        CHECK(host.provide(playbar::PlaybarModel::kContributionId, feed.make_provider()));
        CHECK(host.hosts(playbar::PlaybarModel::kContributionId));

        Wired wired = make_client(kShellId);
        wired.channel->on("editor.play",
                          [](const clientmock::Request&) { return play_reply("playing", 0, true); });
        feed.bind_client(wired.client.get(), wired.client->client_id());

        // Drive it the way the hydration runtime does: through the host's command dispatch.
        bool dispatched = false;
        std::string error_code;
        CHECK(host.invoke(playbar::PlaybarModel::kContributionId, playbar::kPlayCommand,
                          Json::object(), dispatched, error_code));
        CHECK(dispatched);
        CHECK(feed.playbar_model().state() == playbar::PlayState::playing);

        // A command the PANEL does not expose never reaches the provider at all — the host's
        // reachability check refuses it at the seam (a stale mounted tree), so the call itself is
        // reported ill-formed and nothing is dispatched.
        dispatched = true;
        CHECK(!host.invoke(playbar::PlaybarModel::kContributionId, "not.a.command", Json::object(),
                           dispatched, error_code));
        CHECK(!dispatched);
        CHECK(!error_code.empty());
    }

    // --- THE STRUCTURAL CLAIM: no GUI panel holds authoritative selection / play state ------------
    // A second client changes BOTH, and both are visible in the panels' rendered surfaces after zero
    // writes from this Shell. If either panel still owned its state, the only way to reach these
    // renders would be a local write — which the counter would show.
    {
        PanelHost host;
        SessionFeed feed(host, playbar::PlaybarModel::kContributionId);
        scenetree::SceneTreePanel tree(&feed);
        tree.set_model(two_node_model());
        feed.bind_scene_tree(&tree, scenetree::SceneTreePanel::kContributionId);

        Wired wired = make_client(kShellId);
        feed.bind_client(wired.client.get(), wired.client->client_id());

        CHECK(feed.apply_event(kSessionTopicName, selection_fact(kOtherClientId, {"e2"})));
        CHECK(feed.apply_event(kSessionTopicName, play_fact(kOtherClientId, "paused", 11)));

        const std::string tree_html = uitree::render_html(tree.build_panel());
        CHECK(panelstest::mentions(tree_html, "Light (selected)"));
        const std::string bar_html =
            uitree::render_html(playbar::build_playbar_panel(feed.playbar_model()));
        CHECK(panelstest::mentions(bar_html, "PLAY MODE (paused)"));
        CHECK(panelstest::mentions(bar_html, "tick 11"));

        CHECK(feed.writes_issued() == 0);
        CHECK(wired.channel->requests_for("editor.select").empty());
        CHECK(wired.channel->requests_for("editor.play").empty());
    }

    PANELS_TEST_MAIN_END();
}
