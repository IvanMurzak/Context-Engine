// `context daemon` implementation (see daemon_command.h).

#include "context/cli/daemon_command.h"

#include "context/cli/wire_client.h" // shared flag_value (single-sourced with attach/fetch)
#include "context/editor/bridge/transport.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"
#include "context/editor/editorkernel/editor_kernel.h"
#include "context/editor/editorkernel/kernel_server.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/native_watcher.h"
#include "context/kernel/platform.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h> // _getpid
#else
#include <sys/stat.h> // chmod
#include <unistd.h>   // getpid
#endif

namespace context::cli
{

namespace fs = std::filesystem;
using editor::contract::Envelope;
using editor::contract::Json;
using editor::bridge::endpoint_for;
using editor::bridge::ScopeSet;
using editor::bridge::StartOutcome;
using editor::bridge::TransportServer;
using editor::editorkernel::EditorKernel;
using editor::editorkernel::EditorKernelConfig;
using editor::editorkernel::KernelServer;
using editor::filesync::NativeFileStore;
using editor::filesync::NativeWatcher;

namespace
{
long current_pid()
{
#if defined(_WIN32)
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

// A plain 16-hex per-instance token (NOT a cryptographic secret — see write_instance_file for the v1
// trust model). Freshly derived each boot; needs no cross-run stability.
std::string make_token(const std::string& seed)
{
    const std::size_t h = std::hash<std::string>{}(seed);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    auto v = static_cast<std::uint64_t>(h);
    for (int i = 0; i < 16; ++i)
    {
        out.push_back(kHex[(v >> ((15 - i) * 4)) & 0xFULL]);
    }
    return out;
}

// Print an envelope as compact JSON to stdout AND (when `out` is set) to a result file the launcher /
// e2e test reads after the process exits (race-free, unlike stdout piping).
void emit(const Envelope& env, const std::optional<std::string>& out)
{
    const std::string json = env.dump(2);
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (out.has_value())
    {
        std::ofstream f(*out, std::ios::binary | std::ios::trunc);
        if (f)
            f << json << '\n';
    }
}

// Write the R-ARCH-005 discovery hint: `<project>/.editor/instance.json` carrying the bound endpoint
// + pid + protocol major + a per-instance token (R-BRIDGE-007 shape — the token is CARRIED for v1;
// token-gated mutual auth arrives with the remote door, so v1 relies on the ambient OS guard: the
// socket file / pipe DACL). 0600 on POSIX; the Windows owner-SID DACL is a documented follow-up.
bool write_instance_file(const fs::path& project, const std::string& endpoint,
                         const std::string& token, std::string& error)
{
    std::error_code ec;
    const fs::path dir = project / ".editor";
    fs::create_directories(dir, ec);
    const fs::path path = dir / "instance.json";

    Json doc = Json::object();
    doc.set("endpoint", Json(endpoint));
    doc.set("pid", Json(static_cast<std::int64_t>(current_pid())));
    doc.set("protocolMajor",
            Json(static_cast<std::uint64_t>(editor::contract::kProtocolMajor)));
    doc.set("token", Json(token));

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        error = "could not write instance file at " + path.string();
        return false;
    }
    f << doc.dump(2) << '\n';
    f.close();
#if !defined(_WIN32)
    ::chmod(path.string().c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}
} // namespace

int run_daemon(const std::vector<std::string>& args)
{
    const std::optional<std::string> out = flag_value(args, "out");

    // Emit an error envelope (stdout + the optional --out result file) and return its exit code —
    // funnels the early exits that need no transport/kernel teardown (the two that do keep their
    // explicit cleanup below).
    const auto fail = [&out](const std::string& code, const std::string& message) -> int
    {
        const Envelope env = Envelope::failure(code, message);
        emit(env, out);
        return env.exit_code();
    };

    const std::optional<std::string> project_flag = flag_value(args, "project");
    if (!project_flag.has_value())
        return fail("usage.missing_argument", "context daemon requires --project <dir>");

    std::error_code ec;
    const fs::path project = fs::absolute(fs::path(*project_flag), ec);
    if (ec)
        return fail("internal.error", "could not resolve --project '" + *project_flag +
                                          "' to an absolute path: " + ec.message());
    fs::create_directories(project, ec);

    const ScopeSet launch_scopes = flag_value(args, "launch-scopes").has_value()
                                       ? ScopeSet::parse(*flag_value(args, "launch-scopes"))
                                       : ScopeSet::all();

    // R-FILE-002 crawl cadence: this composition runs a REAL OS watcher, so the full re-hash crawl
    // is the LOW-FREQUENCY dropped-event safety net — not the ingest cadence (default: at most once
    // per 30 s). The `reconcile` verb still FORCES a crawl on demand (bulk ops, e.g. a git branch
    // switch). `--crawl-interval-ms 0` restores crawl-every-pass (sensible on watch-hostile mounts,
    // where the watcher is degraded anyway).
    std::uint64_t crawl_interval_ms = 30000;
    if (const std::optional<std::string> raw = flag_value(args, "crawl-interval-ms");
        raw.has_value())
    {
        // Strict parse (parse_u64, not stoull): "-1" / trailing junk must be a usage error, not a
        // silent wrap that turns the crawl safety net off. Bounded so the ms->nanos conversion
        // below cannot overflow either.
        const std::optional<std::uint64_t> parsed = parse_u64(*raw);
        if (!parsed.has_value() ||
            *parsed > std::numeric_limits<std::uint64_t>::max() / 1000000ULL)
            return fail("usage.invalid",
                        "--crawl-interval-ms expects a non-negative integer, got '" + *raw + "'");
        crawl_interval_ms = *parsed;
    }

    // R-CLI-017 spool threshold override (operational/test knob): a response above this many bytes
    // returns a largeResult handle instead of inline data. Default: bridge::kLargeResultThresholdBytes.
    // The e2e tests lower it to drive the oversized path with a routine-sized response.
    std::optional<std::uint64_t> large_result_threshold;
    if (const std::optional<std::string> raw = flag_value(args, "large-result-threshold");
        raw.has_value())
    {
        // Strict parse (see --crawl-interval-ms): "-1" wrapping to ~2^64 would silently disable
        // the R-CLI-017 spool and push oversized results into the transport frame cap.
        large_result_threshold = parse_u64(*raw);
        if (!large_result_threshold.has_value())
            return fail("usage.invalid",
                        "--large-result-threshold expects a non-negative byte count, got '" + *raw +
                            "'");
    }

    // Compose the real-disk EditorKernel. filesync_root is a subdir ("proj") so the `.editor/` control
    // dir (lock, instance file, reconcile index) stays OUTSIDE the reconcile crawl root — the proven
    // native composition (test_editor_kernel_native.cpp).
    NativeFileStore store(project);
    // The REAL OS watcher (RDCW / inotify / FSEvents) over the authored subtree — hints only, hashes
    // stay the truth (R-FILE-002): hints drive routine detection, the cadenced crawl guarantees
    // eventual convergence, and `reconcile` forces a full crawl on demand. The watched dir must
    // exist before registration (a missing root = degraded, by contract), and its logical prefix
    // must equal filesync_root so hints key into the same reconcile index. If registration fails
    // (fd/watch limits, network FS), the kernel emits the visible `watcher.degraded` diagnostic and
    // detection leans on the crawl — never silently.
    fs::create_directories(project / "proj", ec);
    NativeWatcher watcher(project / "proj", "proj");
    context::kernel::SteadyClock clock;
    context::kernel::InlineTaskRunner tasks;

    EditorKernelConfig cfg;
    cfg.project_root = project;
    cfg.filesync_root = "proj";
    cfg.index_path = "proj/.editor/index";
    cfg.min_crawl_interval_nanos = crawl_interval_ms * 1000000ULL;

    EditorKernel kernel(store, watcher, clock, tasks, cfg);
    KernelServer server(kernel); // registers itself as the method backend BEFORE start()
    if (large_result_threshold.has_value())
        server.set_large_result_threshold(*large_result_threshold);

    const StartOutcome outcome = kernel.start(launch_scopes);

    if (outcome == StartOutcome::attach)
    {
        // R-BRIDGE-001: the lock is already held → an instance is live → attach, do NOT boot a second.
        Json data = Json::object();
        data.set("attachSignal", Json(true));
        data.set("project", Json(project.string()));
        data.set("note",
                 Json(std::string("an EditorKernel is already live on this project (R-BRIDGE-001)")));
        emit(Envelope::success(std::move(data)), out);
        return kDaemonAttachSignalExit;
    }
    if (outcome == StartOutcome::error)
        return fail("internal.error",
                    "the EditorKernel daemon failed to boot: " + kernel.daemon().error_message());

    // Booted — this process owns the Project. Bind the loopback transport, publish the discovery hint,
    // and serve until shutdown.
    const std::string endpoint = endpoint_for(kernel.daemon().lock().canonical_key());
    TransportServer transport(endpoint);
    if (!transport.listen())
    {
        const Envelope env = Envelope::failure("internal.error",
                                               "could not bind the IPC endpoint: " + transport.error());
        emit(env, out);
        kernel.stop();
        return env.exit_code();
    }

    // A per-instance token derived from the canonical key + endpoint + pid (carried per R-BRIDGE-007;
    // NOT gated on in v1 — see write_instance_file for the trust model).
    const std::string token = make_token(kernel.daemon().lock().canonical_key() + "|" + endpoint +
                                         "|" + std::to_string(current_pid()));
    std::string write_err;
    if (!write_instance_file(project, endpoint, token, write_err))
    {
        const Envelope env = Envelope::failure("internal.error", write_err);
        emit(env, out);
        transport.stop();
        kernel.stop();
        return env.exit_code();
    }

    // The appearance of instance.json is the "ready" signal a launcher/test waits on; also announce it.
    {
        Json data = Json::object();
        data.set("daemon", Json(std::string("listening")));
        data.set("endpoint", Json(endpoint));
        data.set("project", Json(project.string()));
        data.set("pid", Json(static_cast<std::int64_t>(current_pid())));
        emit(Envelope::success(std::move(data)), std::nullopt);
    }

    const int serve_rc = server.serve(transport);

    transport.stop();
    kernel.stop();
    // Best-effort: remove the now-stale discovery hint so a later client does not attach a dead endpoint.
    fs::remove(project / ".editor" / "instance.json", ec);
    return serve_rc;
}

} // namespace context::cli
