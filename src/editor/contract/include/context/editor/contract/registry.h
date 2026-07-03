// Registry: the SINGLE source of truth for the whole public surface (R-CLI-007/009/013).
//
// R-CLI-009 forbids hand-maintained parity between surfaces: the file-rewriter CLI verbs, the RPC
// methods, the built-in MCP tools, and the introspection artifact are ALL generated from this one
// registry. This header defines the registry's data model (the verb grammar of R-CLI-007) and the
// pure surface generators (cli_surface / rpc_surface / mcp_surface / describe) that project it onto
// each surface. Because every surface is a pure function of the same registry, CLI ≡ RPC ≡ MCP ≡
// introspection holds BY CONSTRUCTION; the conformance test (test_registry_parity) locks it so a
// future divergence fails the build (the R-CLI-009 CI proof).

#pragma once

#include "context/editor/contract/json.h"

#include <string>
#include <vector>

namespace context::editor::contract
{

// A flag. Core flags (the R-CLI-007 fixed set) are honored by every verb; verb-specific flags are
// declared per verb. `reserved` marks a flag accepted-but-inert in v1 (e.g. --atomic-plan), so its
// grammar slot is contract from day one and activating the behavior later is non-breaking.
struct FlagSpec
{
    std::string name;        // without the leading "--", e.g. "if-match"
    std::string value_type;  // "bool" | "string" | "hash" | "generation" | "json"
    std::string description;
    bool reserved = false;
};

// A positional/named argument to a verb.
struct ParamSpec
{
    std::string name;
    std::string value_type; // "string" | "json" | "path" | ...
    bool required = false;
    std::string description;
};

// One verb in the grammar `context [<ns>:]<noun> <verb> [--flags]`.
//   - `ns` empty  => engine verb (bare namespace); non-empty => package-contributed (reserved).
//   - `noun` empty => a global verb (`context <verb>`, e.g. `describe`, `new`); non-empty => a
//     noun-scoped verb (`context <noun> <verb>`, e.g. `package add`).
// Each verb carries its stable identifiers on EVERY surface: rpc_method (R-CLI-004 method-id)
// and mcp_tool. These are derived from ns/noun/verb at registration so they cannot drift.
struct VerbSpec
{
    std::string ns;
    std::string noun;
    std::string verb;
    std::string summary;
    std::vector<ParamSpec> params;
    std::vector<FlagSpec> flags; // verb-specific flags (beyond the core set)
    std::string rpc_method;      // e.g. "describe", "package.add"
    std::string mcp_tool;        // e.g. "context_describe", "context_package_add"
    bool implemented = false;    // false => the surface is reserved; invoking returns
                                 //          contract.unimplemented (R-CLI-009 grammar reservation)

    // The canonical CLI invocation form, e.g. "context describe" or "context [<ns>:]package add".
    [[nodiscard]] std::string cli_command() const;
    // The registry key: ns + noun + verb, unique across the surface.
    [[nodiscard]] std::string key() const;
};

// An advertised event topic (R-BRIDGE-008 core set; statically described per R-CLI-013/014).
struct TopicSpec
{
    std::string name;
    std::string description;
};

class Registry
{
public:
    // The one process-wide registry instance — the single source of truth.
    [[nodiscard]] static const Registry& instance();

    [[nodiscard]] const std::vector<VerbSpec>& verbs() const noexcept { return verbs_; }
    [[nodiscard]] const std::vector<FlagSpec>& core_flags() const noexcept { return core_flags_; }
    [[nodiscard]] const std::vector<TopicSpec>& topics() const noexcept { return topics_; }

    // Look a verb up by its (ns, noun, verb) triple. nullptr when absent.
    [[nodiscard]] const VerbSpec* find_verb(const std::string& ns, const std::string& noun,
                                            const std::string& verb) const;

    // --- surface generators (R-CLI-009): each is a pure projection of `verbs_` ------------------
    // Every generator returns exactly one entry per registered verb, so their sizes are equal and
    // their per-index identities line up — the structural parity the conformance test asserts.
    [[nodiscard]] Json cli_surface() const;
    [[nodiscard]] Json rpc_surface() const;
    [[nodiscard]] Json mcp_surface() const;

    // The whole-contract self-description (R-CLI-013): verbs+params+flags, RPC methods, MCP tools,
    // event topics, the error-code catalog, the core-flag set, and the protocol/capability version.
    [[nodiscard]] Json describe() const;

private:
    Registry();
    std::vector<FlagSpec> core_flags_;
    std::vector<VerbSpec> verbs_;
    std::vector<TopicSpec> topics_;
};

} // namespace context::editor::contract
