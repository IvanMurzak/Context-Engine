// Registry implementation: the single source of truth + its surface projections (see registry.h).

#include "context/editor/contract/registry.h"

#include "context/editor/contract/error_catalog.h"
#include "context/editor/contract/handshake.h"

#include <utility>

namespace context::editor::contract
{

std::string VerbSpec::cli_command() const
{
    std::string cmd = "context ";
    if (!noun.empty())
    {
        if (!ns.empty())
            cmd += ns + ":";
        cmd += noun + " ";
    }
    cmd += verb;
    return cmd;
}

std::string VerbSpec::key() const
{
    return (ns.empty() ? std::string() : ns + ":") + noun + "/" + verb;
}

namespace
{
// The R-CLI-007 core-flag set: honored by EVERY verb. --idempotency-key and --after-generation
// stay reserved-but-accepted in v1 (their behavior activates with the replay store / barrier).
std::vector<FlagSpec> make_core_flags()
{
    return {
        {"json", "bool", "Emit the R-CLI-008 result envelope as JSON on stdout.", false},
        {"project", "path", "Path to the project root the verb operates against.", false},
        {"if-match", "hash",
         "CAS guard: apply only if the target's raw-byte hash matches (R-CLI-006).", false},
        {"after-generation", "generation",
         "Read-your-writes barrier: block until the derived world reflects this generation "
         "(R-CLI-006).",
         true},
        {"dry-run", "bool", "Validate and report the intended effect without writing.", false},
        {"idempotency-key", "string",
         "Client-supplied replay key (R-CLI-016); reserved and accepted, replay store lands later.",
         true},
    };
}

// Derive the stable RPC method-id from the grammar triple: "<noun>.<verb>" ("<verb>" when global).
std::string rpc_method_for(const std::string& noun, const std::string& verb)
{
    return noun.empty() ? verb : noun + "." + verb;
}

// Derive the MCP tool name: "context_[<noun>_]<verb>".
std::string mcp_tool_for(const std::string& noun, const std::string& verb)
{
    return noun.empty() ? "context_" + verb : "context_" + noun + "_" + verb;
}

VerbSpec make_verb(std::string ns, std::string noun, std::string verb, std::string summary,
                   std::vector<ParamSpec> params, std::vector<FlagSpec> flags, bool implemented)
{
    VerbSpec spec;
    spec.rpc_method = rpc_method_for(noun, verb);
    spec.mcp_tool = mcp_tool_for(noun, verb);
    spec.ns = std::move(ns);
    spec.noun = std::move(noun);
    spec.verb = std::move(verb);
    spec.summary = std::move(summary);
    spec.params = std::move(params);
    spec.flags = std::move(flags);
    spec.implemented = implemented;
    return spec;
}

Json param_to_json(const ParamSpec& p)
{
    Json out = Json::object();
    out.set("name", Json(p.name));
    out.set("type", Json(p.value_type));
    out.set("required", Json(p.required));
    out.set("description", Json(p.description));
    return out;
}

Json flag_to_json(const FlagSpec& f)
{
    Json out = Json::object();
    out.set("name", Json(f.name));
    out.set("type", Json(f.value_type));
    out.set("description", Json(f.description));
    out.set("reserved", Json(f.reserved));
    return out;
}

// The flat name list of every input a verb accepts on any surface: params + core flags + verb
// flags. Shared by all three surface generators so parity is exact.
Json param_names(const VerbSpec& v)
{
    Json names = Json::array();
    for (const ParamSpec& p : v.params)
        names.push_back(Json(p.name));
    return names;
}

Json flag_names(const VerbSpec& v, const std::vector<FlagSpec>& core)
{
    Json names = Json::array();
    for (const FlagSpec& f : core)
        names.push_back(Json(f.name));
    for (const FlagSpec& f : v.flags)
        names.push_back(Json(f.name));
    return names;
}
} // namespace

Registry::Registry()
{
    core_flags_ = make_core_flags();

    // The M1 verb surface. Kept small — enough to fix the grammar (global + noun-scoped
    // + reserved-package forms), the envelope, describe, and a runnable-template scaffolder — while
    // the query/batch/subscription verbs (R-CLI-011/012/014/015) reserve their grammar for M3.
    verbs_.push_back(make_verb(
        "", "", "describe",
        "Emit the whole self-describing contract (verbs, methods, tools, topics, error catalog, "
        "protocol) as JSON.",
        /*params=*/{}, /*flags=*/{}, /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "", "new",
        "Scaffold a new project from a template. The default template is a minimal RUNNABLE "
        "skeleton (a scene, a camera, a startable session) — R-QA-006.",
        /*params=*/
        {{"directory", "path", true, "Target directory to scaffold the project into."},
         {"template", "string", false,
          "Template name; defaults to the runnable default template."}},
        /*flags=*/{}, /*implemented=*/true));

    verbs_.push_back(make_verb(
        "", "", "set",
        "Set a value in an authored file (file-rewriter). The atomic write routes through filesync "
        "when integrated; the verb surface + envelope are contract now.",
        /*params=*/
        {{"path", "path", true, "File (and JSON-pointer) to write."},
         {"value", "json", true, "The JSON value to set."}},
        /*flags=*/{}, /*implemented=*/false));

    // A noun-scoped, package-facing verb: fixes `context <noun> <verb>` grammar AND reserves the
    // R-CLI-007 namespace-collision path (inert until a second package source exists).
    verbs_.push_back(make_verb(
        "", "package", "add",
        "Add a package to the project. Its reserved namespace is collision-checked at add time "
        "(R-CLI-007); the check activates when a second package source exists.",
        /*params=*/{{"name", "string", true, "Package name/source to add."}},
        /*flags=*/{}, /*implemented=*/false));

    // The R-BRIDGE-008 core event topics, described statically (R-CLI-013/014). Live
    // package-registered topics join this set at runtime as the package ecosystem lands.
    topics_ = {
        {"files", "Post-derivation file change facts (never raw filesystem noise)."},
        {"derivation", "Derived-world updates, each stamped with the input content-hash."},
        {"diagnostics", "Machine-readable diagnostics through the R-CLI-008 error schema."},
        {"session", "Session lifecycle + restart-class reload announcements."},
        {"clients", "Client attach/detach on the daemon."},
        {"log", "Structured log entries (severity, source, tick, session)."},
    };
}

const Registry& Registry::instance()
{
    static const Registry registry;
    return registry;
}

const VerbSpec* Registry::find_verb(const std::string& ns, const std::string& noun,
                                    const std::string& verb) const
{
    for (const VerbSpec& v : verbs_)
        if (v.ns == ns && v.noun == noun && v.verb == verb)
            return &v;
    return nullptr;
}

Json Registry::cli_surface() const
{
    Json out = Json::array();
    for (const VerbSpec& v : verbs_)
    {
        Json entry = Json::object();
        entry.set("command", Json(v.cli_command()));
        entry.set("ns", Json(v.ns));
        entry.set("noun", Json(v.noun));
        entry.set("verb", Json(v.verb));
        entry.set("params", param_names(v));
        entry.set("flags", flag_names(v, core_flags_));
        out.push_back(std::move(entry));
    }
    return out;
}

Json Registry::rpc_surface() const
{
    Json out = Json::array();
    for (const VerbSpec& v : verbs_)
    {
        Json entry = Json::object();
        entry.set("method", Json(v.rpc_method));
        entry.set("ns", Json(v.ns));
        entry.set("noun", Json(v.noun));
        entry.set("verb", Json(v.verb));
        entry.set("params", param_names(v));
        entry.set("flags", flag_names(v, core_flags_));
        out.push_back(std::move(entry));
    }
    return out;
}

Json Registry::mcp_surface() const
{
    Json out = Json::array();
    for (const VerbSpec& v : verbs_)
    {
        // The MCP tool's input schema exposes params + flags as named inputs; parity with the CLI
        // and RPC surfaces is checked on these same name lists.
        Json input_schema = Json::object();
        input_schema.set("params", param_names(v));
        input_schema.set("flags", flag_names(v, core_flags_));

        Json entry = Json::object();
        entry.set("tool", Json(v.mcp_tool));
        entry.set("ns", Json(v.ns));
        entry.set("noun", Json(v.noun));
        entry.set("verb", Json(v.verb));
        entry.set("params", param_names(v));
        entry.set("flags", flag_names(v, core_flags_));
        entry.set("inputSchema", std::move(input_schema));
        out.push_back(std::move(entry));
    }
    return out;
}

Json Registry::describe() const
{
    Json contract = Json::object();

    contract.set("protocol", protocol_descriptor());

    Json core = Json::array();
    for (const FlagSpec& f : core_flags_)
        core.push_back(flag_to_json(f));
    contract.set("coreFlags", std::move(core));

    Json verbs = Json::array();
    for (const VerbSpec& v : verbs_)
    {
        Json entry = Json::object();
        entry.set("ns", Json(v.ns));
        entry.set("noun", Json(v.noun));
        entry.set("verb", Json(v.verb));
        entry.set("command", Json(v.cli_command()));
        entry.set("summary", Json(v.summary));
        entry.set("rpcMethod", Json(v.rpc_method));
        entry.set("mcpTool", Json(v.mcp_tool));
        entry.set("implemented", Json(v.implemented));
        Json params = Json::array();
        for (const ParamSpec& p : v.params)
            params.push_back(param_to_json(p));
        entry.set("params", std::move(params));
        Json flags = Json::array();
        for (const FlagSpec& f : v.flags)
            flags.push_back(flag_to_json(f));
        entry.set("flags", std::move(flags));
        verbs.push_back(std::move(entry));
    }
    contract.set("verbs", std::move(verbs));

    contract.set("rpcMethods", rpc_surface());
    contract.set("mcpTools", mcp_surface());

    Json topics = Json::array();
    for (const TopicSpec& t : topics_)
    {
        Json entry = Json::object();
        entry.set("name", Json(t.name));
        entry.set("description", Json(t.description));
        topics.push_back(std::move(entry));
    }
    contract.set("eventTopics", std::move(topics));

    Json errors = Json::array();
    for (const ErrorCode& e : catalog())
    {
        Json entry = Json::object();
        entry.set("code", Json(e.code));
        entry.set("message", Json(e.message));
        entry.set("retriable", Json(e.retriable));
        entry.set("exitCode", Json(e.exit_code));
        entry.set("origin", Json(e.origin));
        errors.push_back(std::move(entry));
    }
    contract.set("errorCatalog", std::move(errors));

    // Reserved sections: file kinds + component types populate as packages register (R-CLI-005);
    // empty at M1 with no registered kinds, but the section shape is contract from day one.
    contract.set("fileKinds", Json::array());
    contract.set("componentTypes", Json::array());

    Json out = Json::object();
    out.set("contract", std::move(contract));
    return out;
}

} // namespace context::editor::contract
