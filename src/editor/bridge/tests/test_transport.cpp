// Transport framing unit test (R-BRIDGE-003): an in-process echo server on one thread + a client on
// the main thread over the REAL loopback wire (Unix domain socket / named pipe). Proves the
// length-prefixed framing round-trips whole messages regardless of how the OS splits/coalesces reads
// — happy path, empty frame, a large frame (crosses OS read boundaries), many sequential frames — and
// the failure paths (clean EOF on disconnect; a client to a non-existent endpoint times out).

#include "context/editor/bridge/transport.h"

#include "bridge_test.h"

#include <atomic>
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

    // D20 / R-SEC-002: the bound endpoint is owner-restricted (Windows owner-SID pipe DACL / POSIX
    // 0600 socket) — closing the documented named-pipe gap. Asserted on every platform.
    CHECK(server.endpoint_owner_restricted());

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

    // --- read_frame_timed(): the D19 single-thread-per-connection primitive — a timed read returns a
    //     TIMEOUT when idle (so the owning thread can flush pushed frames) and the FRAME when data
    //     arrives; interleaving a timed read with writes on ONE thread round-trips correctly ---------
    {
        const std::string ep = endpoint_for(unique_key("timed"));
        TransportServer srv2(ep);
        CHECK(srv2.listen());
        CHECK(srv2.endpoint_owner_restricted());

        std::atomic<int> timeouts{0};
        std::thread server_thread(
            [&srv2, &timeouts]()
            {
                std::optional<context::editor::bridge::TransportConnection> conn = srv2.accept();
                if (!conn.has_value())
                    return;
                // ONE thread owns the connection for reads AND writes (no dup): a timed read, and on a
                // frame, an echo write on the SAME handle.
                for (;;)
                {
                    bool timed_out = false;
                    const std::optional<std::string> req = conn->read_frame_timed(50, timed_out);
                    if (timed_out)
                    {
                        ++timeouts;
                        continue; // idle -> the owning thread would flush pushes here, then re-poll
                    }
                    if (!req.has_value())
                        break; // EOF
                    if (!conn->write_frame("echo:" + *req))
                        break;
                }
            });

        TransportClient c2(ep);
        CHECK(c2.connect(3000));
        // Idle first so the server's timed read reports at least one timeout (the flush-then-poll gap).
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const std::optional<std::string> r = c2.request("hi");
        CHECK(r.has_value());
        CHECK(*r == "echo:hi"); // a timed read read the frame + the same thread wrote the response
        const std::optional<std::string> r4 = c2.request("again");
        CHECK(r4.has_value());
        CHECK(*r4 == "echo:again");
        c2.close();
        server_thread.join();
        srv2.stop();
        CHECK(timeouts.load() >= 1); // the idle window produced at least one timed-out poll
    }

    // --- unblock(): a read_frame() blocked on another thread is released without racing the handle
    //     (the D19 daemon-shutdown teardown of a client that sends nothing) -------------------------
    {
        const std::string ep = endpoint_for(unique_key("unblock"));
        TransportServer srv3(ep);
        CHECK(srv3.listen());

        std::optional<context::editor::bridge::TransportConnection> server_conn;
        std::thread accept_thread([&srv3, &server_conn]() { server_conn = srv3.accept(); });

        TransportClient c3(ep);
        CHECK(c3.connect(3000));
        accept_thread.join();
        CHECK(server_conn.has_value());

        // A reader blocks in read_frame() (the client sends nothing); another thread unblock()s it.
        std::atomic<bool> returned{false};
        std::thread reader(
            [&server_conn, &returned]()
            {
                const std::optional<std::string> f = server_conn->read_frame();
                (void)f; // nullopt expected — the read was unblocked, not a real frame
                returned.store(true);
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the reader block
        CHECK(!returned.load());
        server_conn->unblock(); // read-only on the handle -> race-free wake
        reader.join();          // must return (not hang) — the assertion is that this join completes
        CHECK(returned.load());
        c3.close();
        srv3.stop();
    }

    BRIDGE_TEST_MAIN_END();
}
