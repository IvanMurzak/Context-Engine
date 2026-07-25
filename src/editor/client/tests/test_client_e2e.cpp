// T1 (the LIVE half) — context_client driven against a REAL `context daemon` process over the real
// IPC wire. The mock suite (test_subscription.cpp) proves the consumer's state machine; this proves
// the mock and the daemon actually agree, which no amount of mocking can.
//
//   1. attach + subscribe + snapshot-then-delta: a real edit over the wire produces real events that
//      the consumer applies, with real ack cursors.
//   2. D20 enforcement (default ON): an attach carrying NO token is REFUSED by the live daemon, and
//      the same attach WITH the discovered token succeeds.
//
// CONTEXT_BINARY is the built `context` executable path (a compile-time define). The daemon child is
// always reaped (shutdown, else killed) so the test never hangs or leaks a process.

#include "context/editor/client/client.h"
#include "context/editor/client/subscription.h"

#include "client_test.h"
#include "process_util.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using context::editor::client::AttachOptions;
using context::editor::client::Client;
using context::editor::client::ClientEvent;
using context::editor::client::discover_instance;
using context::editor::client::InstanceInfo;
using context::editor::client::SubscriptionConsumer;
using context::editor::client::SubscriptionSpec;
using context::editor::contract::Json;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace
{
fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir =
        fs::temp_directory_path() / ("ctx-client-e2e-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// Ask a live daemon to stop, then reap the child (kill it if it will not go quietly, so CI never
// inherits a stray process).
void shutdown_daemon(ctest_proc::Process& child, const InstanceInfo& instance)
{
    AttachOptions options;
    options.scope = "write,session";
    options.token = instance.token;
    std::unique_ptr<context::editor::client::WireChannel> channel =
        context::editor::client::make_transport_channel(instance.endpoint, 3000);
    if (channel)
    {
        Client client(std::move(channel));
        std::string error;
        if (client.attach(options, error))
            (void)client.call("shutdown", Json::object(), error);
    }
    int exit_code = 0;
    if (!ctest_proc::wait_for(child, 5000, exit_code))
        ctest_proc::kill(child);
    ctest_proc::release(child);
}

// Kill + reap a child we are abandoning (a failed boot).
void abandon_daemon(ctest_proc::Process& child)
{
    ctest_proc::kill(child);
    ctest_proc::release(child);
}

// --- 1. live subscription: snapshot-then-delta + real events + acks -------------------------------
void test_live_subscription_receives_real_events()
{
    const fs::path project = make_temp_project("sub");
    ctest_proc::Process daemon =
        ctest_proc::spawn(CONTEXT_BINARY, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));

    const std::optional<InstanceInfo> instance = discover_instance(project, 15000);
    CHECK(instance.has_value());
    if (!instance.has_value())
    {
        abandon_daemon(daemon);
        return;
    }
    // The daemon publishes a D20 token — enforcement is ON by default since e02.
    CHECK(!instance->token.empty());

    std::unique_ptr<context::editor::client::WireChannel> channel =
        context::editor::client::make_transport_channel(instance->endpoint, 5000);
    CHECK(channel != nullptr);
    Client client(std::move(channel));

    AttachOptions options;
    options.scope = "write,session";
    options.token = instance->token;
    std::string error;
    CHECK(client.attach(options, error));
    CHECK(error.empty());

    std::vector<ClientEvent> seen;
    SubscriptionConsumer::Options consumer_options;
    consumer_options.ack_interval = 1; // ack eagerly so the retention floor is exercised
    consumer_options.poll_timeout_ms = 100;
    SubscriptionConsumer consumer(client, options, consumer_options);
    consumer.on_event([&seen](const std::string&, const ClientEvent& e) { seen.push_back(e); });
    consumer.add(SubscriptionSpec{{}, ""}); // every topic

    CHECK(consumer.start(error));
    CHECK(error.empty());
    // The snapshot-then-delta contract: a snapshot arrived, carrying the live incarnation id.
    CHECK(consumer.stats().snapshots_taken == 1);
    CHECK(!consumer.incarnation_id().empty());
    CHECK(consumer.states()[0].live);

    // Drive REAL work through the daemon: the edit lands on disk, then `reconcile` folds it in and
    // SETTLES — advancing the derived-world generation and publishing the `derivation.settled`
    // quiescence event. (A bare `edit` deliberately does not settle, so it publishes nothing; the
    // events this subscription is here to observe come from the settle.)
    Json edit_params = Json::object();
    edit_params.set("path", Json(std::string("proj/e2e.scene")));
    edit_params.set("content", Json(std::string("entity: 1")));
    CHECK(client.call("edit", std::move(edit_params), error).has_value());
    CHECK(client.call("reconcile", Json::object(), error).has_value());

    // Pump until the events the edit produced have been applied (bounded, so CI never hangs).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (seen.empty() && std::chrono::steady_clock::now() < deadline)
        CHECK(consumer.pump(error));

    CHECK(!seen.empty());
    if (!seen.empty())
    {
        // Every delivered event carries the wire envelope, in the live incarnation, after the
        // snapshot cursor.
        CHECK(seen.front().seq > 0);
        CHECK(seen.front().incarnation_id == consumer.incarnation_id());
        CHECK(!seen.front().topic.empty());
        CHECK(consumer.states()[0].last_seq >= seen.front().seq);
    }

    // Acks reached the daemon (the retention floor advances off the client's cursor).
    CHECK(consumer.flush_acks(error));
    CHECK(consumer.states()[0].acked_seq == consumer.states()[0].last_seq);
    CHECK(consumer.stats().acks_sent > 0);

    consumer.stop();
    shutdown_daemon(daemon, *instance);

    std::error_code ec;
    fs::remove_all(project, ec);
}

// --- 2. D20: tokenless attach is DENIED by the live daemon (enforcement default ON) ---------------
void test_tokenless_attach_is_denied()
{
    const fs::path project = make_temp_project("auth");
    ctest_proc::Process daemon =
        ctest_proc::spawn(CONTEXT_BINARY, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));

    const std::optional<InstanceInfo> instance = discover_instance(project, 15000);
    CHECK(instance.has_value());
    if (!instance.has_value())
    {
        abandon_daemon(daemon);
        return;
    }
    CHECK(!instance->token.empty());

    // No token -> the daemon refuses the handshake. This is the C-F1 step-3 assertion: with the
    // enforcement default flipped ON, an unauthenticated attach cannot reach any verb.
    {
        std::unique_ptr<context::editor::client::WireChannel> channel =
            context::editor::client::make_transport_channel(instance->endpoint, 5000);
        CHECK(channel != nullptr);
        Client anonymous(std::move(channel));
        AttachOptions options;
        options.token.clear();
        std::string error;
        bool rejected = false;
        CHECK(!anonymous.attach(options, error, &rejected));
        CHECK(rejected); // a daemon-side refusal, not a transport hiccup
        CHECK(!anonymous.attached());
    }

    // A WRONG token is refused the same way.
    {
        std::unique_ptr<context::editor::client::WireChannel> channel =
            context::editor::client::make_transport_channel(instance->endpoint, 5000);
        CHECK(channel != nullptr);
        Client impostor(std::move(channel));
        AttachOptions options;
        options.token = "0000000000000000";
        std::string error;
        bool rejected = false;
        CHECK(!impostor.attach(options, error, &rejected));
        CHECK(rejected);
    }

    // The DISCOVERED token attaches cleanly — enforcement gates impostors, not legitimate clients.
    {
        std::unique_ptr<context::editor::client::WireChannel> channel =
            context::editor::client::make_transport_channel(instance->endpoint, 5000);
        CHECK(channel != nullptr);
        Client legitimate(std::move(channel));
        AttachOptions options;
        options.scope = "write,session";
        options.token = instance->token;
        std::string error;
        CHECK(legitimate.attach(options, error));
        CHECK(error.empty());
        CHECK(legitimate.attached());
    }

    shutdown_daemon(daemon, *instance);

    std::error_code ec;
    fs::remove_all(project, ec);
}

// --- 3. connect_to_project: discovery + connect + token seeding in one step -----------------------
void test_connect_to_project_seeds_the_token()
{
    const fs::path project = make_temp_project("discover");
    ctest_proc::Process daemon =
        ctest_proc::spawn(CONTEXT_BINARY, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));

    const std::optional<InstanceInfo> instance = discover_instance(project, 15000);
    CHECK(instance.has_value());
    if (!instance.has_value())
    {
        abandon_daemon(daemon);
        return;
    }

    AttachOptions options;
    options.scope = "write,session";
    std::string error;
    std::unique_ptr<Client> client = Client::connect_to_project(project, 5000, error);
    CHECK(client != nullptr);
    if (client)
    {
        // The discovered token rides on the Client — a consumer never reads the instance file, and
        // never plumbs the token through its own attach options.
        CHECK(client->instance().token == instance->token);
        CHECK(client->connected());
        CHECK(options.token.empty()); // still unset: attach() falls back to the discovered one
        CHECK(client->attach(options, error));
        CHECK(client->attached());
    }

    shutdown_daemon(daemon, *instance);

    std::error_code ec;
    fs::remove_all(project, ec);
}
// --- 4. M9 e09b-1: the POINTER/VALUE composed write, on REAL DISK, end to end ----------------------
//
// WHY THIS HARNESS AND NOT test_kernel_server.cpp: that one hosts the kernel over a MemoryFileStore,
// whose keys are DISJOINT from the real on-disk files a composed read resolves against (it writes its
// compose fixtures with std::ofstream and its edits into memory). A pointer/value write reads
// through compose from real disk and writes through the kernel's store — under that split brain the
// two halves address different worlds, so the path is structurally untestable there. Here the daemon
// is the real process with a real NativeFileStore, so plan -> serialize -> atomic write -> derive is
// exercised exactly as it ships.
//
// Layout note: the daemon roots its FileStore at the project dir and jails the reconcile crawl to the
// `proj/` subdir (daemon_command.cpp), so authored paths are "proj/<file>" — the same string is the
// compose resolver's key and the kernel's write path, which is precisely what makes the write land
// where the read looked.
void test_composed_pointer_value_write_lands_on_real_disk()
{
    const fs::path project = make_temp_project("compose");
    const fs::path authored = project / "proj";
    std::error_code mk;
    fs::create_directories(authored, mk);

    // A real two-file composition on real disk: root instances child, so an `outermost` override
    // lands in root.scene.json — a file the CALLER never names. That is the whole point of the mode.
    const auto write_file = [&](const char* name, const std::string& text)
    {
        std::ofstream out(authored / name, std::ios::binary | std::ios::trunc);
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
      "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "proj/child.scene.json"}]})");

    ctest_proc::Process daemon =
        ctest_proc::spawn(CONTEXT_BINARY, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));
    const std::optional<InstanceInfo> instance = discover_instance(project, 15000);
    CHECK(instance.has_value());
    if (!instance.has_value())
    {
        abandon_daemon(daemon);
        return;
    }

    std::unique_ptr<context::editor::client::WireChannel> channel =
        context::editor::client::make_transport_channel(instance->endpoint, 5000);
    CHECK(channel != nullptr);
    Client client(std::move(channel));
    AttachOptions options;
    options.scope = "write,session";
    options.token = instance->token;
    std::string error;
    CHECK(client.attach(options, error));

    const std::string kRoot = "proj/root.scene.json";
    const std::string kIdPath = "aaaaaaaaaaaaaaa1/ccccccccccccccc1";
    const std::string kPointer = "/components/camera/fov";

    // The CAS token the editor actually holds: the root scene's raw hash, as reported by the SAME
    // read the Inspector hydrates from. Nothing here invents a hash.
    const auto inspect = [&]() -> std::optional<Json>
    {
        Json p = Json::object();
        p.set("path", Json(kRoot));
        p.set("idPath", Json(kIdPath));
        return client.call("editor.inspect", std::move(p), error);
    };
    const std::optional<Json> before = inspect();
    CHECK(before.has_value());
    if (!before.has_value())
    {
        shutdown_daemon(daemon, *instance);
        return;
    }
    const std::string base_token = before->at("data").at("rawHash").as_string();
    CHECK(base_token != "0"); // the file is readable, so there IS a CAS token

    // The well-formed composed request every case below starts from — one definition, so a wire-shape
    // change is made once and every scenario keeps exercising the SAME request it was written for.
    const auto base_params = [&]()
    {
        Json p = Json::object();
        p.set("rootScene", Json(kRoot));
        p.set("idPath", Json(kIdPath));
        p.set("pointer", Json(kPointer));
        p.set("value", Json(std::string("1.0")));
        return p;
    };

    const auto composed_edit = [&](const std::string& value, const std::string& if_match,
                                   bool* rejected) -> std::optional<Json>
    {
        Json p = base_params();
        p.set("value", Json(value)); // a JSON literal in a string (its canonical serialization)
        if (!if_match.empty())
            p.set("ifMatch", Json(if_match));
        return client.call("edit", std::move(p), error, rejected);
    };

    // --- the happy path: a field-addressed write lands in the file COMPOSITION chose --------------
    const std::optional<Json> applied = composed_edit("2.5", base_token, nullptr);
    CHECK(applied.has_value());
    if (!applied.has_value())
    {
        shutdown_daemon(daemon, *instance);
        return;
    }
    CHECK(applied->at("ok").as_bool());
    const Json& wrote = applied->at("data");
    // The caller addressed a FIELD; the daemon reports the FILE it resolved and wrote.
    CHECK(wrote.at("file").as_string() == kRoot);
    // The pointer the reply reports is the one written INSIDE that file — for an outermost
    // override that is the override ENTRY's slot, not the entity-relative field pointer the caller
    // sent. Reporting where the bytes actually landed is the R-CLI-006 provenance contract.
    CHECK(wrote.at("pointer").as_string() == "/overrides/0/value");
    CHECK(wrote.at("target").as_string() == "outermost");
    const std::string token_after_write = wrote.at("rawHash").as_string();
    CHECK(token_after_write != base_token); // the bytes genuinely moved

    // REAL DISK, not a seam: read the file back with a plain stream and see the override.
    {
        std::ifstream in(authored / "root.scene.json", std::ios::binary);
        const std::string on_disk((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        CHECK(on_disk.find("\"overrides\"") != std::string::npos);
        CHECK(on_disk.find(kPointer) != std::string::npos);
        CHECK(on_disk.find("2.5") != std::string::npos);
        // serialize_canonical's byte form (R-FILE-001): exactly one trailing newline.
        CHECK(!on_disk.empty() && on_disk.back() == '\n');
        CHECK(on_disk.size() >= 2 && on_disk[on_disk.size() - 2] != '\n');
    }

    // It went through COMPOSITION, not a blind byte write: the composed read now answers the new
    // value AND marks the field overridden, and the derived world agrees with the reported hash.
    {
        const std::optional<Json> after = inspect();
        CHECK(after.has_value());
        if (after.has_value())
        {
            CHECK(after->at("data").at("rawHash").as_string() == token_after_write);
            const Json& fields = after->at("data").at("inspector").at("fields");
            bool saw_overridden_fov = false;
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                const Json& f = fields.at(i);
                if (f.at("pointer").as_string() == kPointer && f.at("overridden").as_bool() &&
                    f.at("value").as_string() == "2.5")
                {
                    saw_overridden_fov = true;
                    break;
                }
            }
            CHECK(saw_overridden_fov);
        }
        Json qp = Json::object();
        qp.set("path", Json(kRoot));
        const std::optional<Json> derived = client.call("query", std::move(qp), error);
        CHECK(derived.has_value());
        if (derived.has_value())
        {
            CHECK(derived->at("data").at("present").as_bool());
            // The canonical hash the write reported IS the one derivation indexed — proof the bytes
            // on disk are the canonical form, without re-serializing them here.
            CHECK(derived->at("data").at("canonicalHash").as_string() ==
                  wrote.at("canonicalHash").as_string());
        }
    }

    // --- the CAS mismatch: the refusal carries the rebase input, and the SDK can READ it ----------
    // Re-using the now-STALE base token models the concurrent writer the L-30 guarantee is about.
    {
        bool rejected = false;
        const std::optional<Json> stale = composed_edit("9.5", base_token, &rejected);
        CHECK(!stale.has_value());
        CHECK(rejected);
        CHECK(client.last_error_code() == "cas.mismatch");
        // THE e09b-1 point: before this task the payload below existed on the wire but no SDK
        // consumer could reach it — parse_frame lifted `code` and dropped the rest.
        CHECK(!client.last_error_data().is_null());
        const Json& fresh = client.last_error_data().at("data");
        CHECK(fresh.at("path").as_string() == kRoot);
        CHECK(fresh.at("present").as_bool());
        CHECK(fresh.at("expectedRawHash").as_string() == base_token);
        CHECK(fresh.at("actualRawHash").as_string() == token_after_write);
        CHECK(!fresh.at("content").as_string().empty()); // the CURRENT bytes ride along

        // The stale write did NOT clobber: a refused CAS writes nothing.
        // ⚠ Windows: this read MUST be scoped so the handle is CLOSED before the retry below. The
        // daemon writes through filesync atomic-IO (temp + rename), and a rename over a file with an
        // open read handle FAILS on Windows — surfacing as a bare `internal.error` from the write,
        // which reads exactly like a product bug and is not one.
        {
            std::ifstream in(authored / "root.scene.json", std::ios::binary);
            const std::string on_disk((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            CHECK(on_disk.find("9.5") == std::string::npos);
        }

        // REBASE WITHOUT A SECOND READ (design 05 §7): retry using ONLY the token the refusal
        // carried — no re-read of the file, which is exactly what the payload exists to make
        // unnecessary.
        const std::string retry_token = fresh.at("actualRawHash").as_string();
        const std::optional<Json> rebased = composed_edit("9.5", retry_token, nullptr);
        CHECK(rebased.has_value());
        if (rebased.has_value())
            CHECK(rebased->at("data").at("rawHash").as_string() != token_after_write);
    }

    // --- failure paths: every one is a NAMED refusal, never a hopeful write -----------------------
    {
        const auto refused_with = [&](Json params, const char* expected_code)
        {
            bool rejected = false;
            const std::optional<Json> r = client.call("edit", std::move(params), error, &rejected);
            CHECK(!r.has_value());
            CHECK(rejected);
            CHECK(client.last_error_code() == expected_code);
        };

        // Mode confusion is refused, not guessed (the two shapes have no single honest reading).
        {
            Json p = base_params();
            p.set("content", Json(std::string("{}")));
            refused_with(std::move(p), "usage.invalid");
        }
        // A partial composed request names what is missing rather than falling back to the
        // full-content shape and reporting a confusing "requires path and content".
        {
            Json p = Json::object();
            p.set("rootScene", Json(kRoot));
            p.set("pointer", Json(kPointer));
            refused_with(std::move(p), "usage.missing_argument");
        }
        // A mis-typed target would otherwise silently default to `outermost` and write the WRONG
        // FILE — the one outcome this write path exists to prevent.
        {
            Json p = base_params();
            p.set("target", Json(std::string("outermsot")));
            refused_with(std::move(p), "usage.invalid");
        }
        // A retarget on the FULL-CONTENT shape is refused, not silently dropped. `target` and
        // `atInstance` are composed-shape-only, so a request naming one alongside {path, content}
        // has to reach the mode-confusion refusal — if it took the full-content branch instead, that
        // branch would never read them and the write would land in a file the caller did not name.
        {
            Json p = Json::object();
            p.set("path", Json(kRoot));
            p.set("content", Json(std::string("{}")));
            p.set("target", Json(std::string("template")));
            refused_with(std::move(p), "usage.invalid");
        }
        // Present-but-WRONG-TYPE is refused exactly like mis-spelled: read as "absent" instead, a
        // dropped `target`/`atInstance` writes the wrong file and a dropped `ifMatch` turns a
        // CAS-guarded write into an unconditional overwrite. `null` is a JS client's UNSET optional
        // and stays absent (asserted below); every other type is a usage error.
        {
            Json p = base_params();
            p.set("target", Json(std::int64_t{7}));
            refused_with(std::move(p), "usage.invalid");
        }
        {
            Json p = base_params();
            p.set("target", Json(std::string("at-instance")));
            p.set("atInstance", Json::array()); // the NATURAL shape for an id-path, and not a string
            refused_with(std::move(p), "usage.invalid");
        }
        {
            Json p = base_params();
            p.set("ifMatch", Json(std::int64_t{1234567890})); // the hash as a NUMBER, not a string
            refused_with(std::move(p), "usage.invalid");
        }
        // `at-instance` without its addressing prefix, and the prefix without the target.
        {
            Json p = base_params();
            p.set("target", Json(std::string("at-instance")));
            refused_with(std::move(p), "usage.missing_argument");
        }
        {
            Json p = base_params();
            p.set("atInstance", Json(std::string("aaaaaaaaaaaaaaa1")));
            refused_with(std::move(p), "usage.invalid");
        }
        // A malformed id-path (a doubled separator) cannot address an entity.
        {
            Json p = base_params();
            p.set("idPath", Json(std::string("aaaaaaaaaaaaaaa1//ccccccccccccccc1")));
            refused_with(std::move(p), "usage.invalid");
        }
        // A `value` that is not a JSON literal.
        {
            Json p = base_params();
            p.set("value", Json(std::string("{not json")));
            refused_with(std::move(p), "file.parse_error");
        }
        // L-37: the immutable identity pointers survive re-derivation and are never written.
        {
            Json p = base_params();
            p.set("pointer", Json(std::string("/id")));
            p.set("value", Json(std::string("\"bbbbbbbbbbbbbbb2\"")));
            // compose's OWN refusal code, named exactly: an assertion that merely required "some
            // code" would still pass if the L-37 guard were replaced by an unrelated usage error or
            // a parse failure, which is precisely the regression worth catching.
            refused_with(std::move(p), "compose.immutable_pointer");
        }
        // R-SEC-008: a scene path escaping the project root.
        {
            Json p = base_params();
            p.set("rootScene", Json(std::string("../outside.scene.json")));
            refused_with(std::move(p), "path.jail_violation");
        }
        // The COMPLEMENT of the type rule, and the reason it is `null`-tolerant: a generated client
        // pads every declared param, and JSON.stringify keeps `null` while dropping only
        // `undefined`. An explicit null is an UNSET optional, so this request must still WRITE —
        // a strict "present means supplied" reading would refuse it and leave such a client unable
        // to use either shape.
        {
            Json p = base_params();
            p.set("value", Json(std::string("3.5")));
            p.set("target", Json(nullptr));
            p.set("atInstance", Json(nullptr));
            p.set("ifMatch", Json(nullptr));
            const std::optional<Json> ok = client.call("edit", std::move(p), error);
            CHECK(ok.has_value());
            if (ok.has_value())
                CHECK(ok->at("data").at("target").as_string() == "outermost");
        }
    }

    // --- R-SEC-007: the composed mode is NOT a scope bypass ---------------------------------------
    // It is the same `edit` verb, so the dispatcher's file_write requirement already covers it — this
    // asserts that, because a new write SHAPE that forgot its scope check would be a silent hole.
    {
        std::unique_ptr<context::editor::client::WireChannel> ro_channel =
            context::editor::client::make_transport_channel(instance->endpoint, 5000);
        CHECK(ro_channel != nullptr);
        Client reader(std::move(ro_channel));
        AttachOptions ro_options;
        ro_options.scope = "read";
        ro_options.token = instance->token;
        std::string ro_error;
        CHECK(reader.attach(ro_options, ro_error));
        Json p = base_params();
        p.set("value", Json(std::string("0.5")));
        bool rejected = false;
        CHECK(!reader.call("edit", std::move(p), ro_error, &rejected).has_value());
        CHECK(rejected);
        CHECK(reader.last_error_code() == "scope.denied");
    }

    shutdown_daemon(daemon, *instance);

    std::error_code ec;
    fs::remove_all(project, ec);
}
} // namespace

int main()
{
    test_live_subscription_receives_real_events();
    test_tokenless_attach_is_denied();
    test_connect_to_project_seeds_the_token();
    test_composed_pointer_value_write_lands_on_real_disk();
    CLIENT_TEST_MAIN_END();
}
