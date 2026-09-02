// T1 for the LIVE Scene-viewport feed (M9 editor-UX e3, D7): the per-INSTANCE models c3's
// `provide_factory` binds, the `viewport.frame-scene` -> `editor.camera-set` -> `editor.cameras-get`
// round trip through the OPAQUE payload, the failed-write re-arm, and the `viewport.adapter_absent`
// degraded summary.
//
// The daemon is not mocked here: the feed produces `editor.camera-set` PARAMS and consumes an
// `editor.cameras-get` RESULT, and the pump is what moves them (builtin_panels.cpp). Driving the two
// halves against each other — the params this feed emits fed back as the reply the daemon would
// serve — is what proves the payload survives the round trip, and it is the same shape the
// integration test then proves against the REAL `EditorSessionState` persistence.

#include "context/editor/shell/panels/viewport_feed.h"

#include "context/editor/gui/panels/files/files_model.h" // e4 integration: the OTHER selection subject
#include "context/editor/gui/panels/files/files_panel.h"
#include "context/editor/gui/panels/scenetree/scene_tree_model.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/session_feed.h" // e4 integration: the REAL editor.select writer
#include "context/editor/shell/viewport_binding.h"

#include "mock_channel.h"
#include "render_test_rhi.h" // rendertest::FakeDevice — a producer WITH an adapter, no GPU
#include "panels_test.h"
#include "wired_client_test.h" // Wired / make_client -- shared with test_session_feed.cpp

#include <optional>
#include <string>
#include <vector>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace viewport = context::editor::gui::viewport;
namespace scenetree = context::editor::gui::panels::scenetree;
namespace files = context::editor::gui::panels::files;
namespace render = context::render;
namespace kernel = context::kernel;
using Json = context::editor::contract::Json;

namespace
{

constexpr const char* kPanelId = "builtin.viewport";

// The `editor.cameras-get` reply the daemon would serve for a set of `editor.camera-set` params —
// the daemon's own shape (`cameras_json`: an ARRAY of objects carrying their key), assembled from
// what the feed emitted. This is the round trip's turning point.
[[nodiscard]] Json cameras_reply(const std::vector<Json>& writes)
{
    Json cameras = Json::array();
    for (const Json& params : writes)
    {
        Json camera = Json::object();
        camera.set("viewportId", Json(params.at("viewportId").as_string()));
        camera.set("transform", params.at("transform"));
        camera.set("projection", params.at("projection"));
        cameras.push_back(camera);
    }
    Json data = Json::object();
    data.set("cameras", cameras);
    Json envelope = Json::object();
    envelope.set("ok", Json(true));
    envelope.set("data", data);
    return envelope;
}

// ------------------------------------------------------------------- e4: picking (D8) fixtures
//
// `panelstest::Wired`/`make_client` (wired_client_test.h, shared with test_session_feed.cpp) do the
// generic attach wiring; this suite's own addition is the `editor.select` mock reply, in the
// daemon's own shape (kernel_server.cpp): the reply's `ids` is the acted subject's post-write
// selection, `changed` true whenever the ids differ from empty-or-same.
[[nodiscard]] panelstest::Wired make_wired_client(std::uint64_t client_id)
{
    panelstest::Wired wired = panelstest::make_client(client_id);
    wired.channel->on("editor.select",
                       [](const clientmock::Request& request)
                       {
                           Json data = Json::object();
                           data.set("ids", request.params.at("ids"));
                           data.set("mode", Json(std::string("replace")));
                           data.set("changed", Json(true));
                           return clientmock::MockChannel::ok_envelope(std::move(data));
                       });
    return wired;
}

// A one-row Scene tree model whose row identity is the id `panels::pick_selection_id(entity)`
// produces — so a pick landing on `entity` resolves to a REAL row (a non-zero identity_hash), rather
// than an id nothing in the tree recognises.
[[nodiscard]] scenetree::SceneTreeModel model_for(kernel::Entity entity,
                                                  std::uint64_t identity_hash,
                                                  const char* display_name)
{
    scenetree::SceneTreeModel model;
    scenetree::SceneTreeNode node;
    node.identity = panels::pick_selection_id(entity);
    node.identity_hash = identity_hash;
    node.display_name = display_name;
    model.roots.push_back(node);
    model.entity_count = 1;
    return model;
}

// A RenderSnapshot with one drawable at `position`, so a straight-ahead ray from `default_scene_view`
// (position (0,3,8), looking down -Z) hits it: matching x/y, z well in front of the camera.
[[nodiscard]] render::RenderSnapshot snapshot_with(kernel::Entity entity, float x)
{
    render::RenderSnapshot snapshot;
    render::RenderItem item;
    item.entity = entity;
    item.transform.position[0] = x;
    item.transform.position[1] = 3.0f;
    item.transform.position[2] = -2.0f;
    item.transform.scale[0] = 2.0f;
    item.transform.scale[1] = 2.0f;
    item.transform.scale[2] = 2.0f;
    snapshot.items.push_back(item);
    return snapshot;
}

constexpr kernel::Entity kPickEntityA{7u, 3u};
constexpr kernel::Entity kPickEntityB{9u, 4u};

// Wires the follow-up steps every hit/miss/camera-move test below repeats identically: set `tree`'s
// model, bind it to `session`, attach a `Wired` client (this suite's `editor.select` mock) to
// `session`, and point `feed` at `tree`. Construction of `host`/`session`/`tree`/`binding`/`feed`
// itself stays at each call site — none of those types is movable (see e.g. ViewportFeed's deleted
// copy/move ctors), and each one's constructor already takes a reference/pointer to one of the
// others, so there is no non-degenerate "build a fixture struct and hand it back by value" here.
[[nodiscard]] panelstest::Wired wire_picking_fixture(panels::SessionFeed& session,
                                                      scenetree::SceneTreePanel& tree,
                                                      panels::ViewportFeed& feed,
                                                      const scenetree::SceneTreeModel& model,
                                                      std::uint64_t client_id = 5)
{
    tree.set_model(model);
    session.bind_scene_tree(&tree, scenetree::SceneTreePanel::kContributionId);
    panelstest::Wired wired = make_wired_client(client_id);
    session.bind_client(wired.client.get(), wired.client->client_id());
    feed.bind_scene_tree(&tree);
    return wired;
}

void a_hit_moves_the_scene_tree_and_leaves_files_untouched()
{
    shell::PanelHost host;
    panels::SessionFeed session(host, "unused.playbar");
    scenetree::SceneTreePanel tree(&session);
    shell::ViewportBinding binding;
    panels::ViewportFeed feed(host, kPanelId, &binding);
    panelstest::Wired wired =
        wire_picking_fixture(session, tree, feed, model_for(kPickEntityA, 0x77, "Picked"));
    files::FilesPanel files_panel(&session.file_selection_gateway());

    // The Inspector's real trigger (builtin_panels.cpp): a selection listener on the Scene tree.
    // Firing it is what "propagates through the fact those panels already consume" MEANS here — no
    // new channel, the SAME listener a tree-row click would fire.
    std::size_t listener_calls = 0;
    tree.add_selection_listener([&listener_calls](const scenetree::SceneSelection&)
                                { ++listener_calls; });

    const render::RenderSnapshot snapshot = snapshot_with(kPickEntityA, /*x*/ 0.0f);
    const bool wrote = feed.pick("builtin.viewport#1", render::RegionPoint{50, 50},
                                 render::Extent2D{100, 100}, snapshot);
    CHECK(wrote);

    // The write landed as the SAME `editor.select` the Scene tree's own gateway always uses — no new
    // verb, no explicit `subject` (the daemon's entity default, D7/D8's own point).
    const std::vector<clientmock::Request> sent = wired.channel->requests_for("editor.select");
    CHECK(sent.size() == 1);
    if (sent.size() == 1)
    {
        CHECK(!sent[0].params.contains("subject"));
        CHECK(sent[0].params.at("ids").size() == 1);
        CHECK(sent[0].params.at("ids").at(0).as_string() == panels::pick_selection_id(kPickEntityA));
    }

    // POSITIVE direction: the tree really moved, to a REAL row (non-zero hash), and its listener —
    // the Inspector's own re-point trigger — fired.
    CHECK(tree.selection().identity == panels::pick_selection_id(kPickEntityA));
    CHECK(tree.selection().identity_hash == 0x77u);
    CHECK(listener_calls == 1u);

    // NEGATIVE direction, in the SAME fixture family (D1 subject independence): the Files panel never
    // moved — nothing in this call path ever names the `file` subject.
    CHECK(files_panel.selection().identity.empty());
    CHECK(wired.channel->requests_for("editor.select").size() == 1u); // no SECOND write for files
}

void a_miss_clears_the_selection_as_an_empty_replace()
{
    shell::PanelHost host;
    panels::SessionFeed session(host, "unused.playbar");
    scenetree::SceneTreePanel tree(&session);
    shell::ViewportBinding binding;
    panels::ViewportFeed feed(host, kPanelId, &binding);
    panelstest::Wired wired =
        wire_picking_fixture(session, tree, feed, model_for(kPickEntityA, 0x77, "Picked"));

    // First land a real selection — the positive half — so the clear below is a genuine change and
    // not a no-op that would pass trivially with the raycast dead.
    const render::RenderSnapshot hit_snapshot = snapshot_with(kPickEntityA, 0.0f);
    CHECK(feed.pick("builtin.viewport#1", render::RegionPoint{50, 50}, render::Extent2D{100, 100},
                    hit_snapshot));
    CHECK(tree.selection().identity == panels::pick_selection_id(kPickEntityA));

    // A click at the SAME pixel against an EMPTY scene: the raycast has nothing to hit.
    const render::RenderSnapshot empty_snapshot;
    CHECK(feed.pick("builtin.viewport#1", render::RegionPoint{50, 50}, render::Extent2D{100, 100},
                    empty_snapshot));
    CHECK(tree.selection().identity.empty());

    const std::vector<clientmock::Request> sent = wired.channel->requests_for("editor.select");
    CHECK(sent.size() == 2u);
    if (sent.size() == 2u)
    {
        CHECK(sent[1].params.at("ids").size() == 0u); // "an empty replace" — the task's own phrase
    }
}

void the_pick_honours_the_copys_own_camera_not_a_fixed_transform()
{
    // Two entities, offset on X exactly like the two cameras below — a click at the SAME pixel must
    // resolve to a DIFFERENT entity purely because the copy's camera moved.
    scenetree::SceneTreeModel model = model_for(kPickEntityA, 0x77, "A");
    scenetree::SceneTreeNode node_b;
    node_b.identity = panels::pick_selection_id(kPickEntityB);
    node_b.identity_hash = 0x99;
    node_b.display_name = "B";
    model.roots.push_back(node_b);
    model.entity_count = 2;

    shell::PanelHost host;
    panels::SessionFeed session(host, "unused.playbar");
    scenetree::SceneTreePanel tree(&session);
    shell::ViewportBinding binding;
    panels::ViewportFeed feed(host, kPanelId, &binding);
    // Kept alive for the rest of this test even though it is never inspected: `session` stores a
    // raw, non-owning `client::Client*` into it (bind_client), so letting the return value expire
    // here would dangle that pointer the moment `feed.pick()` below drives a write through it.
    const panelstest::Wired wired = wire_picking_fixture(session, tree, feed, model);
    (void)wired;

    render::RenderSnapshot snapshot;
    render::RenderItem item_a;
    item_a.entity = kPickEntityA;
    item_a.transform.position[0] = 0.0f;
    item_a.transform.position[1] = 3.0f;
    item_a.transform.position[2] = -2.0f;
    item_a.transform.scale[0] = 2.0f;
    item_a.transform.scale[1] = 2.0f;
    item_a.transform.scale[2] = 2.0f;
    render::RenderItem item_b = item_a;
    item_b.entity = kPickEntityB;
    item_b.transform.position[0] = 10.0f;
    snapshot.items.push_back(item_a);
    snapshot.items.push_back(item_b);

    // The copy's camera is STILL the Scene default — (0, 3, 8), looking down -Z with no lateral
    // offset — so the SAME pixel resolves to entity A, sitting on the camera's own x = 0 line.
    CHECK(feed.pick("builtin.viewport#1", render::RegionPoint{50, 50}, render::Extent2D{100, 100},
                    snapshot));
    CHECK(tree.selection().identity == panels::pick_selection_id(kPickEntityA));

    // Move the SAME copy's camera to x = 10 (still looking down -Z, per view.h's own convention) —
    // nothing else about the click changes. The pick now rides entity B's line instead.
    render::View moved = binding.camera("builtin.viewport#1");
    moved.transform.position[0] = 10.0f;
    CHECK(binding.set_camera("builtin.viewport#1", moved));

    CHECK(feed.pick("builtin.viewport#1", render::RegionPoint{50, 50}, render::Extent2D{100, 100},
                    snapshot));
    CHECK(tree.selection().identity == panels::pick_selection_id(kPickEntityB));
}

void picking_with_no_scene_tree_bound_writes_nothing()
{
    // The honest no-op every other write seam in this bag reports with nothing to drive.
    shell::PanelHost host;
    shell::ViewportBinding binding;
    panels::ViewportFeed feed(host, kPanelId, &binding);
    const render::RenderSnapshot snapshot = snapshot_with(kPickEntityA, 0.0f);
    CHECK(!feed.pick("builtin.viewport#1", render::RegionPoint{50, 50}, render::Extent2D{100, 100},
                     snapshot));
}

void picking_selection_ids_are_stable_and_distinguish_entities()
{
    // `pick_selection_id` moved here from context::render (picking.h) — it turns a picked entity
    // into the `editor.select` wire id, which is an editor/wire concern, not a raycast one; see
    // `camera_set_params` (viewport_binding.h) for the SAME layering already established for a
    // render value -> wire-params function. This is the property test that moved with it.
    const std::string id_a = panels::pick_selection_id(kPickEntityA);
    const std::string id_b = panels::pick_selection_id(kPickEntityB);
    CHECK(!id_a.empty());
    CHECK(id_a != id_b);
    CHECK(panels::pick_selection_id(kPickEntityA) == id_a); // stable across calls
}

void the_factory_gives_every_copy_its_own_model()
{
    shell::PanelHost host;
    shell::ViewportBinding binding;
    panels::ViewportFeed feed(host, kPanelId, &binding);
    CHECK(host.provide_factory(kPanelId, feed.make_factory()));
    // THE PROBE materialised nothing: the host calls the factory once at bind time with an EMPTY
    // instance id and discards the provider, and a model minted there would be a phantom copy.
    CHECK(feed.instances() == 0u);

    const shell::InstanceOpen a = host.open_instance(kPanelId);
    const shell::InstanceOpen b = host.open_instance(kPanelId);
    // `unlimited` (builtin_roster.cpp): the second open MINTS rather than focusing.
    CHECK(a.outcome == shell::InstanceOutcome::opened);
    CHECK(b.outcome == shell::InstanceOutcome::opened);
    CHECK(a.instance_id != b.instance_id);

    std::string code;
    const std::optional<shell::PanelRender> render_a = host.render(kPanelId, code, a.instance_id);
    const std::optional<shell::PanelRender> render_b = host.render(kPanelId, code, b.instance_id);
    CHECK(render_a.has_value() && render_b.has_value());
    CHECK(feed.instances() == 2u);
    if (render_a.has_value() && render_b.has_value())
    {
        CHECK(render_a->instance_id == a.instance_id);
        CHECK(render_b->instance_id == b.instance_id);
    }

    // TWO MODELS, NOT ONE SHARED. Framing copy A advances A's view generation and leaves B's alone —
    // which is precisely what a `provide()` binding could not do, and the reason the viewport is the
    // one panel bound through `provide_factory`.
    viewport::ViewportPanel* model_a = feed.model(a.instance_id);
    viewport::ViewportPanel* model_b = feed.model(b.instance_id);
    CHECK(model_a != nullptr && model_b != nullptr);
    CHECK(model_a != model_b);
    if (model_a == nullptr || model_b == nullptr)
    {
        return;
    }
    // Move copy A's camera off the default first. Framing a camera that is ALREADY at the framing
    // pose is correctly not a write (`set_camera` refuses an unmoved camera), so a fresh copy would
    // make the dirty-queue assertion below pass for the wrong reason.
    render::View a_camera = binding.camera(a.instance_id);
    a_camera.transform.position[0] = 9.0f;
    CHECK(binding.set_camera(a.instance_id, a_camera));
    CHECK(feed.take_camera_writes().size() == 1u);
    CHECK(binding.dirty_count() == 0u);

    const std::uint64_t b_before = model_b->view_generation();
    bool dispatched = false;
    CHECK(host.invoke(kPanelId, viewport::kFrameSceneCommand, Json::object(), dispatched, code,
                      a.instance_id));
    CHECK(dispatched);
    CHECK(model_a->view_generation() > 0u);
    CHECK(model_b->view_generation() == b_before);

    // And the CAMERAS diverged with them: A owes the daemon a write, B does not.
    CHECK(binding.dirty_count() == 1u);
    const std::vector<Json> writes = feed.take_camera_writes();
    CHECK(writes.size() == 1u);
    if (writes.size() == 1u)
    {
        CHECK(writes[0].at("viewportId").as_string() == a.instance_id);
    }

    // THE VIEWPORT PERSISTS NO PANEL STATE, deliberately (viewport_feed.h): the camera lives where
    // cameras already live. A D6 blob carrying it would be a second source of truth.
    std::string state_code;
    CHECK(!host.get_state(kPanelId, state_code, a.instance_id).has_value());
}

void frame_scene_moves_the_camera_and_the_round_trip_restores_it()
{
    shell::PanelHost host;
    shell::ViewportBinding binding;
    panels::ViewportFeed feed(host, kPanelId, &binding);
    CHECK(host.provide_factory(kPanelId, feed.make_factory()));
    const shell::InstanceOpen a = host.open_instance(kPanelId);
    CHECK(a.outcome == shell::InstanceOutcome::opened);
    CHECK(feed.model(a.instance_id) != nullptr);

    // Put the copy's camera somewhere the human chose, in a 2D projection they chose.
    render::View authored = binding.camera(a.instance_id);
    authored.transform.position[0] = -7.5f;
    authored.transform.position[1] = 1.25f;
    authored.transform.position[2] = 4.0f;
    authored.mode = render::ViewMode::two_d;
    authored.projection.ortho_half_height = 12.0f;
    CHECK(binding.set_camera(a.instance_id, authored));

    // FRAME SCENE returns the PLACEMENT to the default pose and keeps the PROJECTION — the rule
    // `framed_scene_view` states, asserted on the shipping path rather than on the helper alone.
    bool dispatched = false;
    std::string code;
    CHECK(host.invoke(kPanelId, viewport::kFrameSceneCommand, Json::object(), dispatched, code,
                      a.instance_id));
    CHECK(dispatched);
    CHECK(feed.frame_scene_requests() == 1u);
    const render::View framed = binding.camera(a.instance_id);
    CHECK(framed.transform.position[0] == 0.0f);
    CHECK(framed.transform.position[2] != authored.transform.position[2]);
    CHECK(framed.mode == render::ViewMode::two_d);
    CHECK(framed.projection.ortho_half_height == 12.0f);

    // THE ROUND TRIP. The writes the pump owes the daemon, fed back as the reply `editor.cameras-get`
    // would serve, must restore the same camera — the payload is OPAQUE to the daemon, so this is the
    // only place its meaning is checked end to end.
    const std::vector<Json> writes = feed.take_camera_writes();
    CHECK(writes.size() == 1u);
    CHECK(binding.dirty_count() == 0u);

    // A FRESH binding + feed: a restart, or a second window opening the same arrangement.
    shell::PanelHost host2;
    shell::ViewportBinding binding2;
    panels::ViewportFeed feed2(host2, kPanelId, &binding2);
    CHECK(host2.provide_factory(kPanelId, feed2.make_factory()));
    CHECK(feed2.fetch_due());
    feed2.mark_fetched();
    CHECK(!feed2.fetch_due());
    CHECK(feed2.apply_cameras_result(cameras_reply(writes)) == 1u);
    CHECK(feed2.cameras_applied() == 1u);

    const render::View* restored = binding2.find_camera(a.instance_id);
    CHECK(restored != nullptr);
    if (restored != nullptr)
    {
        CHECK(restored->transform.position[0] == framed.transform.position[0]);
        CHECK(restored->transform.position[1] == framed.transform.position[1]);
        CHECK(restored->transform.position[2] == framed.transform.position[2]);
        CHECK(restored->mode == render::ViewMode::two_d);
        CHECK(restored->projection.ortho_half_height == 12.0f);
    }
    // HYDRATION IS NOT AN ECHO: the restored camera owes the daemon nothing. Without this a boot
    // would write every camera straight back out, and two windows would ping-pong forever.
    CHECK(binding2.dirty_count() == 0u);
    CHECK(feed2.take_camera_writes().empty());
}

void a_new_copy_re_arms_the_hydration_read()
{
    // A copy that opens later may be one the daemon already holds a camera for (a restored
    // arrangement, a rehomed panel), so the read must re-arm rather than assuming boot covered it.
    shell::PanelHost host;
    shell::ViewportBinding binding;
    panels::ViewportFeed feed(host, kPanelId, &binding);
    CHECK(host.provide_factory(kPanelId, feed.make_factory()));
    feed.mark_fetched();
    CHECK(!feed.fetch_due());

    const shell::InstanceOpen a = host.open_instance(kPanelId);
    CHECK(a.outcome == shell::InstanceOutcome::opened);
    std::string code;
    CHECK(host.render(kPanelId, code, a.instance_id).has_value());
    CHECK(feed.fetch_due());
}

void a_failed_write_is_re_armed()
{
    shell::PanelHost host;
    shell::ViewportBinding binding;
    panels::ViewportFeed feed(host, kPanelId, &binding);
    CHECK(host.provide_factory(kPanelId, feed.make_factory()));
    const shell::InstanceOpen a = host.open_instance(kPanelId);
    std::string code;
    CHECK(host.render(kPanelId, code, a.instance_id).has_value());

    render::View moved = binding.camera(a.instance_id);
    moved.transform.position[0] = 42.0f;
    CHECK(binding.set_camera(a.instance_id, moved));
    const std::vector<Json> writes = feed.take_camera_writes();
    CHECK(writes.size() == 1u);
    // The write "failed" at the pump. Re-armed WITHOUT changing what is being retried — the camera
    // the human moved is still exactly 42.
    feed.rearm_camera_write(a.instance_id);
    const std::vector<Json> retry = feed.take_camera_writes();
    CHECK(retry.size() == 1u);
    if (retry.size() == 1u)
    {
        CHECK(retry[0].at("viewportId").as_string() == a.instance_id);
        CHECK(retry[0].at("transform").at("position").at(0).as_number() == 42.0);
    }
    // An unknown id re-arms nothing rather than inventing a camera for a copy that never existed.
    feed.rearm_camera_write("builtin.viewport#99");
    CHECK(feed.take_camera_writes().empty());
}

void no_adapter_renders_the_summary_and_reports_the_code()
{
    // The DoD's degraded path: with no adapter the summary model is what the human reads, and
    // `viewport.adapter_absent` is reported in it (R-HEAD-002 — absence is REPORTED).
    shell::PanelHost host;
    shell::ViewportBinding binding; // no device attached
    CHECK(!binding.adapter_available());
    panels::ViewportFeed feed(host, kPanelId, &binding);
    // The feed took the verdict from the binding at construction rather than defaulting optimistically.
    CHECK(!feed.adapter_available());
    CHECK(host.provide_factory(kPanelId, feed.make_factory()));
    const shell::InstanceOpen a = host.open_instance(kPanelId);

    std::string code;
    const std::optional<shell::PanelRender> degraded = host.render(kPanelId, code, a.instance_id);
    CHECK(degraded.has_value());
    if (degraded.has_value())
    {
        CHECK(panelstest::mentions(degraded->html, "viewport.adapter_absent"));
        // The panel is still a REAL panel while degraded: it renders its frame-scene affordance, so
        // the human is not locked out of the one camera action there is.
        CHECK(panelstest::mentions(degraded->html, viewport::kFrameSceneCommand));
    }

    // WITH an adapter the code is gone — the negative half, without which "the html mentions the
    // code" would pass for a panel that always mentions it.
    feed.set_adapter_available(true);
    const std::optional<shell::PanelRender> ok = host.render(kPanelId, code, a.instance_id);
    CHECK(ok.has_value());
    if (ok.has_value())
    {
        CHECK(!panelstest::mentions(ok->html, "viewport.adapter_absent"));
    }
    // And the two renders really are different trees, so the assertion pair is not comparing one
    // string with itself.
    CHECK(degraded.has_value() && ok.has_value() && degraded->html != ok->html);
}

void the_reported_size_follows_the_composited_layer()
{
    // THE REPORT IS THE PRODUCT HERE, not a debug aid: `width`/`height` are fields of the R-HEAD-002
    // present report the panel renders, and the human reads them in the summary line.
    //
    // The ORDERING is the whole bug. A copy's model is materialised by the RENDERER asking for it,
    // which necessarily happens before the producer has ever published a layer for that copy — so
    // the size baked in at materialisation is 0x0 for every viewport that ever exists. Nothing
    // re-read it, so `0x0` was what the panel reported for the life of the window while a 354x260
    // layer was being composited (measured on the live editor).
    //
    // NO DEVICE IS NEEDED to drive this: `ViewportBinding::publish` fills `content_rect` /
    // `content_size` from the REGION and only the `content` view needs an adapter, so the layer this
    // asserts on is the real one the real code path produces.
    shell::PanelHost host;
    // A producer WITH an adapter, because the size is only part of a PRESENT report that has one:
    // `compute_present`'s first rule answers `viewport.adapter_absent` and reports no dimensions at
    // all, so an adapter-less fixture would assert 0x0 against 0x0 and pass with the fix removed.
    rendertest::FakeDevice device;
    shell::ViewportBinding binding;
    binding.attach_device(device);
    CHECK(binding.adapter_available());
    shell::WindowCompositor compositor{shell::CompositorConfig{}};
    panels::ViewportFeed feed(host, kPanelId, &binding);
    CHECK(host.provide_factory(kPanelId, feed.make_factory()));
    const shell::InstanceOpen a = host.open_instance(kPanelId);

    // Materialise the model the way the renderer does, BEFORE any layer exists — the ordering the
    // bug lived in. It must report the honest 0x0 here, which is also what makes the assertion after
    // the publish a real change rather than a value that was always right.
    std::string code;
    CHECK(host.render(kPanelId, code, a.instance_id).has_value());
    const viewport::ViewportPanel* model = feed.model(a.instance_id);
    CHECK(model != nullptr);
    if (model == nullptr)
    {
        return;
    }
    CHECK(model->present().width == 0u);
    CHECK(model->present().height == 0u);

    shell::ShellRegion region;
    region.id = a.instance_id;
    region.kind = shell::RegionKind::viewport;
    region.rect = render::Rect2D{render::Origin2D{48, 96}, render::Extent2D{354u, 260u}};
    const render::RenderSnapshot snapshot;
    const shell::ViewportPublishStats stats = binding.publish({region}, snapshot, compositor);
    CHECK(stats.layers == 1u);

    // The pump's ONE per-frame call into the feed, with the producer UNCHANGED — the path the live
    // editor takes on every iteration, and the one that used to re-read nothing.
    feed.bind_binding(&binding);
    CHECK(model->present().width == 354u);
    CHECK(model->present().height == 260u);

    // …and it FOLLOWS: a resized dock publishes a new rect under the same producer. Without this the
    // case would pass for a fix that read the size exactly once more.
    region.rect = render::Rect2D{render::Origin2D{48, 96}, render::Extent2D{512u, 300u}};
    (void)binding.publish({region}, snapshot, compositor);
    feed.bind_binding(&binding);
    CHECK(model->present().width == 512u);
    CHECK(model->present().height == 300u);

    // A copy whose region went away has no layer any more, and the report goes back to the honest
    // 0x0 rather than freezing at the last size it happened to see.
    (void)binding.publish({}, snapshot, compositor);
    feed.bind_binding(&binding);
    CHECK(model->present().width == 0u);
    CHECK(model->present().height == 0u);
}

void the_framing_helper_keeps_the_projection()
{
    render::View current;
    current.transform.position[0] = 100.0f;
    current.transform.rotation[0] = 0.5f;
    current.mode = render::ViewMode::two_d;
    current.type = render::ViewType::game;
    current.projection.fov_y_radians = 1.0f;
    current.projection.near_z = 0.01f;
    current.viewport_id = 7;

    const render::View framed = panels::framed_scene_view(current);
    CHECK(framed.transform.position[0] == 0.0f);
    CHECK(framed.transform.rotation[0] == 0.0f);
    CHECK(framed.transform.rotation[3] == 1.0f);
    // Everything the human chose survives — including the render SLOT, which is not the camera's to
    // change (view.h's three-ids warning).
    CHECK(framed.mode == render::ViewMode::two_d);
    CHECK(framed.type == render::ViewType::game);
    CHECK(framed.projection.fov_y_radians == 1.0f);
    CHECK(framed.projection.near_z == 0.01f);
    CHECK(framed.viewport_id == 7u);
}

void a_feed_with_no_binding_still_renders()
{
    // A harness / a build with no window: the models render and nothing is pushed, rather than the
    // panel refusing to exist.
    shell::PanelHost host;
    panels::ViewportFeed feed(host, kPanelId, nullptr);
    CHECK(host.provide_factory(kPanelId, feed.make_factory()));
    const shell::InstanceOpen a = host.open_instance(kPanelId);
    std::string code;
    CHECK(host.render(kPanelId, code, a.instance_id).has_value());
    bool dispatched = false;
    CHECK(host.invoke(kPanelId, viewport::kFrameSceneCommand, Json::object(), dispatched, code,
                      a.instance_id));
    CHECK(dispatched);
    CHECK(feed.take_camera_writes().empty());
    CHECK(feed.apply_cameras_result(Json::object()) == 0u);
}

} // namespace

int main()
{
    the_factory_gives_every_copy_its_own_model();
    frame_scene_moves_the_camera_and_the_round_trip_restores_it();
    a_new_copy_re_arms_the_hydration_read();
    a_failed_write_is_re_armed();
    no_adapter_renders_the_summary_and_reports_the_code();
    the_reported_size_follows_the_composited_layer();
    the_framing_helper_keeps_the_projection();
    a_feed_with_no_binding_still_renders();
    a_hit_moves_the_scene_tree_and_leaves_files_untouched();
    a_miss_clears_the_selection_as_an_empty_replace();
    the_pick_honours_the_copys_own_camera_not_a_fixed_transform();
    picking_with_no_scene_tree_bound_writes_nothing();
    picking_selection_ids_are_stable_and_distinguish_entities();
    PANELS_TEST_MAIN_END();
}
