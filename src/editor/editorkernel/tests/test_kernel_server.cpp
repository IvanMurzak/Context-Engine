// KernelServer over the real wire (M1 issue #34): a composed EditorKernel hosted on a
// bridge::TransportServer (one thread) driven by a bridge::TransportClient (main thread), in one
// process. Proves the operational verbs the daemon serves over the transport — the SAME dispatcher a
// cross-process client hits — without spawning processes (that is the cli e2e test's job):
//   * attach handshake over the wire;
//   * `edit` writes through the composed kernel + the read-your-writes barrier reflects it (R-CLI-006);
//   * `query` reads the derived node back, byte-consistent with the edit's canonical hash;
//   * a READ-ONLY session is refused `edit` with scope.denied (R-SEC-007) over the wire;
//   * `describe` routes to the registry through the same dispatcher;
//   * `shutdown` breaks the serve loop cleanly (the thread joins without a kill);
//   * the R-CLI-017 large-result path END-TO-END: an oversized response is spooled + replaced by a
//     `largeResult` handle envelope, and resource.read range-fetches reassemble the EXACT original
//     result over the same channel (plus the unknown/malformed-handle failure paths).

#include "context/editor/editorkernel/kernel_server.h"

#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"
#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/editorkernel/editor_kernel.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/watcher.h"
#include "context/editor/bridge/resource_store.h"
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
    cfg.index_path = "proj/.editor/index";

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

    // ---- ceiling clamp: a daemon launched with a restricted --launch-scopes clamps a wire client
    //      that requests MORE (R-SEC-007 least privilege over the wire). Regression guard: without the
    //      dispatcher-level ceiling the wire attach path (handle -> attach) would grant the requested
    //      scope verbatim, letting any local process escalate past the operator's launch ceiling.
    {
        const fs::path project2 = make_temp_project();
        MemoryFileStore store2;
        NullWatcher watcher2;
        context::kernel::ManualClock clock2;
        context::kernel::InlineTaskRunner tasks2;

        EditorKernelConfig cfg2;
        cfg2.project_root = project2;
        cfg2.filesync_root = "proj";
        cfg2.index_path = "proj/.editor/index";

        EditorKernel kernel2(store2, watcher2, clock2, tasks2, cfg2);
        KernelServer server2(kernel2);
        // Launch ceiling = session-control only (NO file-write): `context daemon --launch-scopes session`.
        CHECK(kernel2.start(ScopeSet::parse("session")) == StartOutcome::booted);

        TransportServer transport2(endpoint_for(
            "ctx-kernelserver-ceiling-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
        CHECK(transport2.listen());

        std::thread srv2([&server2, &transport2]() { server2.serve(transport2); });

        {
            TransportClient esc(transport2.endpoint());
            CHECK(esc.connect(3000));

            // The client ASKS for write,session — the ceiling must clamp file-write away.
            const std::optional<std::string> a =
                esc.request(rpc(1, "attach", attach_params("write,session")));
            CHECK(a.has_value());
            const Json attached = Json::parse(*a);
            CHECK(attached.contains("result"));
            const Json& granted = attached.at("result").at("scopes");
            bool granted_file_write = false;
            for (std::size_t i = 0; i < granted.size(); ++i)
                if (granted.at(i).as_string() == "file-write")
                    granted_file_write = true;
            CHECK(!granted_file_write); // clamped away by the launch ceiling

            // And the clamp is enforced end-to-end: edit is denied though the client requested write.
            Json ep = Json::object();
            ep.set("path", Json(std::string("proj/a.scene")));
            ep.set("content", Json(std::string("entity: 2")));
            const std::optional<std::string> denied = esc.request(rpc(2, "edit", std::move(ep)));
            CHECK(denied.has_value());
            const Json resp = Json::parse(*denied);
            CHECK(resp.contains("error"));
            CHECK(resp.at("error").at("data").at("code").as_string() == "scope.denied");

            // session-control SURVIVES the clamp, so shutdown is accepted and the serve loop stops.
            const std::optional<std::string> s = esc.request(rpc(3, "shutdown", Json::object()));
            CHECK(s.has_value());
            const Json stop_resp = Json::parse(*s);
            CHECK(stop_resp.at("result").at("data").at("stopping").as_bool());
            esc.close();
        }

        srv2.join();
        kernel2.stop();
        std::error_code ec2;
        fs::remove_all(project2, ec2);
    }

    // ---- R-CLI-017 large-result round-trip over the wire: an oversized response returns a handle
    //      (never an oversized frame), and resource.read RANGE fetches reassemble the EXACT original
    //      result envelope. The spool threshold is lowered via the operational knob so `describe`
    //      (a few KB) exercises the oversized path deterministically.
    {
        const fs::path project3 = make_temp_project();
        MemoryFileStore store3;
        NullWatcher watcher3;
        context::kernel::ManualClock clock3;
        context::kernel::InlineTaskRunner tasks3;

        EditorKernelConfig cfg3;
        cfg3.project_root = project3;
        cfg3.filesync_root = "proj";
        cfg3.index_path = "proj/.editor/index";

        EditorKernel kernel3(store3, watcher3, clock3, tasks3, cfg3);
        KernelServer server3(kernel3);
        server3.set_large_result_threshold(512); // force the spool on a routine describe
        CHECK(kernel3.start(ScopeSet::all()) == StartOutcome::booted);

        TransportServer transport3(endpoint_for(
            "ctx-kernelserver-largeresult-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
        CHECK(transport3.listen());

        std::thread srv3([&server3, &transport3]() { server3.serve(transport3); });

        {
            TransportClient lr(transport3.endpoint());
            CHECK(lr.connect(3000));
            CHECK(lr.request(rpc(1, "attach", attach_params("session"))).has_value());

            // What the daemon WOULD have returned inline (the in-process registry projection).
            const std::string expected_result =
                context::editor::contract::Envelope::success(
                    context::editor::contract::Registry::instance().describe())
                    .to_json()
                    .dump(0);
            CHECK(expected_result.size() > 512); // precondition: describe exceeds the threshold

            // The oversized response came back as a SMALL largeResult envelope, not inline data.
            const std::optional<std::string> d = lr.request(rpc(2, "describe", Json::object()));
            CHECK(d.has_value());
            const Json resp = Json::parse(*d);
            const Json& result = resp.at("result");
            CHECK(result.at("ok").as_bool());
            CHECK(!result.at("data").contains("contract")); // NOT inline
            const Json& lr_obj = result.at("data").at("largeResult");
            const std::string uri = lr_obj.at("handle").as_string();
            CHECK(!uri.empty());
            const std::uint64_t total =
                static_cast<std::uint64_t>(lr_obj.at("sizeBytes").as_int());
            CHECK(total == expected_result.size());
            CHECK(lr_obj.contains("localPath")); // the same-FS hint (spool file on real disk)

            // RANGE fetch loop: deliberately small chunks, not a divisor of the total.
            std::string reassembled;
            std::uint64_t offset = 0;
            int id = 10;
            for (;;)
            {
                Json range = Json::object();
                range.set("offsetBytes", Json(offset));
                range.set("lengthBytes", Json(static_cast<std::uint64_t>(777)));
                Json rp = Json::object();
                rp.set("handle", Json(uri));
                rp.set("range", std::move(range));
                const std::optional<std::string> chunk_resp =
                    lr.request(rpc(++id, "resource.read", std::move(rp)));
                CHECK(chunk_resp.has_value());
                if (!chunk_resp.has_value())
                    break;
                const Json chunk = Json::parse(*chunk_resp);
                const Json& cdata = chunk.at("result").at("data");
                CHECK(static_cast<std::uint64_t>(cdata.at("totalBytes").as_int()) == total);
                const auto bytes = context::editor::bridge::hex_decode(
                    cdata.at("chunkHex").as_string());
                CHECK(bytes.has_value());
                if (!bytes.has_value())
                    break;
                CHECK(static_cast<std::uint64_t>(cdata.at("offsetBytes").as_int()) == offset);
                reassembled += *bytes;
                offset += bytes->size();
                if (cdata.at("eof").as_bool())
                    break;
            }
            // Byte-exact reassembly of the ORIGINAL result envelope (the R-QA-013 round-trip).
            CHECK(reassembled == expected_result);
            const Json fetched = Json::parse(reassembled);
            CHECK(fetched.at("ok").as_bool());
            CHECK(fetched.at("data").at("contract").contains("protocol"));

            // Failure paths over the wire: unknown + malformed handles.
            {
                Json rp = Json::object();
                rp.set("handle", Json(std::string("context-res://v0/other-instance/0?bytes=1")));
                const std::optional<std::string> unknown =
                    lr.request(rpc(++id, "resource.read", std::move(rp)));
                CHECK(unknown.has_value());
                const Json u = Json::parse(*unknown);
                CHECK(u.contains("error"));
                CHECK(u.at("error").at("data").at("code").as_string() ==
                      "resource.unknown_handle");

                Json rp2 = Json::object();
                rp2.set("handle", Json(std::string("not-a-resource-uri")));
                const std::optional<std::string> malformed =
                    lr.request(rpc(++id, "resource.read", std::move(rp2)));
                CHECK(malformed.has_value());
                const Json m = Json::parse(*malformed);
                CHECK(m.contains("error"));
                CHECK(m.at("error").at("data").at("code").as_string() ==
                      "resource.unknown_handle");
            }

            // A resource.read chunk reply is NEVER re-spooled even though it exceeds the tiny
            // threshold (the anti-recursion guard): fetching with no range returns the whole
            // payload in one over-threshold chunk, still inline.
            {
                Json rp = Json::object();
                rp.set("handle", Json(uri));
                const std::optional<std::string> whole =
                    lr.request(rpc(++id, "resource.read", std::move(rp)));
                CHECK(whole.has_value());
                const Json w = Json::parse(*whole);
                const Json& wdata = w.at("result").at("data");
                CHECK(wdata.contains("chunkHex")); // inline chunk, not a nested largeResult
                CHECK(wdata.at("eof").as_bool());
            }

            const std::optional<std::string> s =
                lr.request(rpc(++id, "shutdown", Json::object()));
            CHECK(s.has_value());
            lr.close();
        }

        srv3.join();
        kernel3.stop();
        std::error_code ec3;
        fs::remove_all(project3, ec3);
    }

    EDITORKERNEL_TEST_MAIN_END();
}
