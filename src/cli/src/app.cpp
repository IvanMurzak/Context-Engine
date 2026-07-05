// `context` CLI front-end implementation (see app.h).

#include "context/cli/app.h"

#include "context/cli/attach_command.h"
#include "context/cli/bench_command.h"
#include "context/cli/daemon_command.h"
#include "context/cli/editor_driver.h"
#include "context/cli/fetch_command.h"
#include "context/cli/merge_command.h"
#include "context/cli/migrate_command.h"
#include "context/cli/scaffold.h"
#include "context/cli/set_command.h"
#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::FlagSpec;
using editor::contract::Json;
using editor::contract::ParamSpec;
using editor::contract::Registry;
using editor::contract::VerbSpec;

namespace
{
// Split a leading selector token into an optional `<ns>:` prefix + the remainder. A package verb is
// `physics:body add`; an engine verb has no prefix. Only the FIRST ':' splits (values never reach
// this path — selector tokens are resolved before flags/params).
std::pair<std::string, std::string> split_ns(const std::string& token)
{
    const std::size_t colon = token.find(':');
    if (colon == std::string::npos)
        return {std::string(), token};
    return {token.substr(0, colon), token.substr(colon + 1)};
}

// Find a flag spec by name among the verb's flags then the core set. nullptr when unknown.
const FlagSpec* find_flag(const VerbSpec& verb, const Registry& reg, const std::string& name)
{
    for (const FlagSpec& f : verb.flags)
        if (f.name == name)
            return &f;
    for (const FlagSpec& f : reg.core_flags())
        if (f.name == name)
            return &f;
    return nullptr;
}

// A verb whose backing is not yet wired: honor --dry-run by returning the reserved plan, else the
// contract.unimplemented catalog code (R-CLI-009 grammar reservation).
Envelope reserved_surface(const VerbSpec& verb, bool dry_run,
                          const std::vector<std::string>& positionals)
{
    if (dry_run)
    {
        Json plan = Json::object();
        plan.set("verb", Json(verb.cli_command()));
        Json args = Json::array();
        for (const std::string& p : positionals)
            args.push_back(Json(p));
        plan.set("args", std::move(args));
        plan.set("wouldApply", Json(false));
        plan.set("note", Json("dry-run: the verb surface is reserved; backing lands in a later "
                              "task (R-CLI-009)."));
        return Envelope::success(std::move(plan));
    }
    return Envelope::failure("contract.unimplemented",
                             "'" + verb.cli_command() +
                                 "' is reserved in the contract; its backing is not wired yet.");
}

Envelope dispatch(const VerbSpec& verb, const std::vector<std::string>& positionals,
                  const std::map<std::string, std::string>& flags)
{
    // Bind positionals to the verb's declared params, in order; enforce required params.
    std::map<std::string, std::string> bound;
    for (std::size_t i = 0; i < verb.params.size(); ++i)
    {
        if (i < positionals.size())
            bound[verb.params[i].name] = positionals[i];
        else if (verb.params[i].required)
            return Envelope::failure("usage.missing_argument",
                                     "missing required argument: " + verb.params[i].name);
    }

    const bool dry_run = flags.find("dry-run") != flags.end();

    if (verb.noun.empty() && verb.verb == "describe")
        return Envelope::success(Registry::instance().describe());

    // --- M2 composed write path + advisory override hygiene (R-CLI-006 / L-35) ------------------
    // `context set` is the composed-entity file-rewriter (default-outermost override, --edit-template,
    // --at-instance) served one-shot over the project's scene files; it reports the file + JSON-pointer
    // written + both labelled hashes. `context query --overrides <mode> <scene>` is the advisory
    // override-hygiene read — the branch fires ONLY for the --overrides form, so a plain `context
    // query` still falls through to the operational-only rejection (it is daemon-served).
    if (verb.noun.empty() && verb.verb == "set")
        return run_set(positionals, flags);
    if (verb.noun.empty() && verb.verb == "query" && flags.find("overrides") != flags.end())
        return run_override_query(positionals, flags);

    if (verb.noun.empty() && verb.verb == "new")
    {
        const std::string directory = bound.count("directory") ? bound.at("directory") : "";
        const std::string tmpl = bound.count("template") ? bound.at("template") : "default";
        if (dry_run)
            return Envelope::success(scaffold_plan(directory, tmpl));
        return scaffold_project(directory, tmpl);
    }

    // The M2 wave 4 structural-merge family (issue #59, R-FILE-012): merge-file / resolve-conflict /
    // re-key / validate. Placed immediately after the `new` verb region (distinct from the sibling
    // editing near `describe`) so concurrent M2 tasks inserting dispatch branches merge cleanly. Each
    // is backed by src/cli/merge_command.cpp over the src/editor/merge/ engine.
    if (verb.noun.empty() && verb.verb == "merge-file")
        return run_merge_file(bound, flags);
    if (verb.noun.empty() && verb.verb == "resolve-conflict")
        return run_resolve_conflict(bound, flags);
    if (verb.noun.empty() && verb.verb == "re-key")
        return run_rekey(bound, flags);
    if (verb.noun.empty() && verb.verb == "validate")
        return run_validate(bound, flags);

    // `context resource read <handle> [<offset>:<length>]` (alias: `context fetch ...`) — the
    // R-CLI-017 large-result fetch. Drives a RUNNING daemon over the wire via the shared client
    // plumbing; --project (core flag) names the project whose daemon minted the handle.
    if (verb.noun == "resource" && verb.verb == "read")
    {
        const std::string handle = bound.count("handle") ? bound.at("handle") : "";
        if (dry_run)
        {
            Json plan = Json::object();
            plan.set("verb", Json(verb.cli_command()));
            plan.set("handle", Json(handle));
            plan.set("wouldApply", Json(false));
            plan.set("note", Json(std::string(
                                 "dry-run: would fetch the handle from the --project daemon over "
                                 "resource.read (R-CLI-017).")));
            return Envelope::success(std::move(plan));
        }
        std::map<std::string, std::string> merged = flags;
        if (bound.count("range"))
            merged["range"] = bound.at("range");
        return run_fetch(handle, merged);
    }

    // `context migrate [path]` — the L-37 explicit bulk migration path (M2 wave 3): canonicalize +
    // migrate stamped-older payloads + stamp current versions, rewriting files in place. The only
    // disk-writing migration besides tool saves. --dry-run (core flag) is honored INSIDE the
    // command: a full real scan + per-file report, with the writes suppressed.
    if (verb.noun.empty() && verb.verb == "migrate")
    {
        const std::string target = bound.count("path") ? bound.at("path") : "";
        return run_migrate(target, flags);
    }

    // The operational daemon-driver verbs (edit / edit-batch / query / snapshot / reconcile /
    // build / shutdown) are REGISTERED for describe-honesty (R-CLI-009, stability="operational")
    // but are served over RPC by a LIVE daemon — not as one-shot CLI verbs.
    if (verb.stability == "operational")
        return Envelope::failure(
            "contract.operational_only",
            "'" + verb.cli_command() +
                "' is an operational daemon verb served over RPC by a live daemon (start one with "
                "'context daemon', drive it with 'context attach') — not a one-shot CLI verb.");

    // Every other registered verb is a reserved surface at M1.
    return reserved_surface(verb, dry_run, positionals);
}
} // namespace

Envelope run(const std::vector<std::string>& args)
{
    const Registry& reg = Registry::instance();

    if (args.empty())
        return Envelope::failure("usage.invalid",
                                 "no verb supplied — try 'context describe --json'");

    // `editor` is a CLI-local operational command family (a headless in-process driver over the
    // composed EditorKernel), NOT a contract registry verb — so it is intercepted before registry
    // resolution. See editor_driver.h for why it is deliberately outside the CLI ≡ RPC ≡ MCP surface.
    if (args[0] == "editor")
        return run_editor(std::vector<std::string>(args.begin() + 1, args.end()));

    // `attach` is the cross-process counterpart of `editor smoke`: it connects to a running daemon
    // over the IPC wire and drives it as a SEPARATE process (attach_command.h). Also operational —
    // outside the contract registry. (`daemon`, the long-running server, is intercepted in main_cli
    // because it manages its own process lifetime rather than returning a one-shot envelope.)
    if (args[0] == "attach")
        return run_attach(std::vector<std::string>(args.begin() + 1, args.end()));

    // --- resolve the verb selector (1 token global, or 2 tokens noun-scoped) --------------------
    const auto [ns0, head0] = split_ns(args[0]);
    const VerbSpec* verb = nullptr;
    std::size_t cursor = 0;

    if (const VerbSpec* global = reg.find_verb(ns0, "", head0); global != nullptr)
    {
        verb = global;
        cursor = 1;
    }
    if (verb == nullptr && ns0.empty())
    {
        // Registry-owned GLOBAL aliases (e.g. `context fetch` for resource/read, the R-CLI-017
        // naming): generated from the one registry, never a hand-maintained table (R-CLI-009).
        for (const VerbSpec& v : reg.verbs())
        {
            if (!v.cli_alias.empty() && v.ns.empty() && v.cli_alias == head0)
            {
                verb = &v;
                cursor = 1;
                break;
            }
        }
    }
    if (verb == nullptr && args.size() >= 2)
    {
        if (const VerbSpec* scoped = reg.find_verb(ns0, head0, args[1]); scoped != nullptr)
        {
            verb = scoped;
            cursor = 2;
        }
    }

    if (verb == nullptr)
        return Envelope::failure("usage.unknown_verb",
                                 "no such verb: '" + args[0] +
                                     (args.size() >= 2 ? " " + args[1] : "") + "'");

    // --- parse the remaining tokens into flags + positionals ------------------------------------
    std::vector<std::string> positionals;
    std::map<std::string, std::string> flags;
    for (std::size_t i = cursor; i < args.size(); ++i)
    {
        const std::string& tok = args[i];
        if (tok.rfind("--", 0) == 0)
        {
            std::string name = tok.substr(2);
            std::string value;
            bool has_inline_value = false;
            if (const std::size_t eq = name.find('='); eq != std::string::npos)
            {
                value = name.substr(eq + 1);
                name = name.substr(0, eq);
                has_inline_value = true;
            }
            const FlagSpec* spec = find_flag(*verb, reg, name);
            if (spec == nullptr)
                return Envelope::failure("usage.unknown_flag", "unknown flag: --" + name);
            if (spec->value_type == "bool")
            {
                flags[name] = has_inline_value ? value : "true";
            }
            else if (has_inline_value)
            {
                flags[name] = value;
            }
            else if (i + 1 < args.size())
            {
                flags[name] = args[++i];
            }
            else
            {
                return Envelope::failure("usage.missing_argument",
                                         "flag --" + name + " requires a value");
            }
        }
        else
        {
            positionals.push_back(tok);
        }
    }

    return dispatch(*verb, positionals, flags);
}

int main_cli(int argc, char** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);

    // `daemon` is the long-running server process: it manages its own stdout + exit code (a clean
    // shutdown is 0, the R-BRIDGE-001 attach signal is a distinct code), so it is intercepted here
    // rather than through run()'s one-shot envelope-print path.
    if (!args.empty() && args[0] == "daemon")
        return run_daemon(std::vector<std::string>(args.begin() + 1, args.end()));

    // `bench` is the R-FILE-011 benchmark subject: its stdout is the bench/harness.py subject
    // contract (one plain JSON object per invocation), NOT the R-CLI-008 envelope — so it too
    // bypasses run()'s envelope-print path (see bench_command.h).
    if (!args.empty() && args[0] == "bench")
    {
        std::string out_json;
        const int rc = run_bench(std::vector<std::string>(args.begin() + 1, args.end()), out_json);
        std::fwrite(out_json.data(), 1, out_json.size(), stdout);
        std::fputc('\n', stdout);
        return rc;
    }

    const Envelope env = run(args);
    const std::string out = env.dump(2);
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fputc('\n', stdout);
    return env.exit_code();
}

} // namespace context::cli
