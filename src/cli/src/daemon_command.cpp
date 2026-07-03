// `context daemon` implementation (see daemon_command.h).

#include "context/cli/daemon_command.h"

#include "context/editor/bridge/transport.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"
#include "context/editor/editorkernel/editor_kernel.h"
#include "context/editor/editorkernel/kernel_server.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/platform.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
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
using editor::filesync::NullWatcher;

namespace
{
std::optional<std::string> flag_value(const std::vector<std::string>& args, const std::string& name)
{
    const std::string prefix = "--" + name;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == prefix && i + 1 < args.size())
            return args[i + 1];
        const std::string eq = prefix + "=";
        if (args[i].rfind(eq, 0) == 0)
            return args[i].substr(eq.size());
    }
    return std::nullopt;
}

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
    const std::optional<std::string> project_flag = flag_value(args, "project");
    if (!project_flag.has_value())
    {
        emit(Envelope::failure("usage.missing_argument",
                               "context daemon requires --project <dir>"),
             out);
        return Envelope::failure("usage.missing_argument", "").exit_code();
    }

    std::error_code ec;
    const fs::path project = fs::absolute(fs::path(*project_flag), ec);
    fs::create_directories(project, ec);

    const ScopeSet launch_scopes = flag_value(args, "launch-scopes").has_value()
                                       ? ScopeSet::parse(*flag_value(args, "launch-scopes"))
                                       : ScopeSet::all();

    // Compose the real-disk EditorKernel. filesync_root is a subdir ("proj") so the `.editor/` control
    // dir (lock, instance file, reconcile index) stays OUTSIDE the reconcile crawl root — the proven
    // native composition (test_editor_kernel_native.cpp).
    NativeFileStore store(project);
    NullWatcher watcher;
    context::kernel::SteadyClock clock;
    context::kernel::InlineTaskRunner tasks;

    EditorKernelConfig cfg;
    cfg.project_root = project;
    cfg.filesync_root = "proj";
    cfg.index_path = "proj/.editor/reconcile-index";

    EditorKernel kernel(store, watcher, clock, tasks, cfg);
    KernelServer server(kernel); // registers itself as the method backend BEFORE start()

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
    {
        emit(Envelope::failure("internal.error",
                               "the EditorKernel daemon failed to boot: " +
                                   kernel.daemon().error_message()),
             out);
        return Envelope::failure("internal.error", "").exit_code();
    }

    // Booted — this process owns the Project. Bind the loopback transport, publish the discovery hint,
    // and serve until shutdown.
    const std::string endpoint = endpoint_for(kernel.daemon().lock().canonical_key());
    TransportServer transport(endpoint);
    if (!transport.listen())
    {
        emit(Envelope::failure("internal.error",
                               "could not bind the IPC endpoint: " + transport.error()),
             out);
        kernel.stop();
        return Envelope::failure("internal.error", "").exit_code();
    }

    // A per-instance token derived from the canonical key + endpoint + pid (carried per R-BRIDGE-007;
    // NOT gated on in v1 — see write_instance_file for the trust model).
    const std::string token = make_token(kernel.daemon().lock().canonical_key() + "|" + endpoint +
                                         "|" + std::to_string(current_pid()));
    std::string write_err;
    if (!write_instance_file(project, endpoint, token, write_err))
    {
        emit(Envelope::failure("internal.error", write_err), out);
        transport.stop();
        kernel.stop();
        return Envelope::failure("internal.error", "").exit_code();
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
