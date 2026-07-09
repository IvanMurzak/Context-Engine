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

#include <cstdint>
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

    // Deprecation lifecycle (R-CLI-010). A flag marked deprecated:true carries the version it will
    // be removed in (removed_in), surfaced in `describe`. The name/grammar slot stays STABLE across
    // the whole lifecycle (a deprecated flag keeps its spelling until removal). No core or verb flag
    // is deprecated at the M3 freeze — the machinery is inert until the first real deprecation.
    bool deprecated = false;
    std::string removed_in{}; // the protocol version the flag is scheduled for removal in (SemVer)
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

    // Contract stability class (R-CLI-009 honesty; introspected via describe):
    //   "stable"      — the versioned contract surface (default; frozen to promotion at M3).
    //   "operational" — the daemon-driver surface (edit / query / snapshot / …) served by a LIVE
    //                   daemon's method backend over RPC. Registered so `context describe` reflects
    //                   the REAL served surface, but explicitly UNSTABLE: promoted into the stable
    //                   contract (or dropped) at the M3 freeze, without a deprecation cycle.
    std::string stability = "stable";

    // Optional registry-owned alternate GLOBAL CLI spelling (e.g. `context fetch` for resource/read,
    // the R-CLI-017 name). Lives IN the registry so the alias is introspectable and the CLI resolver
    // generates it from the one source of truth — never hand-maintained parity (R-CLI-009).
    std::string cli_alias;

    // Deprecation lifecycle (R-CLI-010). A verb marked deprecated:true carries the version it will
    // be removed in (removed_in), surfaced on every generated surface. rpc_method / mcp_tool (the
    // R-CLI-004 STABLE method-ids) NEVER change across the lifecycle — a deprecated verb keeps its
    // id until removal so a client's stored id resolves for the whole compatibility window. No real
    // verb is deprecated at the M3 freeze; the metadata is inert until the first real deprecation.
    bool deprecated = false;
    std::string removed_in{}; // the protocol version the verb is scheduled for removal in (SemVer)

    // The canonical CLI invocation form, e.g. "context describe" or "context [<ns>:]package add".
    [[nodiscard]] std::string cli_command() const;
    // The registry key: ns + noun + verb, unique across the surface.
    [[nodiscard]] std::string key() const;
};

// An advertised event topic (R-BRIDGE-008 core set; runtime-discoverable per R-CLI-013/014). Each
// topic carries its event-type PAYLOAD SCHEMA so a client can enumerate not just which topics exist
// but what every event on a topic carries (R-CLI-014). `name` may be namespaced (`<ns>:<topic>`) for
// a package-contributed topic (R-CLI-007 namespace rule), which then appears in introspection
// automatically through the same register_topic() seam the engine topics use.
struct TopicSpec
{
    std::string name;
    std::string description;
    Json payload_schema; // the event-payload schema for this topic (fields + their types)
};

// One registered authored file kind (R-CLI-005 / R-DATA-006): the kind id, its schema version,
// and the published introspection entry (the versioned JSON Schema + the derived per-field
// x-ctx-* index, including x-ctx-units — the units-law introspection surface). `entry` is parsed
// from the schema module's canonical projection, so `describe` enumerates the SAME documents the
// derivation validate node enforces — one source of truth, never a hand-maintained copy.
struct FileKindSpec
{
    std::string id;
    std::int64_t version = 0;
    Json entry;
};

class Registry
{
public:
    // The one process-wide registry instance — the single source of truth.
    [[nodiscard]] static const Registry& instance();

    [[nodiscard]] const std::vector<VerbSpec>& verbs() const noexcept { return verbs_; }
    [[nodiscard]] const std::vector<FlagSpec>& core_flags() const noexcept { return core_flags_; }
    [[nodiscard]] const std::vector<TopicSpec>& topics() const noexcept { return topics_; }

    // The registered file kinds (R-CLI-005): each authored kind's versioned schema publication.
    // Engine kinds register at construction through the same register_file_kind() mechanism
    // package-contributed kinds will use when the package system lands — `describe` enumerates
    // whatever is registered, never a hardcoded list.
    [[nodiscard]] const std::vector<FileKindSpec>& file_kinds() const noexcept
    {
        return file_kinds_;
    }

    // Look a file kind up by its id ("ctx:scene"). nullptr when absent.
    [[nodiscard]] const FileKindSpec* find_file_kind(const std::string& id) const;

    // Look an event topic up by its (possibly namespaced) name ("files", "mypkg:combat"). nullptr
    // when absent. Enumeration parity with find_file_kind (R-CLI-014).
    [[nodiscard]] const TopicSpec* find_topic(const std::string& name) const;

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
    // The R-CLI-005 registration mechanism: engine kinds call this from the constructor; live
    // package add/remove re-registration arrives with the package system.
    void register_file_kind(FileKindSpec spec);
    // The R-CLI-014 event-topic registration seam (parity with register_file_kind): the engine core
    // topics register from the constructor; package-contributed namespaced topics join at runtime
    // through the SAME mechanism as the package ecosystem lands, so `describe` enumerates whatever is
    // registered without a second source of truth. A re-registration of the same name replaces it.
    void register_topic(TopicSpec spec);
    std::vector<FlagSpec> core_flags_;
    std::vector<VerbSpec> verbs_;
    std::vector<TopicSpec> topics_;
    std::vector<FileKindSpec> file_kinds_;
};

// The pure per-entry `describe` projections (R-CLI-009: ONE projection, used by both the describe
// generator and the conformance test, so the deprecation metadata a describe consumer reads is
// exactly what the test asserts — never a hand-maintained second copy). Each emits the R-CLI-010
// deprecation lifecycle fields: always a boolean `deprecated`, plus `removedIn` when deprecated.
[[nodiscard]] Json flag_describe_json(const FlagSpec& flag);
[[nodiscard]] Json verb_describe_json(const VerbSpec& verb);

} // namespace context::editor::contract
