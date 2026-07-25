// T1 for the panel composition root (M9 e05d1/e05d3): what binds, what deliberately stays unhosted,
// and the end-to-end path from the real roster through a real provider to a rendered panel.
//
// e05d1 pinned `builtin.scene-tree` / `builtin.inspector` as LISTED-BUT-UNHOSTED (their libraries
// linked `context_compose`, which the D10 shell-boundary gate forbids) — and pinned it HERE so that
// e05d3, whose whole job was to make them hostable, saw this test go red in exactly the place that
// means "you succeeded". e05d3 landed: the kernel-typed builders moved daemon-side, the panel
// libraries are boundary-clean, and this file now asserts the HOSTED state — every hostable panel
// bound, and the Scene tree's selection wired to the Inspector's fetch (R-HUX-011).
//
// M9 e09c added the session-undo host to that set, plus the two assertions that keep its wiring
// honest: the replay reaches the SHARED wire gateway (no second write path), and the journal round
// trips through a real editor-state file via the two bag seams editor_main.cpp drives.

#include "context/editor/shell/panels/builtin_panels.h"

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/shell/editor_state.h" // e09c: the store the undo seams publish into
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/inspector_feed.h" // complete feed types: builtin_panels.h only
#include "context/editor/shell/panels/problems_feed.h"  // forward-declares them, and this file calls
#include "context/editor/shell/panels/scenetree_feed.h" // methods on the bag's members.
#include "context/editor/shell/panels/undo_feed.h"      // e09c: the bag's session undo host
#include "context/editor/shell/panels/wire_override_gateway.h" // e09b-2: the bag's write gateway

#include "context/editor/serializer/json_parse.h"

#include "panels_test.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace gc = context::editor::gui::contract;
namespace scenetree = context::editor::gui::panels::scenetree;
using Json = context::editor::contract::Json;

namespace
{

const Json* find_panel(const Json& listing, const std::string& id)
{
    const Json& list = listing.at("panels");
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        if (list.at(i).at("id").as_string() == id)
        {
            return &list.at(i);
        }
    }
    return nullptr;
}

void binds_every_hostable_panel_and_nothing_else()
{
    shell::PanelHost host;
    const panels::BuiltinPanels bound = panels::install_builtin_panels(host);

    // Every id the module CLAIMS is hostable actually bound — the list and the bindings cannot drift.
    CHECK(bound.bound == panels::hostable_panel_ids().size());
    CHECK(host.hosted_count() == panels::hostable_panel_ids().size());
    for (const std::string& id : panels::hostable_panel_ids())
    {
        CHECK(host.hosts(id));
    }

    // SIX panels, from five different libraries (uitree / problems / the e05d3 pair / the e08b
    // playbar / the e09c session journal) — the panel-agnosticism claim exercised across every
    // hosted shape.
    CHECK(panels::hostable_panel_ids().size() == 6);
    CHECK(host.hosts("placeholder"));
    CHECK(host.hosts(context::editor::gui::panels::problems::ProblemsPanel::kContributionId));
    CHECK(host.hosts("builtin.scene-tree"));
    CHECK(host.hosts("builtin.inspector"));
    CHECK(host.hosts("builtin.playbar"));
    // e09c: rostered since e05b and UNHOSTED until now — which is exactly why the journal's
    // to_json/load_json had no caller. Hosting it is what gives `session.undo` / `session.redo` a
    // reachable path (R-CLI-001).
    CHECK(host.hosts("builtin.session.undo"));

    // The feed owners came back, so every provider's captures stay alive.
    CHECK(bound.problems != nullptr);
    CHECK(bound.scenetree != nullptr);
    CHECK(bound.inspector != nullptr);
    CHECK(bound.session != nullptr);
    // e09b-2: the Inspector's wire write gateway came back too — it is what the panel's raw gateway
    // pointer points at, so a bag missing it would leave the panel pointing at freed memory.
    CHECK(bound.writes != nullptr);
    CHECK(bound.inspector != nullptr && bound.inspector->has_gateway());
    // It starts UNBOUND: `install_builtin_panels` runs at boot, before any daemon connection exists
    // (editor_main.cpp), and the connection is re-derived every frame through `bind_write_client`.
    CHECK(bound.writes != nullptr && !bound.writes->has_client());
    // e09c: the session undo host came back, its journal is bound to a gateway, and the Inspector
    // has somewhere to send its checkpoints.
    CHECK(bound.undo != nullptr);
    CHECK(bound.undo != nullptr && bound.undo->has_gateway());
    CHECK(bound.inspector != nullptr && bound.inspector->has_checkpoint_sink());

    // The whole roster is still LISTED — an unhostable panel is visible and honestly flagged, never
    // hidden.
    const Json listing = host.list();
    CHECK(listing.at("panels").size() == gc::builtin_contributions().size());
    const Json* scenetree_entry = find_panel(listing, "builtin.scene-tree");
    CHECK(scenetree_entry != nullptr && scenetree_entry->at("hosted").as_bool());
    const Json* inspector_entry = find_panel(listing, "builtin.inspector");
    CHECK(inspector_entry != nullptr && inspector_entry->at("hosted").as_bool());
    const Json* problems_entry =
        find_panel(listing, context::editor::gui::panels::problems::ProblemsPanel::kContributionId);
    CHECK(problems_entry != nullptr && problems_entry->at("hosted").as_bool());
    // The observers expose no gestures and persist nothing. REPORTED, not stubbed.
    CHECK(problems_entry != nullptr && !problems_entry->at("gestures").as_bool());
    CHECK(problems_entry != nullptr && !problems_entry->at("persists").as_bool());
    CHECK(scenetree_entry != nullptr && !scenetree_entry->at("gestures").as_bool());
    CHECK(inspector_entry != nullptr && !inspector_entry->at("persists").as_bool());

    // M9 e09b-2 — THE `gestures:false -> true` FLIP, at the ONE place it is decided. The Inspector is
    // the only built-in that WRITES, so it is the only one that can end a gesture with a commit; the
    // composition root binds it a wire gateway, `make_provider` therefore supplies `PanelProvider::gesture`,
    // and `PanelHost::list` reports the capability from what is actually bound. The manifest value
    // asserted here is exactly what `panelhost.ts`'s `manifest?.gestures ?? false` reads when it
    // constructs the panel's `UitreePanelRenderer`, and what `hydration.ts` gates `#bindGestures` on —
    // one fact, three consumers, so this assertion IS the cross-language consistency check.
    CHECK(inspector_entry != nullptr && inspector_entry->at("gestures").as_bool());
    // The playbar is a transport, not a continuous surface: four commands, still no gestures. The
    // flip must be about the panel that gained a write path, not about every hosted panel.
    const Json* playbar_entry = find_panel(listing, "builtin.playbar");
    CHECK(playbar_entry != nullptr && !playbar_entry->at("gestures").as_bool());
}

// e09b-2: the gesture surface the flip turns on, driven through the REAL PanelHost the renderer
// reaches — and its honest posture with no daemon behind it.
void the_inspector_gesture_surface_is_live_but_refuses_without_a_daemon()
{
    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.inspector != nullptr);
    CHECK(bound.writes != nullptr);
    if (bound.inspector == nullptr || bound.writes == nullptr)
    {
        return;
    }

    // A commit with nothing staged is `dispatched:false` — an ordinary outcome, not a protocol fault
    // (panel_host.h's rule), and the call itself is WELL-FORMED, which is what distinguishes a hosted
    // gesture surface from one that refuses with kErrPanelBadGesture.
    bool dispatched = true;
    std::string error_code;
    CHECK(host.gesture("builtin.inspector", shell::GestureVerb::commit, Json::object(), dispatched,
                       error_code));
    CHECK(!dispatched);
    CHECK(error_code.empty());

    // A freshly installed bag has NO model (nothing has hydrated it), so there is no field to stage
    // against and the model layer refuses — the guard BEFORE any gateway is consulted. The
    // no-daemon commit posture itself (the gateway's named refusal, `Status::error`, and the staged
    // gesture KEPT for a retry) needs a staged edit to exist, so it is asserted where a model does:
    // `test_wire_override_gateway.cpp`'s L-30 cases and the `editor-session-concurrent-cas-t2` drill.
    context::editor::serializer::ParseResult value =
        context::editor::serializer::parse_json("2.5");
    CHECK(value.ok);
    CHECK(bound.inspector->panel().stage_edit("/components/camera/fov", std::move(value.root)) ==
          false); // no model yet: an unknown field cannot be staged (the model layer's own guard)

    // A gesture on a node that is not an inspector widget is honestly not dispatched.
    Json params = Json::object();
    params.set("nodeId", Json(std::string("inspector.status")));
    dispatched = true;
    CHECK(host.gesture("builtin.inspector", shell::GestureVerb::begin, params, dispatched,
                       error_code));
    CHECK(!dispatched);

    // And the write client re-points through the ONE seam, in both directions, with no re-provide.
    panels::bind_write_client(bound, nullptr);
    CHECK(!bound.writes->has_client());
}

// e09c — NO SECOND WRITE PATH. The DoD's structural half: an undo/redo replay must go out over the
// SAME `WireOverrideWriteGateway` a live gesture commits through, not through a private in-process
// writer. Asserted by OBSERVATION rather than by reading the wiring — `shell.no_daemon` is a LOCAL
// refusal code minted by exactly one class in the tree (wire_override_gateway.h), so a replay coming
// back carrying it could only have reached THAT gateway. If someone later gave the journal its own
// disk-backed or in-process gateway, this code would change and this assertion would go red.
void the_undo_replay_routes_through_the_shared_wire_gateway()
{
    namespace undo = context::editor::gui::session::undo;
    namespace inspector = context::editor::gui::panels::inspector;

    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.undo != nullptr);
    CHECK(bound.writes != nullptr);
    if (bound.undo == nullptr || bound.writes == nullptr)
    {
        return;
    }
    // No daemon is bound here, which is the point: the wire gateway's honest refusal is the
    // fingerprint. The before/after values are left null deliberately — what is under test is WHICH
    // object the replay talks to, not the L-30 verdict (test_undo_feed.cpp owns that).
    undo::FieldEdit edit;
    edit.root_scene = "scenes/main.scene.json";
    edit.id_path = {"cam"};
    edit.pointer = "/components/camera/fov";
    bound.undo->record(std::move(edit));
    CHECK(bound.undo->journal().can_undo());

    const undo::ReplayResult result = bound.undo->replay_undo();
    // It RESOLVED (a replay with nowhere to write is not silently swallowed)…
    CHECK(result.status == inspector::CommitResult::Status::error);
    // …carrying the one gateway's one code.
    CHECK(!result.edits.empty());
    CHECK(!result.edits.empty() &&
          result.edits.front().code == panels::WireOverrideWriteGateway::kNoDaemonCode);
    // And nothing was written, obviously: an unbound gateway that reported `applied` would be the
    // one unforgivable lie on this path.
    CHECK(bound.writes->writes_applied() == 0u);
}

// e09c — THE TWO PERSISTENCE SEAMS, end to end through the BAG, against a REAL editor-state file.
// This is exactly the path `editor_main.cpp` drives (boot restore + a per-frame publish), and
// asserting it here is what keeps that wiring from being the only place it is ever exercised — the
// failure mode being an editor that journals faithfully in memory and persists nothing.
void the_journal_publishes_to_and_restores_from_the_editor_state_store()
{
    namespace undo = context::editor::gui::session::undo;
    namespace fs = std::filesystem;

    // A local temp-project helper: shell_test.h's pulls context/render/rhi.h, which nothing in this
    // suite builds against (panels_test.h states the rule).
    static const long long ticks = static_cast<long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::error_code ec;
    const fs::path root =
        fs::temp_directory_path(ec) / ("context-builtin-panels-undo-" + std::to_string(ticks));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // ---- session 1: a checkpoint is recorded and published ----
    {
        shell::PanelHost host;
        panels::BuiltinPanels bound = panels::install_builtin_panels(host);
        CHECK(bound.undo != nullptr);
        if (bound.undo == nullptr)
        {
            return;
        }
        shell::EditorStateStore store(root, 0);
        store.load();
        // Nothing recorded yet -> nothing to publish. A seam that dirtied the store every frame
        // would put a file write in an idle editor's loop forever.
        CHECK(!panels::publish_undo_state(bound, store, 0));

        undo::FieldEdit edit;
        edit.root_scene = "scenes/main.scene.json";
        edit.id_path = {"cam"};
        edit.pointer = "/components/camera/fov";
        bound.undo->record(std::move(edit));

        CHECK(panels::publish_undo_state(bound, store, 0));
        CHECK(store.dirty());
        CHECK(store.flush_now());
        // The very next frame re-offers the SAME journal — which must cost nothing.
        CHECK(!panels::publish_undo_state(bound, store, 0));
    }

    // ---- session 2: a fresh bag restores it ----
    {
        shell::EditorStateStore store(root, 0);
        store.load();
        shell::PanelHost host;
        panels::BuiltinPanels bound = panels::install_builtin_panels(host);
        CHECK(bound.undo != nullptr);
        if (bound.undo == nullptr)
        {
            return;
        }
        CHECK(!bound.undo->journal().can_undo()); // a fresh bag starts empty
        CHECK(panels::restore_undo_state(bound, store.state()));
        CHECK(bound.undo->journal().can_undo());
        CHECK(bound.undo->journal().undo_depth() == 1u);
    }

    fs::remove_all(root, ec);
}

void renders_the_hosted_panels_through_the_bridge()
{
    shell::PanelHost host;
    const panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.bound == panels::hostable_panel_ids().size());

    shell::BridgeRouter router;
    CHECK(host.install(router));

    // The placeholder: a panel whose provider supplies `build` ALONE. If the host or the wire
    // envelope assumed any optional capability, this is where it would show.
    std::string placeholder_code;
    const std::optional<shell::PanelRender> placeholder = host.render("placeholder", placeholder_code);
    CHECK(placeholder.has_value());
    if (placeholder.has_value())
    {
        CHECK(!placeholder->html.empty());
        CHECK(!placeholder->focus_order.empty());
        CHECK(!placeholder->commands.empty());
    }

    // Problems: a panel with a live model behind it. Empty right now (no diagnostics have arrived),
    // which must still render a real tree rather than nothing — an empty Problems panel is a
    // legitimate, common state.
    std::string problems_code;
    const std::optional<shell::PanelRender> problems = host.render(
        context::editor::gui::panels::problems::ProblemsPanel::kContributionId, problems_code);
    CHECK(problems.has_value());
    if (problems.has_value())
    {
        CHECK(panelstest::mentions(problems->html, "problems.panel"));
        CHECK(panelstest::mentions(problems->html, "problems.status"));
    }

    // The e05d3 pair render their honest EMPTY states before any daemon read arrives: a scene tree
    // with no scene, an inspector with no selection.
    std::string scenetree_code;
    const std::optional<shell::PanelRender> tree = host.render("builtin.scene-tree", scenetree_code);
    CHECK(tree.has_value());
    if (tree.has_value())
    {
        CHECK(panelstest::mentions(tree->html, "scenetree.panel"));
        CHECK(panelstest::mentions(tree->html, "scenetree.status"));
    }
    std::string inspector_code;
    const std::optional<shell::PanelRender> inspector_render =
        host.render("builtin.inspector", inspector_code);
    CHECK(inspector_render.has_value());
    if (inspector_render.has_value())
    {
        CHECK(panelstest::mentions(inspector_render->html, "inspector.panel"));
        CHECK(panelstest::mentions(inspector_render->html, "No entity selected"));
    }
}

// The live path, end to end through the SAME objects `context_editor` wires: a daemon event reaches
// the feed, the feed touches the host, and the next render carries it.
void a_daemon_event_reaches_the_rendered_panel()
{
    shell::PanelHost host;
    const panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.problems != nullptr);
    if (bound.problems == nullptr)
    {
        return;
    }
    const std::string panel_id = context::editor::gui::panels::problems::ProblemsPanel::kContributionId;

    Json payload = Json::object();
    payload.set("code", Json("file.malformed"));
    payload.set("message", Json("unterminated object"));
    payload.set("severity", Json("error"));
    payload.set("file", Json("scenes/level.json"));
    payload.set("line", Json(7));

    const std::uint64_t before = host.revision(panel_id);
    CHECK(bound.problems->apply_event("diagnostics", payload, 3));
    CHECK(host.revision(panel_id) > before);

    std::string error_code;
    const std::optional<shell::PanelRender> rendered = host.render(panel_id, error_code);
    CHECK(rendered.has_value());
    if (rendered.has_value())
    {
        CHECK(panelstest::mentions(rendered->html, "unterminated object"));
        CHECK(panelstest::mentions(rendered->html, "scenes/level.json"));
        CHECK(panelstest::mentions(rendered->html, "data-command=\"problems.navigate\""));
    }
}

// The e05d3 selection loop, through the SAME wiring `context_editor` gets: adopting a scene tree,
// the DAEMON's selection arriving, and the Inspector's fetch turning up PENDING for exactly that
// identity (R-HUX-011).
//
// M9 e08b re-rooted this loop on daemon truth: the tree no longer decides a selection, so the loop
// is driven the way the live editor drives it — a `selection-changed` fact through the session feed.
// That is the whole point: the Inspector cannot tell (and must not care) whether the human clicked
// this panel or a CLI on another terminal.
void a_daemon_selection_schedules_the_inspector_fetch()
{
    shell::PanelHost host;
    const panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.scenetree != nullptr);
    CHECK(bound.inspector != nullptr);
    CHECK(bound.session != nullptr);
    if (bound.scenetree == nullptr || bound.inspector == nullptr || bound.session == nullptr)
    {
        return;
    }

    scenetree::SceneTreeModel model;
    scenetree::SceneTreeNode node;
    node.identity = "inst1/ent1";
    node.display_name = "Player";
    model.roots.push_back(std::move(node));
    bound.scenetree->panel().set_model(std::move(model));

    CHECK(!bound.inspector->pending().has_value());

    Json fact = Json::object();
    fact.set("event", Json(std::string("selection-changed")));
    fact.set("origin", Json(std::uint64_t{7})); // another client — not this Shell
    Json ids = Json::array();
    ids.push_back(Json(std::string("inst1/ent1")));
    fact.set("ids", std::move(ids));
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic, fact));
    CHECK(bound.inspector->pending() == std::optional<std::string>("inst1/ent1"));

    // A cleared selection (an empty id list) clears the panel and drops the pending fetch.
    Json cleared = Json::object();
    cleared.set("event", Json(std::string("selection-changed")));
    cleared.set("origin", Json(std::uint64_t{7}));
    cleared.set("ids", Json::array());
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic, cleared));
    CHECK(!bound.inspector->pending().has_value());
    CHECK(!bound.inspector->panel().has_selection());

    // With no daemon bound, a row activation writes NOTHING and moves NOTHING — the panel cannot
    // change a selection it does not own (and the composition root leaves it honestly read-only).
    CHECK(!bound.scenetree->panel().select("inst1/ent1"));
    CHECK(!bound.inspector->pending().has_value());
}

} // namespace

int main()
{
    binds_every_hostable_panel_and_nothing_else();
    the_inspector_gesture_surface_is_live_but_refuses_without_a_daemon();
    the_undo_replay_routes_through_the_shared_wire_gateway();
    the_journal_publishes_to_and_restores_from_the_editor_state_store();
    renders_the_hosted_panels_through_the_bridge();
    a_daemon_event_reaches_the_rendered_panel();
    a_daemon_selection_schedules_the_inspector_fetch();
    PANELS_TEST_MAIN_END();
}
