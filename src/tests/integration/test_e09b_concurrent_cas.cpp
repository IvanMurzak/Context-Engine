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
// READ-YOUR-WRITES re-read that makes the panel show what it just wrote. The cross-WINDOW tail of
// that sequence (the fan-out reaching a second editor window) is e10d's live smoke; this drill owns
// the write half.

#include "context/editor/client/client.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/serializer/canonical.h" // the ONE value identity (R-FILE-001)
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/panels/inspector_feed.h"
#include "context/editor/shell/panels/wire_override_gateway.h"
#include "context/editor/shell/shell.h" // kShellScope — attach with the REAL Shell's scope request

#include "integration_test.h"
#include "process_util.h"

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
    CHECK(itest::write_file_raw(authored / "child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Cam",
         "components": {
           "transform": {"position": [1, 2, 3]},
           "camera": {"fov": 1.0, "near": 0.1, "far": 500.0}
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

// Stage a value on the field through the REAL `panel.command` seam the renderer uses, then end the
// gesture through the REAL `panel.gesture` seam. Returns the panel's resolved commit outcome.
[[nodiscard]] inspector::CommitResult stage_and_commit(shell::PanelHost& host,
                                                       panels::BuiltinPanels& builtin,
                                                       const std::string& pointer,
                                                       const std::string& value)
{
    Json edit = Json::object();
    edit.set("nodeId", Json(std::string(panels::kInspectorWidgetPrefix) + pointer));
    edit.set("value", Json(value)); // a JSON literal in a string (its canonical serialization)
    bool dispatched = false;
    std::string code;
    CHECK(host.invoke(inspector::InspectorPanel::kContributionId,
                      inspector::InspectorPanel::kEditCommand, edit, dispatched, code));
    CHECK(dispatched); // the staged gesture (L-20: no write yet)
    CHECK(builtin.inspector->panel().has_staged_edit());

    dispatched = false;
    CHECK(host.gesture(inspector::InspectorPanel::kContributionId, shell::GestureVerb::commit,
                       Json::object(), dispatched, code));
    CHECK(dispatched);
    return builtin.inspector->panel().last_result();
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
    {
        Json edit = Json::object();
        edit.set("nodeId", Json(std::string(panels::kInspectorWidgetPrefix) + kFovPointer));
        edit.set("value", Json(std::string("9.5")));
        bool dispatched = false;
        std::string code;
        CHECK(host.invoke(inspector::InspectorPanel::kContributionId,
                          inspector::InspectorPanel::kEditCommand, edit, dispatched, code));
        CHECK(dispatched);
    }
    CHECK(race_write(*racer, kFovPointer, "7.25")); // THE SAME FIELD, under the in-flight gesture
    {
        bool dispatched = false;
        std::string code;
        CHECK(host.gesture(inspector::InspectorPanel::kContributionId, shell::GestureVerb::commit,
                           Json::object(), dispatched, code));
        CHECK(dispatched);
    }
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
    {
        Json edit = Json::object();
        edit.set("nodeId", Json(std::string(panels::kInspectorWidgetPrefix) + kFovPointer));
        edit.set("value", Json(std::string("42.0")));
        bool dispatched = false;
        std::string code;
        CHECK(host.invoke(inspector::InspectorPanel::kContributionId,
                          inspector::InspectorPanel::kEditCommand, edit, dispatched, code));
        CHECK(dispatched);
        dispatched = false;
        CHECK(host.gesture(inspector::InspectorPanel::kContributionId, shell::GestureVerb::commit,
                           Json::object(), dispatched, code));
        CHECK(dispatched);
    }
    const inspector::CommitResult refused = builtin.inspector->panel().last_result();
    CHECK(refused.status == inspector::CommitResult::Status::error);
    CHECK(refused.code == std::string(panels::WireOverrideWriteGateway::kNoDaemonCode));
    CHECK(builtin.writes->writes_issued() == writes_before); // nothing crossed a wire that is gone
    // An error KEEPS the staged gesture, so the human's in-flight edit is not silently discarded.
    CHECK(builtin.inspector->panel().has_staged_edit());
    CHECK(!mentions(read_authored(project, "root.scene.json"), "42"));

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
