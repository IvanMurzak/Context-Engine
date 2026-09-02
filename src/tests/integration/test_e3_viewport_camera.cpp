// T2 for the Scene viewport's CAMERA ROUND TRIP (M9 editor-UX e3, D7): the Shell's opaque
// `editor.camera-set` payload -> the daemon's `EditorSessionState` -> `.editor/session.json` ->
// RESTART -> `editor.cameras-get` -> back into the Shell's `render::View`.
//
// WHY THIS IS A T2 RATHER THAN ANOTHER SHELL UNIT TEST, and it is the only reason: the two halves
// live on OPPOSITE SIDES OF THE D10 BOUNDARY and nothing else may link both. The Shell (and its T1
// suites) cannot reach `context_editorkernel` — the boundary gate FATAL_ERRORs at configure time on
// exactly that — so a Shell-side test can only ever feed the payload back to itself, which proves
// the codec is self-consistent and says nothing about whether the daemon PERSISTS it. This
// executable links both and closes that gap.
//
// The claim being closed is precise: `transform` / `projection` are carried OPAQUELY
// (editor_session_state.h: "the daemon is the custodian of the human's camera, not its
// interpreter"), so the file format has no idea what a camera means. Opacity is only a safe design
// if the blob survives a round trip through canonical JSON and a process restart BYTE-EXACTLY in the
// only sense that matters — the View that comes back is the View that went in.
//
// Registered as a PLAIN `editor-session-` ctest (not a gate family), so the build job's general
// ctest step auto-runs it on all three OS legs and `--preset dev` builds it: no ci.yml `--target`
// bookkeeping, so the "Not Run = RED" tripwire does not apply.

#include "context/editor/editorkernel/editor_session_state.h"
#include "context/editor/shell/viewport_binding.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
namespace shell = context::editor::shell;
namespace ek = context::editor::editorkernel;
namespace render = context::render;
using Json = context::editor::contract::Json;

namespace
{

int g_failures = 0;

void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

fs::path make_temp_project()
{
    std::error_code ec;
    const fs::path root =
        fs::temp_directory_path(ec) / "context-e3-viewport-camera" /
        std::to_string(static_cast<long long>(fs::file_time_type::clock::now()
                                                  .time_since_epoch()
                                                  .count()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

// The camera a human left in a viewport: off every default, in BOTH halves of the payload, and with
// values a float cannot round-trip through a decimal shortest-representation by accident.
render::View authored_camera()
{
    render::View view;
    view.transform.position[0] = -12.375f;
    view.transform.position[1] = 4.5f;
    view.transform.position[2] = 33.25f;
    view.transform.rotation[0] = 0.125f;
    view.transform.rotation[1] = -0.25f;
    view.transform.rotation[2] = 0.5f;
    view.transform.rotation[3] = 0.8125f;
    view.transform.scale[0] = 1.5f;
    view.transform.scale[1] = 2.25f;
    view.transform.scale[2] = 0.75f;
    view.projection.fov_y_radians = 0.7853982f;
    view.projection.ortho_half_height = 17.5f;
    view.projection.near_z = 0.0625f;
    view.projection.far_z = 4096.0f;
    view.mode = render::ViewMode::two_d;
    view.type = render::ViewType::game;
    return view;
}

bool same_view(const render::View& a, const render::View& b)
{
    for (int i = 0; i < 3; ++i)
    {
        if (a.transform.position[i] != b.transform.position[i] ||
            a.transform.scale[i] != b.transform.scale[i])
        {
            return false;
        }
    }
    for (int i = 0; i < 4; ++i)
    {
        if (a.transform.rotation[i] != b.transform.rotation[i])
        {
            return false;
        }
    }
    return a.projection.fov_y_radians == b.projection.fov_y_radians &&
           a.projection.ortho_half_height == b.projection.ortho_half_height &&
           a.projection.near_z == b.projection.near_z &&
           a.projection.far_z == b.projection.far_z && a.mode == b.mode && a.type == b.type;
}

// Feed one `editor.camera-set` params object through the daemon's own handler shape. Spelled here
// rather than driven through `KernelServer` so the test needs no daemon socket: what it must pin is
// the STATE + the FILE, and `kernel_server.cpp`'s handler is a parameter read plus exactly this call
// (its own parity test covers the RPC surface).
void apply_camera_set(ek::EditorSessionState& state, const Json& params)
{
    CHECK(params.at("viewportId").is_string());
    (void)state.set_camera(params.at("viewportId").as_string(), params.at("transform"),
                           params.at("projection"));
}

// ------------------------------------------------------------------------------------ the cases

void a_camera_survives_the_daemon_and_a_restart()
{
    const fs::path root = make_temp_project();

    // 1. THE SHELL SIDE. Two copies of the Scene viewport (c3's `unlimited` mode), each with its own
    //    camera — the instance ids are the region ids the producer publishes and the `viewportId`s
    //    the daemon keys by, which is the same string on purpose (viewport_binding.h § THREE IDS).
    shell::ViewportBinding binding;
    const render::View first = authored_camera();
    render::View second = authored_camera();
    second.transform.position[0] = 99.5f;
    second.mode = render::ViewMode::three_d;
    CHECK(binding.set_camera("builtin.viewport#1", first));
    CHECK(binding.set_camera("builtin.viewport#2", second));
    const std::vector<std::string> dirty = binding.take_dirty();
    CHECK(dirty.size() == 2u);

    // 2. THE WIRE. One `editor.camera-set` per moved camera, exactly as the pump issues them.
    ek::EditorSessionState live;
    for (const std::string& viewport_id : dirty)
    {
        const render::View* view = binding.find_camera(viewport_id);
        CHECK(view != nullptr);
        if (view != nullptr)
        {
            apply_camera_set(live, shell::camera_set_params(viewport_id, *view));
        }
    }
    CHECK(live.cameras().size() == 2u);

    // 3. THE FILE. The daemon is `.editor/session.json`'s single writer.
    std::string error;
    CHECK(ek::persist_session_state(root, live, error));
    CHECK(error.empty());
    CHECK(fs::exists(ek::session_state_path(root)));

    // 4. THE RESTART. A brand-new state object reads the file back — the same path the daemon takes
    //    on boot, quarantine and all.
    ek::EditorSessionState restored;
    const ek::SessionRestoreReport report = ek::restore_session_state(root, restored);
    CHECK(report.outcome == ek::SessionRestoreOutcome::restored); // parsed, never quarantined
    CHECK(restored.cameras().size() == 2u);

    // 5. BACK TO THE SHELL. `editor.cameras-get`'s reply shape is `cameras_json` — the ONE encoding
    //    shared by the wire and the file — and a FRESH binding adopts it.
    Json reply = Json::object();
    reply.set("cameras", ek::cameras_json(restored));
    shell::ViewportBinding rebooted;
    CHECK(rebooted.apply_cameras_result(reply) == 2u);

    // THE CLAIM: the View that comes back is the View that went in, through a payload the daemon
    // never interpreted and a file it round-tripped through canonical JSON.
    const render::View* back_first = rebooted.find_camera("builtin.viewport#1");
    const render::View* back_second = rebooted.find_camera("builtin.viewport#2");
    CHECK(back_first != nullptr && back_second != nullptr);
    if (back_first != nullptr && back_second != nullptr)
    {
        CHECK(same_view(*back_first, first));
        CHECK(same_view(*back_second, second));
        // …and the two did NOT collapse into one another, which is what a `viewportId` the daemon
        // ignored (or a map keyed by the panel KIND rather than the copy) would produce.
        CHECK(!same_view(*back_first, *back_second));
    }

    // HYDRATION IS NOT AN ECHO: a boot that adopted two cameras owes the daemon no writes.
    CHECK(rebooted.dirty_count() == 0u);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void the_daemon_never_interprets_the_payload()
{
    // The OPACITY claim, from the other side: a camera whose blob carries members this daemon build
    // has never heard of must round-trip untouched. That is what makes it safe for the viewport to
    // evolve its payload without a daemon release — and it is only true if nothing daemon-side
    // reshapes the blob on its way through the file.
    const fs::path root = make_temp_project();

    Json transform = shell::camera_transform_json(authored_camera());
    transform.set("lensShift", Json(0.375));      // a member no build reads today
    transform.set("rig", Json("dolly"));          // …and one that is not even a number
    Json projection = shell::camera_projection_json(authored_camera());
    projection.set("anamorphic", Json(2.39));

    ek::EditorSessionState live;
    CHECK(live.set_camera("builtin.viewport#1", transform, projection));
    std::string error;
    CHECK(ek::persist_session_state(root, live, error));

    ek::EditorSessionState restored;
    (void)ek::restore_session_state(root, restored);
    const auto it = restored.cameras().find("builtin.viewport#1");
    CHECK(it != restored.cameras().end());
    if (it != restored.cameras().end())
    {
        CHECK(it->second.transform.at("lensShift").as_number() == 0.375);
        CHECK(it->second.transform.at("rig").as_string() == "dolly");
        CHECK(it->second.projection.at("anamorphic").as_number() == 2.39);
        // And the members this build DOES read are still there beside them.
        CHECK(it->second.transform.at("position").is_array());
        CHECK(it->second.projection.at("mode").as_string() == "2d");
    }

    // The Shell reads its own members back and IGNORES the rest, rather than refusing the camera —
    // the tolerance rule stated in viewport_binding.h, proved against a real persisted document.
    Json reply = Json::object();
    reply.set("cameras", ek::cameras_json(restored));
    shell::ViewportBinding binding;
    CHECK(binding.apply_cameras_result(reply) == 1u);
    const render::View* view = binding.find_camera("builtin.viewport#1");
    CHECK(view != nullptr);
    if (view != nullptr)
    {
        CHECK(same_view(*view, authored_camera()));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

void a_camera_the_daemon_never_heard_of_is_not_invented()
{
    // The negative half. A fresh project has no cameras, so `editor.cameras-get` answers an EMPTY
    // array — and the Shell must adopt NOTHING rather than mint a default camera per viewport it
    // happens to know about. (The default is minted lazily, per copy, by `camera()`; a hydration
    // that fabricated one would overwrite the daemon's absence with a value on the next write.)
    const fs::path root = make_temp_project();
    ek::EditorSessionState fresh;
    const ek::SessionRestoreReport report = ek::restore_session_state(root, fresh);
    CHECK(report.outcome == ek::SessionRestoreOutcome::fresh);
    CHECK(fresh.cameras().empty());

    Json reply = Json::object();
    reply.set("cameras", ek::cameras_json(fresh));
    shell::ViewportBinding binding;
    CHECK(binding.apply_cameras_result(reply) == 0u);
    CHECK(binding.find_camera("builtin.viewport#1") == nullptr);
    CHECK(binding.dirty_count() == 0u);

    std::error_code ec;
    fs::remove_all(root, ec);
}

} // namespace

int main()
{
    a_camera_survives_the_daemon_and_a_restart();
    the_daemon_never_interprets_the_payload();
    a_camera_the_daemon_never_heard_of_is_not_invented();
    if (g_failures == 0)
    {
        std::printf("editor-session-viewport-camera: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
