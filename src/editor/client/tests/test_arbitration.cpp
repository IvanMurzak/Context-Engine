// M9 e14b — per-project single-instance arbitration (arbitration.h): the pure open decision, the
// presence-marker read (C-F3: read-only, never writes editor-state.json), the focus-request handshake
// round-trip, and the opener/editor handshake driven over REAL files (happy / stale-marker / absent).

#include "context/editor/client/arbitration.h"

#include "client_test.h"

#include <chrono>
#include <fstream>
#include <string>
#include <system_error>

using namespace context::editor::client;
namespace fs = std::filesystem;
namespace contract = context::editor::contract;

namespace
{

fs::path make_temp_project(const char* tag)
{
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
                   ("ctx-e14b-" + std::string(tag) + "-" + std::to_string(stamp) + "-" +
                    std::to_string(++counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / ".editor", ec);
    return dir;
}

void write_file(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// A minimal editor-state.json document carrying (or omitting) a presence marker.
std::string editor_state_with_presence(const PresenceMarker& marker)
{
    contract::Json doc = contract::Json::object();
    doc.set("version", contract::Json(static_cast<std::int64_t>(1)));
    doc.set("presence", marker.to_json());
    return doc.dump(2);
}

// -------------------------------------------------------------------------------- pure decision

void test_decision_is_focus_only_when_marker_present_and_acknowledged()
{
    CHECK(decide_open_action(/*marker*/ true, /*ack*/ true) == OpenAction::focus_existing);
    CHECK(decide_open_action(true, false) == OpenAction::spawn_new);  // present but stale
    CHECK(decide_open_action(false, true) == OpenAction::spawn_new);  // ack without a marker (never)
    CHECK(decide_open_action(false, false) == OpenAction::spawn_new); // nothing present
}

// --------------------------------------------------------------------------- presence marker r/w

void test_presence_marker_round_trips_and_tolerates_garbage()
{
    PresenceMarker marker;
    marker.pid = 4321;
    marker.boot_nonce = "abc123def456";
    const std::optional<PresenceMarker> back = PresenceMarker::from_json(marker.to_json());
    CHECK(back.has_value());
    CHECK(back->pid == 4321);
    CHECK(back->boot_nonce == "abc123def456");

    // A marker with no bootNonce is not meaningful.
    contract::Json no_nonce = contract::Json::object();
    no_nonce.set("pid", contract::Json(static_cast<std::int64_t>(7)));
    CHECK(!PresenceMarker::from_json(no_nonce).has_value());
    CHECK(!PresenceMarker::from_json(contract::Json()).has_value()); // null

    // Parsed out of a whole editor-state document.
    const std::optional<PresenceMarker> from_doc =
        parse_presence_from_editor_state(editor_state_with_presence(marker));
    CHECK(from_doc.has_value() && from_doc->boot_nonce == "abc123def456");

    // Absent / malformed / no-presence documents read as "no editor".
    CHECK(!parse_presence_from_editor_state("").has_value());
    CHECK(!parse_presence_from_editor_state("{ this is not json").has_value());
    CHECK(!parse_presence_from_editor_state("{\"version\":1}").has_value()); // no presence key
}

void test_focus_request_encode_decode()
{
    FocusRequest request;
    request.nonce = "nonce-xyz";
    request.requester_pid = 9090;
    const std::optional<FocusRequest> back = FocusRequest::decode(request.encode());
    CHECK(back.has_value());
    CHECK(back->nonce == "nonce-xyz");
    CHECK(back->requester_pid == 9090);

    CHECK(!FocusRequest::decode("").has_value());
    CHECK(!FocusRequest::decode("not json").has_value());
    CHECK(!FocusRequest::decode("{\"requesterPid\":1}").has_value()); // no nonce
}

// ------------------------------------------------------------------ absent editor => spawn (no wait)

void test_arbitrate_with_no_marker_spawns_without_waiting()
{
    const fs::path project = make_temp_project("nomarker");
    const auto start = std::chrono::steady_clock::now();
    // A generous timeout that must NOT be consumed: with no marker there is nothing to wait for.
    const OpenArbitration arb = arbitrate_open(project, 5000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    CHECK(arb.action == OpenAction::spawn_new);
    CHECK(!arb.marker_present);
    CHECK(!arb.focus_acknowledged);
    CHECK(elapsed < 2000); // returned immediately, did not sit out the focus timeout
    // The opener never wrote editor-state.json (C-F3) — only the read happened; the file stays absent.
    CHECK(!fs::exists(project / ".editor" / "editor-state.json"));
    std::error_code ec;
    fs::remove_all(project, ec);
}

// ------------------------------------------------------ stale marker (no live editor) => spawn

void test_arbitrate_with_a_stale_marker_times_out_and_spawns()
{
    const fs::path project = make_temp_project("stale");
    PresenceMarker marker;
    marker.pid = 12345;
    marker.boot_nonce = make_boot_nonce();
    write_file(project / ".editor" / "editor-state.json", editor_state_with_presence(marker));

    // No watcher consumes the request, so the handshake times out -> spawn (crash-robust liveness).
    const OpenArbitration arb = arbitrate_open(project, 300);
    CHECK(arb.marker_present);
    CHECK(!arb.focus_acknowledged);
    CHECK(arb.action == OpenAction::spawn_new);
    CHECK(arb.existing_pid == 12345);
    // The opener retracted its unacknowledged request rather than leaving litter.
    CHECK(!fs::exists(focus_request_path(project)));
    std::error_code ec;
    fs::remove_all(project, ec);
}

// NOTE: the CONCURRENT live-editor focus path (arbitrate_open blocking until a watcher consumes its
// request -> OpenAction::focus_existing) needs a second thread/process, so it is proven end-to-end in
// the T2 two-process drill (src/tests/integration/test_e14b_arbitration.cpp), keeping this unit suite
// single-threaded (no Threads link on the uniform client-test targets).

// ------------------------------------------------- watcher does not double-fire on one request

void test_watcher_serves_each_request_once()
{
    const fs::path project = make_temp_project("watch");
    FocusRequestWatcher watcher(project);
    CHECK(!watcher.poll()); // nothing pending

    FocusRequest request;
    request.nonce = make_boot_nonce();
    request.requester_pid = 42;
    write_file(focus_request_path(project), request.encode());

    CHECK(watcher.poll());  // consumed
    CHECK(!watcher.poll()); // file gone -> nothing to serve
    CHECK(watcher.served() == 1);

    // A brand-new request (a fresh open) fires again.
    FocusRequest again;
    again.nonce = make_boot_nonce();
    again.requester_pid = 43;
    write_file(focus_request_path(project), again.encode());
    CHECK(watcher.poll());
    CHECK(watcher.served() == 2);
    std::error_code ec;
    fs::remove_all(project, ec);
}

} // namespace

int main()
{
    test_decision_is_focus_only_when_marker_present_and_acknowledged();
    test_presence_marker_round_trips_and_tolerates_garbage();
    test_focus_request_encode_decode();
    test_arbitrate_with_no_marker_spawns_without_waiting();
    test_arbitrate_with_a_stale_marker_times_out_and_spawns();
    test_watcher_serves_each_request_once();
    CLIENT_TEST_MAIN_END();
}
