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
#include "context/editor/contract/handshake.h" // kProtocolMajor (the frozen major the daemon accepts)
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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
    p.set("protocolMajor",
          Json(static_cast<std::uint64_t>(context::editor::contract::kProtocolMajor)));
    p.set("scope", Json(scope));
    return p;
}

// attach params carrying a D20 token (for the auth-over-the-wire sub-test).
Json attach_params_tok(const std::string& scope, const std::string& token)
{
    Json p = attach_params(scope);
    p.set("token", Json(token));
    return p;
}

// A subscribe request over ALL topics (empty topics list => every topic).
std::string subscribe_all(std::int64_t id)
{
    Json p = Json::object();
    p.set("topics", Json::array());
    return rpc(id, "subscribe", std::move(p));
}

// An `edit` request writing `content` to `path`.
std::string edit_req(std::int64_t id, const std::string& path, const std::string& content)
{
    Json p = Json::object();
    p.set("path", Json(path));
    p.set("content", Json(content));
    return rpc(id, "edit", std::move(p));
}

// A single-file `edit-batch` request. Unlike a plain `edit`, a batch runs a settle() that publishes a
// `derivation` event — so a burst of these produces a burst of pushed events (used to overflow a slow
// subscriber's queue deterministically).
std::string editbatch_req(std::int64_t id, const std::string& path, const std::string& content)
{
    Json f = Json::object();
    f.set("path", Json(path));
    f.set("content", Json(content));
    Json files = Json::array();
    files.push_back(std::move(f));
    Json p = Json::object();
    p.set("files", std::move(files));
    return rpc(id, "edit-batch", std::move(p));
}

// Frame classification for a server->client push consumer: is a received frame an `event` push, an
// `event.gap` re-snapshot marker, or a normal id-bearing response?
bool is_event_frame(const Json& f)
{
    return f.is_object() && f.contains("method") && f.at("method").is_string() &&
           f.at("method").as_string() == "event";
}
bool is_gap_frame(const Json& f)
{
    return f.is_object() && f.contains("method") && f.at("method").is_string() &&
           f.at("method").as_string() == "event.gap";
}
bool is_response_frame(const Json& f)
{
    return f.is_object() && (f.contains("result") || f.contains("error"));
}

// Send `req` on a SUBSCRIBED client and read frames until the response whose id == `id` arrives,
// appending every pushed event/gap frame seen along the way to `events`. This is the mandatory
// subscription-consumer pattern: a subscribed client must NEVER use request() (which mistakes an
// async pushed event for the response) — it demuxes by frame shape + id.
std::optional<Json> req_demux(TransportClient& c, std::int64_t id, const std::string& req,
                              std::vector<Json>& events)
{
    if (!c.send(req))
        return std::nullopt;
    for (int i = 0; i < 100000; ++i)
    {
        const std::optional<std::string> f = c.receive();
        if (!f.has_value())
            return std::nullopt;
        const Json parsed = Json::parse(*f);
        if (is_response_frame(parsed) && parsed.contains("id") && parsed.at("id").as_int() == id)
            return parsed;
        if (is_event_frame(parsed) || is_gap_frame(parsed))
            events.push_back(parsed);
    }
    return std::nullopt;
}

// Count `clients` attach/detach events among collected event frames.
void count_client_events(const std::vector<Json>& events, int& attached, int& detached)
{
    attached = 0;
    detached = 0;
    for (const Json& e : events)
    {
        if (!is_event_frame(e))
            continue;
        const Json& inner = e.at("params").at("event");
        if (inner.at("topic").as_string() != "clients")
            continue;
        const std::string kind = inner.at("payload").at("event").as_string();
        if (kind == "attached")
            ++attached;
        else if (kind == "detached")
            ++detached;
    }
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

        // ---- editor.scene-tree / editor.inspect (M9 e05d3): the composed panel reads -------------
        // Served over the SAME disk-backed compose path `context set` plans against, so the fixture
        // is REAL files under the project root (the MemoryFileStore above backs the derivation
        // pipeline, not composition). Root instances child + overrides the child camera's fov —
        // instances/overrides must be VISIBLE in the answered models (L-35).
        {
            const auto write_file = [&](const char* name, const std::string& text)
            {
                std::ofstream out(project / name, std::ios::binary);
                out << text;
            };
            write_file("child.scene.json", R"({
              "$schema": "ctx:scene", "version": 1,
              "entities": [
                {"id": "ccccccccccccccc1", "name": "Cam",
                 "components": {
                   "transform": {"position": [1, 2, 3]},
                   "camera": {"fov": 1.0, "near": 0.1, "far": 500.0}
                 }}
              ]})");
            write_file("root.scene.json", R"({
              "$schema": "ctx:scene", "version": 1,
              "entities": [],
              "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"}],
              "overrides": [
                {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"],
                 "pointer": "/components/camera/fov", "value": 1.4, "base": 1.0}
              ]})");

            // scene-tree: the composed hierarchy, instance boundary + overridden entity visible.
            Json stp = Json::object();
            stp.set("path", Json(std::string("root.scene.json")));
            const std::optional<std::string> st = rw.request(rpc(6, "editor.scene-tree", std::move(stp)));
            CHECK(st.has_value());
            const Json st_resp = Json::parse(*st);
            CHECK(st_resp.contains("result"));
            const Json& tree = st_resp.at("result").at("data").at("sceneTree");
            CHECK(tree.at("ok").as_bool());
            CHECK(tree.at("entityCount").as_int() == 1);
            CHECK(tree.at("roots").size() == 1u);
            const Json& boundary = tree.at("roots").at(std::size_t{0});
            CHECK(boundary.at("identity").as_string() == "aaaaaaaaaaaaaaa1");
            CHECK(boundary.at("kind").as_string() == "instance");
            const Json& cam = boundary.at("children").at(std::size_t{0});
            CHECK(cam.at("identity").as_string() == "aaaaaaaaaaaaaaa1/ccccccccccccccc1");
            CHECK(cam.at("displayName").as_string() == "Cam");
            CHECK(cam.at("overridden").as_bool()); // the L-35 override is visible, not hidden

            // inspect: the schema-driven field set with the override applied + marked, plus the
            // root scene's raw-hash CAS token.
            Json ip = Json::object();
            ip.set("path", Json(std::string("root.scene.json")));
            ip.set("idPath", Json(std::string("aaaaaaaaaaaaaaa1/ccccccccccccccc1")));
            const std::optional<std::string> insp = rw.request(rpc(7, "editor.inspect", std::move(ip)));
            CHECK(insp.has_value());
            const Json insp_resp = Json::parse(*insp);
            CHECK(insp_resp.contains("result"));
            const Json& idata = insp_resp.at("result").at("data");
            const Json& imodel = idata.at("inspector");
            CHECK(imodel.at("present").as_bool());
            CHECK(imodel.at("identity").as_string() == "aaaaaaaaaaaaaaa1/ccccccccccccccc1");
            CHECK(imodel.at("kindId").as_string() == "ctx:scene");
            CHECK(imodel.at("overrideCount").as_int() == 1);
            bool saw_fov = false;
            for (std::size_t i = 0; i < imodel.at("fields").size(); ++i)
            {
                const Json& field = imodel.at("fields").at(i);
                if (field.at("pointer").as_string() == "/components/camera/fov")
                {
                    saw_fov = true;
                    CHECK(field.at("overridden").as_bool());
                    CHECK(field.at("value").as_string() == "1.4"); // the COMPOSED (overridden) value
                }
            }
            CHECK(saw_fov);
            CHECK(!idata.at("rawHash").as_string().empty());
            CHECK(idata.at("rawHash").as_string() != "0"); // the file exists -> a real CAS token

            // inspect an UNKNOWN id-path: present:false — a renderable state, not a protocol fault.
            Json missing = Json::object();
            missing.set("path", Json(std::string("root.scene.json")));
            missing.set("idPath", Json(std::string("does-not-exist")));
            const std::optional<std::string> none = rw.request(rpc(8, "editor.inspect", std::move(missing)));
            CHECK(none.has_value());
            const Json none_resp = Json::parse(*none);
            CHECK(!none_resp.at("result").at("data").at("inspector").at("present").as_bool());

            // inspect with a JAIL-ESCAPING path (R-SEC-008): the raw-hash CAS read must not become
            // an out-of-project existence/content-hash oracle. The `..` path here names the very
            // same on-disk root.scene.json that answered a real token above — escaping form alone
            // must degrade it to the honest no-token "0" (and the composed model to present:false).
            Json escaping = Json::object();
            escaping.set("path", Json(std::string("../") + project.filename().string() +
                                      "/root.scene.json"));
            escaping.set("idPath", Json(std::string("aaaaaaaaaaaaaaa1/ccccccccccccccc1")));
            const std::optional<std::string> jailed =
                rw.request(rpc(9, "editor.inspect", std::move(escaping)));
            CHECK(jailed.has_value());
            const Json jailed_resp = Json::parse(*jailed);
            CHECK(jailed_resp.at("result").at("data").at("rawHash").as_string() == "0");
            CHECK(!jailed_resp.at("result").at("data").at("inspector").at("present").as_bool());
        }

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

    // ============================================================================================
    // M9 D19/D20: multi-client concurrent fan-in + event fan-out + slow-client gap + bounds + auth.
    // Each sub-test boots its own daemon over the real wire on a serve thread (in-process, so real
    // threads exercise the concurrency — the TSan CI leg runs this suite). A subscribing client uses
    // send()/receive() and demuxes response vs pushed `event`/`event.gap` frames.
    // ============================================================================================

    // Drain pushed frames from a subscribed client by sending a read-only `query` MARKER and reading
    // until its response; returns the pushed event/gap frames that arrived first. Deterministic FIFO
    // synchronization: every frame the daemon queued before the marker's reply arrives before it.
    const auto drain = [](TransportClient& c, std::int64_t marker) -> std::vector<Json> {
        std::vector<Json> frames;
        Json qp = Json::object();
        qp.set("path", Json(std::string("proj/marker.scene")));
        if (!c.send(rpc(marker, "query", std::move(qp))))
            return frames;
        for (int i = 0; i < 100000; ++i)
        {
            const std::optional<std::string> f = c.receive();
            if (!f.has_value())
                break;
            const Json parsed = Json::parse(*f);
            if (is_response_frame(parsed))
                break; // the marker's response — everything before it is drained
            if (is_event_frame(parsed) || is_gap_frame(parsed))
                frames.push_back(parsed);
        }
        return frames;
    };

    // --- A: N concurrent clients interleave requests (serialized, no lost updates) + fan-out reaches
    //        a subscriber; the `clients` topic fires on attach AND detach ---------------------------
    {
        const fs::path projectA = make_temp_project();
        MemoryFileStore storeA;
        NullWatcher watcherA;
        context::kernel::ManualClock clockA;
        context::kernel::InlineTaskRunner tasksA;
        EditorKernelConfig cfgA;
        cfgA.project_root = projectA;
        cfgA.filesync_root = "proj";
        cfgA.index_path = "proj/.editor/index";
        EditorKernel kernelA(storeA, watcherA, clockA, tasksA, cfgA);
        KernelServer serverA(kernelA);
        CHECK(kernelA.start(ScopeSet::all()) == StartOutcome::booted);
        TransportServer transportA(endpoint_for(
            "ctx-ks-fanin-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
        CHECK(transportA.listen());
        std::thread srvA([&serverA, &transportA]() { serverA.serve(transportA); });

        // The subscriber attaches (session scope so it can also drive shutdown; subscribe/query are
        // read-baseline regardless) and subscribes to every topic.
        TransportClient sub(transportA.endpoint());
        CHECK(sub.connect(3000));
        CHECK(sub.request(rpc(1, "attach", attach_params("session"))).has_value());
        const std::optional<std::string> sresp = sub.request(subscribe_all(2));
        CHECK(sresp.has_value());
        CHECK(Json::parse(*sresp).at("result").at("ok").as_bool());

        // Two writers attach (write scope) — the subscriber will observe two `clients` attach events.
        constexpr int kEditsPerWriter = 6;
        const auto writer = [&](const std::string& prefix, int& ok_count) {
            TransportClient w(transportA.endpoint());
            if (!w.connect(3000))
                return;
            if (!w.request(rpc(1, "attach", attach_params("write,session"))).has_value())
                return;
            for (int i = 0; i < kEditsPerWriter; ++i)
            {
                const std::optional<std::string> e = w.request(
                    edit_req(10 + i, "proj/" + prefix + std::to_string(i) + ".scene",
                             "entity: " + std::to_string(i)));
                if (e.has_value() && Json::parse(*e).at("result").at("ok").as_bool())
                    ++ok_count;
            }
            w.close();
        };

        // Run the two writers CONCURRENTLY: interleaved requests must serialize (no lost updates).
        int ok1 = 0, ok2 = 0;
        std::thread w1([&] { writer("a", ok1); });
        std::thread w2([&] { writer("b", ok2); });
        w1.join();
        w2.join();
        CHECK(ok1 == kEditsPerWriter); // every interleaved edit landed
        CHECK(ok2 == kEditsPerWriter);

        // Serialization proof: each writer's LAST edit is present in the derived world — no write was
        // lost to a concurrent-request race (all 2N edits already reported ok above). The subscriber
        // is a push consumer, so it demuxes (req_demux) instead of request()ing (which would mistake a
        // pushed event for the response). Collect pushed events along the way for the fan-out proof.
        std::vector<Json> events;
        Json qa = Json::object();
        qa.set("path", Json(std::string("proj/a5.scene")));
        const std::optional<Json> ra = req_demux(sub, 3, rpc(3, "query", std::move(qa)), events);
        CHECK(ra.has_value());
        CHECK(ra->at("result").at("data").at("present").as_bool()); // w1's last edit landed
        Json qb = Json::object();
        qb.set("path", Json(std::string("proj/b5.scene")));
        const std::optional<Json> rb = req_demux(sub, 4, rpc(4, "query", std::move(qb)), events);
        CHECK(rb.has_value());
        CHECK(rb->at("result").at("data").at("present").as_bool()); // w2's last edit landed

        // Fan-out proof: the subscriber received `clients` ATTACH events for both writers, and (once
        // the writers' disconnects are processed) DETACH events too. Bounded-retry demux so async
        // detach events still in flight get collected.
        int attached = 0, detached = 0;
        for (int round = 0; round < 100; ++round)
        {
            count_client_events(events, attached, detached);
            if (attached >= 2 && detached >= 2)
                break;
            (void)req_demux(sub, 100 + round, rpc(100 + round, "query", Json::object()), events);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(attached >= 2); // fan-out delivered both writers' attach events to the subscriber
        CHECK(detached >= 2); // the `clients` topic fires on detach too

        // A subscribed client shuts down via req_demux (keep READING so the daemon's fan-out writes
        // never block its own thread from reading this shutdown — a fire-and-forget send + close would
        // deadlock a stuck-writing conn thread against the client's close-flush).
        CHECK(req_demux(sub, 9, rpc(9, "shutdown", Json::object()), events).has_value());
        sub.close();
        srvA.join();
        kernelA.stop();
        std::error_code ecA;
        fs::remove_all(projectA, ecA);
    }

    // --- B: a SLOW client (subscribes, stops reading) hits the gap-marker path; the daemon never
    //        stalls (a concurrent writer's edits all still succeed) --------------------------------
    {
        const fs::path projectB = make_temp_project();
        MemoryFileStore storeB;
        NullWatcher watcherB;
        context::kernel::ManualClock clockB;
        context::kernel::InlineTaskRunner tasksB;
        EditorKernelConfig cfgB;
        cfgB.project_root = projectB;
        cfgB.filesync_root = "proj";
        cfgB.index_path = "proj/.editor/index";
        EditorKernel kernelB(storeB, watcherB, clockB, tasksB, cfgB);
        KernelServer serverB(kernelB);
        serverB.set_max_outbound_frames(4); // a tiny event budget so a slow client overflows fast
        CHECK(kernelB.start(ScopeSet::all()) == StartOutcome::booted);
        TransportServer transportB(endpoint_for(
            "ctx-ks-gap-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
        CHECK(transportB.listen());
        std::thread srvB([&serverB, &transportB]() { serverB.serve(transportB); });

        // The slow client subscribes to every topic, then STOPS reading (never drains its pushes).
        // Session scope so it can drive the final shutdown (subscribe is read-baseline regardless).
        TransportClient slow(transportB.endpoint());
        CHECK(slow.connect(3000));
        CHECK(slow.request(rpc(1, "attach", attach_params("session"))).has_value());
        CHECK(slow.request(subscribe_all(2)).has_value());

        // A writer PIPELINES a burst of edit-batches (each settles -> a `derivation` event). Pipelined
        // sends (no wait) make the daemon dispatch them back-to-back with NO poll throttle, so the
        // event burst reaches the slow client's outbound queue far faster than its poll-cadence flush —
        // overflowing the tiny budget deterministically (not a timing race).
        constexpr int kBurst = 40;
        TransportClient w(transportB.endpoint());
        CHECK(w.connect(3000));
        CHECK(w.request(rpc(1, "attach", attach_params("write,session"))).has_value());
        for (int i = 0; i < kBurst; ++i)
            CHECK(w.send(editbatch_req(10 + i, "proj/f" + std::to_string(i) + ".scene",
                                       "entity: " + std::to_string(i))));
        // Read all responses back (the writer is NOT subscribed, so responses arrive in order): the
        // daemon processed the whole burst -> it NEVER stalled on the slow client.
        int wrote = 0;
        for (int i = 0; i < kBurst; ++i)
        {
            const std::optional<std::string> e = w.receive();
            if (e.has_value() && Json::parse(*e).at("result").at("ok").as_bool())
                ++wrote;
        }
        CHECK(wrote == kBurst); // the daemon NEVER stalled on the slow client — every edit succeeded

        // Now the slow client drains: it must receive a gap marker (it missed events over budget).
        bool saw_gap = false;
        for (int round = 0; round < 50 && !saw_gap; ++round)
        {
            for (const Json& f : drain(slow, 1000 + round))
                if (is_gap_frame(f))
                    saw_gap = true;
            if (!saw_gap)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(saw_gap); // the slow-client gap-marker path was exercised

        w.close();
        // The (previously slow) client shuts down via req_demux — it RESUMES reading, which drains its
        // socket, unblocks the daemon's stuck fan-out write, and lets the daemon read this shutdown.
        std::vector<Json> drained;
        CHECK(req_demux(slow, 9, rpc(9, "shutdown", Json::object()), drained).has_value());
        slow.close();
        srvB.join();
        kernelB.stop();
        std::error_code ecB;
        fs::remove_all(projectB, ecB);
    }

    // --- C: the max-connections bound — the (N+1)th attach is refused daemon.busy; a freed slot is
    //        reusable (the refusal is transient) ----------------------------------------------------
    {
        const fs::path projectC = make_temp_project();
        MemoryFileStore storeC;
        NullWatcher watcherC;
        context::kernel::ManualClock clockC;
        context::kernel::InlineTaskRunner tasksC;
        EditorKernelConfig cfgC;
        cfgC.project_root = projectC;
        cfgC.filesync_root = "proj";
        cfgC.index_path = "proj/.editor/index";
        EditorKernel kernelC(storeC, watcherC, clockC, tasksC, cfgC);
        KernelServer serverC(kernelC);
        serverC.set_max_connections(2); // only two concurrent clients
        CHECK(kernelC.start(ScopeSet::all()) == StartOutcome::booted);
        TransportServer transportC(endpoint_for(
            "ctx-ks-bound-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
        CHECK(transportC.listen());
        std::thread srvC([&serverC, &transportC]() { serverC.serve(transportC); });

        // Two clients connect + attach and STAY connected (they occupy both slots). c2 takes session
        // scope so it can drive the final shutdown.
        TransportClient c1(transportC.endpoint());
        CHECK(c1.connect(3000));
        CHECK(c1.request(rpc(1, "attach", attach_params("read"))).has_value());
        TransportClient c2(transportC.endpoint());
        CHECK(c2.connect(3000));
        CHECK(c2.request(rpc(1, "attach", attach_params("session"))).has_value());

        // The 3rd attach is refused daemon.busy (the reject-conn replies then closes).
        TransportClient c3(transportC.endpoint());
        CHECK(c3.connect(3000));
        const std::optional<std::string> busy = c3.request(rpc(1, "attach", attach_params("read")));
        CHECK(busy.has_value());
        const Json bj = Json::parse(*busy);
        CHECK(bj.contains("error"));
        CHECK(bj.at("error").at("data").at("code").as_string() == "daemon.busy");
        c3.close();

        // Free a slot; a new client now attaches (the refusal was transient/retriable).
        c1.close();
        bool reused = false;
        for (int round = 0; round < 50 && !reused; ++round)
        {
            TransportClient c4(transportC.endpoint());
            if (c4.connect(3000))
            {
                const std::optional<std::string> a = c4.request(rpc(1, "attach", attach_params("read")));
                if (a.has_value() && Json::parse(*a).contains("result"))
                {
                    reused = true;
                    c4.close();
                    break;
                }
            }
            c4.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(reused); // the slot freed by c1's detach was reusable

        CHECK(c2.request(rpc(9, "shutdown", Json::object())).has_value());
        c2.close();
        srvC.join();
        kernelC.stop();
        std::error_code ecC;
        fs::remove_all(projectC, ecC);
    }

    // --- D: D20 attach-token enforcement over the wire (flag ON) — accept + deny paths ------------
    {
        const fs::path projectD = make_temp_project();
        MemoryFileStore storeD;
        NullWatcher watcherD;
        context::kernel::ManualClock clockD;
        context::kernel::InlineTaskRunner tasksD;
        EditorKernelConfig cfgD;
        cfgD.project_root = projectD;
        cfgD.filesync_root = "proj";
        cfgD.index_path = "proj/.editor/index";
        EditorKernel kernelD(storeD, watcherD, clockD, tasksD, cfgD);
        KernelServer serverD(kernelD);
        CHECK(kernelD.start(ScopeSet::all()) == StartOutcome::booted);
        kernelD.set_attach_auth("the-secret-token", true); // enforcement ON (before serve())
        TransportServer transportD(endpoint_for(
            "ctx-ks-auth-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
        CHECK(transportD.listen());
        std::thread srvD([&serverD, &transportD]() { serverD.serve(transportD); });

        // Missing token -> attach.denied (the same daemon SURVIVES for the next client).
        {
            TransportClient c(transportD.endpoint());
            CHECK(c.connect(3000));
            const std::optional<std::string> a = c.request(rpc(1, "attach", attach_params("read")));
            CHECK(a.has_value());
            const Json j = Json::parse(*a);
            CHECK(j.contains("error"));
            CHECK(j.at("error").at("data").at("code").as_string() == "attach.denied");
            c.close();
        }
        // Wrong token -> attach.denied.
        {
            TransportClient c(transportD.endpoint());
            CHECK(c.connect(3000));
            const std::optional<std::string> a =
                c.request(rpc(1, "attach", attach_params_tok("read", "wrong")));
            CHECK(a.has_value());
            CHECK(Json::parse(*a).at("error").at("data").at("code").as_string() == "attach.denied");
            c.close();
        }
        // Correct token -> attaches (session scope so it can also drive shutdown); a read verb works.
        {
            TransportClient c(transportD.endpoint());
            CHECK(c.connect(3000));
            const std::optional<std::string> a =
                c.request(rpc(1, "attach", attach_params_tok("session", "the-secret-token")));
            CHECK(a.has_value());
            CHECK(Json::parse(*a).contains("result"));
            const std::optional<std::string> d = c.request(rpc(2, "describe", Json::object()));
            CHECK(d.has_value());
            CHECK(Json::parse(*d).at("result").at("ok").as_bool());
            CHECK(c.request(rpc(9, "shutdown", Json::object())).has_value());
            c.close();
        }

        srvD.join();
        kernelD.stop();
        std::error_code ecD;
        fs::remove_all(projectD, ecD);
    }

    // ---- M9 e08a: DAEMON SESSION STATE over the wire (D7 tier 1) ---------------------------------
    //      TWO clients attached to ONE daemon: a change driven by either is seen by BOTH, stamped
    //      with `origin` = the ACTING client's id — the echo-suppression contract e08b's panels and
    //      e08c's bus both depend on. Also proves the R-SEC-007 classification (a read-only token is
    //      refused the whole family), the L-51 play machine over RPC, and the clean-shutdown
    //      persistence + restore round-trip through a REAL `.editor/session.json`.
    const fs::path projectE = make_temp_project();
    {
        MemoryFileStore storeE;
        NullWatcher watcherE;
        context::kernel::ManualClock clockE;
        context::kernel::InlineTaskRunner tasksE;

        EditorKernelConfig cfgE;
        cfgE.project_root = projectE;
        cfgE.filesync_root = "proj";
        cfgE.index_path = "proj/.editor/index";

        EditorKernel kernelE(storeE, watcherE, clockE, tasksE, cfgE);
        KernelServer serverE(kernelE);
        CHECK(kernelE.start(ScopeSet::all()) == StartOutcome::booted);

        TransportServer transportE(endpoint_for(
            "ctx-kernelserver-session-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
        CHECK(transportE.listen());
        std::thread srvE([&serverE, &transportE]() { serverE.serve(transportE); });

        // A READ-ONLY token is refused the whole family — reads included (design 05 §4 puts the
        // editor session-state namespace on session_control; what it reads is the LIVE human
        // session, not authored data).
        {
            TransportClient ro(transportE.endpoint());
            CHECK(ro.connect(3000));
            CHECK(ro.request(rpc(1, "attach", attach_params("read"))).has_value());
            for (const char* method :
                 {"editor.select", "editor.selection-get", "editor.camera-set",
                  "editor.cameras-get", "editor.play", "editor.pause", "editor.stop", "editor.step"})
            {
                const std::optional<std::string> denied =
                    ro.request(rpc(2, method, Json::object()));
                CHECK(denied.has_value());
                const Json resp = Json::parse(*denied);
                CHECK(resp.contains("error"));
                CHECK(resp.at("error").at("data").at("code").as_string() == "scope.denied");
            }
            ro.close();
        }

        {
            TransportClient a(transportE.endpoint());
            TransportClient b(transportE.endpoint());
            CHECK(a.connect(3000));
            CHECK(b.connect(3000));

            // Each client learns its OWN id from the attach reply — the value it must compare
            // `origin` against to recognise (and drop) a fact it caused.
            const std::optional<std::string> a_attach =
                a.request(rpc(1, "attach", attach_params("session")));
            CHECK(a_attach.has_value());
            const Json a_result = Json::parse(*a_attach).at("result");
            CHECK(a_result.contains("clientId"));
            const std::int64_t a_id = a_result.at("clientId").as_int();
            const std::optional<std::string> b_attach =
                b.request(rpc(1, "attach", attach_params("session")));
            CHECK(b_attach.has_value());
            const std::int64_t b_id = Json::parse(*b_attach).at("result").at("clientId").as_int();
            CHECK(a_id > 0);
            CHECK(b_id > 0);
            CHECK(a_id != b_id); // two clients are never the same origin

            std::vector<Json> a_events;
            std::vector<Json> b_events;
            CHECK(req_demux(a, 2, subscribe_all(2), a_events).has_value());
            CHECK(req_demux(b, 2, subscribe_all(2), b_events).has_value());

            // Collect the `session` facts a client has seen, newest last.
            const auto session_facts = [](const std::vector<Json>& events) {
                std::vector<Json> out;
                for (const Json& e : events)
                {
                    if (!is_event_frame(e))
                        continue;
                    const Json& inner = e.at("params").at("event");
                    if (inner.at("topic").as_string() == "session")
                        out.push_back(inner.at("payload"));
                }
                return out;
            };
            const auto find_fact = [&session_facts](const std::vector<Json>& events,
                                                    const std::string& kind) -> std::optional<Json> {
                std::optional<Json> found;
                for (const Json& payload : session_facts(events))
                    if (payload.at("event").as_string() == kind)
                        found = payload;
                return found;
            };

            // --- A selects; BOTH clients converge on the same selection -------------------------
            Json sel = Json::object();
            Json id_list = Json::array();
            id_list.push_back(Json(std::string("root/child")));
            id_list.push_back(Json(std::string("root/other")));
            sel.set("ids", std::move(id_list));
            const std::optional<Json> sel_resp = req_demux(a, 3, rpc(3, "editor.select", sel), a_events);
            CHECK(sel_resp.has_value());
            CHECK(sel_resp->at("result").at("ok").as_bool());
            CHECK(sel_resp->at("result").at("data").at("changed").as_bool());
            CHECK(sel_resp->at("result").at("data").at("ids").size() == 2);

            // B reads the selection back over its OWN connection: one truth, two clients.
            const std::optional<Json> b_sel =
                req_demux(b, 3, rpc(3, "editor.selection-get", Json::object()), b_events);
            CHECK(b_sel.has_value());
            const Json& b_ids = b_sel->at("result").at("data").at("ids");
            CHECK(b_ids.size() == 2);
            CHECK(b_ids.at(0).as_string() == "root/child");

            // ...and B RECEIVED the fact, stamped with A's id — so B knows it did not cause it.
            const std::optional<Json> b_fact = find_fact(b_events, "selection-changed");
            CHECK(b_fact.has_value());
            CHECK(b_fact->at("origin").as_int() == a_id);
            CHECK(b_fact->at("origin").as_int() != b_id); // B applies it (not an echo of its own)
            CHECK(b_fact->at("mode").as_string() == "replace");
            CHECK(b_fact->at("ids").size() == 2);

            // A ALSO receives its own fact — with origin == A, which is exactly the echo it must
            // suppress. That both sides see the same frame and disambiguate by `origin` alone IS
            // the contract (no side-channel, no per-client filtering in the daemon).
            const std::optional<Json> a_fact = find_fact(a_events, "selection-changed");
            CHECK(a_fact.has_value());
            CHECK(a_fact->at("origin").as_int() == a_id);

            // --- an UNCHANGED re-select publishes NOTHING (no echo generator) -------------------
            const std::size_t before = session_facts(b_events).size();
            const std::optional<Json> again = req_demux(a, 4, rpc(4, "editor.select", sel), a_events);
            CHECK(again.has_value());
            CHECK(!again->at("result").at("data").at("changed").as_bool());
            // Drain B by making a round-trip on B's own connection; no new session fact may appear.
            CHECK(req_demux(b, 4, rpc(4, "editor.selection-get", Json::object()), b_events).has_value());
            CHECK(session_facts(b_events).size() == before);

            // --- B moves a camera; A sees it with origin == B ------------------------------------
            Json cam = Json::object();
            cam.set("viewportId", Json(std::string("main")));
            Json transform = Json::object();
            transform.set("x", Json(3));
            cam.set("transform", std::move(transform));
            CHECK(req_demux(b, 5, rpc(5, "editor.camera-set", std::move(cam)), b_events).has_value());
            CHECK(req_demux(a, 5, rpc(5, "editor.cameras-get", Json::object()), a_events).has_value());
            const std::optional<Json> a_cam_fact = find_fact(a_events, "camera-changed");
            CHECK(a_cam_fact.has_value());
            CHECK(a_cam_fact->at("origin").as_int() == b_id);
            CHECK(a_cam_fact->at("viewportId").as_string() == "main");

            // --- play control over RPC (L-51) ----------------------------------------------------
            // Pausing in `edit` is refused with the reserved play.* code, not a silent no-op.
            const std::optional<Json> bad_pause =
                req_demux(a, 6, rpc(6, "editor.pause", Json::object()), a_events);
            CHECK(bad_pause.has_value());
            CHECK(bad_pause->contains("error"));
            CHECK(bad_pause->at("error").at("data").at("code").as_string() == "play.not_running");

            const std::optional<Json> play =
                req_demux(a, 7, rpc(7, "editor.play", Json::object()), a_events);
            CHECK(play.has_value());
            CHECK(play->at("result").at("data").at("state").as_string() == "playing");

            Json step = Json::object();
            step.set("ticks", Json(std::string("4"))); // the CLI projection delivers a flag as text
            CHECK(req_demux(a, 8, rpc(8, "editor.step", std::move(step)), a_events).has_value());

            const std::optional<Json> stop =
                req_demux(a, 9, rpc(9, "editor.stop", Json::object()), a_events);
            CHECK(stop.has_value());
            CHECK(stop->at("result").at("data").at("state").as_string() == "edit");
            CHECK(stop->at("result").at("data").at("simTick").as_int() == 0); // L-51 discard

            // B observed the whole play-state sequence — the L-51 indicator feed.
            CHECK(req_demux(b, 6, rpc(6, "editor.selection-get", Json::object()), b_events).has_value());
            std::vector<std::string> b_play_states;
            for (const Json& payload : session_facts(b_events))
                if (payload.at("event").as_string() == "play-state")
                    b_play_states.push_back(payload.at("state").as_string() + ":" +
                                            std::to_string(payload.at("simTick").as_int()));
            CHECK(b_play_states.size() == 3);
            CHECK(b_play_states[0] == "playing:0");
            CHECK(b_play_states[1] == "playing:4"); // stepping leaves playing/paused alone
            CHECK(b_play_states[2] == "edit:0");

            // A malformed step is a usage failure, never a silent default.
            Json bad_step = Json::object();
            bad_step.set("ticks", Json(std::string("2x")));
            CHECK(req_demux(a, 10, rpc(10, "editor.play", Json::object()), a_events).has_value());
            const std::optional<Json> bad =
                req_demux(a, 11, rpc(11, "editor.step", std::move(bad_step)), a_events);
            CHECK(bad.has_value());
            CHECK(bad->at("error").at("data").at("code").as_string() == "usage.invalid");

            // A malformed select likewise.
            Json bad_sel = Json::object();
            bad_sel.set("ids", Json::array());
            bad_sel.set("mode", Json(std::string("obliterate")));
            const std::optional<Json> bad_mode =
                req_demux(a, 12, rpc(12, "editor.select", std::move(bad_sel)), a_events);
            CHECK(bad_mode.has_value());
            CHECK(bad_mode->at("error").at("data").at("code").as_string() == "usage.invalid");

            // --- ids are NEVER REUSED within a daemon lifetime ----------------------------------
            // The second half of the echo-suppression trust argument (docs/editor-session-state.md):
            // "two live clients differ" is not enough — a client that RECONNECTS after another has
            // dropped must not inherit a departed client's id, or a fact still in flight would be
            // mis-attributed and wrongly suppressed. e08b's panels and e08c's bus both key on this.
            a.close();
            TransportClient c(transportE.endpoint());
            CHECK(c.connect(3000));
            const std::optional<std::string> c_attach =
                c.request(rpc(1, "attach", attach_params("session")));
            CHECK(c_attach.has_value());
            const std::int64_t c_id = Json::parse(*c_attach).at("result").at("clientId").as_int();
            CHECK(c_id != a_id); // A's slot is free, but its IDENTITY is spent
            CHECK(c_id != b_id);
            CHECK(c_id > b_id); // monotonic: a fresh id is always beyond every id ever issued

            CHECK(c.request(rpc(2, "shutdown", Json::object())).has_value());
            c.close();
            b.close();
        }

        srvE.join();
        kernelE.stop();

        // --- the clean-shutdown write: the daemon is the SINGLE writer of .editor/session.json ----
        CHECK(fs::exists(projectE / ".editor" / "session.json"));
        {
            context::editor::editorkernel::EditorSessionState restored;
            const auto report =
                context::editor::editorkernel::restore_session_state(projectE, restored);
            CHECK(report.outcome ==
                  context::editor::editorkernel::SessionRestoreOutcome::restored);
            CHECK(restored.selection().size() == 2);
            CHECK(restored.selection()[0] == "root/child");
            CHECK(restored.cameras().count("main") == 1);
            // Play state is never persisted (L-51) — a restarted daemon holds no live session.
            CHECK(restored.play_state() ==
                  context::editor::editorkernel::EditorPlayState::edit);
        }
    }

    // --- and a FRESH daemon on the same project RESTORES it (the "restore on next attach" half) ---
    {
        MemoryFileStore storeF;
        NullWatcher watcherF;
        context::kernel::ManualClock clockF;
        context::kernel::InlineTaskRunner tasksF;

        EditorKernelConfig cfgF;
        cfgF.project_root = projectE; // the SAME project the previous daemon persisted
        cfgF.filesync_root = "proj";
        cfgF.index_path = "proj/.editor/index";

        EditorKernel kernelF(storeF, watcherF, clockF, tasksF, cfgF);
        KernelServer serverF(kernelF);
        CHECK(kernelF.start(ScopeSet::all()) == StartOutcome::booted);

        TransportServer transportF(endpoint_for(
            "ctx-kernelserver-restore-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
        CHECK(transportF.listen());
        std::thread srvF([&serverF, &transportF]() { serverF.serve(transportF); });

        {
            TransportClient c(transportF.endpoint());
            CHECK(c.connect(3000));
            CHECK(c.request(rpc(1, "attach", attach_params("session"))).has_value());
            const std::optional<std::string> got =
                c.request(rpc(2, "editor.selection-get", Json::object()));
            CHECK(got.has_value());
            const Json got_resp = Json::parse(*got);
            const Json& ids = got_resp.at("result").at("data").at("ids");
            CHECK(ids.size() == 2);
            CHECK(ids.at(0).as_string() == "root/child"); // the human's selection survived a restart
            CHECK(c.request(rpc(3, "shutdown", Json::object())).has_value());
            c.close();
        }

        srvF.join();
        kernelF.stop();
    }
    {
        std::error_code ecE;
        fs::remove_all(projectE, ecE);
    }

    EDITORKERNEL_TEST_MAIN_END();
}
