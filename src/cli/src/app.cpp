// `context` CLI front-end implementation (see app.h).

#include "context/cli/app.h"

#include "context/cli/scaffold.h"
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

    if (verb.noun.empty() && verb.verb == "new")
    {
        const std::string directory = bound.count("directory") ? bound.at("directory") : "";
        const std::string tmpl = bound.count("template") ? bound.at("template") : "default";
        if (dry_run)
            return Envelope::success(scaffold_plan(directory, tmpl));
        return scaffold_project(directory, tmpl);
    }

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

    // --- resolve the verb selector (1 token global, or 2 tokens noun-scoped) --------------------
    const auto [ns0, head0] = split_ns(args[0]);
    const VerbSpec* verb = nullptr;
    std::size_t cursor = 0;

    if (const VerbSpec* global = reg.find_verb(ns0, "", head0); global != nullptr)
    {
        verb = global;
        cursor = 1;
    }
    else if (args.size() >= 2)
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

    const Envelope env = run(args);
    const std::string out = env.dump(2);
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fputc('\n', stdout);
    return env.exit_code();
}

} // namespace context::cli
