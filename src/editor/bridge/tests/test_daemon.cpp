// Daemon unit tests (R-BRIDGE-001 / R-ARCH-005 / R-SEC-007): boot acquires the single-instance lock
// atomically, a 2nd write-capable instantiation gets the attach signal, the event stream +
// dispatcher come up, and the launch-time operator scope ceiling clamps an attaching client.

#include "context/editor/bridge/daemon.h"

#include "context/editor/bridge/scope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"

#include "bridge_test.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>

namespace fs = std::filesystem;
using namespace context::editor::bridge;
using context::editor::contract::ClientHandshake;
using context::editor::contract::Envelope;
using context::editor::contract::Json;

namespace
{
fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("ctx-bridge-daemon-" + std::string(tag) + "-" +
                                                std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}
} // namespace

int main()
{
    // --- boot: lock acquired atomically, stream + dispatcher up, session announced --------------
    const fs::path project = make_temp_project("boot");
    {
        Daemon d(project);
        CHECK(d.start(/*write_capable=*/true) == StartOutcome::booted);
        CHECK(d.running());
        CHECK(d.lock().held());
        CHECK(d.lock().mode() == LockMode::exclusive);

        // The session-started event is on the stream (replayed from the ring since seq 0).
        bool gapped = false;
        auto events = d.events().replay_since(0, gapped);
        bool saw_start = false;
        for (const auto& e : events)
            if (e.topic == "session" &&
                e.payload.at("event").as_string() == "session.started")
                saw_start = true;
        CHECK(saw_start);

        // --- FAILURE/attach path: a 2nd write-capable daemon on the SAME project gets attach ----
        {
            Daemon second(project);
            CHECK(second.start(/*write_capable=*/true) == StartOutcome::attach);
            CHECK(!second.running()); // it did NOT boot a second instance
        }

        // A read-only (shared) start also conflicts with the live exclusive lock.
        {
            Daemon reader(project);
            CHECK(reader.start(/*write_capable=*/false) == StartOutcome::attach);
        }

        d.stop();
        CHECK(!d.running());
    }

    // --- after stop, the project is free again --------------------------------------------------
    {
        Daemon d(project);
        CHECK(d.start(/*write_capable=*/true) == StartOutcome::booted);
    }

    // --- launch-time operator scope ceiling clamps an attaching client (R-SEC-007) --------------
    {
        const fs::path proj = make_temp_project("scope");
        Daemon d(proj);
        CHECK(d.start(/*write_capable=*/true, ScopeSet::parse("write")) == StartOutcome::booted);

        ClientHandshake client;
        client.protocol_major = context::editor::contract::kProtocolMajor; // in-window (frozen major)
        client.capabilities = {"describe"};
        // The client asks for build+install, but the operator ceiling only permits file-write.
        auto result = d.attach_client(client, ScopeSet::parse("write build"));
        CHECK(std::holds_alternative<Session>(result));
        const Session& s = std::get<Session>(result);
        CHECK(s.scopes.has(Scope::file_write));
        CHECK(!s.scopes.has(Scope::build_install)); // clamped away by the launch ceiling

        // Consequently the install is still rejected at the dispatcher for this clamped session.
        const Envelope install = d.dispatcher().dispatch("package.add", Json::object(), s);
        CHECK(!install.ok());
        CHECK(install.error()->code == kScopeDeniedCode);
        // R-SEC-007: the bridge points kScopeDeniedCode at the promoted catalog entry, so the
        // scope-denied envelope now classes as the permission exit (6), not the generic error (1).
        CHECK(install.exit_code() == 6);

        d.stop();
        std::error_code ec;
        fs::remove_all(proj, ec);
    }

    // Best-effort cleanup.
    std::error_code ec;
    fs::remove_all(project, ec);

    BRIDGE_TEST_MAIN_END();
}
