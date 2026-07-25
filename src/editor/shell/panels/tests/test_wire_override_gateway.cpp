// T1 for the M9 e09b-2 WIRE override-write gateway (R-QA-013): the `edit` request shape the daemon
// actually parses, the `cas.mismatch` refusal payload the L-30 engine rebases from, the
// `editor.inspect` re-read, the fail-closed unbound posture, and the L-30 rebase AND drop paths
// driven through the REAL `commit_override_write` engine over this gateway.
//
// ⚠ THE MOCK IS AT THE **WIRE**, NOT AT THE CLIENT (mock_channel.h's standing warning): a double
// standing in for `client::Client` would let this suite pass over frames the daemon never sends. Here
// the frames are dispatcher.cpp's, a REAL `client::Client` parses them, and what is asserted is the
// gateway's behaviour over the real SDK. The cross-process `editor-session-concurrent-cas-t2` drill
// against a live `context daemon` is what proves the two agree.
//
// THE DRIFT GUARDS ARE THE FIRST TWO CASES, and they matter more than they look. The gateway MIRRORS
// two vocabularies it structurally cannot link (D10 forbids `context_compose` on the Shell's closure):
// `compose::write_target_token` and `builders::join_identity`. This TEST does link both, so the
// mirrors are asserted against the authorities themselves rather than against a copy of them. A drift
// there does not produce a compile error — it writes to the WRONG FILE, silently.

#include "context/editor/shell/panels/wire_override_gateway.h"

#include "context/editor/shell/panels/undo_feed.h" // e09c: the replay rides THIS gateway

#include "context/editor/client/client.h"
#include "context/editor/compose/compose_write.h"                       // write_target_token
#include "context/editor/gui/panels/builders/inspector_builder.h"       // build_inspector_model
#include "context/editor/gui/panels/builders/scene_tree_builder.h"      // join_identity
#include "context/editor/gui/panels/builders/wire.h"                    // inspector_to_wire
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include "panels_wire_test.h" // the shared REAL-client-over-a-scripted-wire fixture

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace panels = context::editor::shell::panels;
namespace inspector = context::editor::gui::panels::inspector;
namespace builders = context::editor::gui::panels::builders;
namespace compose = context::editor::compose;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;
namespace client = context::editor::client;
namespace undo = context::editor::gui::session::undo;
using context::editor::shell::panels::WireOverrideWriteGateway;
using Json = context::editor::contract::Json;

namespace
{

constexpr const char* kRoot = "root.scene.json";
constexpr const char* kPointer = "/components/camera/fov";
const std::vector<std::string> kIdPath = {"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"};
constexpr std::uint64_t kBaseHash = 111111111111111111ull; // > 2^53: the decimal-string discipline

// --- the scripted wire ------------------------------------------------------------------------
//
// `Wired` + `make_wired_client` live in panels_wire_test.h: the attach handshake's flat-in-`result`
// shape is a property of the DAEMON, so it is written down once rather than per suite.
using panelstest::make_wired_client;
using panelstest::Wired;

// The daemon's `edit` success data (kernel_server.cpp finish_edit + serve_composed_edit).
[[nodiscard]] Json edit_reply(const char* raw_hash, bool reflected)
{
    Json data = Json::object();
    data.set("file", Json(std::string(kRoot)));
    data.set("pointer", Json(std::string("/overrides/0/value")));
    data.set("target", Json(std::string("outermost")));
    data.set("rawHash", Json(std::string(raw_hash)));
    data.set("canonicalHash", Json(std::string("77")));
    data.set("reflected", Json(reflected));
    return clientmock::MockChannel::ok_envelope(std::move(data));
}

// The `cas.mismatch` nested `error.data.data` payload (design 05 §7). ONE conflict object for a
// single `edit` — the `conflicts` ARRAY is `edit-batch`'s shape (e09b-1's correction to the design
// prose). Reaching for the array here would find nothing and turn every rebase into a drop.
[[nodiscard]] Json cas_detail(const char* actual)
{
    Json d = Json::object();
    d.set("path", Json(std::string(kRoot)));
    d.set("present", Json(true));
    d.set("expectedRawHash", Json(std::to_string(kBaseHash)));
    d.set("actualRawHash", Json(std::string(actual)));
    d.set("content", Json(std::string("{\"$schema\":\"ctx:scene\"}\n")));
    return d;
}

// --- a real composed entity, so `editor.inspect` replies carry the REAL builder's wire shape ------

class MapResolver final : public compose::SceneResolver
{
public:
    bool add(const std::string& path, const std::string& json)
    {
        serializer::ParseResult parsed = serializer::parse_json(json);
        if (!parsed.ok)
        {
            return false;
        }
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, parsed.root);
        if (!doc.has_value())
        {
            return false;
        }
        docs_.emplace(path, std::move(*doc));
        return true;
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        const auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// An `editor.inspect` reply built by the DAEMON'S OWN builder over a real composed scene, with `fov`
// at `fov_value` — so what this suite hands the gateway is the wire shape the daemon emits, not a
// hand-written lookalike that could drift from it.
[[nodiscard]] Json inspect_reply(const char* fov_value, const char* raw_hash)
{
    MapResolver r;
    CHECK(r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Cam",
         "components": {
           "transform": {"position": [1, 2, 3]},
           "camera": {"fov": 1.0, "near": 0.1, "far": 500.0}
         }}
      ]})"));
    CHECK(r.add(kRoot, std::string(R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [],
      "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"}],
      "overrides": [
        {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"],
         "pointer": "/components/camera/fov", "value": )") +
                      fov_value + R"(, "base": 1.0}
      ]})"));

    const compose::ComposedScene scene = compose::flatten(kRoot, r);
    CHECK(scene.ok);
    const compose::ComposedEntity* entity = nullptr;
    for (const compose::ComposedEntity& e : scene.entities)
    {
        if (e.id_path == kIdPath)
        {
            entity = &e;
        }
    }
    CHECK(entity != nullptr);
    const schema::KindSchema* kind = schema::engine_schemas().latest("ctx:scene");
    CHECK(kind != nullptr);

    Json data = Json::object();
    if (entity != nullptr && kind != nullptr)
    {
        data.set("inspector",
                 builders::inspector_to_wire(builders::build_inspector_model(*entity, *kind, kRoot)));
    }
    data.set("rawHash", Json(std::string(raw_hash)));
    return clientmock::MockChannel::ok_envelope(std::move(data));
}

[[nodiscard]] serializer::JsonValue literal(const char* json)
{
    serializer::ParseResult parsed = serializer::parse_json(json);
    CHECK(parsed.ok);
    return std::move(parsed.root);
}

[[nodiscard]] bool canonical_equal(const serializer::JsonValue& a, const serializer::JsonValue& b)
{
    std::string sa;
    std::string sb;
    const bool oka = serializer::serialize_canonical(a, sa);
    const bool okb = serializer::serialize_canonical(b, sb);
    return oka == okb && sa == sb;
}

[[nodiscard]] inspector::OverrideWriteRequest fov_request(const char* value)
{
    inspector::OverrideWriteRequest request;
    request.root_scene = kRoot;
    request.id_path = kIdPath;
    request.pointer = kPointer;
    request.value = literal(value);
    request.target = inspector::OverrideWriteTarget::outermost;
    return request;
}

// --- cases ----------------------------------------------------------------------------------------

// DRIFT GUARD 1: the target vocabulary. `compose::write_target_token` is the authority the daemon
// parses with; the gateway mirrors it because D10 forbids linking compose on the Shell's closure.
void target_tokens_mirror_the_compose_authority()
{
    CHECK(WireOverrideWriteGateway::write_target_token(inspector::OverrideWriteTarget::outermost) ==
          compose::write_target_token(compose::WriteTarget::outermost));
    CHECK(WireOverrideWriteGateway::write_target_token(
              inspector::OverrideWriteTarget::defining_template) ==
          compose::write_target_token(compose::WriteTarget::defining_template));
    CHECK(WireOverrideWriteGateway::write_target_token(
              inspector::OverrideWriteTarget::at_instance) ==
          compose::write_target_token(compose::WriteTarget::at_instance));
    // And every mirrored token survives the daemon's own inverse — a token `parse_write_target`
    // refuses is a write the daemon rejects outright (it deliberately does NOT default to
    // `outermost`), so this is the assertion that the mirror is USABLE, not merely equal.
    for (const inspector::OverrideWriteTarget t : {inspector::OverrideWriteTarget::outermost,
                                                   inspector::OverrideWriteTarget::defining_template,
                                                   inspector::OverrideWriteTarget::at_instance})
    {
        CHECK(compose::parse_write_target(WireOverrideWriteGateway::write_target_token(t))
                  .has_value());
    }
}

// DRIFT GUARD 2: the identity encoding. `builders::join_identity` is the authority every wire
// consumer keys by, and `split_identity` is what the daemon parses `idPath` with (kernel_server's
// read_id_path). A gateway whose join disagrees addresses a DIFFERENT entity.
void id_path_join_mirrors_the_builders_authority()
{
    const std::vector<std::vector<std::string>> paths = {
        {"e1"}, kIdPath, {"a", "b", "c", "d"}};
    for (const std::vector<std::string>& path : paths)
    {
        const std::string joined = WireOverrideWriteGateway::join_id_path(path);
        CHECK(joined == builders::join_identity(path));
        const std::optional<std::vector<std::string>> back = builders::split_identity(joined);
        CHECK(back.has_value());
        CHECK(back.has_value() && *back == path);
    }
    CHECK(WireOverrideWriteGateway::join_id_path({}).empty());
}

// The `edit` request the daemon actually parses: the composed (pointer/value) shape, the canonical
// value literal, the explicit target, and the CAS token as a DECIMAL STRING.
void an_applied_write_sends_the_composed_edit_shape()
{
    Wired wired = make_wired_client();
    wired.channel->on("edit", [](const clientmock::Request&) { return edit_reply("222", true); });

    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());
    CHECK(gateway.has_client());

    const inspector::WriteAttempt attempt = gateway.attempt(fov_request("2.5"), kBaseHash);
    CHECK(attempt.applied);
    CHECK(!attempt.cas_mismatch);
    CHECK(attempt.file == kRoot);
    CHECK(attempt.pointer == "/overrides/0/value"); // where the bytes LANDED (provenance)
    CHECK(attempt.raw_hash == 222u);
    CHECK(gateway.writes_issued() == 1);
    CHECK(gateway.writes_applied() == 1);
    CHECK(gateway.barrier_misses() == 0);

    const std::vector<clientmock::Request> sent = wired.channel->requests_for("edit");
    CHECK(sent.size() == 1);
    if (sent.size() == 1)
    {
        const Json& p = sent[0].params;
        CHECK(p.at("rootScene").as_string() == kRoot);
        CHECK(p.at("idPath").as_string() == builders::join_identity(kIdPath));
        CHECK(p.at("pointer").as_string() == kPointer);
        // A JSON literal in a STRING, canonical, with NO trailing newline — byte-identical to what
        // the daemon's own `inspector_to_wire` produces for the READ side of the same field.
        CHECK(p.at("value").as_string() == "2.5");
        // SENT EXPLICITLY: an unparseable token is refused by the daemon rather than defaulted, so
        // omitting it would let a future retarget silently write the outermost file.
        CHECK(p.at("target").as_string() == "outermost");
        // A DECIMAL STRING — the hash exceeds 2^53 and cannot survive a JSON number.
        CHECK(p.at("ifMatch").is_string());
        CHECK(p.at("ifMatch").as_string() == std::to_string(kBaseHash));
        // `at-instance` is not in play, so its addressing prefix must be ABSENT, not empty: an empty
        // string would reach the daemon's own "at-instance without atInstance" refusal.
        CHECK(!p.contains("atInstance"));
        // The full-content shape's keys are never mixed in — the daemon refuses mode confusion.
        CHECK(!p.contains("content"));
        CHECK(!p.contains("path"));
    }
}

// A zero base hash means "no guard" — exactly `context set` without --if-match — and must NOT be sent
// as the literal string "0", which the daemon would read as a real (never-matching) precondition.
void a_zero_base_hash_sends_no_precondition()
{
    Wired wired = make_wired_client();
    wired.channel->on("edit", [](const clientmock::Request&) { return edit_reply("222", true); });
    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());
    CHECK(gateway.attempt(fov_request("2.5"), 0).applied);
    const std::vector<clientmock::Request> sent = wired.channel->requests_for("edit");
    CHECK(sent.size() == 1);
    CHECK(sent.size() == 1 && !sent[0].params.contains("ifMatch"));
}

// An `at-instance` retarget carries its addressing prefix, joined the same way.
void an_at_instance_retarget_carries_its_prefix()
{
    Wired wired = make_wired_client();
    wired.channel->on("edit", [](const clientmock::Request&) { return edit_reply("222", true); });
    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());

    inspector::OverrideWriteRequest request = fov_request("2.5");
    request.target = inspector::OverrideWriteTarget::at_instance;
    request.at_instance = {"aaaaaaaaaaaaaaa1"};
    CHECK(gateway.attempt(request, kBaseHash).applied);

    const std::vector<clientmock::Request> sent = wired.channel->requests_for("edit");
    CHECK(sent.size() == 1);
    if (sent.size() == 1)
    {
        CHECK(sent[0].params.at("target").as_string() == "at-instance");
        CHECK(sent[0].params.at("atInstance").as_string() == "aaaaaaaaaaaaaaa1");
    }
}

// The daemon's own read-your-writes barrier (finish_edit -> query_after_hash) reports `reflected`.
// A miss is still an APPLIED write — the bytes are on disk — but it must be COUNTED, not swallowed.
void a_barrier_miss_is_counted_but_still_applied()
{
    Wired wired = make_wired_client();
    wired.channel->on("edit", [](const clientmock::Request&) { return edit_reply("222", false); });
    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());

    const inspector::WriteAttempt attempt = gateway.attempt(fov_request("2.5"), kBaseHash);
    CHECK(attempt.applied); // NOT downgraded: a slow derivation did not un-write the file
    CHECK(gateway.barrier_misses() == 1);
}

// A `cas.mismatch` refusal: the fresh on-disk state rides `error.data.data`, so the retry token is
// read WITHOUT a second round trip (design 05 §7).
void a_cas_mismatch_lifts_the_retry_token_from_the_refusal()
{
    Wired wired = make_wired_client();
    wired.channel->fail_method("edit", "stale precondition", "cas.mismatch", cas_detail("999"));
    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());

    const inspector::WriteAttempt attempt = gateway.attempt(fov_request("2.5"), kBaseHash);
    CHECK(!attempt.applied);
    CHECK(attempt.cas_mismatch);
    CHECK(attempt.code == "cas.mismatch");
    CHECK(attempt.raw_hash == 999u); // the CURRENT token, from the refusal itself
    CHECK(gateway.cas_refusals() == 1);
    CHECK(gateway.writes_applied() == 0);
}

// Any OTHER refusal is an error, NOT a CAS mismatch — reporting one would send the L-30 engine into
// a rebase loop over a request the write path will never accept.
void a_non_cas_refusal_is_an_error_not_a_mismatch()
{
    Wired wired = make_wired_client();
    wired.channel->fail_method("edit", "read-only session", "scope.denied");
    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());

    const inspector::WriteAttempt attempt = gateway.attempt(fov_request("2.5"), kBaseHash);
    CHECK(!attempt.applied);
    CHECK(!attempt.cas_mismatch);
    CHECK(attempt.code == "scope.denied"); // the daemon's code VERBATIM (R-CLI-008 exit classes)
}

// The FAIL-CLOSED posture: with no daemon bound nothing crosses the wire, the refusal is named, and a
// re-read reports `present:false` with a 0 token. (What closes the door is the `attempt` refusal, not
// the re-read: `commit_override_write` compares the re-read VALUE against the collision base and never
// looks at `present` — see wire_override_gateway.h § LIFETIME. The engine never reaches the re-read
// here at all, which the L-30 case below asserts.)
void an_unbound_gateway_refuses_and_writes_nothing()
{
    WireOverrideWriteGateway gateway;
    CHECK(!gateway.has_client());

    const inspector::WriteAttempt attempt = gateway.attempt(fov_request("2.5"), kBaseHash);
    CHECK(!attempt.applied);
    CHECK(!attempt.cas_mismatch); // NOT a concurrency event — there was no writer, only no daemon
    CHECK(attempt.code == std::string(WireOverrideWriteGateway::kNoDaemonCode));
    CHECK(gateway.writes_issued() == 0); // nothing was even attempted on a wire that is not there

    const inspector::FieldState state = gateway.read(kRoot, kIdPath, kPointer);
    CHECK(!state.present);
    CHECK(state.raw_hash == 0u);
    CHECK(gateway.reads_issued() == 0);
}

// The L-30 re-read: the gateway resolves the FIELD's current value out of a real `editor.inspect`
// reply, through the SAME parser the Inspector panel hydrates from.
void the_reread_resolves_the_field_and_the_cas_token()
{
    Wired wired = make_wired_client();
    wired.channel->on("editor.inspect",
                      [](const clientmock::Request&) { return inspect_reply("1.4", "333"); });
    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());

    const inspector::FieldState state = gateway.read(kRoot, kIdPath, kPointer);
    CHECK(state.present);
    CHECK(state.raw_hash == 333u);
    CHECK(canonical_equal(state.value, literal("1.4")));
    CHECK(gateway.reads_issued() == 1);

    const std::vector<clientmock::Request> sent = wired.channel->requests_for("editor.inspect");
    CHECK(sent.size() == 1);
    if (sent.size() == 1)
    {
        CHECK(sent[0].params.at("path").as_string() == kRoot);
        CHECK(sent[0].params.at("idPath").as_string() == builders::join_identity(kIdPath));
    }

    // A pointer no field carries is `present:false` WITH the token still resolved — the honest split
    // between "the file moved" and "this field is gone".
    const inspector::FieldState missing = gateway.read(kRoot, kIdPath, "/components/camera/nope");
    CHECK(!missing.present);
    CHECK(missing.raw_hash == 333u);
}

// --- the L-30 policy, driven through the REAL engine over this gateway --------------------------

// REBASE: a concurrent writer moved the FILE but not THIS field, so the gesture rebases onto the new
// state and retries. The transient refusal is what makes the retry observable — a permanent one is
// indistinguishable from a drop.
void a_concurrent_write_elsewhere_rebases()
{
    Wired wired = make_wired_client();
    wired.channel->fail_method_times("edit", 1, "stale precondition", "cas.mismatch",
                                     cas_detail("999"));
    wired.channel->on("edit", [](const clientmock::Request&) { return edit_reply("444", true); });
    // The re-read shows the field UNCHANGED at its collision base -> the external change was elsewhere.
    wired.channel->on("editor.inspect",
                      [](const clientmock::Request&) { return inspect_reply("1.4", "999"); });

    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());

    const inspector::CommitResult result =
        inspector::commit_override_write(gateway, fov_request("2.5"), kRoot, kIdPath, kPointer,
                                         literal("1.4"), kBaseHash);
    CHECK(result.status == inspector::CommitResult::Status::rebased);
    CHECK(result.raw_hash == 444u);
    CHECK(result.file == kRoot);
    CHECK(gateway.cas_refusals() == 1);
    CHECK(gateway.reads_issued() == 1);
    CHECK(gateway.writes_applied() == 1);
    // The RETRY guarded on the token the refusal carried — no invented hash, no unguarded overwrite.
    const std::vector<clientmock::Request> sent = wired.channel->requests_for("edit");
    CHECK(sent.size() == 2);
    if (sent.size() == 2)
    {
        CHECK(sent[0].params.at("ifMatch").as_string() == std::to_string(kBaseHash));
        CHECK(sent[1].params.at("ifMatch").as_string() == "999");
    }
}

// DROP: a concurrent writer moved THIS field. The gesture is refused LOUDLY and NOTHING is written —
// the guarantee this whole write path exists for.
void a_concurrent_write_to_the_same_field_drops_loudly()
{
    Wired wired = make_wired_client();
    wired.channel->fail_method("edit", "stale precondition", "cas.mismatch", cas_detail("999"));
    // The re-read shows the field MOVED (1.4 -> 2.0) under the in-flight gesture.
    wired.channel->on("editor.inspect",
                      [](const clientmock::Request&) { return inspect_reply("2.0", "999"); });

    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());

    const inspector::CommitResult result =
        inspector::commit_override_write(gateway, fov_request("2.5"), kRoot, kIdPath, kPointer,
                                         literal("1.4"), kBaseHash);
    CHECK(result.status == inspector::CommitResult::Status::dropped);
    CHECK(result.code == "cas.mismatch");
    CHECK(!result.message.empty()); // the LOUD diagnostic (the e09b-3 surface consumes it)
    CHECK(result.raw_hash == 999u);
    CHECK(gateway.writes_applied() == 0); // NOTHING was overwritten
    // Exactly ONE attempt: the drop decision is made on the first re-read, never by retrying into a
    // race the engine has already lost.
    CHECK(wired.channel->requests_for("edit").size() == 1);
}

// With NO daemon the FIRST attempt is refused, so the gesture never reaches the L-30 re-read at all —
// the fail-closed posture stated at the gateway's `read()`. Note this is an `error`, NOT a `dropped`:
// a drop CONSUMES the gesture (the field moved under it), while an error KEEPS it, which is the right
// outcome for "the daemon is momentarily gone" and is what the assertions below pin.
void a_lost_daemon_mid_gesture_refuses_rather_than_overwrites()
{
    WireOverrideWriteGateway gateway; // never bound
    const inspector::CommitResult result =
        inspector::commit_override_write(gateway, fov_request("2.5"), kRoot, kIdPath, kPointer,
                                         literal("1.4"), kBaseHash);
    // The first attempt is a plain refusal (not a CAS event), so the engine reports it as an error
    // and KEEPS the caller's edit — it did not silently succeed and it did not silently discard.
    CHECK(result.status == inspector::CommitResult::Status::error);
    CHECK(result.code == std::string(WireOverrideWriteGateway::kNoDaemonCode));
}

// e09c — AN UNDO REPLAY IS THE SAME WIRE WRITE A GESTURE IS. The DoD's "no second write path" half,
// asserted at the FRAME rather than at a refusal code: drive a real `UndoFeed` bound to this real
// gateway and read the `edit` request that comes out. A journal that grew a private writer would
// send nothing here, and one that mis-addressed would send a different frame — neither can pass.
//
// It also pins the direction, which a code-level assertion cannot: the frame carries the checkpoint's
// BEFORE value (the undo target), guarded on the CAS token the daemon reported for the re-read.
void an_undo_replay_sends_the_same_edit_frame_a_gesture_does()
{
    Wired wired = make_wired_client();
    // The field currently holds the AFTER value (2.5) — i.e. the state the recorded gesture left.
    wired.channel->on("editor.inspect",
                      [](const clientmock::Request&)
                      { return inspect_reply("2.5", std::to_string(kBaseHash).c_str()); });
    wired.channel->on("edit", [](const clientmock::Request&) { return edit_reply("222", true); });

    WireOverrideWriteGateway gateway;
    gateway.bind_client(wired.client.get());

    context::editor::shell::PanelHost host;
    panels::UndoFeed feed(host, std::string(undo::UndoJournal::kContributionId));
    feed.bind_gateway(&gateway);

    undo::FieldEdit edit;
    edit.root_scene = kRoot;
    edit.id_path = kIdPath;
    edit.pointer = kPointer;
    edit.before = literal("1.4");
    edit.after = literal("2.5");
    feed.record(std::move(edit));

    const undo::ReplayResult result = feed.replay_undo();
    CHECK(result.ok());
    // THROUGH THIS GATEWAY OBJECT, not merely through one of its class: these counters live on the
    // instance the composition root binds, so a journal writing through a second gateway would leave
    // them at zero while every status assertion above still passed.
    CHECK(gateway.reads_issued() == 1);
    CHECK(gateway.writes_issued() == 1);
    CHECK(gateway.writes_applied() == 1);

    const std::vector<clientmock::Request> sent = wired.channel->requests_for("edit");
    CHECK(sent.size() == 1);
    if (sent.size() == 1)
    {
        const Json& p = sent.front().params;
        CHECK(p.at("rootScene").as_string() == kRoot);
        CHECK(p.at("idPath").as_string() == builders::join_identity(kIdPath));
        CHECK(p.at("pointer").as_string() == kPointer);
        // The BEFORE value — an undo restores what was there, and it says so on the wire. Same
        // canonical-literal-in-a-string encoding a live gesture sends (the case above).
        CHECK(p.at("value").as_string() == "1.4");
        CHECK(p.at("target").as_string() == "outermost");
        // CAS-guarded on the token the re-read reported, exactly as a live gesture is.
        CHECK(p.at("ifMatch").is_string());
        CHECK(p.at("ifMatch").as_string() == std::to_string(kBaseHash));
    }
}

} // namespace

int main()
{
    target_tokens_mirror_the_compose_authority();
    id_path_join_mirrors_the_builders_authority();
    an_applied_write_sends_the_composed_edit_shape();
    a_zero_base_hash_sends_no_precondition();
    an_at_instance_retarget_carries_its_prefix();
    a_barrier_miss_is_counted_but_still_applied();
    a_cas_mismatch_lifts_the_retry_token_from_the_refusal();
    a_non_cas_refusal_is_an_error_not_a_mismatch();
    an_unbound_gateway_refuses_and_writes_nothing();
    the_reread_resolves_the_field_and_the_cas_token();
    a_concurrent_write_elsewhere_rebases();
    a_concurrent_write_to_the_same_field_drops_loudly();
    a_lost_daemon_mid_gesture_refuses_rather_than_overwrites();
    an_undo_replay_sends_the_same_edit_frame_a_gesture_does();
    PANELS_TEST_MAIN_END();
}
