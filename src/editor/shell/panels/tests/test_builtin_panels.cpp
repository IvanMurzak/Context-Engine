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
#include "context/editor/shell/write_notice.h" // e09b-3: the LOUD relay the bag's sinks feed

#include "context/editor/serializer/json_parse.h"

#include "panels_wire_test.h" // e09c: the shared REAL-client-over-a-scripted-wire fixture

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace gc = context::editor::gui::contract;
namespace scenetree = context::editor::gui::panels::scenetree;
namespace client = context::editor::client;
using Json = context::editor::contract::Json;

namespace
{

using panelstest::make_wired_client;
using panelstest::Wired;

// The MINIMUM gateway a replay can land through: the field always reads back as the value the
// checkpoint expects, and the write always applies. Deliberately not a project-file model — what the
// one case using it tests is the PUMP's reaction to a landed replay, not the write policy
// (test_undo_feed.cpp owns the L-30 verdicts, test_wire_override_gateway.cpp owns the frame).
class AlwaysApplies final : public context::editor::gui::panels::inspector::OverrideWriteGateway
{
public:
    explicit AlwaysApplies(context::editor::serializer::JsonValue current)
        : current_(std::move(current))
    {
    }

    context::editor::gui::panels::inspector::WriteAttempt
    attempt(const context::editor::gui::panels::inspector::OverrideWriteRequest& request,
            std::uint64_t) const override
    {
        context::editor::gui::panels::inspector::WriteAttempt out;
        out.applied = true;
        out.file = request.root_scene;
        out.pointer = request.pointer;
        out.raw_hash = ++raw_hash_;
        return out;
    }

    context::editor::gui::panels::inspector::FieldState
    read(const std::string&, const std::vector<std::string>&, const std::string&) const override
    {
        context::editor::gui::panels::inspector::FieldState out;
        out.present = true;
        out.raw_hash = raw_hash_;
        out.value = current_;
        return out;
    }

private:
    context::editor::serializer::JsonValue current_;
    mutable std::uint64_t raw_hash_ = 100;
};

[[nodiscard]] context::editor::serializer::JsonValue jnum(double value)
{
    context::editor::serializer::JsonValue v;
    v.type = context::editor::serializer::JsonValue::Type::number;
    v.number_value = value;
    return v;
}

// A one-node scene tree holding `identity`. `identity_hash` is explicit because a case about a REFETCH
// needs the hash to be able to RE-RESOLVE to a different value — left at the model's 0 default, a
// vanished node resolves to 0 as well and the panel notifies nobody.
[[nodiscard]] scenetree::SceneTreeModel tree_with_one_node(const char* identity,
                                                          std::uint64_t identity_hash = 0)
{
    scenetree::SceneTreeModel model;
    scenetree::SceneTreeNode node;
    node.identity = identity;
    node.display_name = "Player";
    node.identity_hash = identity_hash;
    model.roots.push_back(std::move(node));
    return model;
}

// The daemon's `selection-changed` session fact for `ids`, from a FOREIGN client (origin 7) so echo
// suppression passes it through. ONE spelling of this payload for every case that drives it — three
// hand-rolled copies would drift from the wire shape independently.
[[nodiscard]] Json selection_fact(const std::vector<std::string>& ids)
{
    Json fact = Json::object();
    fact.set("event", Json(std::string("selection-changed")));
    fact.set("origin", Json(std::uint64_t{7}));
    Json list = Json::array();
    for (const std::string& id : ids)
    {
        list.push_back(Json(id));
    }
    fact.set("ids", std::move(list));
    return fact;
}

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

    // FIVE panels, from four different libraries (uitree / problems / the e05d3 pair / the e09c
    // session journal) — the panel-agnosticism claim exercised across every hosted shape.
    // (AMENDED by editor-window-chrome e1: the docked `builtin.playbar` retired, 6 -> 5.)
    CHECK(panels::hostable_panel_ids().size() == 5);
    CHECK(host.hosts("placeholder"));
    CHECK(host.hosts(context::editor::gui::panels::problems::ProblemsPanel::kContributionId));
    CHECK(host.hosts("builtin.scene-tree"));
    CHECK(host.hosts("builtin.inspector"));
    // e09c: rostered since e05b and UNHOSTED until now — which is exactly why the journal's
    // to_json/load_json had no caller. Hosting it is what gives `session.undo` / `session.redo` a
    // reachable path (R-CLI-001).
    CHECK(host.hosts("builtin.session.undo"));

    // THE e1 RETIREMENT, pinned both ways (D2 — the strip is the Play Bar's only home): the docked
    // playbar is not hosted, not claimed hostable, and not even ON the roster any more — so the
    // hydration runtime cannot reach it through any `panel.*` verb.
    CHECK(!host.hosts("builtin.playbar"));
    CHECK(!host.knows("builtin.playbar"));
    for (const std::string& id : panels::hostable_panel_ids())
    {
        CHECK(id != "builtin.playbar");
    }

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
    // The flip is about the ONE panel that gained a write path, not about every hosted panel: the
    // journal's replay surface is command-driven, still no gestures.
    const Json* undo_entry = find_panel(listing, "builtin.session.undo");
    CHECK(undo_entry != nullptr && !undo_entry->at("gestures").as_bool());
    // e1: the retired playbar does not appear in the listing at all — `panel.list` projects the
    // ROSTER, and the roster no longer carries it (an absent entry, not a `hosted:false` one).
    CHECK(find_panel(listing, "builtin.playbar") == nullptr);
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
// writer.
//
// PINNED AT THE OBJECT, NOT AT THE CLASS. An earlier form of this test asserted the replay came back
// carrying `shell.no_daemon` — a code exactly one class in the tree mints. That proved only that
// SOME `WireOverrideWriteGateway` was reached: a journal handed its OWN second instance would have
// minted the identical code and passed. The gateway's per-instance counters do not have that hole,
// because they live on the object `install_builtin_panels` bound. So: bind a client, drive a replay,
// and assert the counters on `bound.writes` moved. A second gateway leaves them at zero.
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
    undo::FieldEdit edit;
    edit.root_scene = "scenes/main.scene.json";
    edit.id_path = {"cam"};
    edit.pointer = "/components/camera/fov";
    bound.undo->record(std::move(edit));
    CHECK(bound.undo->journal().can_undo());

    // (1) NO DAEMON. The replay resolves — a write with nowhere to go is not silently swallowed —
    // as an ERROR, and the step is KEPT: the human's history must survive the daemon being briefly
    // gone, which is the difference between a refusal and a drop (undo_journal.h § undo).
    const undo::ReplayResult refused = bound.undo->replay_undo();
    CHECK(refused.status == inspector::CommitResult::Status::error);
    CHECK(!refused.edits.empty());
    CHECK(!refused.edits.empty() &&
          refused.edits.front().code == std::string(undo::UndoJournal::kReadUnavailableCode));
    CHECK(bound.undo->replay_refusals() == 1u);
    CHECK(bound.undo->replay_drops() == 0u);
    CHECK(bound.undo->journal().can_undo()); // KEPT — nothing was written, so nothing was lost
    CHECK(bound.writes->writes_applied() == 0u);

    // (2) A DAEMON. `editor.inspect` is left unscripted, so the mock refuses it and the read fails —
    // enough to prove WHICH object issued it, which is all this case is about (the full landed-write
    // frame is test_wire_override_gateway.cpp's `an_undo_replay_sends_the_same_edit_frame…`).
    Wired wired = make_wired_client();
    panels::bind_write_client(bound, wired.client.get());
    CHECK(bound.writes->has_client());
    const std::size_t reads_before = bound.writes->reads_issued();

    const undo::ReplayResult over_the_wire = bound.undo->replay_undo();
    CHECK(over_the_wire.status == inspector::CommitResult::Status::error);
    // THE ASSERTION: the read left its mark on the bag's OWN gateway instance.
    CHECK(bound.writes->reads_issued() == reads_before + 1);
    CHECK(bound.writes->writes_applied() == 0u); // the read failed, so nothing was attempted
    CHECK(bound.undo->journal().can_undo());     // still kept

    panels::bind_write_client(bound, nullptr);
}

// e09b-3 — THE HUE DECISION, which nothing else in this PR covers.
//
// `bind_write_notice_relay` is the ONE place a CommitResult/ReplayResult becomes a WriteNotice, and
// the KIND it picks is what chooses the human's colour (06 §2: `wait` = a co-writer got there first
// and nothing was lost, so re-apply; `bad` = the write path refused). Every other test stops on one
// side of that join — the feed suites assert the RESULT handed to the sink, and test_write_notice.cpp
// starts from a hand-built WriteNotice — so the mapping itself had no assertion at all: swap the two
// constants at the translation point and the entire suite stays green while a routine collision tells
// the human their project is unreachable. The cross-language pins do not close it either; they
// compare each constant's SPELLING, never which status it is chosen for.
void a_refused_write_reaches_the_relay_as_a_notice_hued_for_its_status()
{
    namespace undo = context::editor::gui::session::undo;
    namespace inspector = context::editor::gui::panels::inspector;

    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.undo != nullptr);
    CHECK(bound.inspector != nullptr);
    if (bound.undo == nullptr || bound.inspector == nullptr)
    {
        return;
    }

    shell::UiMirrorStore mirror;
    shell::WriteNoticeRelay relay;
    relay.bind_store(&mirror);
    panels::bind_write_notice_relay(bound, relay);

    // BOTH sinks, not only the one this case drives. The gesture path and the replay path are wired
    // together here, so a regression binding just one would leave half the editor silent — and the
    // half still working would keep every other assertion in the suite green.
    CHECK(bound.inspector->has_notice_sink());
    CHECK(bound.undo->has_notice_sink());

    undo::FieldEdit edit;
    edit.root_scene = "scenes/main.scene.json";
    edit.id_path = {"cam"};
    edit.pointer = "/components/camera/fov";
    bound.undo->record(std::move(edit));

    // No daemon: the replay resolves as an ERROR — the write PATH refused and NO concurrency event
    // was observed — which is `bad`, not `wait`.
    const undo::ReplayResult refused = bound.undo->replay_undo();
    CHECK(refused.status == inspector::CommitResult::Status::error);

    CHECK(relay.published() == 1u);
    CHECK(relay.delivered() == 1u); // no windows provider -> the primary, exactly once
    const std::vector<Json> queued = mirror.take(shell::kPrimaryWindowId);
    CHECK(queued.size() == 1u);
    if (queued.size() == 1u)
    {
        CHECK(queued[0].at("topic").as_string() == shell::kUiTopicWriteNotice);
        CHECK(queued[0].at("origin").as_string() == shell::kWriteNoticeOrigin);
        // THE ASSERTION THIS CASE EXISTS FOR: an `error` status is hued as a REFUSAL, never a drop.
        // Stated both ways on purpose — the equality pins the mapping, and the inequality keeps the
        // case meaningful if the two constants ever collapse to the same string.
        CHECK(queued[0].at("payload").at("kind").as_string() == shell::kWriteNoticeKindRefusal);
        CHECK(queued[0].at("payload").at("kind").as_string() != shell::kWriteNoticeKindDrop);
        // The VERB reaches the human as the action they actually took, not as a generic "write".
        CHECK(queued[0].at("payload").at("action").as_string() == "undo");
        CHECK(queued[0].at("payload").at("code").as_string() ==
              std::string(undo::UndoJournal::kReadUnavailableCode));
        // And the explanatory edit was lifted rather than left empty (notice_from_replay's
        // front()-skip), so the toast carries a reason and not just a headline.
        CHECK(!queued[0].at("payload").at("message").as_string().empty());
    }
}

// e09c — THE TWO PERSISTENCE SEAMS, end to end through the BAG, against a REAL editor-state file.
// This is exactly the path `editor_main.cpp` drives (boot restore + a per-frame publish), and
// asserting it here is what keeps that wiring from being the only place it is ever exercised — the
// failure mode being an editor that journals faithfully in memory and persists nothing.
void the_journal_publishes_to_and_restores_from_the_editor_state_store()
{
    namespace undo = context::editor::gui::session::undo;

    const std::filesystem::path root =
        panelstest::make_temp_project("context-builtin-panels", "undo");

    // ---- session 1: a checkpoint is recorded and published ----
    {
        shell::PanelHost host;
        panels::BuiltinPanels bound = panels::install_builtin_panels(host);
        CHECK(bound.undo != nullptr);
        // Contained to this block rather than `return`ed from the whole case: an early return here
        // would silently skip session 2 AND leak the temp project past the cleanup below.
        if (bound.undo != nullptr)
        {
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
    }

    // ---- session 2: a fresh bag restores it ----
    {
        shell::EditorStateStore store(root, 0);
        store.load();
        shell::PanelHost host;
        panels::BuiltinPanels bound = panels::install_builtin_panels(host);
        CHECK(bound.undo != nullptr);
        if (bound.undo != nullptr)
        {
            CHECK(!bound.undo->journal().can_undo()); // a fresh bag starts empty
            CHECK(panels::restore_undo_state(bound, store.state()));
            CHECK(bound.undo->journal().can_undo());
            CHECK(bound.undo->journal().undo_depth() == 1u);
        }
    }

    panelstest::cleanup(root);
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
    namespace problems_panel = context::editor::gui::panels::problems;
    const std::string panel_id = problems_panel::ProblemsPanel::kContributionId;

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

// M9 e09d: a SHELL-LOCAL problem reaches the Problems panel through the same dispatch a daemon
// diagnostic takes. This is what makes the 07 §6 "loudly" of an editor-state recovery real inside a
// GUI: the only other channel is stderr, which the human whose window layout just vanished is not
// watching. The corrupt-recovery diagnostic is the concrete caller (editor_main.cpp).
void a_shell_local_problem_reaches_the_rendered_panel()
{
    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.problems != nullptr);
    if (bound.problems == nullptr)
    {
        return;
    }
    namespace problems_panel = context::editor::gui::panels::problems;
    const std::string panel_id = problems_panel::ProblemsPanel::kContributionId;
    const std::uint64_t before = host.revision(panel_id);

    CHECK(panels::report_local_problem(
        bound, shell::kEditorStateInvalidCode,
        "the editor's own session file was unreadable and has been reset to defaults",
        "/projects/demo/.editor/editor-state.json"));
    CHECK(host.revision(panel_id) > before);

    std::string error_code;
    const std::optional<shell::PanelRender> rendered = host.render(panel_id, error_code);
    CHECK(rendered.has_value());
    if (rendered.has_value())
    {
        CHECK(panelstest::mentions(rendered->html, "has been reset to defaults"));
        // The FILE is rendered too, not just the sentence: "your layout was reset" without saying
        // which project's file is a diagnostic nobody can act on.
        CHECK(panelstest::mentions(rendered->html, "editor-state.json"));
    }

    // A bag with no Problems feed is a NO-OP, not a crash: the caller has already reported to stderr,
    // and an editor that fell over because it could not render its own diagnostic would be the joke
    // version of loud.
    panels::BuiltinPanels empty;
    CHECK(!panels::report_local_problem(empty, shell::kEditorStateInvalidCode, "m", "p"));
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

    bound.scenetree->panel().set_model(tree_with_one_node("inst1/ent1"));

    CHECK(!bound.inspector->pending().has_value());

    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic,
                                      selection_fact({"inst1/ent1"})));
    CHECK(bound.inspector->pending() == std::optional<std::string>("inst1/ent1"));

    // A cleared selection (an empty id list) clears the panel and drops the pending fetch.
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic, selection_fact({})));
    CHECK(!bound.inspector->pending().has_value());
    CHECK(!bound.inspector->panel().has_selection());

    // With no daemon bound, a row activation writes NOTHING and moves NOTHING — the panel cannot
    // change a selection it does not own (and the composition root leaves it honestly read-only).
    CHECK(!bound.scenetree->panel().select("inst1/ent1"));
    CHECK(!bound.inspector->pending().has_value());
}

// M9 x9 (CE #449) — THE L-30 GUARD ON THE PATH THAT NEVER TOUCHES `InspectorFeed::apply_event`.
//
// x9 made a plain `edit` publish `derivation.settled`, so the settle now reaches EVERY feed on this
// bag rather than one — and it reaches the Inspector's staged gesture through the SCENE TREE, by a
// door `apply_event`'s own L-30 guard does not watch. The full causal chain, and why the deferral
// therefore lives in `InspectorFeed::request` where BOTH doors pass, is stated once at its canonical
// site: inspector_feed.h § request.
//
// WHAT THIS CASE PINS, and why it is driven through `pump_panel_feeds` over a real client rather than
// by poking the panels: the damage would be done by an `editor.inspect` RPC that the pump issues LATER
// IN THE SAME CALL that refetched the tree, so the honest assertion is that the RPC never went out —
// `requests_for("editor.inspect")` — not the proxy `!pending()`. Asserting the tree refetch DID happen
// is what keeps that negative non-vacuous: a regression that simply stopped refetching would satisfy
// "no inspect request" trivially. The tail matters as much as the guard — a guard that turned this
// into a PERMANENT refusal would look identical if only the negatives were asserted — so the release
// is driven and the RPC is then observed to go out.
//
// ⚠ PLANT: delete the `has_staged_edit()` branch in `InspectorFeed::request` and this REDS — the
// `editor.inspect` request goes out during the first pump and the staged edit is gone.
void a_settle_driven_tree_refetch_does_not_rebase_a_staged_gesture()
{
    namespace inspector = context::editor::gui::panels::inspector;

    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host); // non-const: the pump mutates it
    CHECK(bound.scenetree != nullptr);
    CHECK(bound.inspector != nullptr);
    CHECK(bound.session != nullptr);
    if (bound.scenetree == nullptr || bound.inspector == nullptr || bound.session == nullptr)
    {
        return;
    }

    // The tree holds the entity with a NON-ZERO identity hash — the trigger IS that hash re-resolving.
    bound.scenetree->panel().set_model(tree_with_one_node("inst1/ent1", 0xAAAA));

    // The DAEMON's selection (e08b truth), which also arms the first Inspector fetch through the very
    // listener under test. Claim it the way the pump does, so the assertions below are unambiguous.
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic,
                                      selection_fact({"inst1/ent1"})));
    CHECK(bound.inspector->pending() == std::optional<std::string>("inst1/ent1"));
    bound.inspector->mark_fetched();

    // Hydrate the Inspector on that identity with the CAS base the gesture will guard against, then
    // stage the gesture — the human is mid-edit.
    inspector::InspectorModel inspected;
    inspected.has_entity = true;
    inspected.root_scene = "root.scene.json";
    inspected.id_path = {"inst1", "ent1"};
    inspected.identity = "inst1/ent1";
    inspector::InspectorField editable;
    editable.pointer = "/components/camera/fov";
    editable.label = "fov";
    editable.kind = inspector::WidgetKind::number;
    editable.editable = true;
    editable.value = jnum(6.5);
    inspected.fields.push_back(editable);
    bound.inspector->panel().set_model(std::move(inspected), 0x1234);
    CHECK(bound.inspector->panel().stage_edit("/components/camera/fov", jnum(9.75)));
    CHECK(bound.inspector->panel().has_staged_edit());

    // The wire: the tree refetch comes back with the entity GONE — what another window's full-content
    // `edit` looks like from here. `editor.inspect` is scripted too, so that if the guard regresses the
    // failure is a REQUEST THAT WENT OUT rather than a transport error that could be mistaken for one.
    Wired wired = make_wired_client();
    wired.channel->on("editor.scene-tree",
                      [](const clientmock::Request&)
                      {
                          Json tree = Json::object();
                          tree.set("roots", Json::array()); // an EMPTY tree, not an absent one
                          return clientmock::MockChannel::ok_envelope(std::move(tree));
                      });
    wired.channel->on("editor.inspect", [](const clientmock::Request&)
                      { return clientmock::MockChannel::ok_envelope(Json::object()); });

    // A settle arrives and marks the tree's fetch due (claim it first — a feed is born due for its
    // initial hydration, so without this the pump would refetch for the wrong reason).
    bound.scenetree->mark_fetched();
    CHECK(!bound.scenetree->fetch_due());
    Json settled = Json::object();
    settled.set("event", Json(std::string("derivation.settled")));
    settled.set("generation", Json(std::uint64_t{7}));
    CHECK(panels::apply_scenetree_event(*bound.scenetree, panels::kDerivationTopic, settled, 7));
    CHECK(bound.scenetree->fetch_due());

    // ONE pump — the same call that refetches the tree is the one that would serve the re-read.
    panels::pump_panel_feeds(bound, *wired.client, "root.scene.json");

    // THE ASSERTIONS. The tree WAS refetched (so the trigger really fired and the negative below is
    // not vacuous), no `editor.inspect` went out, the re-read is OWED rather than dropped, and both
    // halves the gesture depends on survive: the staged edit and the CAS base it will commit against.
    CHECK(wired.channel->requests_for("editor.scene-tree").size() == 1);
    CHECK(wired.channel->requests_for("editor.inspect").empty());
    CHECK(bound.inspector->refresh_deferred());
    CHECK(bound.inspector->panel().has_staged_edit());
    CHECK(bound.inspector->panel().base_raw_hash() == 0x1234u);
    CHECK(bound.inspector->rereads_armed() == 0); // deferred is NOT armed, and is not counted as one

    // AND IT IS A DEFERRAL, NOT A REFUSAL. End the gesture through the real seam, pump again, and the
    // re-read the guard withheld now actually goes out over the wire.
    bool dispatched = false;
    std::string error_code;
    CHECK(host.gesture("builtin.inspector", shell::GestureVerb::cancel, Json::object(), dispatched,
                       error_code));
    CHECK(dispatched);
    CHECK(!bound.inspector->panel().has_staged_edit());
    CHECK(!bound.inspector->refresh_deferred());
    CHECK(bound.inspector->rereads_armed() == 1);
    panels::pump_panel_feeds(bound, *wired.client, "root.scene.json");
    CHECK(wired.channel->requests_for("editor.inspect").size() == 1);
}

// ------------------------ M9 x10 (CE #452): the SELECTION door, at the composition root, over a wire
//
// THE DEFECT x9's guard did not cover. The same FOREIGN `selection-changed` fact the case above rides
// can carry a DIFFERENT identity, or NO identities at all — and the composition root's selection
// listener then took a door the SAME-identity guard let straight through, so the pump's
// `editor.inspect` landed, `set_model` discarded the human's staged edit AND re-based the L-30 CAS base
// onto the mover's post-write state. Silently, because a defeated compare-and-swap is
// indistinguishable from a successful edit.
//
// DRIVEN THE SAME WAY x9's case is, and for the same reason: the damage would be done by an
// `editor.inspect` RPC the pump issues, so the honest assertion is that the RPC NEVER WENT OUT
// (`requests_for("editor.inspect")`), never the proxy `!pending()`. Asserting that the FACT was applied
// is what keeps that negative non-vacuous — a regression that stopped applying session facts at all
// would satisfy "no inspect request" trivially.

// The inspected entity + its editable field, hydrated the way a landed `editor.inspect` leaves the
// panel. One spelling for the three cases below, so none of them can drift into staging a different
// gesture from its siblings.
[[nodiscard]] context::editor::gui::panels::inspector::InspectorModel inspected_camera()
{
    namespace inspector = context::editor::gui::panels::inspector;
    inspector::InspectorModel model;
    model.has_entity = true;
    model.root_scene = "root.scene.json";
    model.id_path = {"inst1", "ent1"};
    model.identity = "inst1/ent1";
    inspector::InspectorField editable;
    editable.pointer = "/components/camera/fov";
    editable.label = "fov";
    editable.kind = inspector::WidgetKind::number;
    editable.editable = true;
    editable.value = jnum(6.5);
    model.fields.push_back(editable);
    return model;
}

// ⚠ PLANT: narrow `InspectorFeed::request`'s guard back to the SAME-identity test and this REDS — the
// `editor.inspect` request goes out during the pump and the staged edit and CAS base are gone.
void a_foreign_selection_move_does_not_rebase_a_staged_gesture()
{
    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host); // non-const: the pump mutates it
    CHECK(bound.scenetree != nullptr);
    CHECK(bound.inspector != nullptr);
    CHECK(bound.session != nullptr);
    if (bound.scenetree == nullptr || bound.inspector == nullptr || bound.session == nullptr)
    {
        return;
    }

    bound.scenetree->panel().set_model(tree_with_one_node("inst1/ent1", 0xAAAA));
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic,
                                      selection_fact({"inst1/ent1"})));
    CHECK(bound.inspector->pending() == std::optional<std::string>("inst1/ent1"));
    bound.inspector->mark_fetched();

    bound.inspector->panel().set_model(inspected_camera(), 0x1234);
    CHECK(bound.inspector->panel().stage_edit("/components/camera/fov", jnum(9.75)));
    CHECK(bound.inspector->panel().has_staged_edit());

    Wired wired = make_wired_client();
    // `editor.inspect` is SCRIPTED so that a guard regression shows up as a REQUEST THAT WENT OUT
    // rather than as a transport error somebody could mistake for one.
    wired.channel->on("editor.inspect", [](const clientmock::Request&)
                      { return clientmock::MockChannel::ok_envelope(Json::object()); });

    // ANOTHER CLIENT MOVES THE SHARED SELECTION, mid-gesture. Applied (so the negatives below are not
    // vacuous) and, since x10, WITHHELD rather than served.
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic,
                                      selection_fact({"inst1/ent2"})));
    CHECK(bound.inspector->selections_deferred() == 1u);
    CHECK(bound.inspector->selection_deferred());
    CHECK(!bound.inspector->pending().has_value());

    // THE PUMP — the call that would actually destroy the gesture.
    panels::pump_panel_feeds(bound, *wired.client, "root.scene.json");
    CHECK(wired.channel->requests_for("editor.inspect").empty());
    CHECK(bound.inspector->panel().has_staged_edit());
    CHECK(bound.inspector->panel().base_raw_hash() == 0x1234u);
    CHECK(bound.inspector->abandons_observed() == 0u); // nothing was lost, so nothing was reported

    // AND IT IS A DEFERRAL, NOT A REFUSAL: the move is served once the gesture ends, so the panel
    // follows the daemon's selection rather than sitting on a stale one forever. A guard that turned
    // this into a permanent refusal would look identical if only the negatives above were asserted.
    bool dispatched = false;
    std::string error_code;
    CHECK(host.gesture("builtin.inspector", shell::GestureVerb::cancel, Json::object(), dispatched,
                       error_code));
    CHECK(dispatched);
    CHECK(!bound.inspector->selection_deferred());
    CHECK(bound.inspector->pending() == std::optional<std::string>("inst1/ent2"));
    panels::pump_panel_feeds(bound, *wired.client, "root.scene.json");
    CHECK(wired.channel->requests_for("editor.inspect").size() == 1);
}

// The OTHER arm, and the one the brief flags as easiest to leave uncovered: an EMPTY id list routes to
// `request_clear`, which ran `panel_.clear()` — discarding the staged edit AND the owed settle deferral
// outright, with not even a pump involved.
//
// ⚠ PLANT: delete `request_clear`'s `has_staged_edit()` branch and this REDS on the staged edit, the
// model, and the still-owed refresh — all three, without any RPC.
void a_foreign_selection_clear_does_not_discard_a_staged_gesture()
{
    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.scenetree != nullptr);
    CHECK(bound.inspector != nullptr);
    CHECK(bound.session != nullptr);
    if (bound.scenetree == nullptr || bound.inspector == nullptr || bound.session == nullptr)
    {
        return;
    }

    bound.scenetree->panel().set_model(tree_with_one_node("inst1/ent1", 0xAAAA));
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic,
                                      selection_fact({"inst1/ent1"})));
    bound.inspector->mark_fetched();
    bound.inspector->panel().set_model(inspected_camera(), 0x1234);
    CHECK(bound.inspector->panel().stage_edit("/components/camera/fov", jnum(9.75)));

    // A settle is ALSO owed, which is what makes the "and the owed deferral" half observable: the old
    // `panel_.clear()` reset it, so the release after the gesture ended had nothing to release.
    Json settled = Json::object();
    settled.set("event", Json(std::string("derivation.settled")));
    settled.set("generation", Json(std::uint64_t{9}));
    CHECK(!panels::apply_inspector_event(*bound.inspector, panels::kDerivationTopic, settled));
    CHECK(bound.inspector->refresh_deferred());

    // THE FOREIGN CLEAR. Applied by the session feed (the scene tree's rendered selection really does
    // go empty), and withheld by the Inspector.
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic, selection_fact({})));
    // The TREE really did go empty — so the Inspector's survival below is a DECISION it made, not a
    // fact that never reached it.
    CHECK(bound.scenetree->panel().selection().identity.empty());
    CHECK(bound.inspector->selections_deferred() == 1u);
    CHECK(bound.inspector->panel().has_staged_edit());
    CHECK(bound.inspector->panel().has_selection());        // the Inspector did NOT
    CHECK(bound.inspector->panel().base_raw_hash() == 0x1234u);
    CHECK(bound.inspector->refresh_deferred());             // and the settle is still owed
    CHECK(bound.inspector->abandons_observed() == 0u);

    // Ending the gesture performs the clear — and the refresh dies with the selection rather than
    // arming a re-read of an entity nothing is inspecting.
    bool dispatched = false;
    std::string error_code;
    CHECK(host.gesture("builtin.inspector", shell::GestureVerb::cancel, Json::object(), dispatched,
                       error_code));
    CHECK(dispatched);
    CHECK(!bound.inspector->panel().has_selection());
    CHECK(!bound.inspector->selection_deferred());
    CHECK(!bound.inspector->refresh_deferred());
    CHECK(!bound.inspector->pending().has_value());
    CHECK(bound.inspector->rereads_armed() == 0);
}

// x10 — THE HUE/CATEGORY DECISION for an ABANDONMENT, the exact sibling of e09b-3's
// `a_refused_write_reaches_the_relay_as_a_notice_hued_for_its_status`.
//
// `bind_write_notice_relay` is the ONE place an `AbandonedGesture` becomes a `WriteNotice`, and the KIND
// it picks decides both the human's colour and their SENTENCE. Every other test stops on one side of
// that join — the feed suite asserts the record handed to the sink, notifications.test.ts starts from a
// hand-built notice — so swapping the constant at the translation point would leave both suites green
// while telling the human a co-writer changed their field, which in this case is simply untrue.
void an_abandoned_gesture_reaches_the_relay_as_its_own_notice_kind()
{
    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.inspector != nullptr);
    if (bound.inspector == nullptr)
    {
        return;
    }

    shell::UiMirrorStore mirror;
    shell::WriteNoticeRelay relay;
    relay.bind_store(&mirror);
    panels::bind_write_notice_relay(bound, relay);
    CHECK(bound.inspector->has_abandon_sink());

    // The one undeferrable interleaving: a fetch armed with nothing staged, the human then types, and
    // the pump serves the claimed fetch. Everything here goes through the REAL seams the live Shell
    // drives — `request_inspector`, `pump_panel_feeds`, and a real client over the scripted wire.
    bound.inspector->panel().set_model(inspected_camera(), 0x1234);
    CHECK(panels::request_inspector(bound, "inst1/ent2"));
    CHECK(bound.inspector->panel().stage_edit("/components/camera/fov", jnum(9.75)));
    CHECK(bound.inspector->panel().has_staged_edit());

    Wired wired = make_wired_client();
    wired.channel->on("editor.inspect",
                      [](const clientmock::Request&)
                      {
                          Json data = Json::object();
                          Json model = Json::object();
                          model.set("present", Json(true));
                          model.set("rootScene", Json(std::string("root.scene.json")));
                          model.set("identity", Json(std::string("inst1/ent2")));
                          data.set("inspector", std::move(model));
                          data.set("rawHash", Json(std::string("777")));
                          return clientmock::MockChannel::ok_envelope(std::move(data));
                      });
    panels::pump_panel_feeds(bound, *wired.client, "root.scene.json");

    // The loss happened (so the assertions below are about a real event) and it was REPORTED.
    CHECK(!bound.inspector->panel().has_staged_edit());
    CHECK(bound.inspector->abandons_observed() == 1u);
    CHECK(relay.published() == 1u);
    CHECK(relay.delivered() == 1u); // no windows provider -> the primary, exactly once
    const std::vector<Json> queued = mirror.take(shell::kPrimaryWindowId);
    CHECK(queued.size() == 1u);
    if (queued.size() == 1u)
    {
        CHECK(queued[0].at("topic").as_string() == shell::kUiTopicWriteNotice);
        CHECK(queued[0].at("origin").as_string() == shell::kWriteNoticeOrigin);
        // THE ASSERTION THIS CASE EXISTS FOR. Stated three ways on purpose: the equality pins the
        // mapping, and the two inequalities keep the case meaningful — an abandonment routed through
        // either e09b-3 kind would hand the human a confidently wrong story about how their typing
        // disappeared, and both mis-routings are one edited constant away.
        CHECK(queued[0].at("payload").at("kind").as_string() == shell::kWriteNoticeKindAbandoned);
        CHECK(queued[0].at("payload").at("kind").as_string() != shell::kWriteNoticeKindDrop);
        CHECK(queued[0].at("payload").at("kind").as_string() != shell::kWriteNoticeKindRefusal);
        CHECK(queued[0].at("payload").at("action").as_string() == "edit");
        CHECK(queued[0].at("payload").at("code").as_string() ==
              std::string(panels::kGestureAbandonedCode));
        CHECK(queued[0].at("payload").at("pointer").as_string() == "/components/camera/fov");
        CHECK(!queued[0].at("payload").at("message").as_string().empty());
    }
}

// e09c READ-YOUR-REPLAYS, at the composition root. `pump_panel_feeds` is what turns a landed replay
// into an Inspector re-read; without it the panel keeps the pre-undo value AND the pre-undo CAS
// token, so the human's very next edit to that field is dropped as a "concurrent writer" that is
// really their own Ctrl+Z. Asserted through the flag's CONSUMPTION, which is observable without a
// hydrated Inspector model: delete the pump's block and the flag is still raised afterwards.
void the_pump_consumes_a_landed_replay_and_rearms_the_inspector()
{
    namespace undo = context::editor::gui::session::undo;

    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.undo != nullptr);
    CHECK(bound.inspector != nullptr);
    if (bound.undo == nullptr || bound.inspector == nullptr)
    {
        return;
    }
    // An in-memory gateway in place of the wire one: what is under test here is the PUMP's reaction
    // to a landed replay, not the wire (test_wire_override_gateway.cpp owns that).
    const AlwaysApplies store(jnum(75.0));
    bound.undo->bind_gateway(&store);

    undo::FieldEdit edit;
    edit.root_scene = "scenes/main.scene.json";
    edit.id_path = {"cam"};
    edit.pointer = "/components/camera/fov";
    edit.before = jnum(60.0);
    edit.after = jnum(75.0);
    bound.undo->record(std::move(edit));
    CHECK(bound.undo->replay_undo().ok());

    Wired wired = make_wired_client();
    panels::pump_panel_feeds(bound, *wired.client, "scenes/main.scene.json");
    // CONSUMED by the pump — which it can only be if the pump reached the re-arm block.
    CHECK(!bound.undo->take_replay_landed());

    bound.undo->bind_gateway(nullptr);
}

// e09c — a bag with NO undo host: both persistence seams tolerate it, and neither claims to have
// done work. Reachable in production, not merely defensive: `install_builtin_panels` DROPS the host
// when `PanelHost::provide` refuses the id, and the host's roster is caller-supplied.
void the_undo_persistence_seams_tolerate_a_bag_with_no_host()
{
    const std::filesystem::path root = panelstest::make_temp_project("context-builtin-panels", "bare");
    panels::BuiltinPanels bare; // every member null, exactly as a hand-built bag is
    shell::EditorStateStore store(root, 0);
    store.load();
    CHECK(!panels::restore_undo_state(bare, store.state()));
    CHECK(!panels::publish_undo_state(bare, store, 0));
    panelstest::cleanup(root);
}

// e09c — an EMPTY-but-well-formed journal document restores NOTHING, and says so. It parses
// perfectly, so a seam answering "did the blob load?" would say `true` — and `editor_main.cpp` would
// then announce "restored the previous session's undo history" for a history that is empty.
void an_empty_journal_document_restores_nothing()
{
    shell::PanelHost host;
    panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.undo != nullptr);
    if (bound.undo != nullptr)
    {
        shell::EditorState empty_state;
        empty_state.undo = bound.undo->to_blob(); // a real serialization of an EMPTY journal
        CHECK(empty_state.undo.is_string() && !empty_state.undo.as_string().empty());
        CHECK(!panels::restore_undo_state(bound, empty_state));
        CHECK(!bound.undo->journal().can_undo());
    }
}

} // namespace

int main()
{
    binds_every_hostable_panel_and_nothing_else();
    the_inspector_gesture_surface_is_live_but_refuses_without_a_daemon();
    the_undo_replay_routes_through_the_shared_wire_gateway();
    a_refused_write_reaches_the_relay_as_a_notice_hued_for_its_status();
    the_journal_publishes_to_and_restores_from_the_editor_state_store();
    the_pump_consumes_a_landed_replay_and_rearms_the_inspector();
    the_undo_persistence_seams_tolerate_a_bag_with_no_host();
    an_empty_journal_document_restores_nothing();
    renders_the_hosted_panels_through_the_bridge();
    a_daemon_event_reaches_the_rendered_panel();
    a_shell_local_problem_reaches_the_rendered_panel();
    a_daemon_selection_schedules_the_inspector_fetch();
    a_settle_driven_tree_refetch_does_not_rebase_a_staged_gesture();
    a_foreign_selection_move_does_not_rebase_a_staged_gesture();
    a_foreign_selection_clear_does_not_discard_a_staged_gesture();
    an_abandoned_gesture_reaches_the_relay_as_its_own_notice_kind();
    PANELS_TEST_MAIN_END();
}
