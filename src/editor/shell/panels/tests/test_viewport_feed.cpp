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

#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/viewport_binding.h"

#include "panels_test.h"

#include <optional>
#include <string>
#include <vector>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace viewport = context::editor::gui::viewport;
namespace render = context::render;
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
    the_framing_helper_keeps_the_projection();
    a_feed_with_no_binding_still_renders();
    PANELS_TEST_MAIN_END();
}
