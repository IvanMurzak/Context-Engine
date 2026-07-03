// Transport framing unit test (R-BRIDGE-003): an in-process echo server on one thread + a client on
// the main thread over the REAL loopback wire (Unix domain socket / named pipe). Proves the
// length-prefixed framing round-trips whole messages regardless of how the OS splits/coalesces reads
// — happy path, empty frame, a large frame (crosses OS read boundaries), many sequential frames — and
// the failure paths (clean EOF on disconnect; a client to a non-existent endpoint times out).

#include "context/editor/bridge/transport.h"

#include "bridge_test.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

using context::editor::bridge::endpoint_for;
using context::editor::bridge::TransportClient;
using context::editor::bridge::TransportServer;

namespace
{
// A per-run-unique endpoint key so a leftover socket/pipe from a crashed prior run cannot collide.
std::string unique_key(const char* tag)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("ctx-test-") + tag + "-" + std::to_string(now);
}
} // namespace

int main()
{
    const std::string endpoint = endpoint_for(unique_key("transport"));
    TransportServer server(endpoint);
    CHECK(server.listen());

    // Echo server: accept one client, echo each framed message back until the peer disconnects.
    std::thread srv(
        [&server]()
        {
            std::optional<context::editor::bridge::TransportConnection> conn = server.accept();
            if (!conn.has_value())
                return;
            for (;;)
            {
                const std::optional<std::string> frame = conn->read_frame();
                if (!frame.has_value())
                    break; // clean EOF
                if (!conn->write_frame(*frame))
                    break;
            }
        });

    TransportClient client(endpoint);
    CHECK(client.connect(3000));

    // Happy path: a normal (compact-JSON-shaped) frame round-trips byte-exact.
    const std::optional<std::string> r1 = client.request("{\"jsonrpc\":\"2.0\",\"id\":1}");
    CHECK(r1.has_value());
    CHECK(*r1 == "{\"jsonrpc\":\"2.0\",\"id\":1}");

    // Edge: an empty payload is a valid frame (length prefix 0).
    const std::optional<std::string> r2 = client.request("");
    CHECK(r2.has_value());
    CHECK(r2->empty());

    // Edge: a large payload forces multiple OS read chunks — the framing must still reassemble it.
    const std::string big(200000, 'x');
    const std::optional<std::string> r3 = client.request(big);
    CHECK(r3.has_value());
    CHECK(r3->size() == big.size());
    CHECK(*r3 == big);

    // Edge: many sequential frames on ONE connection stay aligned (no cross-frame bleed).
    for (int i = 0; i < 8; ++i)
    {
        const std::string msg = "message-number-" + std::to_string(i);
        const std::optional<std::string> r = client.request(msg);
        CHECK(r.has_value());
        CHECK(*r == msg);
    }

    // Failure path: closing the client makes the server's next read_frame() return a clean EOF, so the
    // echo thread exits (joined below without hanging).
    client.close();
    srv.join();
    server.stop();

    // Failure path: a client to an endpoint no daemon is bound to times out (does not hang).
    TransportClient orphan(endpoint_for(unique_key("no-daemon")));
    CHECK(!orphan.connect(250));

    BRIDGE_TEST_MAIN_END();
}
