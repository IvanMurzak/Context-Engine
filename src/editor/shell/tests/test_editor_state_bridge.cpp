// T1 for the editor-state + region-map bridge (M9 e05d2): the layout/panels round-trip and its three
// persistence triggers, region-map parsing and emission into the arbiter, the untrusted-input degrade
// paths, and the full JSON-RPC binding over a real BridgeRouter.
//
// WHAT THIS PROVES AND WHAT IT DELEGATES. The Shell being the SINGLE WRITER of the editor-state file
// is asserted STRUCTURALLY, not here: this bridge has no path to the file, only to the store's
// opaque-blob setters (a compile-time fact — grep editor_state_bridge.* for `ofstream`/`rename` finds
// nothing). What this file proves is the wire behaviour: a published blob reaches the store, comes
// back byte-identical through `editor.state.get`, survives the store's debounce/flush/atomic-write,
// and a published region set reaches a live InputArbiter and routes a pointer. The per-panel D6
// schemaVersion-mismatch degrade lives in test_panel_host.cpp (PanelHost::restore_state), which this
// bridge does not touch — editor-core assembles the `panels` blob FROM panel.state.get and applies it
// back via panel.state.set, so the mismatch path is that surface's, exercised there. The LIVE TS
// round-trip (boot -> dock -> restore) is the e05d4 CEF smoke's; there is no local TS/DOM tier.

#include "context/editor/shell/editor_state_bridge.h"

#include "context/editor/shell/editor_state.h"
#include "context/editor/shell/input.h"
#include "context/editor/shell/ipc_bridge.h"

#include "shell_test.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace context::editor::shell;
namespace fs = std::filesystem;
using Json = context::editor::contract::Json;

namespace
{

// A layout blob shaped like Dockview's toJSON: an opaque object the Shell never interprets.
Json make_layout(const std::string& tag)
{
    Json grid = Json::object();
    grid.set("orientation", Json("HORIZONTAL"));
    grid.set("tag", Json(tag));
    Json layout = Json::object();
    layout.set("grid", grid);
    layout.set("activeGroup", Json("group-1"));
    return layout;
}

// A panels blob: {panelId -> {schemaVersion, data}} — assembled by editor-core from panel.state.get.
Json make_panels()
{
    Json problems = Json::object();
    problems.set("schemaVersion", Json(1));
    Json data = Json::object();
    data.set("collapsed", Json(true));
    problems.set("data", data);
    Json panels = Json::object();
    panels.set("builtin.problems", problems);
    return panels;
}

Json region(const std::string& id, const char* kind, std::int64_t x, std::int64_t y,
            std::int64_t w, std::int64_t h)
{
    Json rect = Json::object();
    rect.set("x", Json(x));
    rect.set("y", Json(y));
    rect.set("width", Json(w));
    rect.set("height", Json(h));
    Json out = Json::object();
    out.set("id", Json(id));
    out.set("kind", Json(kind));
    out.set("rect", rect);
    return out;
}

// Dispatch one JSON-RPC request over a router and return the parsed `result` (or a null Json + set
// `refused` when it was an error envelope).
Json dispatch_result(BridgeRouter& router, const std::string& method, const Json& params,
                     bool& refused, std::string& reason)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(7));
    request.set("method", Json(method));
    request.set("params", params);
    const BridgeDispatch dispatch = router.dispatch(request.dump());
    const Json response = Json::parse(dispatch.response);
    // "refused" here means the CALL returned an error — either an envelope-level refusal
    // (dispatch.reject != none) or a handler that returned BridgeResult::error (a valid envelope with
    // an `error` member). Both surface as an `error` member in the response, so read that.
    refused = response.contains("error");
    if (refused)
    {
        reason = response.at("error").at("data").at("reason").as_string();
        return Json();
    }
    reason.clear();
    return response.at("result");
}

// --- the region vocabulary -----------------------------------------------------------------------

void region_kind_tokens_round_trip_and_reject_the_unknown()
{
    CHECK(std::string(region_kind_token(RegionKind::viewport)) == "viewport");
    CHECK(std::string(region_kind_token(RegionKind::native)) == "native");
    CHECK(parse_region_kind("viewport") == std::optional<RegionKind>(RegionKind::viewport));
    CHECK(parse_region_kind("native") == std::optional<RegionKind>(RegionKind::native));
    // A token no side of the closed set names is REFUSED, not defaulted into viewport — a renderer
    // that means a kind the Shell does not know cannot have its input routed correctly.
    CHECK(parse_region_kind("gizmo") == std::nullopt);
    CHECK(parse_region_kind("") == std::nullopt);
}

void parse_shell_region_reads_a_well_formed_element_and_clamps_negatives()
{
    ShellRegion out;
    CHECK(parse_shell_region(region("view.scene", "viewport", 100, 60, 640, 480), out));
    CHECK(out.id == "view.scene");
    CHECK(out.kind == RegionKind::viewport);
    CHECK(out.rect.origin.x == 100u);
    CHECK(out.rect.origin.y == 60u);
    CHECK(out.rect.size.width == 640u);
    CHECK(out.rect.size.height == 480u);

    // A negative pixel in a hostile / corrupted payload clamps to 0 rather than wrapping to a huge
    // unsigned rect that would swallow the whole window on hit-test.
    ShellRegion clamped;
    CHECK(parse_shell_region(region("view.neg", "native", -50, -10, 200, 100), clamped));
    CHECK(clamped.rect.origin.x == 0u);
    CHECK(clamped.rect.origin.y == 0u);
}

void parse_shell_region_rejects_malformed_elements()
{
    ShellRegion out;
    CHECK(!parse_shell_region(Json("not-an-object"), out));
    // No id.
    CHECK(!parse_shell_region(region("", "viewport", 0, 0, 10, 10), out));
    // Unknown kind.
    CHECK(!parse_shell_region(region("v", "gizmo", 0, 0, 10, 10), out));

    // No rect.
    Json no_rect = Json::object();
    no_rect.set("id", Json("v"));
    no_rect.set("kind", Json("viewport"));
    CHECK(!parse_shell_region(no_rect, out));
}

// --- the restore path (editor.state.get) ---------------------------------------------------------

void snapshot_is_an_empty_document_before_the_store_is_bound()
{
    EditorStateBridge bridge;
    const Json snap = bridge.snapshot();
    CHECK(snap.at("layout").is_object());
    CHECK(snap.at("panels").is_object());
    CHECK(snap.at("layout").size() == 0u);
}

void snapshot_returns_the_persisted_blob_verbatim()
{
    const fs::path root = shelltest::make_temp_project("context-e05d2", "snapshot");
    EditorStateStore store(root, 0);
    store.load();
    store.set_layout(make_layout("saved"), 0);
    store.set_panels(make_panels(), 0);

    EditorStateBridge bridge;
    bridge.bind_store(&store, [] { return std::uint64_t{0}; });

    const Json snap = bridge.snapshot();
    CHECK(snap.at("layout").at("grid").at("tag").as_string() == "saved");
    CHECK(snap.at("panels").at("builtin.problems").at("schemaVersion").as_int() == 1);
    shelltest::cleanup(root);
}

// --- the persistence path (editor.state.publish) -------------------------------------------------

void a_published_blob_round_trips_through_the_store_and_a_restart()
{
    const fs::path root = shelltest::make_temp_project("context-e05d2", "roundtrip");
    std::uint64_t clock_now = 0;
    {
        EditorStateStore store(root, 0); // no debounce: flush is immediate
        EditorStateBridge bridge;
        bridge.bind_store(&store, [&clock_now] { return clock_now; });

        Json params = Json::object();
        params.set("layout", make_layout("published"));
        params.set("panels", make_panels());
        std::string error_code;
        CHECK(bridge.publish_state(params, error_code));
        CHECK(error_code.empty());
        CHECK(bridge.states_published() == 1u);
        CHECK(store.flush_if_due(1));
    }
    // A FRESH store loads the file the publish produced — this is the boot-after-restart restore.
    EditorStateStore reopened(root);
    bool loaded = false;
    reopened.load(&loaded);
    CHECK(loaded);
    EditorStateBridge restore_bridge;
    restore_bridge.bind_store(&reopened, [] { return std::uint64_t{0}; });
    const Json snap = restore_bridge.snapshot();
    CHECK(snap.at("layout").at("grid").at("tag").as_string() == "published");
    CHECK(snap.at("panels").at("builtin.problems").at("data").at("collapsed").as_bool());
    shelltest::cleanup(root);
}

void publish_state_requires_at_least_one_blob_and_a_bound_store()
{
    // Unbound store -> not_ready (the guard against a renderer racing the wiring).
    {
        EditorStateBridge bridge;
        Json params = Json::object();
        params.set("layout", make_layout("x"));
        std::string code;
        CHECK(!bridge.publish_state(params, code));
        CHECK(code == kErrEditorNotReady);
    }
    // Neither layout nor panels -> bad_params (an editor with a docking root always has a toJSON, so
    // an empty publish is a wiring bug, not an empty layout to record over the saved one).
    {
        const fs::path root = shelltest::make_temp_project("context-e05d2", "empty");
        EditorStateStore store(root, 0);
        EditorStateBridge bridge;
        bridge.bind_store(&store, [] { return std::uint64_t{0}; });
        std::string code;
        CHECK(!bridge.publish_state(Json::object(), code));
        CHECK(code == kErrEditorBadParams);
        CHECK(!bridge.publish_state(Json("not-an-object"), code));
        CHECK(code == kErrEditorBadParams);
        CHECK(store.write_count() == 0);
        shelltest::cleanup(root);
    }
}

void the_three_persistence_triggers_are_all_covered()
{
    const fs::path root = shelltest::make_temp_project("context-e05d2", "triggers");
    std::uint64_t clock_now = 0;
    EditorStateStore store(root, 500'000); // a 500 ms quiet period
    EditorStateBridge bridge;
    bridge.bind_store(&store, [&clock_now] { return clock_now; });

    Json params = Json::object();
    params.set("layout", make_layout("t0"));
    std::string code;

    // TRIGGER 1 — DEBOUNCED DURING INTERACTION. A burst of publishes (a sash drag) marks the store
    // dirty but writes only once the quiet period elapses. The bridge coalesces onto the store's
    // existing debounce, so a drag is one file write, not one per frame.
    clock_now = 1'000;
    CHECK(bridge.publish_state(params, code));
    CHECK(store.dirty());
    CHECK(!store.flush_if_due(clock_now)); // too soon
    params.set("layout", make_layout("t1"));
    clock_now = 100'000;
    CHECK(bridge.publish_state(params, code));
    CHECK(!store.flush_if_due(400'000)); // still within the quiet period
    CHECK(store.write_count() == 0);
    CHECK(store.flush_if_due(501'000)); // elapsed (measured from the first dirtying change)
    CHECK(store.write_count() == 1);

    // TRIGGER 3 — CRASH-RESTORE. The debounced write above is a COMPLETE, atomic file: a fresh store
    // loads the last-known-good blob with NO graceful shutdown in between, which is exactly what a
    // non-graceful exit leaves behind.
    {
        EditorStateStore after_crash(root);
        bool loaded = false;
        after_crash.load(&loaded);
        CHECK(loaded);
        CHECK(after_crash.state().layout.at("grid").at("tag").as_string() == "t1");
    }

    // TRIGGER 2 — ON-EXIT. A change made after the last write, then flushed unconditionally (the
    // clean-shutdown path WindowManager::shutdown drives): waiting out a quiet period on the way down
    // would just lose the user's last change.
    params.set("layout", make_layout("final"));
    clock_now = 600'000;
    CHECK(bridge.publish_state(params, code));
    CHECK(store.flush_now());
    CHECK(store.write_count() == 2);
    {
        EditorStateStore reopened(root);
        reopened.load();
        CHECK(reopened.state().layout.at("grid").at("tag").as_string() == "final");
    }
    shelltest::cleanup(root);
}

// --- the region-map path (editor.regions.publish) ------------------------------------------------

void a_published_region_map_reaches_a_live_arbiter_and_routes_a_pointer()
{
    InputArbiter arbiter;
    EditorStateBridge bridge;
    bridge.bind_regions([&arbiter](std::vector<ShellRegion> regions)
                        { arbiter.regions().publish(std::move(regions)); });

    Json params = Json::object();
    Json regions = Json::array();
    regions.push_back(region("view.scene", "viewport", 100, 100, 200, 150));
    regions.push_back(region("overlay.gizmo", "native", 400, 0, 80, 80));
    params.set("regions", regions);

    std::vector<ShellRegion> parsed;
    std::string code;
    CHECK(bridge.publish_regions(params, code, &parsed));
    CHECK(code.empty());
    CHECK(parsed.size() == 2u);
    CHECK(bridge.regions_published() == 1u);

    // The regions are now LIVE in the arbiter's map: a pointer inside the viewport rect routes to the
    // native viewport path instead of the browser — the whole point of publishing them.
    CHECK(arbiter.regions().size() == 2u);
    const ShellRegion* hit = arbiter.regions().hit_test(PointI{150, 150});
    CHECK(hit != nullptr);
    CHECK(hit != nullptr && hit->id == "view.scene");
    const ShellRegion* native = arbiter.regions().hit_test(PointI{420, 40});
    CHECK(native != nullptr && native->kind == RegionKind::native);
    // Outside every rect -> the browser's (no region claims it).
    CHECK(arbiter.regions().hit_test(PointI{10, 10}) == nullptr);
}

void an_empty_publish_clears_the_map_and_a_bad_element_is_skipped_not_fatal()
{
    InputArbiter arbiter;
    // Seed the arbiter so we can prove an empty publish CLEARS it (a removed viewport's stale rect
    // must not outlive its panel).
    std::vector<ShellRegion> seeded;
    ShellRegion seed;
    seed.id = "stale";
    seed.rect.origin = {0, 0};
    seed.rect.size = {50, 50};
    seeded.push_back(seed);
    arbiter.regions().publish(std::move(seeded));
    CHECK(arbiter.regions().size() == 1u);

    EditorStateBridge bridge;
    bridge.bind_regions([&arbiter](std::vector<ShellRegion> regions)
                        { arbiter.regions().publish(std::move(regions)); });

    // Empty array is VALID and clears the map.
    Json empty = Json::object();
    empty.set("regions", Json::array());
    std::string code;
    CHECK(bridge.publish_regions(empty, code));
    CHECK(arbiter.regions().size() == 0u);

    // A mix of one valid and two malformed elements publishes only the valid one — a hostile element
    // cannot deny the rest of the map.
    Json mixed = Json::object();
    Json arr = Json::array();
    arr.push_back(region("good", "viewport", 10, 10, 20, 20));
    arr.push_back(Json("garbage"));
    arr.push_back(region("bad-kind", "wormhole", 0, 0, 5, 5));
    mixed.set("regions", arr);
    std::vector<ShellRegion> parsed;
    CHECK(bridge.publish_regions(mixed, code, &parsed));
    CHECK(parsed.size() == 1u);
    CHECK(parsed.size() == 1u && parsed[0].id == "good");
}

void publish_regions_rejects_a_missing_array_and_an_unbound_sink()
{
    // Unbound sink -> not_ready.
    {
        EditorStateBridge bridge;
        Json params = Json::object();
        params.set("regions", Json::array());
        std::string code;
        CHECK(!bridge.publish_regions(params, code));
        CHECK(code == kErrEditorNotReady);
    }
    // No `regions` array at all -> bad_params (distinct from an empty array, which is valid).
    {
        EditorStateBridge bridge;
        bridge.bind_regions([](std::vector<ShellRegion>) {});
        std::string code;
        CHECK(!bridge.publish_regions(Json::object(), code));
        CHECK(code == kErrEditorBadParams);
        Json wrong = Json::object();
        wrong.set("regions", Json("not-an-array"));
        CHECK(!bridge.publish_regions(wrong, code));
        CHECK(code == kErrEditorBadParams);
    }
}

// --- the full JSON-RPC binding over a real router ------------------------------------------------

void the_methods_bind_and_answer_over_a_real_router()
{
    const fs::path root = shelltest::make_temp_project("context-e05d2", "router");
    EditorStateStore store(root, 0);
    InputArbiter arbiter;
    std::uint64_t clock_now = 0;

    EditorStateBridge bridge;
    bridge.bind_store(&store, [&clock_now] { return clock_now; });
    bridge.bind_regions([&arbiter](std::vector<ShellRegion> regions)
                        { arbiter.regions().publish(std::move(regions)); });

    BridgeRouter router;
    CHECK(bridge.install(router));
    CHECK(router.has_method("editor.state.get"));
    CHECK(router.has_method("editor.state.publish"));
    CHECK(router.has_method("editor.regions.publish"));
    // A second install is a name collision, and the method must SAY so rather than silently no-op.
    BridgeRouter clash;
    CHECK(bridge.install(clash));
    CHECK(!bridge.install(clash));

    bool refused = false;
    std::string reason;

    // publish -> get, over the wire.
    Json publish_params = Json::object();
    publish_params.set("layout", make_layout("wire"));
    publish_params.set("panels", make_panels());
    const Json publish_result = dispatch_result(router, "editor.state.publish", publish_params,
                                                refused, reason);
    CHECK(!refused);
    CHECK(publish_result.at("stored").as_bool());
    CHECK(bridge.states_published() == 1u);

    const Json get_result = dispatch_result(router, "editor.state.get", Json::object(), refused,
                                            reason);
    CHECK(!refused);
    CHECK(get_result.at("layout").at("grid").at("tag").as_string() == "wire");
    CHECK(bridge.state_reads() == 1u);

    // regions, over the wire.
    Json region_params = Json::object();
    Json arr = Json::array();
    arr.push_back(region("view", "viewport", 0, 0, 100, 100));
    region_params.set("regions", arr);
    const Json region_result = dispatch_result(router, "editor.regions.publish", region_params,
                                               refused, reason);
    CHECK(!refused);
    CHECK(region_result.at("regions").as_int() == 1);
    CHECK(arbiter.regions().size() == 1u);

    // A malformed publish refuses with the right reason rather than crashing the router.
    (void)dispatch_result(router, "editor.state.publish", Json::object(), refused, reason);
    CHECK(refused);
    CHECK(reason == kErrEditorBadParams);

    shelltest::cleanup(root);
}

} // namespace

int main()
{
    region_kind_tokens_round_trip_and_reject_the_unknown();
    parse_shell_region_reads_a_well_formed_element_and_clamps_negatives();
    parse_shell_region_rejects_malformed_elements();
    snapshot_is_an_empty_document_before_the_store_is_bound();
    snapshot_returns_the_persisted_blob_verbatim();
    a_published_blob_round_trips_through_the_store_and_a_restart();
    publish_state_requires_at_least_one_blob_and_a_bound_store();
    the_three_persistence_triggers_are_all_covered();
    a_published_region_map_reaches_a_live_arbiter_and_routes_a_pointer();
    an_empty_publish_clears_the_map_and_a_bad_element_is_skipped_not_fatal();
    publish_regions_rejects_a_missing_array_and_an_unbound_sink();
    the_methods_bind_and_answer_over_a_real_router();
    SHELL_TEST_MAIN_END();
}
