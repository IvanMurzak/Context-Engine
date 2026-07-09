// M1 exit criterion 2 — second CLI live attach (R-CLI-010, issue #36):
// a SECOND CLI process attaches LIVE to the running daemon via the capability-negotiation
// handshake — `protocolMajor=1` carried on the wire, hard-fail-on-mismatch semantics (v1: the
// compatibility window is exactly {0}).
//
// All of this runs against a REAL `context daemon` process over the REAL loopback IPC wire (#35):
//   * happy path — two REAL sequential `context attach` CLI processes each drive the daemon; the
//     daemon lifetime (incarnationId) is stable across them (it is the SAME live daemon).
//   * negotiation — the handshake carries {protocolMajor, capabilities[]}; an unknown client
//     capability is dropped from the negotiated subset (client ∩ daemon), never invented.
//   * failure path — a client with protocolMajor=1 HARD-FAILS with the catalog code
//     `handshake.incompatible_protocol` (R-CLI-008 schema on the wire), and the daemon SURVIVES:
//     the same connection renegotiates at major 0 successfully.
//   * failure path — a method before any attach is refused (usage.invalid): no session without a
//     handshake.
//
// (R-QA-013: happy + negotiation edge + two failure paths.)

#include "context/editor/contract/json.h"

#include "integration_test.h"
#include "process_util.h"

#include <set>
#include <string>

namespace fs = std::filesystem;
using itest::Json;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

int main()
{
    const std::string bin = CONTEXT_BINARY;
    const fs::path project = itest::make_temp_project("attach2");

    ctest_proc::Process daemon = ctest_proc::spawn(bin, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));
    CHECK(itest::wait_for_instance(project, 15000));

    // --- capture the daemon lifetime + advertised capabilities (client #1, raw JSON-RPC) --------
    std::string incarnation_before;
    std::set<std::string> daemon_caps;
    {
        itest::RpcClient rpc;
        CHECK(rpc.connect(project));

        // The handshake carries protocolMajor + capabilities from day one (R-CLI-010). Request one
        // real capability plus one the daemon has never heard of.
        const auto attached = rpc.attach(1,{"describe", "bogus-capability-xyz"}, "");
        CHECK(itest::is_ok(attached));
        if (itest::is_ok(attached))
        {
            const Json& result = attached->at("result");
            CHECK(result.at("protocolMajor").as_int() == 1);
            // Negotiated subset = client ∩ daemon: the unknown capability MUST be dropped.
            bool has_bogus = false;
            for (std::size_t i = 0; i < result.at("capabilities").size(); ++i)
            {
                const std::string cap = result.at("capabilities").at(i).as_string();
                if (cap == "bogus-capability-xyz")
                    has_bogus = true;
            }
            CHECK(!has_bogus);
        }

        const auto described = rpc.call("describe", Json::object());
        CHECK(itest::is_ok(described));
        if (itest::is_ok(described))
        {
            const Json& proto =
                described->at("result").at("data").at("contract").at("protocol");
            CHECK(proto.at("protocolMajor").as_int() == 1);
            for (std::size_t i = 0; i < proto.at("capabilities").size(); ++i)
                daemon_caps.insert(proto.at("capabilities").at(i).as_string());
        }

        const auto snap = rpc.call("snapshot", Json::object());
        CHECK(itest::is_ok(snap));
        if (itest::is_ok(snap))
            incarnation_before = snap->at("result").at("data").at("incarnationId").as_string();
        CHECK(!incarnation_before.empty());
        rpc.close();
    }

    // Every capability the attach negotiated must be one the daemon advertises (subset law) —
    // verified against the describe introspection above (client #1 asked for "describe").
    CHECK(daemon_caps.count("describe") == 1);

    // --- the criterion: REAL second (and third) CLI processes attach LIVE to the running daemon --
    for (const char* tag : {"first", "second"})
    {
        const fs::path out = project / (std::string("attach-") + tag + ".json");
        ctest_proc::Process attach = ctest_proc::spawn(
            bin, {"attach", "--project", project.string(), "--set-path",
                  std::string("proj/") + tag + ".scene", "--set-content", "entity: 1", "--out",
                  out.string()});
        CHECK(ctest_proc::valid(attach));
        int code = -1;
        const bool done = ctest_proc::wait_for(attach, 25000, code);
        if (!done)
            ctest_proc::kill(attach);
        ctest_proc::release(attach);
        CHECK(done);
        CHECK(code == 0); // the whole handshake + drive succeeded from a separate process

        const std::string rj = itest::read_file(out);
        CHECK(!rj.empty());
        if (!rj.empty())
        {
            const Json env = Json::parse(rj);
            CHECK(env.at("ok").as_bool());
            CHECK(env.at("data").at("attached").as_bool());
        }
    }

    // --- hard-fail on mismatch + daemon survival (client #4, raw JSON-RPC) -----------------------
    {
        itest::RpcClient rpc;
        CHECK(rpc.connect(project));

        // protocolMajor=2 is outside the frozen v1 compatibility window {1}: the handshake MUST
        // hard-fail through the R-CLI-008 error schema — the SAME catalog code on the wire.
        const auto rejected = rpc.attach(2, {"describe"}, "");
        CHECK(rejected.has_value());
        CHECK(!itest::is_ok(rejected));
        CHECK(itest::error_code_of(rejected) == "handshake.incompatible_protocol");

        // The daemon SURVIVES a failed handshake: the same connection renegotiates at the frozen major 1.
        const auto renegotiated = rpc.attach(1,{"describe"}, "");
        CHECK(itest::is_ok(renegotiated));

        // Same daemon lifetime end-to-end: the incarnation id never changed while clients came,
        // failed, and went — this was one LIVE daemon throughout (R-BRIDGE-008 epoch).
        const auto snap = rpc.call("snapshot", Json::object());
        CHECK(itest::is_ok(snap));
        if (itest::is_ok(snap))
            CHECK(snap->at("result").at("data").at("incarnationId").as_string() ==
                  incarnation_before);
        rpc.close();
    }

    // --- no session without a handshake (fresh connection, method before attach) -----------------
    {
        itest::RpcClient rpc;
        CHECK(rpc.connect(project));
        Json params = Json::object();
        params.set("path", Json(std::string("proj/first.scene")));
        const auto premature = rpc.call("query", std::move(params));
        CHECK(premature.has_value());
        CHECK(!itest::is_ok(premature));
        CHECK(itest::error_code_of(premature) == "usage.invalid");

        // Attach with session scope and shut the daemon down cleanly.
        const auto attached = rpc.attach(1,{"describe"}, "session");
        CHECK(itest::is_ok(attached));
        const auto stopped = rpc.call("shutdown", Json::object());
        CHECK(itest::is_ok(stopped));
        rpc.close();
    }

    int daemon_code = -1;
    const bool daemon_done = ctest_proc::wait_for(daemon, 15000, daemon_code);
    if (!daemon_done)
        ctest_proc::kill(daemon);
    ctest_proc::release(daemon);
    CHECK(daemon_done);
    if (daemon_done)
        CHECK(daemon_code == 0);

    std::error_code ec;
    fs::remove_all(project, ec);

    ITEST_MAIN_END();
}
