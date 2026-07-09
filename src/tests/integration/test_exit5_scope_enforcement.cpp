// M1 exit criterion 5 — scope enforcement (R-SEC-007, issue #36):
// a read/query-scoped token cannot install a package or trigger a build via DIRECT RPC — the
// denial happens in the DISPATCHER (adapter-level tool filtering is bypassable via direct RPC,
// which is exactly the door this test uses) and maps to the catalog `scope.denied` with
// permission-class exit code 6 (the exit-wire integration of #31, verified here end-to-end over
// the REAL IPC transport of #35).
//
//   * daemon A boots with `--launch-scopes read` (the operator ceiling): a client REQUESTING
//     write/session/build is clamped to the read/query baseline (least privilege), and every
//     mutating method — package.add (install), build (trigger a build), edit, edit-batch — is
//     refused with `scope.denied` BEFORE the verb resolves (an under-scoped token cannot even
//     learn whether the verb is implemented: never `contract.unimplemented`). Reads (query,
//     describe, snapshot) still work.
//   * exit-class 6 — asserted from the daemon's own wire-visible catalog AND the in-process
//     exit-code table, and `bridge::kScopeDeniedCode` is the same catalog string.
//   * daemon B (default ceiling: all scopes) is the control: the SAME wire calls succeed when the
//     scope is granted — proving the ceiling (not a missing backing) produced the denials — and a
//     client that requests NO scopes stays read-only by default (unrecognized-client rule).
//
// R-QA-013: failure paths (the four denials + default-deny) + happy-path controls + on-disk
// no-side-effect proof.

#include "context/editor/bridge/scope.h"
#include "context/editor/contract/error_catalog.h"
#include "context/editor/contract/json.h"

#include "integration_test.h"
#include "process_util.h"

#include <string>

namespace fs = std::filesystem;
using itest::Json;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace
{
Json edit_params(const std::string& path, const std::string& content)
{
    Json p = Json::object();
    p.set("path", Json(path));
    p.set("content", Json(content));
    return p;
}

Json batch_params(const std::string& path, const std::string& content)
{
    Json f = Json::object();
    f.set("path", Json(path));
    f.set("content", Json(content));
    Json files = Json::array();
    files.push_back(std::move(f));
    Json p = Json::object();
    p.set("files", std::move(files));
    return p;
}
} // namespace

int main()
{
    const std::string bin = CONTEXT_BINARY;

    // The bridge's denial constant IS the catalog code, and the catalog classes it as permission
    // exit 6 (the R-CLI-008 exit-code table) — the in-process half of the exit-wire assertion.
    CHECK(std::string(context::editor::bridge::kScopeDeniedCode) == "scope.denied");
    CHECK(context::editor::contract::exit_code_for("scope.denied") == 6);
    CHECK(context::editor::contract::exit_code_for("scope.insufficient") == 6);

    // ============================================================================================
    // Daemon A — operator ceiling `read`: everything mutating is denied at the dispatcher.
    // ============================================================================================
    {
        const fs::path project = itest::make_temp_project("scope-read");
        ctest_proc::Process daemon = ctest_proc::spawn(
            bin, {"daemon", "--project", project.string(), "--launch-scopes", "read"});
        CHECK(ctest_proc::valid(daemon));
        CHECK(itest::wait_for_instance(project, 15000));

        itest::RpcClient rpc;
        CHECK(rpc.connect(project));

        // The client ASKS for everything; the launch-time ceiling clamps it to read/query only.
        const auto attached = rpc.attach(1, {"describe"}, "write,session,build");
        CHECK(itest::is_ok(attached));
        if (itest::is_ok(attached))
        {
            const Json& scopes = attached->at("result").at("scopes");
            CHECK(scopes.size() == 1);
            CHECK(scopes.size() == 1 && scopes.at(0).as_string() == "read-query");
        }

        // --- the criterion, verbatim: install a package via DIRECT RPC -> scope.denied ----------
        Json pkg = Json::object();
        pkg.set("name", Json(std::string("evil-package")));
        const auto install = rpc.call("package.add", std::move(pkg));
        CHECK(install.has_value());
        CHECK(!itest::is_ok(install));
        CHECK(itest::error_code_of(install) == "scope.denied");

        // --- and trigger a build via DIRECT RPC -> scope.denied ---------------------------------
        const auto build = rpc.call("build", Json::object());
        CHECK(build.has_value());
        CHECK(!itest::is_ok(build));
        CHECK(itest::error_code_of(build) == "scope.denied");

        // --- file writes (single + intent-logged batch) are denied too --------------------------
        const auto edit = rpc.call("edit", edit_params("proj/deny.scene", "entity: 666"));
        CHECK(!itest::is_ok(edit));
        CHECK(itest::error_code_of(edit) == "scope.denied");
        const auto batch =
            rpc.call("edit-batch", batch_params("proj/deny-batch.scene", "entity: 666"));
        CHECK(!itest::is_ok(batch));
        CHECK(itest::error_code_of(batch) == "scope.denied");

        // --- session control is out of scope as well --------------------------------------------
        const auto stop_denied = rpc.call("shutdown", Json::object());
        CHECK(!itest::is_ok(stop_denied));
        CHECK(itest::error_code_of(stop_denied) == "scope.denied");

        // No side effects reached disk: the denials happened BEFORE any backing ran.
        CHECK(itest::read_file(project / "proj" / "deny.scene").empty());
        CHECK(itest::read_file(project / "proj" / "deny-batch.scene").empty());

        // --- reads still work (the baseline every token holds) ----------------------------------
        Json q = Json::object();
        q.set("path", Json(std::string("proj/deny.scene")));
        const auto query = rpc.call("query", std::move(q));
        CHECK(itest::is_ok(query));
        if (itest::is_ok(query))
            CHECK(!query->at("result").at("data").at("present").as_bool());
        const auto snap = rpc.call("snapshot", Json::object());
        CHECK(itest::is_ok(snap));

        // --- exit-class 6, read from the DAEMON'S OWN wire-visible catalog ----------------------
        const auto described = rpc.call("describe", Json::object());
        CHECK(itest::is_ok(described));
        if (itest::is_ok(described))
        {
            const Json& catalog =
                described->at("result").at("data").at("contract").at("errorCatalog");
            bool found = false;
            for (std::size_t i = 0; i < catalog.size(); ++i)
            {
                if (catalog.at(i).at("code").as_string() != "scope.denied")
                    continue;
                found = true;
                CHECK(catalog.at(i).at("exitCode").as_int() == 6); // permission class, R-SEC-007
                CHECK(!catalog.at(i).at("retriable").as_bool());   // a denial is deterministic
            }
            CHECK(found);
        }
        rpc.close();

        // A read-ceiling daemon cannot be shut down over the wire BY DESIGN — teardown is the
        // test's job (kill + reap; this is scaffolding, not the behavior under test).
        ctest_proc::kill(daemon);
        int code = -1;
        (void)ctest_proc::wait_for(daemon, 15000, code);
        ctest_proc::release(daemon);

        std::error_code ec;
        fs::remove_all(project, ec);
    }

    // ============================================================================================
    // Daemon B — default ceiling (all scopes): the control + the default-deny rule.
    // ============================================================================================
    {
        const fs::path project = itest::make_temp_project("scope-all");
        ctest_proc::Process daemon =
            ctest_proc::spawn(bin, {"daemon", "--project", project.string()});
        CHECK(ctest_proc::valid(daemon));
        CHECK(itest::wait_for_instance(project, 15000));

        // Control: WITH the file_write scope granted, the same wire `edit` succeeds — so daemon A's
        // denials were the dispatcher's ceiling clamp, not a missing backing.
        {
            itest::RpcClient rpc;
            CHECK(rpc.connect(project));
            const auto attached = rpc.attach(1, {"describe"}, "write");
            CHECK(itest::is_ok(attached));
            const auto edit = rpc.call("edit", edit_params("proj/allow.scene", "entity: 1"));
            CHECK(itest::is_ok(edit));
            if (itest::is_ok(edit))
                CHECK(edit->at("result").at("data").at("reflected").as_bool());
            CHECK(itest::read_file(project / "proj" / "allow.scene") == "entity: 1");

            // build+install stays denied even for a write-scoped token (least privilege per
            // scope FAMILY, not a single "privileged" bit).
            const auto install = rpc.call("package.add", Json::object());
            CHECK(!itest::is_ok(install));
            CHECK(itest::error_code_of(install) == "scope.denied");
            rpc.close();
        }

        // Default-deny: a client that requests NOTHING is the unrecognized-client baseline —
        // read/query only, so writes are refused even though the ceiling would allow them.
        {
            itest::RpcClient rpc;
            CHECK(rpc.connect(project));
            const auto attached = rpc.attach(1, {"describe"}, "");
            CHECK(itest::is_ok(attached));
            const auto edit = rpc.call("edit", edit_params("proj/default-deny.scene", "entity: 2"));
            CHECK(!itest::is_ok(edit));
            CHECK(itest::error_code_of(edit) == "scope.denied");
            CHECK(itest::read_file(project / "proj" / "default-deny.scene").empty());
            rpc.close();
        }

        // Clean shutdown with an explicitly-granted session scope.
        {
            itest::RpcClient rpc;
            CHECK(rpc.connect(project));
            const auto attached = rpc.attach(1, {"describe"}, "session");
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
    }

    ITEST_MAIN_END();
}
