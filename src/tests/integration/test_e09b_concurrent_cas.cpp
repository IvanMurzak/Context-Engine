// The M9 e09b-2 CONCURRENT-CAS T2 DRILL (design 05 §7-§8, L-20 / L-30 / R-CLI-006) — the parent e09
// Definition-of-Done box 2 at the ENGINE level: the REAL Shell panel composition
// (`install_builtin_panels` -> InspectorFeed + WireOverrideWriteGateway) committing a real gesture
// over the REAL daemon `edit` RPC, with a REAL SECOND CLIENT racing the SAME FILE — asserting BOTH
// L-30 outcomes:
//
//   * REBASE — the racer moved the FILE but not THIS field: the gesture rebases onto the fresh CAS
//     token and lands, and BOTH writes survive.
//   * DROP   — the racer moved THIS FIELD: the gesture is refused LOUDLY (`CommitResult::dropped`
//     with the `cas.mismatch` code and a non-empty diagnostic) and the racer's value is STILL on
//     disk. Nothing was overwritten. That is the user-data-integrity guarantee of this milestone,
//     and it is the one an in-process gateway can never prove.
//
// WHY A CROSS-PROCESS DRILL AND NOT ONLY THE T1 SUITE. The T1 suite
// (`editor-shell-test_wire_override_gateway`) scripts the WIRE, so it proves the gateway's behaviour
// against the frames the daemon is DOCUMENTED to send. Only a live daemon proves the two agree — and
// a CAS race is exactly the thing a mock cannot check, because a mock has no notion of another
// connection actually moving bytes on disk between two of our own calls. The racer here is a SECOND
// `client::Client` on its OWN wire connection (its own daemon-minted client id), writing through the
// same daemon write queue a human on another terminal or a scripted agent would.
//
// It also covers the canonical 05 §8 flow's editor half end to end — `panel.command inspector.edit`
// (stage) -> `panel.gesture commit` -> RPC `edit` -> real bytes on real disk -> the R-CLI-006
// READ-YOUR-WRITES re-read that makes the panel show what it just wrote.
//
// SINCE M9 e09e-2 IT ALSO OWNS THE FAN-OUT HALF (§ 5) — the OTHER end of that same 05 §8 chain,
// where `derivation.settled{gen}` reaches every subscribed client. A SECOND `BuiltinPanels` bag on
// its OWN client, fed by a REAL `SubscriptionConsumer` over the REAL `derivation` topic, refreshes
// from a write it did not make; and — the assertion the task actually turns on — does NOT refresh
// while its own gesture is staged, so the L-30 collision base cannot be silently re-based under the
// human's in-flight edit. Two live CEF windows are still e09e-3's smoke; the cross-CLIENT model
// propagation is proven here, CEF-free, on all three default `build` legs.
//
// SINCE M9 x9 (CE #449) THE PUBLISHER IS LIVE, AND THAT CHANGED THIS FILE IN TWO WAYS.
//   * § 5's settle is now produced by the racer's PLAIN `edit` alone. It used to need a `reconcile`
//     chaser, because a bare `edit` published nothing — so every fan-out assertion here was reached
//     through a path no editor client takes. The chaser is gone (see the note where `force_settle`
//     used to live), which is what makes these sections fail if the publisher regresses.
//   * § 5d is NEW, and the hazard it covers is one x9 CREATED: an applied commit now publishes an
//     event the committing window itself receives, so a window can re-base its OWN gesture with no
//     concurrency involved at all. It also asserts §8's FIRST fact — `files.changed`, which had no
//     producer whatsoever before x9 — over the same real wire.
//
// SINCE M9 x10 (CE #452) IT ALSO OWNS THE SELECTION DOOR (§ 5e). Every section above is about
// `derivation.settled`; § 5e is about the OTHER way a staged gesture used to die — a racer's
// `editor.select` moving the SHARED selection (daemon state since e08b), which reached the Inspector
// through the Scene tree and the composition root's selection listener, past x9's SAME-identity guard.
// This is the only tier that can drive it, because a foreign `selection-changed` needs a SECOND
// daemon-minted `origin` and the live two-window CEF smoke shares one connection (CE #455). The drill
// therefore subscribes to `session` as well, and binds the observer bag's session feed to its own
// client exactly as `editor_main.cpp` does.

#include "context/editor/client/client.h"
#include "context/editor/client/subscription.h" // e09e-2: the REAL `derivation` fan-out consumer
#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/serializer/canonical.h" // the ONE value identity (R-FILE-001)
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/panels/inspector_feed.h"
#include "context/editor/shell/panels/problems_feed.h" // kDerivationSettledEvent (the settle's spelling)
#include "context/editor/shell/panels/scenetree_feed.h" // x10 § 5e: the TREE's rendered selection, to
                                                        // prove the foreign fact really landed there
                                                        // (builtin_panels.h only forward-declares it)
#include "context/editor/shell/panels/wire_override_gateway.h"
#include "context/editor/shell/shell.h" // kShellScope — attach with the REAL Shell's scope request

#include "integration_test.h"
#include "process_util.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace client = context::editor::client;
namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace inspector = context::editor::gui::panels::inspector;
namespace fs = std::filesystem;
using context::editor::contract::Json;

#if !defined(CONTEXT_BINARY)
#error "CONTEXT_BINARY (the built `context` executable path) must be defined by CMake"
#endif

namespace
{
const fs::path kBinary = fs::path(CONTEXT_BINARY);

// The daemon roots its FileStore at the project dir and jails the reconcile crawl to `proj/`
// (daemon_command.cpp), so authored paths are `proj/<file>` — the SAME string is the compose
// resolver's key and the kernel's write path, which is what makes the write land where the read
// looked.
const std::string kRoot = "proj/root.scene.json";
const std::string kIdentity = "aaaaaaaaaaaaaaa1/ccccccccccccccc1";
const std::string kFovPointer = "/components/camera/fov";
const std::string kNearPointer = "/components/camera/near";
// Design 05 §8's FIRST fact rides the core `files` topic (M9 x9 / CE #449). Spelled here rather than
// pulled from `panels::` because no FEED consumes it — the Inspector rides `derivation` — so there is
// no panels-side constant to share, and inventing one would advertise a consumer that does not exist.
const std::string kFilesTopic = "files";

void remove_tree(const fs::path& path)
{
    std::error_code ec;
    fs::remove_all(path, ec); // best-effort
}

// Read the authored file back with a plain stream — REAL DISK, not a seam.
//
// ⚠ SCOPED DELIBERATELY, and every caller must keep it that way. filesync's atomic_write is
// temp + rename, and on Windows a rename over a target that still has an OPEN READ HANDLE fails —
// surfacing as a bare `internal.error` from the NEXT write, which reads exactly like a product defect
// and is not one (conventions.md records the full triage). Returning by value here is what closes the
// handle before the caller's next write.
[[nodiscard]] std::string read_authored(const fs::path& project, const char* name)
{
    std::ifstream in(project / "proj" / name, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A real two-file composition on real disk: root INSTANCES child, so an `outermost` override lands in
// root.scene.json — a file the panel never names. That is the whole point of the composed write mode.
//
// Seeds through `itest::write_file_raw` (the shared L-19 raw-write helper) rather than a local
// ofstream: it creates the parent directories AND reports success, so a failed seed reddens here
// instead of surfacing later as an unexplained empty inspect.
void seed_project(const fs::path& project)
{
    const fs::path authored = project / "proj";
    // TWO entities since M9 x10: § 5e needs a SECOND, REAL id-path for the racer to move the shared
    // selection ONTO, so the deferred selection it produces resolves to something `editor.inspect` can
    // actually answer about. Nothing in § 1-§ 5d addresses `ccccccccccccccc2` (every read is by
    // id-path), so the composed model growing by one entity is inert for them.
    CHECK(itest::write_file_raw(authored / "child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Cam",
         "components": {
           "transform": {"position": [1, 2, 3]},
           "camera": {"fov": 1.0, "near": 0.1, "far": 500.0}
         }},
        {"id": "ccccccccccccccc2", "name": "Cam2",
         "components": {
           "transform": {"position": [4, 5, 6]},
           "camera": {"fov": 2.0, "near": 0.2, "far": 400.0}
         }}
      ]})"));
    CHECK(itest::write_file_raw(authored / "root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [],
      "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "proj/child.scene.json"}]})"));
}

ctest_proc::Process spawn_daemon(const fs::path& project)
{
    ctest_proc::Process daemon =
        ctest_proc::spawn(kBinary.string(), {"daemon", "--project", project.string()});
    if (!ctest_proc::valid(daemon))
        return daemon;
    if (!itest::wait_for_instance(project, itest::scaled_timeout_ms(20000), std::string()))
    {
        ctest_proc::kill(daemon);
        ctest_proc::release(daemon);
        return ctest_proc::Process{};
    }
    return daemon;
}

void reap(ctest_proc::Process& daemon)
{
    if (!ctest_proc::valid(daemon))
        return;
    int code = 0;
    if (!ctest_proc::wait_for(daemon, itest::scaled_timeout_ms(8000), code))
        ctest_proc::kill(daemon);
    ctest_proc::release(daemon);
}

// Attach one client with the REAL Shell's scope request (daemon_lifecycle.cpp's).
std::unique_ptr<client::Client> attach_client(const fs::path& project, std::string& error)
{
    std::unique_ptr<client::Client> c =
        client::Client::connect_to_project(project, itest::scaled_timeout_ms(8000), error);
    if (!c)
        return nullptr;
    client::AttachOptions options;
    options.scope = shell::kShellScope; // "read,write,session"
    if (!c->attach(options, error))
        return nullptr;
    return c;
}

// THE RACER: a composed `edit` on its OWN connection, UNGUARDED (no ifMatch) — exactly a second
// editor / agent / CLI writing while our gesture is in flight.
[[nodiscard]] bool race_write(client::Client& racer, const std::string& pointer,
                              const std::string& value)
{
    Json p = Json::object();
    p.set("rootScene", Json(kRoot));
    p.set("idPath", Json(kIdentity));
    p.set("pointer", Json(pointer));
    p.set("value", Json(value));
    std::string error;
    const std::optional<Json> reply = racer.call("edit", std::move(p), error);
    if (!reply.has_value())
        std::fprintf(stderr, "racer edit refused: %s (%s)\n", racer.last_error_code().c_str(),
                     error.c_str());
    return reply.has_value();
}

// THE RACER MOVING THE SHARED SELECTION (M9 x10, CE #452) — `editor.select` on its OWN connection,
// which is exactly what a second editor window, a CLI or an AI agent does. Selection has been DAEMON
// state since e08b, so this is a WRITE to state our observer bag renders, and the daemon publishes a
// `selection-changed` fact stamped with the RACER's client id — foreign to the observer, so it survives
// echo suppression (session_feed.cpp) instead of being dropped as our own echo.
[[nodiscard]] bool race_select(client::Client& racer, const std::string& identity)
{
    Json p = Json::object();
    Json ids = Json::array();
    ids.push_back(Json(identity));
    p.set("ids", std::move(ids));
    std::string error;
    const std::optional<Json> reply = racer.call("editor.select", std::move(p), error);
    if (!reply.has_value())
        std::fprintf(stderr, "racer select refused: %s (%s)\n", racer.last_error_code().c_str(),
                     error.c_str());
    return reply.has_value();
}

// Hydrate the Inspector through the REAL live-read path: the pump issues `editor.inspect` and the
// feed adopts BOTH the model and the root scene's raw-byte CAS token.
void hydrate_inspector(panels::BuiltinPanels& builtin, client::Client& c)
{
    builtin.inspector->request(kIdentity);
    panels::pump_panel_feeds(builtin, c, kRoot);
}

// The current composed value of a field, as the panel model holds it after a hydration.
[[nodiscard]] std::string field_value(const inspector::InspectorPanel& panel,
                                      const std::string& pointer, bool& overridden)
{
    overridden = false;
    for (const inspector::InspectorField& f : panel.model().fields)
    {
        if (f.pointer == pointer)
        {
            overridden = f.overridden;
            std::string out;
            if (!context::editor::serializer::serialize_canonical(f.value, out))
                return {};
            if (!out.empty() && out.back() == '\n')
                out.pop_back();
            return out;
        }
    }
    return {};
}

// Stage a value on the field through the REAL `panel.command` seam the renderer uses, WITHOUT ending
// the gesture — the L-20 in-flight state (no write yet). Separate from the commit below because
// several scenarios need something to happen INSIDE that window, which is the only window L-30 is
// about.
void stage_only(shell::PanelHost& host, const std::string& pointer, const std::string& value)
{
    Json edit = Json::object();
    edit.set("nodeId", Json(std::string(panels::kInspectorWidgetPrefix) + pointer));
    edit.set("value", Json(value)); // a JSON literal in a string (its canonical serialization)
    bool dispatched = false;
    std::string code;
    CHECK(host.invoke(inspector::InspectorPanel::kContributionId,
                      inspector::InspectorPanel::kEditCommand, edit, dispatched, code));
    CHECK(dispatched);
}

// End the gesture through the REAL `panel.gesture` seam (L-20: gesture end IS the commit). The
// `dispatched` CHECK is load-bearing rather than ceremonial: a provider that found NOTHING staged
// answers `Status::none` and reports `dispatched:false`, which is exactly what a lost gesture looks
// like from here.
void commit_gesture(shell::PanelHost& host)
{
    bool dispatched = false;
    std::string code;
    CHECK(host.gesture(inspector::InspectorPanel::kContributionId, shell::GestureVerb::commit,
                       Json::object(), dispatched, code));
    CHECK(dispatched);
}

// Stage then immediately commit — the uninterrupted gesture. Returns the panel's resolved outcome.
[[nodiscard]] inspector::CommitResult stage_and_commit(shell::PanelHost& host,
                                                       panels::BuiltinPanels& builtin,
                                                       const std::string& pointer,
                                                       const std::string& value)
{
    stage_only(host, pointer, value);
    CHECK(builtin.inspector->panel().has_staged_edit());
    commit_gesture(host);
    return builtin.inspector->panel().last_result();
}

// ⚠ THERE IS DELIBERATELY NO `force_settle` HELPER HERE ANY MORE (M9 x9, CE #449). This drill used to
// carry one, calling `reconcile` after every `race_write`, because a bare `edit` published NOTHING: it
// ran only the read-your-writes barrier (`query_after_hash`), and `derivation.settled` was reachable
// only from `edit-batch` / `reconcile` / the two await barriers — none of which a real editor client
// takes, since the Shell's ONLY write is RPC `edit` (wire_override_gateway.cpp). Every fan-out
// assertion below was therefore reached through a path no user ever walks: the events were real, but
// the TRIGGER was a stand-in.
//
// x9 wired the publisher half, so `race_write`'s plain composed `edit` now publishes `files.changed`
// (from `EditorKernel`'s ingest seam) and then `derivation.settled{gen}` (from the settle its
// `finish_edit` tail runs) all by itself. Removing the chaser is what makes these sections
// NON-VACUOUS: if the `edit` publisher regresses, `pump_until_settle` finds nothing and this drill
// REDS, where before it would have gone on proving `reconcile` still works.
//
// Still measured and still true: no daemon TIMER drives the R-FILE-002 crawl, so an edit made outside
// every client (a git checkout) is folded in only by an explicit `reconcile` — which publishes through
// the same one producer.

// Pump a REAL subscription until one more `derivation` fact has been applied, bounded so a regression
// surfaces as a legible failure instead of hanging the CI job. Returns false when none arrived —
// which every caller CHECKs, because every assertion about what the fan-out DID is vacuous if the
// fan-out never happened at all.
[[nodiscard]] bool pump_until_settle(client::SubscriptionConsumer& consumer,
                                     const std::size_t& settles_seen, std::size_t before)
{
    // 10s base, so BOTH calls together stay inside the ctest TIMEOUT even on the sanitize/tsan legs
    // (kSanitizerTimeoutScale is 4, so 40s each, on top of this drill's 80s boot wait and its
    // attaches). A budget that can overrun the cap would surface a regression as a bare ctest
    // timeout — exactly the illegible failure the bound above exists to avoid. The happy path is
    // sub-second: the settle is already published before the first pump (the `edit` that produced it
    // had returned its reply, and the publish happens inside that call).
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(itest::scaled_timeout_ms(10000));
    std::string error;
    while (settles_seen == before && std::chrono::steady_clock::now() < deadline)
    {
        if (!consumer.pump(error))
        {
            std::fprintf(stderr, "subscription pump failed: %s\n", error.c_str());
            return false;
        }
    }
    return settles_seen > before;
}

// The same bounded pump for a `session` `selection-changed` fact (M9 x10). A SEPARATE counter rather
// than a reuse of `pump_until_settle`'s: `editor.select` publishes NO derivation settle at all (it moves
// session state, not files — kernel_server.cpp), so waiting on the settle counter here would time out
// and red for the wrong reason. Every caller CHECKs the return, because an assertion about what the
// selection fan-out DID is vacuous if the fact never arrived.
[[nodiscard]] bool pump_until_selection(client::SubscriptionConsumer& consumer,
                                        const std::size_t& facts_seen, std::size_t before)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(itest::scaled_timeout_ms(10000));
    std::string error;
    while (facts_seen == before && std::chrono::steady_clock::now() < deadline)
    {
        if (!consumer.pump(error))
        {
            std::fprintf(stderr, "subscription pump failed: %s\n", error.c_str());
            return false;
        }
    }
    return facts_seen > before;
}

[[nodiscard]] bool mentions(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// -------------------------------------------------------------------- the drill

void a_gesture_commits_over_the_wire_and_survives_a_concurrent_writer()
{
    const fs::path project = itest::make_temp_project("e09b-cas");
    seed_project(project);

    ctest_proc::Process daemon = spawn_daemon(project);
    CHECK(ctest_proc::valid(daemon));
    if (!ctest_proc::valid(daemon))
    {
        remove_tree(project);
        return;
    }

    std::string error;
    const std::unique_ptr<client::Client> shell_client = attach_client(project, error);
    const std::unique_ptr<client::Client> racer = attach_client(project, error);
    CHECK(shell_client != nullptr);
    CHECK(racer != nullptr);
    if (shell_client == nullptr || racer == nullptr)
    {
        std::fprintf(stderr, "client attach failed: %s\n", error.c_str());
        reap(daemon);
        remove_tree(project);
        return;
    }
    // TWO REAL CONNECTIONS: distinct daemon-minted ids. A single client racing itself would prove
    // nothing about concurrency — it would just be two of our own sequential writes.
    CHECK(shell_client->client_id() > 0);
    CHECK(racer->client_id() > 0);
    CHECK(shell_client->client_id() != racer->client_id());

    // The REAL Shell panel composition, and the ONE seam that points its write path at the daemon —
    // the same two calls editor_main.cpp makes.
    shell::PanelHost host;
    panels::BuiltinPanels builtin = panels::install_builtin_panels(host);
    CHECK(builtin.inspector != nullptr);
    CHECK(builtin.writes != nullptr);
    if (builtin.inspector == nullptr || builtin.writes == nullptr)
    {
        reap(daemon);
        remove_tree(project);
        return;
    }
    panels::bind_write_client(builtin, shell_client.get());
    CHECK(builtin.writes->has_client());

    // The Inspector advertises the gesture surface the renderer branches on — the manifest fact
    // `panelhost.ts` reads to install the pointer handlers at all.
    {
        const Json listing = host.list();
        bool saw_gestures = false;
        for (std::size_t i = 0; i < listing.at("panels").size(); ++i)
        {
            const Json& entry = listing.at("panels").at(i);
            if (entry.at("id").as_string() == inspector::InspectorPanel::kContributionId)
                saw_gestures = entry.at("gestures").as_bool();
        }
        CHECK(saw_gestures);
    }

    // === 1. THE CANONICAL 05 §8 FLOW — command -> gesture -> RPC -> real bytes ====================
    hydrate_inspector(builtin, *shell_client);
    CHECK(builtin.inspector->results_applied() == 1);
    CHECK(builtin.inspector->panel().has_selection());
    CHECK(builtin.inspector->panel().base_raw_hash() != 0); // a real CAS token, from a real read
    bool overridden = false;
    CHECK(field_value(builtin.inspector->panel(), kFovPointer, overridden) == "1");
    CHECK(!overridden); // the authored value, not yet an override

    const inspector::CommitResult applied = stage_and_commit(host, builtin, kFovPointer, "2.5");
    CHECK(applied.status == inspector::CommitResult::Status::applied);
    // Composition chose the file; the panel never named it. This is exactly what the Shell cannot
    // compute for itself (`context_compose` is D10-forbidden to it) and why the daemon serves it.
    CHECK(applied.file == kRoot);
    CHECK(applied.written_pointer == "/overrides/0/value");
    CHECK(applied.raw_hash != 0);
    CHECK(builtin.writes->writes_applied() == 1);
    {
        const std::string on_disk = read_authored(project, "root.scene.json");
        CHECK(mentions(on_disk, "\"overrides\""));
        CHECK(mentions(on_disk, kFovPointer));
        CHECK(mentions(on_disk, "2.5"));
    }

    // READ-YOUR-WRITES (05 §7): the commit re-armed the fetch, and the next pump shows the panel the
    // value it just wrote — now flagged `overridden`. Without that barrier the panel would render a
    // value it had already successfully written as if it had not.
    CHECK(builtin.inspector->rereads_armed() == 1);
    CHECK(builtin.inspector->pending().has_value());
    panels::pump_panel_feeds(builtin, *shell_client, kRoot);
    CHECK(builtin.inspector->results_applied() == 2);
    CHECK(field_value(builtin.inspector->panel(), kFovPointer, overridden) == "2.5");
    CHECK(overridden);
    // The panel adopted the post-write CAS token, so the NEXT gesture guards on live state.
    CHECK(builtin.inspector->panel().base_raw_hash() == applied.raw_hash);

    // === 2. REBASE — the racer moved the FILE, not THIS field =====================================
    const std::uint64_t base_before_rebase = builtin.inspector->panel().base_raw_hash();
    CHECK(race_write(*racer, kNearPointer, "0.25")); // a DIFFERENT field, same file
    const inspector::CommitResult rebased = stage_and_commit(host, builtin, kFovPointer, "3.5");
    CHECK(rebased.status == inspector::CommitResult::Status::rebased);
    CHECK(rebased.file == kRoot);
    CHECK(rebased.raw_hash != base_before_rebase); // it guarded on the FRESH token, not the stale one
    CHECK(builtin.writes->cas_refusals() == 1);    // the race really did refuse the first attempt
    CHECK(builtin.writes->reads_issued() >= 1);    // and the L-30 engine really did re-read
    CHECK(builtin.inspector->drops_observed() == 0);
    {
        // BOTH writes survive: a rebase is not a last-writer-wins overwrite.
        const std::string on_disk = read_authored(project, "root.scene.json");
        CHECK(mentions(on_disk, "3.5"));
        CHECK(mentions(on_disk, "0.25"));
    }
    panels::pump_panel_feeds(builtin, *shell_client, kRoot); // drain the read-your-writes re-fetch
    CHECK(field_value(builtin.inspector->panel(), kFovPointer, overridden) == "3.5");

    // === 3. DROP — the racer moved THIS field ====================================================
    // The staged gesture's collision base is the value at STAGE time, so the racer must write
    // BETWEEN the stage and the commit — which is precisely the window L-30 exists for. Stage first,
    // by hand, so the race lands inside it.
    stage_only(host, kFovPointer, "9.5");
    CHECK(race_write(*racer, kFovPointer, "7.25")); // THE SAME FIELD, under the in-flight gesture
    commit_gesture(host);
    const inspector::CommitResult dropped = builtin.inspector->panel().last_result();
    CHECK(dropped.status == inspector::CommitResult::Status::dropped);
    CHECK(dropped.code == "cas.mismatch");
    CHECK(!dropped.message.empty()); // the LOUD diagnostic; its human surface is e09b-3
    CHECK(mentions(dropped.message, kFovPointer));
    CHECK(builtin.inspector->drops_observed() == 1);
    CHECK(builtin.inspector->last_commit().status == inspector::CommitResult::Status::dropped);
    // A drop does NOT re-arm the read-your-writes fetch — there was no write to observe.
    CHECK(builtin.inspector->rereads_armed() == 2);
    {
        // THE GUARANTEE: the racer's value is still on disk and 9.5 was never written. An honest
        // refusal beat a hopeful write.
        const std::string on_disk = read_authored(project, "root.scene.json");
        CHECK(mentions(on_disk, "7.25"));
        CHECK(!mentions(on_disk, "9.5"));
    }

    // === 4. FAIL-CLOSED — a commit after the daemon link is cleared writes nothing ================
    // The lifecycle clears the binding before it frees the client (editor_main.cpp); a renderer
    // message already queued can still reach the gesture provider, and this is what it must do.
    panels::bind_write_client(builtin, nullptr);
    CHECK(!builtin.writes->has_client());
    const std::size_t writes_before = builtin.writes->writes_issued();
    stage_only(host, kFovPointer, "42.0");
    commit_gesture(host);
    const inspector::CommitResult refused = builtin.inspector->panel().last_result();
    CHECK(refused.status == inspector::CommitResult::Status::error);
    CHECK(refused.code == std::string(panels::WireOverrideWriteGateway::kNoDaemonCode));
    CHECK(builtin.writes->writes_issued() == writes_before); // nothing crossed a wire that is gone
    // An error KEEPS the staged gesture, so the human's in-flight edit is not silently discarded.
    CHECK(builtin.inspector->panel().has_staged_edit());
    CHECK(!mentions(read_authored(project, "root.scene.json"), "42"));

    // === 5. THE FAN-OUT HALF (M9 e09e-2) — a SECOND bag, on its OWN client ========================
    //
    // Design 05 §8's tail: `derivation.settled{gen}` reaches "all subscribed clients (window 1,
    // window 2, CLI, agents)". So this section boots a SECOND, INDEPENDENT `BuiltinPanels` bag — the
    // panel composition a second editor window runs — over its OWN `client::Client` with its OWN
    // daemon-minted id, fed by a REAL `SubscriptionConsumer` on the REAL `derivation` topic through
    // the REAL `apply_inspector_event` seam `editor_main.cpp` calls. Nothing here synthesizes a
    // settle payload: the fact asserted on is the one the daemon actually published, which is the
    // only way the READER is proven against the wire rather than against our idea of it.
    //
    // ⚠⚠ THE ASSERTIONS THAT MATTER ARE THE NEGATIVE ONES. Refreshing on every settle is the easy
    // implementation and the WRONG one: `set_model` discards the staged gesture AND adopts the fresh
    // file as its CAS base. Nothing errors, nothing crashes — a defeated compare-and-swap looks
    // exactly like a successful edit. So the drill stages a gesture, lets the racer move the SAME
    // field, delivers the REAL settle, PUMPS (as the owner loop does in that same frame), and then
    // requires the commit to STILL DROP.
    //
    // The damage takes TWO steps, and 5b covers both, because the first alone is not the data loss:
    // a served refresh clears `staged_`, so an IMMEDIATE commit would answer `none` and write nothing
    // (loud enough to catch). The silent overwrite needs the human to keep typing — the next
    // `inspector.edit` re-stages, and `stage_edit` takes its base_value from the model the refresh
    // just replaced while `base_raw_hash_` is the racer's post-write token, so the commit CASes
    // against the very state it is racing, finds no mismatch, APPLIES, and clobbers the racer with no
    // drop and no notice. 5b therefore re-stages before committing, which is what makes the on-disk
    // assertions below detectors rather than decoration: with the guard they are unchanged (the model
    // never moved, so the same base is recorded and the commit still drops), and without it `9.75`
    // lands on disk.
    std::string observer_error;
    const std::unique_ptr<client::Client> observer = attach_client(project, observer_error);
    CHECK(observer != nullptr);
    if (observer == nullptr)
    {
        std::fprintf(stderr, "observer attach failed: %s\n", observer_error.c_str());
        reap(daemon);
        remove_tree(project);
        return;
    }
    // A THIRD real connection: three distinct daemon-minted ids. A second bag sharing window 1's
    // client would prove nothing about fan-out — it would be one client talking to itself.
    CHECK(observer->client_id() > 0);
    CHECK(observer->client_id() != shell_client->client_id());
    CHECK(observer->client_id() != racer->client_id());

    shell::PanelHost observer_host;
    panels::BuiltinPanels observed = panels::install_builtin_panels(observer_host);
    CHECK(observed.inspector != nullptr);
    CHECK(observed.writes != nullptr);
    if (observed.inspector == nullptr || observed.writes == nullptr)
    {
        reap(daemon);
        remove_tree(project);
        return;
    }
    panels::bind_write_client(observed, observer.get());
    CHECK(observed.writes->has_client());

    client::AttachOptions observer_attach;
    observer_attach.scope = shell::kShellScope;
    observer_attach.token = observer->instance().token; // the D20 token replayed on a reconnect
    client::SubscriptionConsumer::Options consumer_options;
    consumer_options.poll_timeout_ms = 100;
    client::SubscriptionConsumer settles(*observer, observer_attach, consumer_options);
    std::size_t settles_seen = 0;
    // M9 x9: §8's FIRST fact. `files.changed` had no producer at all before x9, so this counter and
    // `last_file_changed` are what make "the plain `edit` published BOTH facts" assertable here rather
    // than only at the kernel tier — and they are read over the REAL wire, from the REAL daemon.
    std::size_t files_seen = 0;
    std::string last_file_changed;
    // M9 x10 (CE #452): `session` facts, so § 5e can drive a REAL foreign selection move. Counted here
    // rather than read off the feed because the feed's own `session_facts_applied` counts only facts it
    // APPLIED, and the wait below must not depend on the very decision under test.
    std::size_t session_facts_seen = 0;
    settles.on_event(
        [&observed, &settles_seen, &files_seen, &last_file_changed, &session_facts_seen](
            const std::string&, const client::ClientEvent& event)
        {
            if (event.topic == panels::kSessionTopic)
            {
                ++session_facts_seen;
                // The session leg of the live Shell's dispatch (editor_main.cpp's `on_event`), which
                // is what carries a foreign `selection-changed` into the Scene tree and, through the
                // composition root's selection listener, at the Inspector's staged gesture.
                (void)panels::apply_session_event(*observed.session, event.topic, event.payload);
            }
            if (event.topic == kFilesTopic)
            {
                ++files_seen;
                last_file_changed = event.payload.at("path").as_string();
            }
            // Count the SETTLE specifically, not merely the topic: every assertion downstream reads
            // `events_applied()`, which counts only `derivation.settled`. The two coincide today (the
            // topic carries exactly one event kind), so this is forward-proofing rather than a live
            // fix — but the T1 sibling already uses `derivation.started` as its negative case, and the
            // day such an event ships, a topic-only predicate would let `pump_until_settle` return
            // before the settle arrived and red the assertions below spuriously.
            if (event.topic == panels::kDerivationTopic &&
                event.payload.at("event").as_string() == panels::kDerivationSettledEvent)
            {
                ++settles_seen;
            }
            // The inspector leg of the dispatch the live Shell makes (editor_main.cpp's `on_event`),
            // argument for argument: nothing is pre-sorted, because topic filtering is the feed's own
            // job. The live lambda also drives the problems / scenetree / session seams; only the
            // inspector one is wired here, so this drill proves the derivation -> Inspector path and
            // deliberately says nothing about cross-feed interactions on the same event.
            (void)panels::apply_inspector_event(*observed.inspector, event.topic, event.payload);
        });
    settles.add(client::SubscriptionSpec{
        {std::string(panels::kDerivationTopic), kFilesTopic, std::string(panels::kSessionTopic)}, ""});
    // x10: the observer's session feed needs its OWN daemon-minted id, or echo suppression cannot tell
    // its own writes from a racer's — the same binding `editor_main.cpp` makes at every point the daemon
    // link changes. Without it the feed is a plain subscriber (client_id_ == 0), which would apply even
    // its own echoes; § 5e's fact is foreign either way, so this is honesty about the wiring rather than
    // a load-bearing step for the assertions.
    CHECK(observed.session != nullptr);
    if (observed.session != nullptr)
    {
        panels::bind_session_client(*observed.session, observer.get());
    }
    CHECK(settles.start(observer_error));
    CHECK(settles.states().size() == 1u);
    CHECK(settles.states().size() == 1u && settles.states()[0].live);

    // Hydrate the second window's Inspector on the SAME entity, through its own live read path.
    hydrate_inspector(observed, *observer);
    CHECK(observed.inspector->results_applied() == 1);
    CHECK(observed.inspector->panel().base_raw_hash() != 0);
    CHECK(field_value(observed.inspector->panel(), kFovPointer, overridden) == "7.25");

    // --- 5a. THE PLAIN FAN-OUT: idle panel, someone else writes, the settle re-reads it -----------
    //
    // ⚠ THE RACER'S PLAIN `edit` IS THE ONLY TRIGGER (M9 x9). No `reconcile` chaser, no `edit-batch`,
    // no `await_*` — see the note where `force_settle` used to live. So this is the §8 chain reached by
    // the exact call a second editor window / agent / CLI makes.
    {
        const std::size_t settles_before = settles_seen;
        const std::size_t files_before = files_seen;
        const std::size_t rereads_before = observed.inspector->rereads_armed();
        CHECK(race_write(*racer, kFovPointer, "5.5")); // window 2 is not editing; a co-writer moves it
        CHECK(pump_until_settle(settles, settles_seen, settles_before));

        // §8's chain, in §8's order, from ONE plain edit: `files.changed` for the file COMPOSITION
        // chose (the caller named a FIELD, never this path), then exactly ONE settle. The count is the
        // storm check — an edit that published two settles would double every client's traffic.
        CHECK(files_seen == files_before + 1);
        CHECK(last_file_changed == kRoot);
        CHECK(settles_seen == settles_before + 1);

        CHECK(observed.inspector->events_applied() >= 1u); // the settle was RECOGNIZED, not ignored
        CHECK(observed.inspector->rereads_armed() > rereads_before); // …and it armed the re-read
        CHECK(!observed.inspector->refresh_deferred());
        CHECK(observed.inspector->pending().has_value());
        panels::pump_panel_feeds(observed, *observer, kRoot);
        CHECK(observed.inspector->results_applied() == 2);
        // THE FAN-OUT: window 2 shows a value it never wrote and never re-selected for.
        CHECK(field_value(observed.inspector->panel(), kFovPointer, overridden) == "5.5");
    }

    // --- 5b. THE GUARD: a settle arriving MID-GESTURE must not re-base the L-30 collision base ----
    const std::uint64_t observer_base = observed.inspector->panel().base_raw_hash();
    CHECK(observer_base != 0);
    observed.inspector->mark_fetched(); // claim anything armed, so "nothing armed" below is meaningful
    CHECK(!observed.inspector->pending().has_value());
    const std::size_t results_before = observed.inspector->results_applied();
    const std::size_t rereads_before = observed.inspector->rereads_armed();

    stage_only(observer_host, kFovPointer, "9.75"); // the human is mid-edit in window 2
    CHECK(observed.inspector->panel().has_staged_edit());

    {
        const std::size_t settles_before = settles_seen;
        const std::size_t files_before = files_seen;
        const std::size_t events_before = observed.inspector->events_applied();
        CHECK(race_write(*racer, kFovPointer, "6.5")); // the racer moves THE SAME FIELD, mid-gesture
        CHECK(pump_until_settle(settles, settles_seen, settles_before)); // its OWN plain edit settles
        CHECK(files_seen == files_before + 1);

        // RECOGNIZED — so the four negatives below cannot pass merely because the fact was dropped by
        // a topic filter, which is the one way this whole section could read green while proving
        // nothing.
        CHECK(observed.inspector->events_applied() > events_before);
        CHECK(observed.inspector->refresh_deferred());       // owed, and knowingly withheld
        CHECK(!observed.inspector->pending().has_value());   // NOT armed
        CHECK(observed.inspector->rereads_armed() == rereads_before);
        CHECK(observed.inspector->panel().has_staged_edit()); // the human's edit survived
        CHECK(observed.inspector->panel().base_raw_hash() == observer_base); // NOT re-based

        // The owner loop pumps in the SAME frame it dispatches events, so pump here too: this is the
        // step that would actually destroy the gesture if the settle had armed the fetch, and it is
        // what makes the assertions above load-bearing rather than merely early.
        panels::pump_panel_feeds(observed, *observer, kRoot);
        CHECK(observed.inspector->results_applied() == results_before);
        CHECK(observed.inspector->panel().has_staged_edit());
        CHECK(observed.inspector->panel().base_raw_hash() == observer_base);
    }

    // THE PAYOFF: the L-30 guarantee is intact at the second bag too. The gesture's base is still the
    // pre-race token and its collision base is still 5.5, so the commit CAS-fails, re-reads, sees the
    // field MOVED, and DROPS — the racer's 6.5 survives and 9.75 is never written.
    //
    // The human keeps typing before ending the gesture — the ordinary shape, and the step that makes
    // the on-disk assertions below discriminating (see the section preamble). Under the correct code
    // the model is untouched, so this re-records the SAME base and changes nothing; with the guard
    // removed it re-bases onto the racer's post-write state and the commit applies instead of
    // dropping.
    stage_only(observer_host, kFovPointer, "9.75");
    CHECK(observed.inspector->panel().has_staged_edit());
    commit_gesture(observer_host);
    const inspector::CommitResult observer_dropped = observed.inspector->panel().last_result();
    CHECK(observer_dropped.status == inspector::CommitResult::Status::dropped);
    // Deliberately NOT redundant with the line above, because CHECK is non-fatal: `none` is the
    // specific status a served settle produces (nothing staged left to commit), so naming it makes a
    // failure say WHICH way it broke instead of just "not dropped". Do not simplify this away.
    CHECK(observer_dropped.status != inspector::CommitResult::Status::none);
    CHECK(observer_dropped.code == "cas.mismatch");
    CHECK(mentions(observer_dropped.message, kFovPointer));
    CHECK(observed.inspector->drops_observed() == 1);
    {
        const std::string on_disk = read_authored(project, "root.scene.json");
        CHECK(mentions(on_disk, "6.5"));
        CHECK(!mentions(on_disk, "9.75"));
    }

    // --- 5c. …AND IT DOES REFRESH ONCE THE GESTURE RESOLVES ---------------------------------------
    // The drop consumed the gesture, so the withheld re-read is released — which is what makes the
    // loud "re-make your edit against what is there now" actionable instead of leaving the human
    // staring at the value that is no longer on disk. ONE re-read, not one per deferred settle.
    CHECK(!observed.inspector->refresh_deferred());
    CHECK(observed.inspector->rereads_armed() == rereads_before + 1);
    CHECK(observed.inspector->pending().has_value());
    panels::pump_panel_feeds(observed, *observer, kRoot);
    CHECK(observed.inspector->results_applied() == results_before + 1);
    CHECK(field_value(observed.inspector->panel(), kFovPointer, overridden) == "6.5");
    CHECK(observed.inspector->panel().base_raw_hash() != observer_base); // now guarding live state

    // --- 5d. THE SELF-ECHO (M9 x9): a window's OWN edit must not re-base its OWN next gesture ------
    //
    // THE HAZARD THIS SECTION EXISTS FOR IS CREATED BY x9 ITSELF. Until the publisher half landed, the
    // editor's writes emitted nothing, so a window could never receive an event caused by its own
    // `edit` — the deferral proven in 5b only ever had to survive a FOREIGN writer, and the self-echo
    // path was unreachable. Now every applied commit publishes a settle that comes straight back to the
    // window that made it, on its own subscription, one pump later. If that echo were served while the
    // human's NEXT gesture is in flight, `set_model` would discard the staged edit AND adopt a new CAS
    // base — the same silent re-base 5b guards, except triggered by the user's own keystrokes rather
    // than by a race, which makes it reachable with no concurrency at all.
    //
    // The shape is the ordinary one: commit, keep typing, and the echo lands mid-next-gesture.
    {
        const std::size_t settles_before = settles_seen;
        const std::size_t files_before = files_seen;

        // 1) Window 2's OWN write, through its OWN gateway — the real `edit` RPC, which now publishes.
        //    A DIFFERENT field from 5b's, so nothing here can pass on leftover state from that section.
        const inspector::CommitResult self_applied =
            stage_and_commit(observer_host, observed, kNearPointer, "0.25");
        CHECK(self_applied.status == inspector::CommitResult::Status::applied);

        // 2) Read-your-writes: the commit armed its own re-read and the owner loop serves it in the
        //    same frame. Afterwards nothing is staged and nothing is pending, so every negative below
        //    is about the ECHO and not about leftover bookkeeping.
        CHECK(observed.inspector->pending().has_value());
        panels::pump_panel_feeds(observed, *observer, kRoot);
        CHECK(!observed.inspector->pending().has_value());
        CHECK(!observed.inspector->panel().has_staged_edit());
        CHECK(!observed.inspector->refresh_deferred());
        const std::uint64_t self_base = observed.inspector->panel().base_raw_hash();
        CHECK(self_base != 0);

        // 3) The human keeps working. The echo has been queued on the client since step 1 (Client::call
        //    parks pushed frames; only the consumer's pump dispatches them), so it arrives DURING this
        //    gesture — which is exactly the interleaving a real owner loop produces.
        stage_only(observer_host, kFovPointer, "11.5");
        CHECK(observed.inspector->panel().has_staged_edit());
        const std::size_t rereads_at_stage = observed.inspector->rereads_armed();
        const std::size_t events_at_stage = observed.inspector->events_applied();
        const std::size_t results_at_stage = observed.inspector->results_applied();

        CHECK(pump_until_settle(settles, settles_seen, settles_before));
        // ONE logical edit -> ONE settle + ONE files fact, self-published. If a single write ever
        // published twice, this window would receive its own echo twice per keystroke.
        CHECK(settles_seen == settles_before + 1);
        CHECK(files_seen == files_before + 1);
        CHECK(last_file_changed == kRoot);

        // THE ASSERTIONS THAT MATTER, and they are the negative ones: the echo was RECOGNIZED (so a
        // topic-filter regression cannot pass this by dropping it) and DEFERRED, and the human's
        // in-flight gesture plus its L-30 collision base are untouched — across the pump that would
        // actually perform the damaging re-read.
        CHECK(observed.inspector->events_applied() > events_at_stage);
        CHECK(observed.inspector->refresh_deferred());
        CHECK(!observed.inspector->pending().has_value());
        CHECK(observed.inspector->rereads_armed() == rereads_at_stage);
        CHECK(observed.inspector->panel().has_staged_edit());
        CHECK(observed.inspector->panel().base_raw_hash() == self_base);
        panels::pump_panel_feeds(observed, *observer, kRoot);
        CHECK(observed.inspector->results_applied() == results_at_stage);
        CHECK(observed.inspector->panel().has_staged_edit());
        CHECK(observed.inspector->panel().base_raw_hash() == self_base);

        // 4) …and the gesture still lands. NOBODY moved the file since `self_base` was adopted, so the
        //    CAS matches and the commit APPLIES. This is the other half of "the self-echo is harmless":
        //    a guard that turned an uncontested own-edit into a spurious DROP would be its own defect,
        //    and it would look identical to correctness if only the negatives above were asserted.
        const std::size_t drops_before = observed.inspector->drops_observed();
        commit_gesture(observer_host);
        const inspector::CommitResult second = observed.inspector->panel().last_result();
        CHECK(second.status == inspector::CommitResult::Status::applied);
        CHECK(observed.inspector->drops_observed() == drops_before);
        CHECK(mentions(read_authored(project, "root.scene.json"), "11.5"));
    }

    // --- 5e. THE SELECTION DOOR (M9 x10, CE #452): another client MOVES the shared selection ---------
    //
    // THE HAZARD, and why it is a different door from every one above. 5b/5d prove that a
    // `derivation.settled` — foreign or self — is withheld while a gesture is staged. Neither touches
    // SELECTION, which has been DAEMON state since e08b: a racer's `editor.select` publishes a
    // `selection-changed` fact that survives echo suppression, reaches `SceneTreePanel::apply_selection`
    // and, through the composition root's selection listener, called `InspectorFeed::request` with a
    // DIFFERENT identity — which x9's SAME-identity guard let straight through. The pump then served it,
    // `set_model` discarded the human's staged edit AND re-based the L-30 collision base onto the
    // racer's post-write state, and NOTHING reported an error. That is CE #452, and it is the only open
    // defect in this milestone that destroys a human's work with no notice.
    //
    // ⚠ THIS IS THE ONLY TIER THAT CAN DRIVE IT FOR REAL. A foreign `selection-changed` requires a
    // SECOND daemon-minted `origin` — a fact carrying our own id is dropped by design — and the live
    // two-window CEF smoke (`editor-cef-smoke-shell-inspector-fanout`) binds BOTH windows to ONE
    // connection, so every fact it could produce is a self-echo (CE #455, open, deliberately out of
    // x10's scope). This drill already holds three SEPARATE clients with distinct ids, which is exactly
    // what the door needs.
    {
        const std::string other_identity = "aaaaaaaaaaaaaaa1/ccccccccccccccc2";
        // CLEAN SLATE, and the pump is load-bearing rather than tidy-up. § 5d's final commit APPLIED, so
        // read-your-writes armed a fetch that is still PENDING here — exactly as it is in a real owner
        // loop between the commit and the next frame. Serving it now is what the owner loop does; NOT
        // serving it would leave § 5e measuring x10's LOUD path (a pre-armed fetch landing on a staged
        // gesture — an abandonment) instead of its DEFERRAL path. Measured: without this pump the
        // section reds on `results_applied`/`has_staged_edit`/`abandons_observed`, which is the code
        // behaving correctly against a mis-staged scenario.
        panels::pump_panel_feeds(observed, *observer, kRoot);
        CHECK(!observed.inspector->panel().has_staged_edit());
        CHECK(!observed.inspector->pending().has_value());
        CHECK(!observed.inspector->selection_deferred());
        const std::uint64_t selection_base = observed.inspector->panel().base_raw_hash();
        CHECK(selection_base != 0);
        const std::size_t results_at_stage = observed.inspector->results_applied();
        const std::size_t abandons_before = observed.inspector->abandons_observed();
        const std::size_t facts_before = session_facts_seen;

        // The human is mid-edit in window 2.
        stage_only(observer_host, kFovPointer, "13.5");
        CHECK(observed.inspector->panel().has_staged_edit());

        // …and the racer moves the shared selection out from under them, over the real wire.
        CHECK(race_select(*racer, other_identity));
        CHECK(pump_until_selection(settles, session_facts_seen, facts_before));

        // The fact was APPLIED by the session feed — the tree's rendered selection really moved — so
        // the negatives below cannot pass merely because the fact was dropped in transit or swallowed
        // as an echo, which is the one way this whole section could read green while proving nothing.
        CHECK(observed.scenetree != nullptr);
        CHECK(observed.scenetree != nullptr &&
              observed.scenetree->panel().selection().identity == other_identity);
        CHECK(panels::session_facts_applied(*observed.session) >= 1u);

        // THE ASSERTIONS THAT MATTER, all negative: the move was RECOGNIZED and WITHHELD, nothing was
        // armed, and the human's gesture plus its L-30 collision base are untouched.
        CHECK(observed.inspector->selections_deferred() >= 1u);
        CHECK(observed.inspector->selection_deferred());
        CHECK(observed.inspector->deferred_selection().has_value() &&
              observed.inspector->deferred_selection()->identity == other_identity);
        CHECK(!observed.inspector->pending().has_value());
        CHECK(observed.inspector->panel().has_staged_edit());
        CHECK(observed.inspector->panel().base_raw_hash() == selection_base);

        // …ACROSS THE PUMP that would actually perform the damaging re-read. Under the pre-x10 code the
        // fetch is pending here, this call serves it, and both assertions after it flip.
        panels::pump_panel_feeds(observed, *observer, kRoot);
        CHECK(observed.inspector->results_applied() == results_at_stage);
        CHECK(observed.inspector->panel().has_staged_edit());
        CHECK(observed.inspector->panel().base_raw_hash() == selection_base);
        CHECK(observed.inspector->panel().model().identity == kIdentity); // still the edited entity
        // NOTHING WAS LOST, so nothing was reported: the LOUD half must stay silent on the path the
        // deferral covers, or the human is toasted for an edit that is still in their hands.
        CHECK(observed.inspector->abandons_observed() == abandons_before);

        // THE PAYOFF, and the half a "just refuse it" fix would fail: the gesture still lands on the
        // entity it was staged on — NOBODY moved the file, so the CAS matches and the commit APPLIES —
        // and only THEN does the panel follow the daemon to the new selection.
        const std::size_t drops_before = observed.inspector->drops_observed();
        commit_gesture(observer_host);
        const inspector::CommitResult after_move = observed.inspector->panel().last_result();
        CHECK(after_move.status == inspector::CommitResult::Status::applied);
        CHECK(observed.inspector->drops_observed() == drops_before);
        CHECK(mentions(read_authored(project, "root.scene.json"), "13.5"));
        // The withheld MOVE outranks the applied commit's own read-your-writes re-read, because the
        // re-read is owed to an entity the selection has left. So the pending fetch is the MOVE.
        CHECK(!observed.inspector->selection_deferred());
        CHECK(observed.inspector->pending() == std::optional<std::string>(other_identity));
        panels::pump_panel_feeds(observed, *observer, kRoot);
        CHECK(observed.inspector->results_applied() == results_at_stage + 1);
        CHECK(observed.inspector->panel().model().identity == other_identity);
        CHECK(observed.inspector->abandons_observed() == abandons_before);
    }

    settles.stop();
    panels::bind_write_client(observed, nullptr);

    std::string shutdown_error;
    (void)shell_client->call("shutdown", Json::object(), shutdown_error);
    reap(daemon);
    remove_tree(project);
}

} // namespace

int main()
{
    a_gesture_commits_over_the_wire_and_survives_a_concurrent_writer();
    ITEST_MAIN_END();
}
