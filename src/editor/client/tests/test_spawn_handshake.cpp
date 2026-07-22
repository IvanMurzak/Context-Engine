// The D18/e14a spawn handshake: the single-line ready marker `context daemon` prints so a spawning
// parent (the editor Shell) reads the D20 attach token off the child's STDOUT — never argv/env.
// format_daemon_ready_line / parse_daemon_ready_line are pure and single-sourced (both the daemon and
// the Shell link context_client), so the round-trip + the non-marker rejections are asserted here.

#include "context/editor/client/instance.h"

#include "client_test.h"

#include <optional>
#include <string>

using namespace context::editor::client;

namespace
{

void test_round_trips_endpoint_token_protocol_pid()
{
    InstanceInfo info;
    info.endpoint = "\\\\.\\pipe\\context-abc";
    info.token = "0123456789abcdef";
    info.protocol_major = 1;
    info.pid = 4242;

    const std::string line = format_daemon_ready_line(info);
    // A single line beginning with the shared prefix (a spawning parent greps stdout for it).
    CHECK(line.rfind(kDaemonReadyLinePrefix, 0) == 0);
    CHECK(line.find('\n') == std::string::npos);

    const std::optional<InstanceInfo> parsed = parse_daemon_ready_line(line);
    CHECK(parsed.has_value());
    CHECK(parsed->endpoint == info.endpoint);
    CHECK(parsed->token == info.token); // the token travels via this stdio line, asserted
    CHECK(parsed->protocol_major == 1u);
    CHECK(parsed->pid == 4242);
}

void test_leading_whitespace_is_tolerated()
{
    InstanceInfo info;
    info.endpoint = "ep";
    info.token = "tok";
    info.protocol_major = 1;
    const std::string line = "   \t" + format_daemon_ready_line(info);
    const std::optional<InstanceInfo> parsed = parse_daemon_ready_line(line);
    CHECK(parsed.has_value());
    CHECK(parsed->endpoint == "ep");
    CHECK(parsed->token == "tok");
}

void test_non_marker_lines_are_rejected()
{
    // The daemon's own pretty "listening" envelope, a log line, an empty line — none is a ready marker.
    CHECK(!parse_daemon_ready_line("{").has_value());
    CHECK(!parse_daemon_ready_line("  \"daemon\": \"listening\",").has_value());
    CHECK(!parse_daemon_ready_line("").has_value());
    CHECK(!parse_daemon_ready_line("context.daemon.readyX {\"endpoint\":\"e\"}").has_value());
    // The prefix present but with no JSON payload, or a payload missing the endpoint, is not usable.
    CHECK(!parse_daemon_ready_line("context.daemon.ready").has_value());
    CHECK(!parse_daemon_ready_line("context.daemon.ready {\"token\":\"t\"}").has_value());
    CHECK(!parse_daemon_ready_line("context.daemon.ready not-json").has_value());
}

void test_empty_token_still_parses_but_carries_no_token()
{
    // A daemon launched --no-require-attach-token publishes an empty token; the line is still a valid
    // ready marker (the endpoint is what a client cannot proceed without).
    InstanceInfo info;
    info.endpoint = "ep";
    info.token.clear();
    info.protocol_major = 1;
    const std::optional<InstanceInfo> parsed = parse_daemon_ready_line(format_daemon_ready_line(info));
    CHECK(parsed.has_value());
    CHECK(parsed->endpoint == "ep");
    CHECK(parsed->token.empty());
}

} // namespace

int main()
{
    test_round_trips_endpoint_token_protocol_pid();
    test_leading_whitespace_is_tolerated();
    test_non_marker_lines_are_rejected();
    test_empty_token_still_parses_but_carries_no_token();
    CLIENT_TEST_MAIN_END();
}
