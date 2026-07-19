// Real IPC transport (R-BRIDGE-003/007 / L-29): the loopback-only wire that frames the bridge's
// JSON-RPC 2.0 dispatcher between a daemon PROCESS and a separate client process.
//
// The transport is deliberately protocol-agnostic — it moves opaque byte frames, not JSON — so the
// EXISTING dispatcher (dispatcher.h) stays the one JSON-RPC engine (the task reuses it, it is not
// re-invented here). One local endpoint per Project: a Unix domain socket on POSIX, a named pipe on
// Windows (the two idiomatic loopback primitives). No ambient network — the wire is local-only by
// construction (R-SEC-002: the remote door is a separate, explicitly-gated v2 feature).
//
// Framing: each message is a 4-byte big-endian unsigned length prefix followed by exactly that many
// payload bytes. It is binary-clean (does not assume newline-free payloads) and self-delimiting, so a
// stream socket / byte-mode pipe that coalesces or splits OS reads still yields whole messages.
//
// Serial single-connection model (M1): the daemon accepts one client at a time and serves it to
// disconnect before accepting the next — matching the daemon-per-worktree single-instance design
// (R-BRIDGE-001) and the write-serialization guarantee (L-50). Concurrent multi-client fan-in is a
// documented follow-up (the design's write queue), out of this task's scope.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace context::editor::bridge
{

// The largest single frame the transport will read/write. A hostile or corrupt length prefix cannot
// force an unbounded allocation; a legitimate oversized response never fights this cap — the serve
// layer spools it and returns an R-CLI-017 resource handle instead (resource_store.h +
// KernelServer::finalize_response), fetched in bounded resource.read chunks.
inline constexpr std::uint32_t kMaxFrameBytes = 64u * 1024u * 1024u;

// Derive the stable loopback endpoint string for a Project, keyed by its canonical path. POSIX:
// "<tmp>/context-<hash>.sock"; Windows: "\\.\pipe\context-<hash>". The daemon binds this and records
// it (verbatim) in `.editor/instance.json` so a client discovers the exact endpoint rather than
// recomputing it — the R-ARCH-005 discovery hint.
[[nodiscard]] std::string endpoint_for(const std::string& canonical_project_key);

// One connected byte-stream: a server-accepted client, or a client's own connection. Owns the native
// handle (socket fd / pipe HANDLE) and closes it on destruction. Move-only.
class TransportConnection
{
public:
    TransportConnection() = default;
    ~TransportConnection();

    TransportConnection(const TransportConnection&) = delete;
    TransportConnection& operator=(const TransportConnection&) = delete;
    TransportConnection(TransportConnection&& other) noexcept;
    TransportConnection& operator=(TransportConnection&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;

    // Read exactly one framed message. nullopt on a clean peer disconnect (EOF) OR any I/O / framing
    // error (error() then carries the reason; a disconnect leaves it empty).
    [[nodiscard]] std::optional<std::string> read_frame();

    // Write exactly one framed message. false on any I/O error (peer gone, etc.).
    [[nodiscard]] bool write_frame(std::string_view payload);

    // Wait up to `timeout_ms` for a readable frame, then read it. Returns the frame on success;
    // nullopt with `timed_out=true` when the wait elapsed with no data (the caller loops to flush any
    // queued push frames + re-poll), or nullopt with `timed_out=false` on a clean EOF / I/O error
    // (error() then carries the reason for a real error; empty for a disconnect). Lets ONE thread own
    // a connection for BOTH reads and writes by interleaving a bounded read with queued writes —
    // required on Windows, where a blocked synchronous read on a duplicated handle serializes with a
    // concurrent write on the SAME pipe file object. POSIX: poll(POLLIN) (event-driven — wakes the
    // instant data arrives, so a request incurs no added latency; the perf-critical bench runs here).
    // Windows: PeekNamedPipe + a short sleep (a bounded poll).
    [[nodiscard]] std::optional<std::string> read_frame_timed(int timeout_ms, bool& timed_out);

    // Unblock a peer thread blocked in THIS connection's read_frame()/read_frame_timed(), from a
    // DIFFERENT thread, WITHOUT racing on the handle: POSIX shutdown(SHUT_RDWR) / Windows
    // DisconnectNamedPipe — both only READ the native handle value, never mutate the member, so this
    // is data-race-free against the owning thread's in-flight read (that thread then observes
    // EOF/error and close()s the handle itself, single-threaded). Best-effort + idempotent; the D19
    // serve loop uses it to tear down a client stalled mid-frame on daemon shutdown.
    void unblock() noexcept;

    void close() noexcept;

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // Internal: adopt a native handle produced by TransportServer::accept / TransportClient::connect.
#if defined(_WIN32)
    explicit TransportConnection(void* handle) noexcept : handle_(handle) {}
#else
    explicit TransportConnection(int fd) noexcept : fd_(fd) {}
#endif

private:
    [[nodiscard]] bool read_exact(void* buf, std::size_t n);
    [[nodiscard]] bool write_all(const void* buf, std::size_t n);

    std::string error_;
#if defined(_WIN32)
    void* handle_ = nullptr; // HANDLE; nullptr / INVALID_HANDLE_VALUE when closed
#else
    int fd_ = -1;
#endif
};

// Server side: bind + listen on the endpoint, then accept clients serially.
class TransportServer
{
public:
    explicit TransportServer(std::string endpoint);
    ~TransportServer();

    TransportServer(const TransportServer&) = delete;
    TransportServer& operator=(const TransportServer&) = delete;

    // Bind + listen. false (+ error()) on failure (e.g. the endpoint is already bound). Idempotent:
    // a stale POSIX socket file at the path is removed first.
    [[nodiscard]] bool listen();

    // Block until the next client connects. nullopt after stop(), or on an accept error while not
    // stopped (error() carries the reason).
    [[nodiscard]] std::optional<TransportConnection> accept();

    // Stop listening + release the endpoint (unlink the POSIX socket file). Idempotent. A subsequent
    // accept() returns nullopt. CAUTION: in the M1 serial model stop() runs on the SAME thread that
    // runs accept() (after it returns). Calling stop() from another thread WHILE accept() is blocked
    // closes the listener fd/HANDLE out from under a blocked syscall — undefined behavior on both
    // platforms; a future concurrent graceful-shutdown must use CancelSynchronousIo/CancelIoEx
    // (Windows) or a self-pipe/eventfd wakeup (POSIX) instead of relying on this.
    void stop() noexcept;

    [[nodiscard]] const std::string& endpoint() const noexcept { return endpoint_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // D20 / R-SEC-002 ambient-guard assertion: is the bound endpoint restricted to the owner? Windows
    // — the named-pipe DACL grants ONLY the current-user SID (no Everyone/Authenticated-Users ACE, no
    // NULL "everyone" DACL); POSIX — the socket file mode is exactly 0600. False when not listening,
    // when the endpoint is world-reachable, or on a query error. This is what closes the documented
    // Windows named-pipe DACL gap (POSIX already 0600). Must be called while listening (it inspects
    // the live endpoint).
    [[nodiscard]] bool endpoint_owner_restricted() const;

private:
    std::string endpoint_;
    std::string error_;
    bool listening_ = false;
    bool stopped_ = false;
#if defined(_WIN32)
    void* pending_ = nullptr;  // the current unconnected named-pipe instance HANDLE
    void* security_ = nullptr; // owner-only SECURITY_ATTRIBUTES holder (PipeSecurity*), applied to
                               // every pipe instance; built lazily in listen(), freed in stop()/dtor
#else
    int listen_fd_ = -1;
#endif
};

// Client side: connect to a daemon's endpoint, with bounded retry-backoff for the boot race (the
// daemon may still be coming up when the client first tries).
class TransportClient
{
public:
    explicit TransportClient(std::string endpoint);

    // Connect, retrying with a short backoff until `timeout_ms` elapses. false (+ error()) on timeout.
    [[nodiscard]] bool connect(int timeout_ms = 3000);

    // Send one framed request and block for one framed response. nullopt on any transport failure OR
    // a peer disconnect before a response arrived. NB: request() assumes the very next inbound frame
    // IS the response — correct for a NON-subscribing client, but a client that has subscribed also
    // receives async server-pushed `event` frames interleaved; such a client must use send()+receive()
    // and demux by frame shape (the D19 push consumer / e02's context_client model).
    [[nodiscard]] std::optional<std::string> request(std::string_view request_json);

    // Async-client primitives (the D19 push consumer needs raw framed I/O, not just request()):
    // send() writes one framed request WITHOUT waiting for a reply; receive() reads one framed message
    // — a response OR a server-pushed `event` / `event.gap` notification. false/nullopt on a transport
    // failure or peer disconnect (error() carries the reason).
    [[nodiscard]] bool send(std::string_view request_json);
    [[nodiscard]] std::optional<std::string> receive();

    void close() noexcept;

    [[nodiscard]] bool connected() const noexcept { return conn_.valid(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    std::string endpoint_;
    std::string error_;
    TransportConnection conn_;
};

} // namespace context::editor::bridge
