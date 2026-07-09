// M1 exit criterion 4 — contract parity + additive-only catalog (R-CLI-008/009, issue #36):
// CI proves CLI ≡ RPC ≡ MCP ≡ introspection parity generated from the ONE registry, and enforces
// that the error-code catalog is additive-only.
//
// The in-process generator parity lives in src/editor/contract/tests/test_registry_parity.cpp; this
// gate proves the same contract END-TO-END across the real shipped surfaces:
//   * the CLI dispatch path (`context describe` through cli::run — the shipped verb dispatcher),
//   * the RPC surface over the REAL wire (a real `context daemon` process serving `describe`, #35),
//   * the MCP + introspection projections of the registry,
// and asserts they are all BYTE-IDENTICAL projections of the single registry, that the wire-visible
// error catalog contains every frozen v0 baseline code (additive-only holds on what clients actually
// see, not just in a unit test), and that the exit-code table survives the wire (scope.denied -> 6,
// handshake.incompatible_protocol -> 7 — the R-SEC-007 exit-class the criterion-5 gate denies with).
//
// R-QA-013: happy path (parity + baseline present) + failure path (a fabricated catalog REMOVAL is
// caught by the enforcement predicate — the check actually detects violations, it is not
// vacuously green).

#include "context/cli/app.h"
#include "context/editor/contract/error_catalog.h"
#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"

#include "integration_test.h"
#include "process_util.h"

#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace context::editor::contract;
using itest::Json;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace
{
std::set<std::string> catalog_codes_of(const Json& describe_doc)
{
    std::set<std::string> codes;
    const Json& catalog = describe_doc.at("contract").at("errorCatalog");
    for (std::size_t i = 0; i < catalog.size(); ++i)
        codes.insert(catalog.at(i).at("code").as_string());
    return codes;
}

int wire_exit_code_for(const Json& describe_doc, const std::string& code)
{
    const Json& catalog = describe_doc.at("contract").at("errorCatalog");
    for (std::size_t i = 0; i < catalog.size(); ++i)
        if (catalog.at(i).at("code").as_string() == code)
            return static_cast<int>(catalog.at(i).at("exitCode").as_int());
    return -1;
}
} // namespace

int main()
{
    const std::string bin = CONTEXT_BINARY;
    const Registry& reg = Registry::instance();

    // --- the single source of truth + its in-process projections --------------------------------
    const Json registry_describe = reg.describe();
    const std::string registry_dump = registry_describe.dump();

    // --- surface 1: the CLI dispatch path (the shipped `context describe` verb) -----------------
    const Envelope cli_env = context::cli::run({"describe"});
    CHECK(cli_env.ok());
    const Json cli_json = cli_env.to_json();
    CHECK(cli_json.at("ok").as_bool());
    CHECK(cli_json.at("data").dump() == registry_dump); // CLI ≡ registry, byte-for-byte

    // --- surface 2: the RPC `describe` over the REAL wire from a REAL daemon process ------------
    Json wire_describe;
    {
        const fs::path project = itest::make_temp_project("parity");
        ctest_proc::Process daemon =
            ctest_proc::spawn(bin, {"daemon", "--project", project.string()});
        CHECK(ctest_proc::valid(daemon));
        CHECK(itest::wait_for_instance(project, 15000));

        itest::RpcClient rpc;
        CHECK(rpc.connect(project));
        const auto attached = rpc.attach(1, {"describe"}, "session");
        CHECK(itest::is_ok(attached));

        const auto described = rpc.call("describe", Json::object());
        CHECK(itest::is_ok(described));
        if (itest::is_ok(described))
        {
            wire_describe = described->at("result").at("data");
            // RPC-over-the-wire ≡ registry, byte-for-byte: the daemon serves the SAME contract the
            // registry generates — no hand-maintained parity anywhere (R-CLI-009). Both sides are
            // normalized through one parse→dump cycle so the comparison measures CONTENT, immune to
            // any original-vs-round-tripped formatting asymmetry.
            CHECK(wire_describe.dump() == Json::parse(registry_dump).dump());

            // The attach handshake's negotiated capabilities are drawn from the introspected
            // protocol descriptor — handshake ≡ introspection.
            const Json& proto_caps = wire_describe.at("contract").at("protocol").at("capabilities");
            std::set<std::string> advertised;
            for (std::size_t i = 0; i < proto_caps.size(); ++i)
                advertised.insert(proto_caps.at(i).as_string());
            const Json& negotiated = attached->at("result").at("capabilities");
            for (std::size_t i = 0; i < negotiated.size(); ++i)
                CHECK(advertised.count(negotiated.at(i).as_string()) == 1);
        }

        const auto stopped = rpc.call("shutdown", Json::object());
        CHECK(itest::is_ok(stopped));
        rpc.close();

        int daemon_code = -1;
        const bool daemon_done = ctest_proc::wait_for(daemon, 15000, daemon_code);
        if (!daemon_done)
            ctest_proc::kill(daemon);
        ctest_proc::release(daemon);
        CHECK(daemon_done);

        std::error_code ec;
        fs::remove_all(project, ec);
    }
    CHECK(!wire_describe.is_null());

    // --- surfaces 3+4: the MCP + CLI/RPC projections are per-verb projections of the same verbs --
    const std::size_t n = reg.verbs().size();
    const Json cli_surface = reg.cli_surface();
    const Json rpc_surface = reg.rpc_surface();
    const Json mcp_surface = reg.mcp_surface();
    CHECK(cli_surface.size() == n);
    CHECK(rpc_surface.size() == n);
    CHECK(mcp_surface.size() == n);
    if (!wire_describe.is_null())
    {
        const Json& wire_verbs = wire_describe.at("contract").at("verbs");
        const Json& wire_rpc = wire_describe.at("contract").at("rpcMethods");
        const Json& wire_mcp = wire_describe.at("contract").at("mcpTools");
        CHECK(wire_verbs.size() == n);
        CHECK(wire_rpc.size() == n);
        CHECK(wire_mcp.size() == n);
        for (std::size_t i = 0; i < n; ++i)
        {
            // Per-verb identity lines up across every surface a client can reach.
            CHECK(wire_verbs.at(i).at("rpcMethod").as_string() ==
                  rpc_surface.at(i).at("method").as_string());
            CHECK(wire_mcp.at(i).at("tool").as_string() ==
                  mcp_surface.at(i).at("tool").as_string());
            CHECK(wire_verbs.at(i).at("verb").as_string() ==
                  cli_surface.at(i).at("verb").as_string());
        }
    }

    // --- additive-only (R-CLI-008): the WIRE-visible catalog still carries the frozen baseline ---
    if (!wire_describe.is_null())
    {
        const std::set<std::string> wire_codes = catalog_codes_of(wire_describe);
        for (const std::string& frozen : baseline_v0_codes())
            CHECK(wire_codes.count(frozen) == 1); // nothing shipped in v0 ever disappears

        // The exit-code table survives the wire: the permission class (6) criterion 5 denies with,
        // and the protocol class (7) criterion 2 hard-fails with.
        CHECK(wire_exit_code_for(wire_describe, "scope.denied") == 6);
        CHECK(wire_exit_code_for(wire_describe, "scope.insufficient") == 6);
        CHECK(wire_exit_code_for(wire_describe, "handshake.incompatible_protocol") == 7);
    }

    // The in-process enforcement point CI calls (test_error_catalog + here): nothing missing today.
    CHECK(missing_from_catalog(baseline_v0_codes(), catalog()).empty());

    // --- failure path: the enforcement actually CATCHES a removal (not vacuously green) ----------
    {
        std::vector<ErrorCode> mutilated = catalog();
        // "Remove" scope.denied — exactly the violation the additive-only rule forbids.
        std::vector<ErrorCode> without;
        for (const ErrorCode& e : mutilated)
            if (e.code != "scope.denied")
                without.push_back(e);
        const std::vector<std::string> missing = missing_from_catalog(baseline_v0_codes(), without);
        CHECK(missing.size() == 1);
        CHECK(!missing.empty() && missing.front() == "scope.denied");
    }

    ITEST_MAIN_END();
}
