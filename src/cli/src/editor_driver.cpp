// `context editor …` in-process driver implementation (see editor_driver.h).

#include "context/cli/editor_driver.h"

#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"
#include "context/editor/editorkernel/editor_kernel.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/platform.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace context::cli
{

namespace fs = std::filesystem;
using editor::contract::ClientHandshake;
using editor::contract::Envelope;
using editor::contract::Json;
using editor::editorkernel::EditorKernel;
using editor::editorkernel::EditorKernelConfig;
using editor::editorkernel::EditOutcome;
using editor::filesync::MemoryFileStore;
using editor::filesync::NullWatcher;
using editor::bridge::Scope;
using editor::bridge::ScopeSet;
using editor::bridge::Session;
using editor::bridge::StartOutcome;

namespace
{
// Minimal flag lookup: returns the value of `--name <value>` or `--name=<value>`, or nullopt.
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

// Resolve the daemon-lock project directory: `--project <dir>` when given, else a fresh temp dir.
fs::path resolve_project_dir(const std::vector<std::string>& args)
{
    if (const std::optional<std::string> p = flag_value(args, "project"))
        return fs::path(*p);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("context-editor-smoke-" + std::to_string(stamp));
}

Json edit_summary(const std::string& path, std::uint64_t canonical_hash, bool reflected)
{
    Json out = Json::object();
    out.set("path", Json(path));
    // Decimal STRING, not a number: Json's number type is double-backed and a full-range 64-bit
    // canonical hash (> 2^53) would lose precision (R-CLI-006 replay key must round-trip losslessly).
    out.set("canonicalHash", Json(std::to_string(canonical_hash)));
    out.set("reflected", Json(reflected));
    return out;
}

// `context editor smoke` — boot the composed EditorKernel in-process and drive the M1 attach path end
// to end, returning a self-describing envelope. Headless + in-memory (the composed loop's smoke, the
// same public API a future cross-process CLI attaches through).
Envelope run_smoke(const std::vector<std::string>& args)
{
    // Only an auto-generated temp dir is ours to delete on exit. A caller-supplied `--project <dir>`
    // names a REAL directory (its daemon lock lives at `<dir>/.editor/lock`) that we must never remove
    // — wiping a live project would be catastrophic data loss.
    const bool owns_project = !flag_value(args, "project").has_value();
    const fs::path project = resolve_project_dir(args);
    std::error_code ec;
    fs::create_directories(project, ec);

    // Remove the AUTO-CREATED temp lock directory on EVERY exit path, not just the happy tail: a boot /
    // attach / edit failure below returns early, and without this guard each failed `context editor
    // smoke` invocation would leak a `context-editor-smoke-*` directory under the system temp folder. A
    // caller-supplied `--project` dir is left untouched (owns=false). Declared before the kernel so it
    // is destroyed AFTER the daemon releases its lock.
    struct ProjectCleanup
    {
        fs::path dir;
        bool owns;
        ~ProjectCleanup()
        {
            if (!owns)
                return;
            std::error_code cleanup_ec;
            fs::remove_all(dir, cleanup_ec);
        }
    } project_cleanup{project, owns_project};

    MemoryFileStore store;
    NullWatcher watcher;
    context::kernel::SteadyClock clock;
    context::kernel::InlineTaskRunner tasks;

    EditorKernelConfig cfg;
    cfg.project_root = project;
    cfg.filesync_root = "proj";
    cfg.index_path = "proj/.editor/index";

    EditorKernel kernel(store, watcher, clock, tasks, cfg);

    const StartOutcome outcome = kernel.start(ScopeSet::all());
    if (outcome != StartOutcome::booted)
        return Envelope::failure("internal.error",
                                 outcome == StartOutcome::attach
                                     ? "an EditorKernel is already live on this project directory"
                                     : "the EditorKernel daemon failed to boot: " +
                                           kernel.daemon().error_message());

    // Attach a client over the capability handshake, requesting the file-write scope.
    ClientHandshake client;
    client.protocol_major = 0;
    client.capabilities = {"describe"};
    auto attached = kernel.attach(client, ScopeSet::parse("write"));
    if (const Envelope* fail = std::get_if<Envelope>(&attached))
    {
        kernel.stop();
        return *fail;
    }
    const Session& session = std::get<Session>(attached);

    // CLI-verb edit through filesync atomic-IO with the attached session's negotiated scopes, then a
    // read-your-writes barrier query.
    EditOutcome first = kernel.edit_file("proj/a.scene", "entity: 1", session.scopes);
    if (!first.ok)
    {
        kernel.stop();
        return first.envelope();
    }
    const std::optional<editor::derivation::DerivedSource> after_first =
        kernel.query_after_hash("proj/a.scene", first.ticket.canonical_hash);
    const bool first_reflected =
        after_first.has_value() && after_first->canonical_hash == first.ticket.canonical_hash;

    // Raw out-of-band edit of the same file, then fold it in + settle.
    store.write("proj/a.scene", "entity: 2");
    kernel.ingest_external();
    kernel.settle();
    const std::optional<editor::derivation::DerivedSource> after_raw = kernel.query("proj/a.scene");
    const std::uint64_t raw_hash = after_raw.has_value() ? after_raw->canonical_hash : 0;
    const bool raw_reflected = after_raw.has_value() && raw_hash != first.ticket.canonical_hash;

    Json data = Json::object();
    data.set("note", Json(std::string("headless in-process composed-loop smoke (M1, issue #30)")));
    data.set("attached", Json(session.attached));
    Json scopes = Json::array();
    for (const std::string& s : session.scopes.names())
        scopes.push_back(Json(s));
    data.set("scopes", std::move(scopes));
    data.set("cliVerbEdit",
             edit_summary("proj/a.scene", first.ticket.canonical_hash, first_reflected));
    data.set("rawEdit", edit_summary("proj/a.scene", raw_hash, raw_reflected));
    data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel.world().alive_count())));
    data.set("generation", Json(kernel.generation()));

    Envelope env = Envelope::success(std::move(data), kernel.generation());
    if (!first_reflected || !raw_reflected)
        env.add_warning("the derived world did not reflect an edit within the read barrier bound");

    kernel.stop();
    return env; // project_cleanup removes the temp lock directory as it goes out of scope
}
} // namespace

Envelope run_editor(const std::vector<std::string>& args)
{
    if (args.empty())
        return Envelope::failure("usage.missing_argument",
                                 "usage: context editor smoke [--project <dir>]");
    if (args[0] == "smoke")
        return run_smoke(std::vector<std::string>(args.begin() + 1, args.end()));
    return Envelope::failure("usage.unknown_verb", "no such editor subcommand: '" + args[0] + "'");
}

} // namespace context::cli
