// KernelServer implementation (see kernel_server.h).

#include "context/editor/editorkernel/kernel_server.h"

#include "context/editor/compose/flatten.h"          // e05d3: the editor.* composed reads
#include "context/editor/compose/project_resolver.h" // e05d3: the disk-backed scene resolver
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/contract/resource_handle.h"
#include "context/editor/derivation/derivation_graph.h"
#include "context/editor/filesync/content_hash.h"       // e05d3: the inspect CAS token
#include "context/editor/filesync/native_file_store.h"  // e05d3: root-scene raw-byte read
#include "context/editor/filesync/path_jail.h"          // e05d3: R-SEC-008 jail on the raw read
#include "context/editor/gui/panels/builders/inspector_builder.h"  // e05d3: kernel-side builders
#include "context/editor/gui/panels/builders/scene_tree_builder.h" // e05d3
#include "context/editor/gui/panels/builders/wire.h"               // e05d3: model -> wire JSON
#include "context/editor/schema/kind_schema.h" // e05d3: engine_schemas() kind lookup

#include "context/editor/bridge/event_stream.h" // e08a: Stability on the loud recovery diagnostic

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace context::editor::editorkernel
{

using contract::Envelope;
using contract::Json;

namespace
{
// A full-range 64-bit canonical hash exceeds 2^53, so it is carried as a DECIMAL STRING to round-trip
// losslessly over the wire (mirrors editor_driver.cpp — the R-CLI-006 replay key must not lose bits).
Json hash_string(std::uint64_t h)
{
    return Json(std::to_string(h));
}

std::optional<std::string> string_param(const Json& params, const std::string& key)
{
    if (params.contains(key) && params.at(key).is_string())
        return params.at(key).as_string();
    return std::nullopt;
}

// ---- M9 e08a session-state helpers ---------------------------------------------------------------

// A `session` topic payload skeleton: the event class + the ECHO-SUPPRESSION contract (`origin` =
// the acting client's id, the same value that client got back from `attach` as `clientId`; 0 = the
// daemon itself). Every editor session-state fact is stamped this way, so the one rule a client
// needs is "ignore a fact whose origin is me".
Json session_fact(const char* event, std::uint64_t origin)
{
    Json ev = Json::object();
    ev.set("event", Json(std::string(event)));
    ev.set("origin", Json(origin));
    return ev;
}

// The wire shapes of selection / cameras come from editor_session_state.h
// (`selection_ids_json` / `cameras_json`) rather than being rebuilt here: the `session` facts, these
// `editor.*-get` replies, and `.editor/session.json` are documented to carry the SAME shape, so they
// share the one encoder instead of a comment promising two copies agree.

// Strict unsigned parse for `editor step --ticks` (the CLI projection delivers a flag as a string).
// Deliberately strict: "-1" wrapping to ~2^64 would step the session ~forever, and a trailing-garbage
// tolerance would silently accept "2x" as 2.
std::optional<std::uint64_t> parse_ticks(const std::string& raw)
{
    if (raw.empty())
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char ch : raw)
    {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (0xFFFFFFFFFFFFFFFFULL - digit) / 10ULL)
            return std::nullopt; // overflow
        value = value * 10ULL + digit;
    }
    return value;
}

// ---- D19 multi-client fan-in support --------------------------------------------------------------

// The connection thread's poll interval. On POSIX a request incurs NO added latency (poll() wakes the
// instant data arrives); this only bounds pushed-event delivery + shutdown-notice latency. On Windows
// it is the idle re-poll cadence. Small so an editor feels live; a handful of connections × this
// cadence is negligible CPU.
constexpr int kPollIntervalMs = 5;

// The BOUNDED window serve()'s teardown gives each connection thread to notice stop_, flush what it
// has already queued (the `shutdown` verb's own reply included) and finish, before the teardown
// force-unblocks the stragglers. A responsive peer's thread gets there within one kPollIntervalMs, so
// this is normally consumed in single-digit milliseconds; the cap only bounds a peer that will not
// read. See the teardown comment in serve() (CE #352) for why draining before unblock() is required.
constexpr int kShutdownDrainGraceMs = 500;

// One frame queued for a connection's own thread to flush. `is_event` frames count against the
// per-connection event budget (droppable under backpressure); response frames never drop.
struct OutFrame
{
    std::string data;
    bool is_event = false;
};

// Per-connection serve state. ONE thread per connection owns `conn` for BOTH reads and writes (a
// timed read interleaved with flushing the outbound queue) — no duplicated handle, so there is never
// a concurrent read + write on the same OS handle (which deadlocks a synchronous Windows pipe file
// object). `session` + `live` + `id` are touched ONLY under the serve loop's dispatch mutex; the
// outbound queue has its own lock; another dispatch thread's fan-out enqueues onto it, this
// connection's thread flushes it.
struct Conn
{
    Conn(bridge::TransportConnection c, std::uint64_t cid, std::size_t budget)
        : conn(std::move(c)), id(cid), out_budget(budget)
    {
        // Stamp the connection's identity onto its session BEFORE the attach handshake: the
        // dispatcher carries it across attach and reports it back as `clientId`, and it is what the
        // `session` topic's `origin` field carries for echo suppression (M9 e08a / D7).
        session.client_id = cid;
    }

    bridge::TransportConnection conn; // this connection's thread only (reads AND writes)
    bridge::Session session;          // dispatch-mutex only
    std::uint64_t id = 0;             // per-daemon client id (the `clients` topic)
    bool live = true;                 // dispatch-mutex: receives fan-out while true
    bool reject = false;              // over-cap connection: reply daemon.busy then close
    bool gap_owed = false;            // dispatch-mutex: an event was dropped -> owe a gap marker
    std::atomic<bool> finished{false}; // set LAST by the thread; reap may then join it

    std::mutex out_mu;             // guards the outbound queue (producers = fan-out on any thread)
    std::deque<OutFrame> out;      // pending frames (responses + events), flushed by THIS conn's thread
    std::size_t queued_events = 0; // count of is_event frames currently in `out`
    std::size_t out_budget = 256;  // max queued event frames before dropping + owing a gap

    std::thread thread;
};

// A JSON-RPC 2.0 notification frame (server->client): {jsonrpc:"2.0", method, params}, no id.
std::string notification_frame(std::string method, contract::Json params)
{
    contract::Json out = contract::Json::object();
    out.set("jsonrpc", contract::Json(std::string("2.0")));
    out.set("method", contract::Json(std::move(method)));
    out.set("params", std::move(params));
    return out.dump(0);
}

// A JSON-RPC 2.0 notification carrying one pushed event (server->client). `subId` routes it to the
// originating subscription so a client with several subscriptions can demux.
std::string event_frame(const std::string& sub_id, contract::Json event)
{
    contract::Json params = contract::Json::object();
    params.set("subId", contract::Json(sub_id));
    params.set("event", std::move(event));
    return notification_frame("event", std::move(params));
}

// A connection-level gap marker: the client missed events (its outbound budget overflowed or a
// subscription queue gapped) and must re-snapshot every subscription (R-BRIDGE-008).
std::string gap_frame()
{
    return notification_frame("event.gap", contract::Json::object());
}

// The daemon.busy JSON-RPC error reply for an over-cap connection's attach (mirrors the dispatcher's
// error-response shape: same catalog `code` in error.data.code). The request id is echoed when present.
std::string busy_response(const std::string& request_json)
{
    contract::Json id;
    try
    {
        const contract::Json req = contract::Json::parse(request_json);
        if (req.is_object() && req.contains("id"))
            id = req.at("id");
    }
    catch (const std::exception&)
    {
        // A malformed first frame from an over-cap client: reply with a null id (still daemon.busy).
    }
    const Envelope env = Envelope::failure(kDaemonBusyCode);
    contract::Json data = contract::Json::object();
    if (env.error().has_value())
    {
        data.set("code", contract::Json(env.error()->code));
        data.set("message", contract::Json(env.error()->message));
        data.set("retriable", contract::Json(env.error()->retriable));
    }
    contract::Json err = contract::Json::object();
    err.set("code", contract::Json(-32000)); // the JSON-RPC server-error band (mirrors dispatcher.cpp)
    err.set("message",
            contract::Json(env.error().has_value() ? env.error()->message : std::string("busy")));
    err.set("data", std::move(data));
    contract::Json out = contract::Json::object();
    out.set("jsonrpc", contract::Json(std::string("2.0")));
    out.set("id", id);
    out.set("error", std::move(err));
    return out.dump(0);
}

// Enqueue a RESPONSE frame (never dropped) for the connection's thread to flush.
void enqueue_response(Conn& c, std::string data)
{
    std::lock_guard<std::mutex> lk(c.out_mu);
    c.out.push_back(OutFrame{std::move(data), false});
}

// Enqueue an EVENT frame under the per-connection budget; false (dropped) when the budget is full.
bool enqueue_event(Conn& c, std::string data)
{
    std::lock_guard<std::mutex> lk(c.out_mu);
    if (c.queued_events >= c.out_budget)
        return false;
    c.out.push_back(OutFrame{std::move(data), true});
    ++c.queued_events;
    return true;
}

// Flush every queued outbound frame to the socket. Called by the connection's OWN thread (which also
// owns reads), so reads and writes never overlap on the handle. The blocking write happens WITHOUT
// the out lock held (the lock only guards the queue, briefly), so a producer's fan-out never blocks on
// a slow client. false on a write failure (peer gone) -> the caller tears the connection down.
bool flush_outbound(Conn& c)
{
    std::deque<OutFrame> pending;
    {
        std::lock_guard<std::mutex> lk(c.out_mu);
        pending.swap(c.out);
        c.queued_events = 0; // everything currently queued is now being flushed
    }
    for (const OutFrame& f : pending)
        if (!c.conn.write_frame(f.data))
            return false; // peer gone
    return true;
}

// Best-effort: a throwaway connection unblocks a serve-thread accept() parked waiting for a client so
// the accept loop can observe stop_ and exit. The acceptor closes the straggler immediately.
void wake_endpoint(const std::string& endpoint)
{
    bridge::TransportClient waker(endpoint);
    (void)waker.connect(500);
    waker.close();
}
} // namespace

KernelServer::KernelServer(EditorKernel& kernel) : kernel_(kernel)
{
    // Register as the dispatcher's method backend. MUST precede kernel.start() (the dispatcher is
    // constructed at boot and captures the backend then).
    kernel_.set_method_backend(this);
}

bridge::ResourceStore& KernelServer::resources() const
{
    // Lazy: the incarnation id lives on the daemon's EventStream, which exists only after
    // kernel.start() — and every caller of this accessor runs on a started daemon (see header).
    if (!resources_.has_value())
        resources_.emplace(kernel_.config().project_root / ".editor" / "resources",
                           kernel_.events().incarnation_id());
    return *resources_;
}

SessionRestoreReport KernelServer::restore_session()
{
    return restore_session_state(kernel_.config().project_root, session_);
}

bool KernelServer::persist_session(std::string& error) const
{
    return persist_session_state(kernel_.config().project_root, session_, error);
}

std::optional<Envelope> KernelServer::invoke(const std::string& method, const Json& params,
                                             const bridge::Session& session) const
{
    // `edit` — the daemon-initiated cross-process file write (the file_write scope was already
    // enforced by the dispatcher; edit_file re-checks it defensively). Writes through filesync
    // atomic-IO, then runs the read-your-writes barrier so the reply reports whether the derived
    // world already reflects the write (R-CLI-006).
    if (method == "edit")
    {
        const std::optional<std::string> path = string_param(params, "path");
        const std::optional<std::string> content = string_param(params, "content");
        if (!path.has_value() || !content.has_value())
            return Envelope::failure("usage.missing_argument",
                                     "edit requires string 'path' and 'content' params");

        EditOutcome out = kernel_.edit_file(*path, *content, session.scopes);
        if (!out.ok)
            return out.envelope();

        const std::optional<derivation::DerivedSource> observed =
            kernel_.query_after_hash(*path, out.ticket.canonical_hash);
        const bool reflected =
            observed.has_value() && observed->canonical_hash == out.ticket.canonical_hash;

        Json data = Json::object();
        data.set("path", Json(*path));
        // The R-FILE-001 two-hash split, labelled: rawHash = on-disk byte identity (CAS
        // `--if-match`); canonicalHash = canonical-content identity (the R-CLI-006 barrier key).
        data.set("rawHash", hash_string(out.ticket.raw_hash));
        data.set("canonicalHash", hash_string(out.ticket.canonical_hash));
        data.set("reflected", Json(reflected));
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        data.set("generation", Json(kernel_.generation()));

        Envelope env = Envelope::success(std::move(data), kernel_.generation());
        if (!reflected)
            env.add_warning("the derived world did not reflect the edit within the read barrier bound");
        return env;
    }

    // `edit-batch` — the daemon-initiated MULTI-file write, serialized through the R-FILE-004
    // crash-recovery intent log (the dispatcher already enforced file_write via the scope table;
    // edit_files re-checks it defensively). The reply reports each file's canonical hash plus
    // whether the derived world reflects the whole batch after a settle.
    if (method == "edit-batch")
    {
        if (!params.contains("files") || !params.at("files").is_array() ||
            params.at("files").size() == 0)
            return Envelope::failure("usage.missing_argument",
                                     "edit-batch requires a non-empty 'files' array of "
                                     "{path, content} objects");

        std::vector<BatchEdit> edits;
        edits.reserve(params.at("files").size());
        for (std::size_t i = 0; i < params.at("files").size(); ++i)
        {
            const Json& f = params.at("files").at(i);
            const std::optional<std::string> path = string_param(f, "path");
            const std::optional<std::string> content = string_param(f, "content");
            if (!path.has_value() || !content.has_value())
                return Envelope::failure("usage.missing_argument",
                                         "edit-batch files[" + std::to_string(i) +
                                             "] needs string 'path' and 'content'");
            edits.push_back(BatchEdit{*path, *content});
        }

        EditBatchOutcome out = kernel_.edit_files(edits, session.scopes);
        if (!out.ok)
        {
            Envelope fail = Envelope::failure(out.error_code);
            if (!out.error_detail.empty())
                fail.add_warning(out.error_detail);
            return fail;
        }

        kernel_.settle(); // drain the batch into the derived world (R-BRIDGE-008 quiescence)

        Json files = Json::array();
        bool all_reflected = true;
        for (const derivation::WriteTicket& t : out.tickets)
        {
            const std::optional<derivation::DerivedSource> node = kernel_.query(t.path);
            const bool reflected = node.has_value() && node->canonical_hash == t.canonical_hash;
            all_reflected = all_reflected && reflected;
            Json entry = Json::object();
            entry.set("path", Json(t.path));
            entry.set("rawHash", hash_string(t.raw_hash)); // R-FILE-001 split (see `edit` above)
            entry.set("canonicalHash", hash_string(t.canonical_hash));
            entry.set("reflected", Json(reflected));
            files.push_back(std::move(entry));
        }

        Json data = Json::object();
        data.set("opId", Json(out.op_id));
        data.set("files", std::move(files));
        data.set("allReflected", Json(all_reflected));
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        data.set("generation", Json(kernel_.generation()));
        return Envelope::success(std::move(data), kernel_.generation());
    }

    // `snapshot` — the R-BRIDGE-008 current-state snapshot a newly-attached or RECONNECTING client
    // reads before deltas: the incarnation epoch id (a restart forces a fresh snapshot rather than
    // trusting a stale "since seq N" cursor), the derived-world generation, the last seq — plus the
    // boot-time R-FILE-004 recovery diagnostics so a reconnecting client learns about any op the
    // previous incarnation left incomplete. Read-only (read/query baseline).
    if (method == "snapshot")
    {
        Json data = kernel_.events().snapshot();
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        data.set("worldGeneration", Json(kernel_.generation()));
        Json recovery = Json::array();
        for (const auto& d : kernel_.recovery_diagnostics())
            recovery.push_back(diagnostic_json(d));
        data.set("recoveryDiagnostics", std::move(recovery));
        return Envelope::success(std::move(data), kernel_.generation());
    }

    // `reconcile` — fold EXTERNAL (out-of-band) edits into the derived world: drain watcher hints +
    // FORCE the full re-hash crawl (content hash authoritative over watchers, R-FILE-002), then
    // settle. This is the crawl-on-demand path for bulk ops (e.g. after a git branch switch), so it
    // bypasses the low-frequency crawl cadence deliberately. Read-only w.r.t. authored state (it
    // makes the daemon notice on-disk truth; it writes nothing), so it sits on the read/query
    // baseline.
    if (method == "reconcile")
    {
        const std::size_t changes = kernel_.ingest_external(CrawlMode::force).size();
        const std::uint64_t generation = kernel_.settle();
        Json data = Json::object();
        data.set("changes", Json(static_cast<std::uint64_t>(changes)));
        data.set("generation", Json(generation));
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        return Envelope::success(std::move(data), generation);
    }

    // `query` — read the derived node for a path + world stats. Read-only (read/query baseline).
    if (method == "query")
    {
        const std::optional<std::string> path = string_param(params, "path");
        if (!path.has_value())
            return Envelope::failure("usage.missing_argument", "query requires a string 'path' param");

        const std::optional<derivation::DerivedSource> node = kernel_.query(*path);
        Json data = Json::object();
        data.set("path", Json(*path));
        data.set("present", Json(node.has_value()));
        if (node.has_value())
        {
            data.set("canonicalHash", hash_string(node->canonical_hash));
            data.set("nodeGeneration", Json(node->generation));
        }
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        data.set("generation", Json(kernel_.generation()));
        return Envelope::success(std::move(data), kernel_.generation());
    }

    // `editor.scene-tree` — the M9 e05d3 composed SCENE-TREE read (D17/D18): flatten the requested
    // root scene over the SAME disk-backed compose path `context set` plans against (a FRESH
    // ProjectSceneResolver — always the current on-disk truth), build the boundary-clean panel model
    // KERNEL-SIDE (gui/panels/builders), and answer it as data. The Shell's Scene tree panel
    // hydrates from exactly this; the panel model crossing AS DATA is what keeps the editor an
    // ordinary client (no compose on its closure). Read-only (read/query baseline). A scene that
    // does not resolve is not an error envelope: flatten reports it honestly INSIDE the model
    // (ok=false, zero entities), and the panel renders that state — the same tolerance the
    // diagnostics feed applies.
    if (method == "editor.scene-tree")
    {
        const std::optional<std::string> path = string_param(params, "path");
        if (!path.has_value())
            return Envelope::failure("usage.missing_argument",
                                     "editor.scene-tree requires a string 'path' param");

        const compose::ProjectSceneResolver resolver(kernel_.config().project_root);
        const compose::ComposedScene scene = compose::flatten(*path, resolver);

        Json data = Json::object();
        data.set("sceneTree", gui::panels::builders::scene_tree_to_wire(
                                  gui::panels::builders::build_scene_tree(scene)));
        data.set("generation", Json(kernel_.generation()));
        return Envelope::success(std::move(data), kernel_.generation());
    }

    // `editor.inspect` — the M9 e05d3 composed INSPECTOR read (D17/D18): resolve one composed
    // entity by its L-35 id-path, intersect its kind schema's R-CLI-005 introspection with its
    // composed value (kernel-side, gui/panels/builders), and answer the editable field set as data —
    // plus the root scene file's raw-byte hash, the CAS token a subsequent `set --if-match` (the
    // e09 wire write) guards on. An entity that does not resolve — unknown id-path, unregistered
    // kind — answers `{present: false}` rather than a failure envelope: "nothing is selected there"
    // is a renderable panel state, not a protocol fault, and it mints no new catalog code.
    if (method == "editor.inspect")
    {
        const std::optional<std::string> path = string_param(params, "path");
        const std::optional<std::string> id_path_joined = string_param(params, "idPath");
        if (!path.has_value() || !id_path_joined.has_value())
            return Envelope::failure("usage.missing_argument",
                                     "editor.inspect requires string 'path' and 'idPath' params");

        const compose::ProjectSceneResolver resolver(kernel_.config().project_root);
        const compose::ComposedScene scene = compose::flatten(*path, resolver);

        // The builders library owns the identity-key encoding AND its inverse lookup — the same
        // join_identity every wire consumer keys by (a third hand-rolled copy here would be the
        // drift risk the exported helper exists to close).
        const compose::ComposedEntity* entity =
            gui::panels::builders::find_entity_by_identity(scene, *id_path_joined);

        Json data = Json::object();
        // The driving kind is the ROOT SCENE's — only `ctx:scene` documents enter composition
        // (compose/scene_model.h), and its introspection publishes the entity fields under
        // `/entities/[]/…` (exactly what build_inspector_model intersects). A registry without it
        // would be an engine-registration defect — answered as the same honest present:false as an
        // unknown entity, never invented.
        const schema::KindSchema* kind =
            entity == nullptr ? nullptr : schema::engine_schemas().latest("ctx:scene");

        if (entity == nullptr || kind == nullptr)
        {
            Json inspector = Json::object();
            inspector.set("present", Json(false));
            data.set("inspector", std::move(inspector));
        }
        else
        {
            data.set("inspector", gui::panels::builders::inspector_to_wire(
                                      gui::panels::builders::build_inspector_model(*entity, *kind,
                                                                                   *path)));
        }

        // The root scene file's CURRENT raw-byte hash — the outermost target's CAS token (the same
        // read the disk-backed gateway's FieldState carries). 0 when the file is unreadable.
        // The R-SEC-008 jail is the CALLER's duty on a NativeFileStore (native_file_store.h): an
        // escaping wire `path` (`../`, absolute) must not turn this read into an out-of-project
        // existence/content-hash oracle — the composed read above needs no twin check because
        // ProjectSceneResolver jails internally. An escaping path answers 0, the same honest
        // "no CAS token" an unreadable file gets.
        std::uint64_t raw_hash = 0;
        if (filesync::is_inside_jail(".", *path))
        {
            const filesync::NativeFileStore store(kernel_.config().project_root);
            if (const std::optional<std::string> bytes = store.read(*path))
                raw_hash = filesync::content_hash(*bytes);
        }
        data.set("rawHash", hash_string(raw_hash));
        data.set("generation", Json(kernel_.generation()));
        return Envelope::success(std::move(data), kernel_.generation());
    }

    // --- M9 e08a: the DAEMON SESSION-STATE verbs (D7 tier 1, design 05 §4) ----------------------
    // Selection / cameras / play live HERE, in the daemon, so a second window, the CLI, and a
    // scripted agent all read and drive ONE truth. Every real change publishes a `session` topic
    // fact stamped with `origin` = the acting client's id; an unchanged call publishes NOTHING (an
    // event for a no-op would be an echo generator, and `origin` exists precisely to kill echoes).
    // The dispatcher already enforced session_control (scope.cpp) before we are reached.
    if (method == "editor.select")
    {
        if (!params.contains("ids") || !params.at("ids").is_array())
            return Envelope::failure("usage.missing_argument",
                                     "editor select requires an 'ids' array of L-35 id-paths");
        std::vector<std::string> ids;
        ids.reserve(params.at("ids").size());
        for (std::size_t i = 0; i < params.at("ids").size(); ++i)
        {
            const Json& id = params.at("ids").at(i);
            if (!id.is_string())
                return Envelope::failure("usage.invalid",
                                         "editor select 'ids[" + std::to_string(i) +
                                             "]' must be an L-35 id-path string");
            ids.push_back(id.as_string());
        }

        SelectionMode mode = SelectionMode::replace;
        if (const std::optional<std::string> raw = string_param(params, "mode"); raw.has_value())
        {
            const std::optional<SelectionMode> parsed = parse_selection_mode(*raw);
            if (!parsed.has_value())
                return Envelope::failure("usage.invalid",
                                         "editor select 'mode' must be one of replace | add | "
                                         "toggle | remove, got '" +
                                             *raw + "'");
            mode = *parsed;
        }

        const bool changed = session_.apply_selection(ids, mode);
        if (changed)
        {
            Json ev = session_fact("selection-changed", session.client_id);
            ev.set("ids", selection_ids_json(session_));
            ev.set("mode", Json(std::string(selection_mode_token(mode))));
            kernel_.events().publish("session", std::move(ev));
        }

        Json data = Json::object();
        data.set("ids", selection_ids_json(session_));
        data.set("mode", Json(std::string(selection_mode_token(mode))));
        data.set("changed", Json(changed));
        return Envelope::success(std::move(data));
    }

    if (method == "editor.selection-get")
    {
        Json data = Json::object();
        data.set("ids", selection_ids_json(session_));
        return Envelope::success(std::move(data));
    }

    if (method == "editor.camera-set")
    {
        const std::optional<std::string> viewport = string_param(params, "viewportId");
        if (!viewport.has_value() || viewport->empty())
            return Envelope::failure("usage.missing_argument",
                                     "editor camera-set requires a non-empty string 'viewportId'");
        // transform/projection are carried OPAQUELY (the viewport owns their meaning), so an absent
        // member is a null — not an error. A camera with neither is still a legitimate registration.
        Json transform = params.contains("transform") ? params.at("transform") : Json();
        Json projection = params.contains("projection") ? params.at("projection") : Json();

        const bool changed =
            session_.set_camera(*viewport, std::move(transform), std::move(projection));
        if (changed)
        {
            Json ev = session_fact("camera-changed", session.client_id);
            ev.set("viewportId", Json(*viewport));
            kernel_.events().publish("session", std::move(ev));
        }

        Json data = Json::object();
        data.set("viewportId", Json(*viewport));
        data.set("changed", Json(changed));
        return Envelope::success(std::move(data));
    }

    if (method == "editor.cameras-get")
    {
        Json data = Json::object();
        data.set("cameras", cameras_json(session_));
        return Envelope::success(std::move(data));
    }

    if (method == "editor.play" || method == "editor.pause" || method == "editor.stop" ||
        method == "editor.step")
    {
        PlayOutcome outcome;
        if (method == "editor.play")
            outcome = session_.play();
        else if (method == "editor.pause")
            outcome = session_.pause();
        else if (method == "editor.stop")
            outcome = session_.stop();
        else
        {
            // `--ticks` is a verb FLAG, so it arrives as a string over the CLI projection and as a
            // number over a hand-written RPC call — accept both rather than making the wire shape
            // depend on which door the client came through.
            std::uint64_t ticks = 1;
            if (params.contains("ticks"))
            {
                const Json& raw = params.at("ticks");
                if (raw.is_number() && raw.as_int() >= 0)
                    ticks = static_cast<std::uint64_t>(raw.as_int());
                else if (raw.is_string())
                {
                    const std::optional<std::uint64_t> parsed = parse_ticks(raw.as_string());
                    if (!parsed.has_value())
                        return Envelope::failure(
                            "usage.invalid", "editor step '--ticks' expects a non-negative integer, "
                                             "got '" + raw.as_string() + "'");
                    ticks = *parsed;
                }
                else
                {
                    return Envelope::failure("usage.invalid",
                                             "editor step '--ticks' expects a non-negative integer");
                }
            }
            outcome = session_.step(ticks);
        }

        if (!outcome.ok)
            return Envelope::failure(outcome.error_code,
                                     "no live play session: start one with `editor play` first "
                                     "(L-51 edit state).");

        if (outcome.changed)
        {
            Json ev = session_fact("play-state", session.client_id);
            ev.set("state", Json(std::string(play_state_token(outcome.state))));
            ev.set("simTick", Json(outcome.sim_tick));
            kernel_.events().publish("session", std::move(ev));
        }

        Json data = Json::object();
        data.set("state", Json(std::string(play_state_token(outcome.state))));
        data.set("simTick", Json(outcome.sim_tick));
        data.set("changed", Json(outcome.changed));
        return Envelope::success(std::move(data));
    }

    // `resource.read` — the R-CLI-017 large-result fetch (CLI: `context fetch`): read a bounded,
    // hex-encoded chunk of a spooled oversized result by its opaque handle. v1 resolves against
    // THIS live daemon only (same-filesystem scope); a foreign / stale / malformed handle is
    // resource.unknown_handle. Read-only (read/query baseline).
    if (method == "resource.read")
    {
        const std::optional<std::string> uri = string_param(params, "handle");
        if (!uri.has_value())
            return Envelope::failure("usage.missing_argument",
                                     "resource.read requires a string 'handle' param");
        const std::optional<contract::ResourceHandle> handle =
            contract::ResourceHandle::parse(*uri);
        if (!handle.has_value())
            return Envelope::failure("resource.unknown_handle",
                                     "malformed resource URI: " + *uri);

        std::uint64_t offset = 0;
        std::uint64_t length = 0; // 0 = up to the chunk cap
        if (params.contains("range") && params.at("range").is_object())
        {
            const Json& range = params.at("range");
            if (range.contains("offsetBytes"))
                offset = static_cast<std::uint64_t>(range.at("offsetBytes").as_int());
            if (range.contains("lengthBytes"))
                length = static_cast<std::uint64_t>(range.at("lengthBytes").as_int());
        }

        const std::optional<bridge::ResourceStore::ReadResult> chunk =
            resources().read(*handle, offset, length);
        if (!chunk.has_value())
            return Envelope::failure("resource.unknown_handle",
                                     "no such resource on this daemon instance (expired, foreign, "
                                     "or out-of-range read): " +
                                         *uri);

        Json data = Json::object();
        data.set("handle", Json(*uri));
        data.set("offsetBytes", Json(chunk->offset));
        data.set("lengthBytes", Json(static_cast<std::uint64_t>(chunk->bytes.size())));
        data.set("totalBytes", Json(chunk->total));
        data.set("eof", Json(chunk->eof));
        data.set("chunkHex", Json(bridge::hex_encode(chunk->bytes)));
        return Envelope::success(std::move(data));
    }

    // `shutdown` — ask the serve loop to stop after replying (session_control scope, enforced above).
    if (method == "shutdown")
    {
        stop_.store(true); // `stop_` is mutable; the serve loop breaks after this reply is written
        Json data = Json::object();
        data.set("stopping", Json(true));
        return Envelope::success(std::move(data));
    }

    return std::nullopt; // not an operational verb — let the dispatcher route it (describe / reserved)
}

std::string KernelServer::finalize_response(std::string response) const
{
    // The R-CLI-017 oversized-response gate. Anything at or under the threshold travels inline.
    if (response.size() <= large_result_threshold_)
        return response;

    // Only a well-formed SUCCESS result spools: error responses stay inline (they are small by
    // construction), and a malformed/unexpected shape passes through untouched (defensive — the
    // transport frame cap remains the hard backstop).
    Json doc;
    try
    {
        doc = Json::parse(response);
    }
    catch (const std::exception&)
    {
        return response;
    }
    if (!doc.is_object() || !doc.contains("result") || !doc.at("result").is_object())
        return response;
    const Json& result = doc.at("result");
    if (!result.contains("ok") || !result.at("ok").as_bool())
        return response;

    // Never spool a reply that IS the large-result mechanism (a resource.read chunk or an already-
    // spooled largeResult) — that would recurse the fetch.
    if (result.contains("data") && result.at("data").is_object() &&
        (result.at("data").contains("chunkHex") || result.at("data").contains("largeResult")))
        return response;

    // Spool the WHOLE original result envelope: the fetched payload re-parses as exactly what the
    // daemon would have returned inline (ok/data/generationAfter/warnings).
    const std::string payload = result.dump(0);
    const std::optional<contract::ResourceHandle> handle = resources().put(payload);
    if (!handle.has_value())
        return response; // spool failed: degrade to inline (the frame cap still bounds it)

    Json data = Json::object();
    data.set("largeResult", handle->to_json(resources().local_path_hint(*handle)));

    Json replacement = Json::object();
    replacement.set("ok", Json(true));
    replacement.set("data", std::move(data));
    if (result.contains("generationAfter"))
        replacement.set("generationAfter", result.at("generationAfter"));
    Json warnings = Json::array();
    if (result.contains("warnings") && result.at("warnings").is_array())
    {
        for (std::size_t i = 0; i < result.at("warnings").size(); ++i)
            warnings.push_back(result.at("warnings").at(i));
    }
    warnings.push_back(Json(std::string(
        "result exceeded the inline threshold; fetch it via resource.read (context fetch)")));
    replacement.set("warnings", std::move(warnings));

    Json out = Json::object();
    out.set("jsonrpc", Json(std::string("2.0")));
    if (doc.contains("id"))
        out.set("id", doc.at("id"));
    out.set("result", std::move(replacement));
    return out.dump(0);
}

int KernelServer::serve(bridge::TransportServer& server)
{
    const std::string endpoint = server.endpoint();

    // --- M9 e08a: restore the daemon-owned editor session state BEFORE the first client attaches --
    // (03 §1: the daemon is the single writer of `.editor/session.json`.) A corrupt file is renamed
    // aside and defaults are loaded — LOUDLY (07 §6): a `diagnostics` event, so any client that
    // subscribes with `sinceSeq: 0` replays it out of the ring, PLUS stderr, so the operator sees it
    // even with nobody attached. Never fatal: a daemon that refused to boot over a convenience file
    // would be strictly worse than one that forgot a selection.
    {
        const SessionRestoreReport report = restore_session();
        if (report.outcome == SessionRestoreOutcome::recovered)
        {
            const std::string message =
                "the editor session file was unreadable and has been reset to defaults: " +
                report.detail +
                (report.quarantined_path.empty()
                     ? std::string()
                     : "; the corrupt file was moved aside to " + report.quarantined_path);
            Json diag = Json::object();
            diag.set("code", Json(std::string(kEditorSessionStateInvalidCode)));
            diag.set("message", Json(message));
            diag.set("pointer", Json(report.path));
            kernel_.events().publish("diagnostics", std::move(diag), bridge::Stability::stable);
            std::fprintf(stderr, "[editor-session] %s\n", message.c_str());
            std::fflush(stderr);
        }
    }

    // ONE dispatch mutex serializes every handle() + finalize_response() + fan-out (L-50: the mutation
    // model stays single-threaded — concurrency lives at the transport, not the write queue). It also
    // guards the connection registry + per-connection `session`/`live`. It is a serve()-local, so it
    // outlives every reader/writer thread (all joined before serve() returns).
    std::mutex dispatch_mu;
    std::vector<std::shared_ptr<Conn>> conns; // all connections (guarded by dispatch_mu)
    std::uint64_t next_client_id = 0;

    // Fan every event a dispatch just published out to each subscribed, live connection's outbound
    // queue. Runs UNDER dispatch_mu (serialized with dispatch; sees a consistent event stream). A conn
    // that cannot take an event (budget full) or whose subscription queue gapped is owed a re-snapshot
    // gap marker, delivered as soon as its outbound queue drains — the daemon never blocks on it.
    auto fan_out = [this, &conns]() {
        for (const std::shared_ptr<Conn>& c : conns)
        {
            if (!c->live || !c->session.attached || c->session.subscriptions.empty())
                continue;
            if (c->gap_owed) // deliver an owed gap marker first, once the budget allows
            {
                if (enqueue_event(*c, gap_frame()))
                    c->gap_owed = false;
                else
                    continue; // still no room — keep the gap owed, skip events this pass
            }
            for (const std::string& sub_id : c->session.subscriptions)
            {
                for (const bridge::Event& ev : kernel_.events().poll(sub_id))
                {
                    if (!enqueue_event(*c, event_frame(sub_id, ev.to_json())))
                    {
                        c->gap_owed = true; // outbound budget full -> the client will re-snapshot
                        break;
                    }
                }
                if (kernel_.events().sub_gapped(sub_id))
                {
                    c->gap_owed = true; // the subscription's own bounded queue overflowed
                    kernel_.events().reset_sub_gap(sub_id);
                }
            }
        }
    };

    // Reap finished connections (join their threads) so thread objects don't accumulate over a
    // long-lived daemon's connect/disconnect churn. Joins OUTSIDE the lock.
    auto reap = [&conns, &dispatch_mu]() {
        std::vector<std::shared_ptr<Conn>> done;
        {
            std::lock_guard<std::mutex> lk(dispatch_mu);
            for (auto it = conns.begin(); it != conns.end();)
            {
                if ((*it)->finished.load())
                {
                    done.push_back(*it);
                    it = conns.erase(it);
                }
                else
                    ++it;
            }
        }
        for (const std::shared_ptr<Conn>& c : done)
            if (c->thread.joinable())
                c->thread.join();
    };

    // ONE thread per connection owns the handle for BOTH reads and writes: flush any queued outbound
    // frames (responses + pushed events), then a bounded timed read; dispatch a received request under
    // the ONE dispatch mutex (+ fan out its events), enqueue the response, repeat to disconnect. Reads
    // and writes never overlap on the handle (so a synchronous Windows pipe never deadlocks), and a
    // slow client can never stall the daemon — the outbound queue is bounded (gap marker on overflow).
    auto conn_body = [this, &dispatch_mu, &fan_out, &endpoint](std::shared_ptr<Conn> c) {
        if (c->reject)
        {
            // Over-cap (D19 bound): read the attach frame (bounded), reply daemon.busy, flush, close.
            for (int i = 0; i < 600 && !stop_.load(); ++i)
            {
                bool timed_out = false;
                const std::optional<std::string> req = c->conn.read_frame_timed(kPollIntervalMs, timed_out);
                if (timed_out)
                    continue;
                if (req.has_value())
                    enqueue_response(*c, busy_response(*req));
                break;
            }
            (void)flush_outbound(*c);
        }
        else
        {
            for (;;)
            {
                if (!flush_outbound(*c))
                    break; // peer gone while flushing responses/events
                if (stop_.load())
                {
                    // A `shutdown` verb was served (its reply was just flushed above); wake the parked
                    // acceptor so the whole daemon winds down.
                    wake_endpoint(endpoint);
                    break;
                }
                bool timed_out = false;
                const std::optional<std::string> request =
                    c->conn.read_frame_timed(kPollIntervalMs, timed_out);
                if (timed_out)
                    continue; // no request pending -> loop to flush pushed events + re-poll
                if (!request.has_value())
                    break; // client disconnected (or a framing error)

                std::string response;
                {
                    std::lock_guard<std::mutex> lk(dispatch_mu);
                    // Dispatch, then the R-CLI-017 oversized-response gate, then fan out any events
                    // this dispatch published to every subscribed connection — all serialized (L-50).
                    response = finalize_response(
                        kernel_.daemon().dispatcher().handle(*request, c->session));
                    fan_out();
                }
                if (!response.empty())
                    enqueue_response(*c, std::move(response)); // flushed at the top of the next loop iteration
            }
        }

        // --- teardown: leave the fan-out set, announce detach to the remaining subscribers ---------
        {
            std::lock_guard<std::mutex> lk(dispatch_mu);
            c->live = false; // stop receiving fan-out
            for (const std::string& sub_id : c->session.subscriptions)
                kernel_.events().unsubscribe(sub_id);
            c->session.subscriptions.clear();
            if (!c->reject && c->session.attached)
            {
                // The `clients` topic fires on detach (mirrors the attach event the dispatcher emits).
                Json ev = Json::object();
                ev.set("event", Json(std::string("detached")));
                ev.set("clientId", Json(c->id));
                kernel_.events().publish("clients", std::move(ev));
                fan_out();
            }
        }
        c->finished.store(true); // reap (or the shutdown join) may now join this connection's thread
    };

    int rc = 0;
    while (!stop_.load())
    {
        reap();

        std::optional<bridge::TransportConnection> accepted = server.accept();
        if (stop_.load())
        {
            // Either a genuine `shutdown` (the wake connection) or an external stop() — wind down.
            break;
        }
        if (!accepted.has_value())
        {
            // A null accept() is EITHER a clean stop()/listener-closed OR a genuine listener error —
            // only the latter sets server.error(). Surface an error as a non-zero exit.
            if (!server.error().empty())
                rc = 1;
            break;
        }

        // D19 bound: is the daemon already serving its max concurrent clients?
        bool at_capacity = false;
        {
            std::lock_guard<std::mutex> lk(dispatch_mu);
            std::size_t serving = 0;
            for (const std::shared_ptr<Conn>& c : conns)
                if (c->live && !c->reject)
                    ++serving;
            at_capacity = serving >= max_connections_;
        }

        auto c = std::make_shared<Conn>(std::move(*accepted), ++next_client_id, max_outbound_frames_);
        c->reject = at_capacity;
        {
            std::lock_guard<std::mutex> lk(dispatch_mu);
            conns.push_back(c);
        }
        c->thread = std::thread(conn_body, c);
    }

    // --- shutdown: let each connection thread FINISH ON ITS OWN first, then nudge only the ones that
    // could not. Snapshot + release the lock first — a thread's teardown needs dispatch_mu, so joining
    // under it would deadlock.
    //
    // The drain window below is LOAD-BEARING, not politeness (CE #352). `unblock()` half-closes the
    // socket (POSIX `shutdown(fd, SHUT_RDWR)`; Windows `DisconnectNamedPipe`), which DISCARDS anything
    // the connection has queued but not yet written — and the `shutdown` verb's own reply is exactly
    // that: `handle_operational` sets `stop_` while building the reply, `conn_body` enqueues it at the
    // BOTTOM of its iteration, and writes it via `flush_outbound()` at the TOP of the next one. Force-
    // unblocking in that window destroys the reply, and the client that asked for the shutdown sees
    // "peer disconnected before responding" instead of `{ok:true, data:{stopping:true}}`.
    //
    // It stayed invisible because the acceptor is normally PARKED in accept() and is only woken by
    // wake_endpoint(), which the replying thread calls AFTER its flush succeeded. But any connection
    // arriving in that same instant unparks the acceptor early — the `while (!stop_.load())` condition
    // then exits the loop immediately and we reach this teardown before the reply is written. Measured
    // on Linux with a single-variable A/B (one throwaway connect+close immediately before `shutdown`):
    // 0/40 replies lost without it, 19/40 lost with it.
    //
    // Draining is an ORDERING fix, not a widened timeout: a responsive peer's thread reaches its loop
    // top within one kPollIntervalMs, flushes everything queued, observes stop_ and marks itself
    // finished — so this returns in milliseconds. The bound exists only for a peer that will not READ
    // (its thread is stuck inside a blocking write) or is stalled mid-frame; those are precisely the
    // stragglers unblock() is for, and the daemon must never wait on them indefinitely.
    std::vector<std::shared_ptr<Conn>> remaining;
    {
        std::lock_guard<std::mutex> lk(dispatch_mu);
        remaining = conns;
    }
    const auto drain_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kShutdownDrainGraceMs);
    for (;;)
    {
        bool all_finished = true;
        for (const std::shared_ptr<Conn>& c : remaining)
            if (!c->finished.load())
            {
                all_finished = false;
                break;
            }
        if (all_finished || std::chrono::steady_clock::now() >= drain_deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (const std::shared_ptr<Conn>& c : remaining)
        if (!c->finished.load())
            c->conn.unblock(); // read-only on the handle -> race-free with the thread's in-flight read
    for (const std::shared_ptr<Conn>& c : remaining)
        if (c->thread.joinable())
        {
            c->thread.join();
        }
    {
        std::lock_guard<std::mutex> lk(dispatch_mu);
        conns.clear();
    }

    // --- M9 e08a: the CLEAN-SHUTDOWN write of the daemon-owned session state --------------------
    // Every connection thread has joined, so nothing else can touch the state. A failed write is
    // reported, never fatal — losing a remembered selection must not turn a clean stop into a
    // non-zero exit that a supervisor reads as a crash.
    {
        std::string persist_error;
        if (!persist_session(persist_error))
        {
            std::fprintf(stderr, "[editor-session] could not persist the editor session state: %s\n",
                         persist_error.c_str());
            std::fflush(stderr);
        }
    }
    return rc;
}

} // namespace context::editor::editorkernel
