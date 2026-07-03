// KernelServer over the real wire (M1 issue #34): a composed EditorKernel hosted on a
// bridge::TransportServer (one thread) driven by a bridge::TransportClient (main thread), in one
// process. Proves the operational verbs the daemon serves over the transport — the SAME dispatcher a
// cross-process client hits — without spawning processes (that is the cli e2e test's job):
//   * attach handshake over the wire;
//   * `edit` writes through the composed kernel + the read-your-writes barrier reflects it (R-CLI-006);
//   * `query` reads the derived node back, byte-consistent with the edit's canonical hash;
//   * a READ-ONLY session is refused `edit` with scope.denied (R-SEC-007) over the wire;
//   * `describe` routes to the registry through the same dispatcher;
//   * `shutdown` breaks the serve loop cleanly (the thread joins without a kill).

#include "context/editor/editorkernel/kernel_server.h"

#include "context/editor/contract/json.h"
#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/editorkernel/editor_kernel.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/watcher.h"
#include "context/editor/bridge/transport.h"
#include "context/kernel/platform.h"

#include "editorkernel_test.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using context::editor::contract::Json;
using context::editor::derivation::canonical_parse;
using context::editor::editorkernel::EditorKernel;
using context::editor::editorkernel::EditorKernelConfig;
using context::editor::editorkernel::KernelServer;
using context::editor::filesync::MemoryFileStore;
using context::editor::filesync::NullWatcher;
using context::editor::bridge::endpoint_for;
using context::editor::bridge::ScopeSet;
using context::editor::bridge::StartOutcome;
using context::editor::bridge::TransportClient;
using context::editor::bridge::TransportServer;

namespace
{
fs::path make_temp_project()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("ctx-kernelserver-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string rpc(std::int64_t id, const std::string& method, Json params)
{
    Json req = Json::object();
    req.set("jsonrpc", Json(std::string("2.0")));
    req.set("id", Json(id));
    req.set("method", Json(method));
    req.set("params", std::move(params));
    return req.dump(0);
}

Json attach_params(const std::string& scope)
{
    Json p = Json::object();
    p.set("protocolMajor", Json(static_cast<std::uint64_t>(0)));
    p.set("scope", Json(scope));
    return p;
}
} // namespace

int main()
{
    const fs::path project = make_temp_project();

    MemoryFileStore store; // the wire + backend are under test here, not disk (native disk = e2e test)
    NullWatcher watcher;
    context::kernel::ManualClock clock;
    context::kernel::InlineTaskRunner tasks;

    EditorKernelConfig cfg;
    cfg.project_root = project;
    cfg.filesync_root = "proj";
    cfg.index_path = "proj/.editor/reconcile-index";

    EditorKernel kernel(store, watcher, clock, tasks, cfg);
    KernelServer server(kernel); // registers the method backend BEFORE start()
    CHECK(kernel.start(ScopeSet::all()) == StartOutcome::booted);

    TransportServer transport(endpoint_for(
        "ctx-kernelserver-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    CHECK(transport.listen());

    std::thread srv([&server, &transport]() { server.serve(transport); });

    // ---- failure path first: a READ-ONLY session cannot edit (R-SEC-007 over the wire) -----------
    {
        TransportClient ro(transport.endpoint());
        CHECK(ro.connect(3000));

        const std::optional<std::string> a = ro.request(rpc(1, "attach", attach_params("read")));
        CHECK(a.has_value());

        Json ep = Json::object();
        ep.set("path", Json(std::string("proj/a.scene")));
        ep.set("content", Json(std::string("entity: 9")));
        const std::optional<std::string> denied = ro.request(rpc(2, "edit", std::move(ep)));
        CHECK(denied.has_value());
        const Json resp = Json::parse(*denied);
        CHECK(resp.contains("error"));
        CHECK(resp.at("error").contains("data"));
        CHECK(resp.at("error").at("data").at("code").as_string() == "scope.denied");
        ro.close();
    }

    // ---- happy path: a write session edits + queries + read-your-writes, then shuts down ----------
    const std::uint64_t expected_hash = canonical_parse("entity: 1").canonical_hash;
    {
        TransportClient rw(transport.endpoint());
        CHECK(rw.connect(3000));

        const std::optional<std::string> a = rw.request(rpc(1, "attach", attach_params("write,session")));
        CHECK(a.has_value());
        const Json attached = Json::parse(*a);
        CHECK(attached.contains("result"));

        // edit
        Json ep = Json::object();
        ep.set("path", Json(std::string("proj/a.scene")));
        ep.set("content", Json(std::string("entity: 1")));
        const std::optional<std::string> e = rw.request(rpc(2, "edit", std::move(ep)));
        CHECK(e.has_value());
        const Json edit_resp = Json::parse(*e);
        CHECK(edit_resp.contains("result"));
        const Json& edit_data = edit_resp.at("result").at("data");
        CHECK(edit_resp.at("result").at("ok").as_bool());
        CHECK(edit_data.at("reflected").as_bool());
        CHECK(edit_data.at("worldEntities").as_int() == 1);
        CHECK(edit_data.at("canonicalHash").as_string() == std::to_string(expected_hash));

        // query — read-your-writes: the derived node matches the edit's canonical hash
        Json qp = Json::object();
        qp.set("path", Json(std::string("proj/a.scene")));
        const std::optional<std::string> q = rw.request(rpc(3, "query", std::move(qp)));
        CHECK(q.has_value());
        const Json query_resp = Json::parse(*q);
        const Json& query_data = query_resp.at("result").at("data");
        CHECK(query_data.at("present").as_bool());
        CHECK(query_data.at("canonicalHash").as_string() == std::to_string(expected_hash));
        CHECK(query_data.at("worldEntities").as_int() == 1);

        // describe routes to the registry through the same dispatcher
        const std::optional<std::string> d = rw.request(rpc(4, "describe", Json::object()));
        CHECK(d.has_value());
        const Json desc_resp = Json::parse(*d);
        CHECK(desc_resp.contains("result"));
        CHECK(desc_resp.at("result").at("ok").as_bool());
        CHECK(desc_resp.at("result").at("data").at("contract").contains("protocol"));

        // shutdown — breaks the serve loop (the thread joins below without a kill)
        const std::optional<std::string> s = rw.request(rpc(5, "shutdown", Json::object()));
        CHECK(s.has_value());
        const Json stop_resp = Json::parse(*s);
        CHECK(stop_resp.at("result").at("data").at("stopping").as_bool());
        rw.close();
    }

    srv.join();
    CHECK(server.stop_requested());

    kernel.stop();
    std::error_code ec;
    fs::remove_all(project, ec);

    EDITORKERNEL_TEST_MAIN_END();
}
