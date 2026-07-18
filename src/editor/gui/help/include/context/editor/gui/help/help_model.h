// In-editor contextual help, GENERATED from the live contract (R-HUX-010 / R-CLI-013 / R-HUX-004 /
// R-QA-006). The help a human reads in the editor is a pure projection of the ONE contract registry
// (src/editor/contract/) — never a hand-written parallel doc that rots. Per-verb help is the same
// introspection `context describe` / `context <verb> --help` emit (R-HUX-004: human discoverability
// built on R-CLI-013 introspection); the getting-started references point at the R-QA-006
// human-onboarding samples. Pure C++ over the in-process registry + the committed corpus — no
// network fetch (offline by construction), CEF-free, CI-assertable on the default matrix.

#pragma once

#include "context/editor/contract/registry.h"

#include <optional>
#include <string>
#include <vector>

namespace context::editor::gui::help
{

// A getting-started sample reference (R-QA-006 human-onboarding set — the getting-started references
// R-HUX-010 names). A curated subset of samples/: the readable, tutorial-shaped RUNNABLE projects.
// `path` is project-relative into the committed corpus (offline — the editor opens it locally, never
// fetches it).
struct SampleRef
{
    std::string id;      // the samples/ directory name, e.g. "platformer-2d"
    std::string title;   // human title, e.g. "2D Platformer"
    std::string path;    // project-relative corpus path, e.g. "samples/platformer-2d"
    std::string summary; // one-line description
};

// The R-QA-006 human-onboarding samples the getting-started panel references (R-HUX-010). Static +
// in-process — resolved from the committed corpus, offline by construction (no network). The
// gui-help-getting-started ctest asserts every referenced `path` exists under samples/, so a removed
// or renamed sample reddens the build (rots-if-drift), never a dangling in-editor link.
[[nodiscard]] std::vector<SampleRef> getting_started_samples();

// One verb's help, GENERATED from the live contract registry (R-CLI-013 / R-HUX-004). Every field is
// projected from contract::VerbSpec, so it cannot drift from introspection — the SAME source
// `context describe` and `context <verb> --help` (verb_describe_json) emit. Never hand-written.
struct VerbHelp
{
    std::string command;              // the CLI invocation form, e.g. "context session step"
    std::string summary;              // == VerbSpec::summary (the live introspection summary)
    std::string rpc_method;           // == VerbSpec::rpc_method (the R-CLI-004 stable id)
    std::string mcp_tool;             // == VerbSpec::mcp_tool
    std::vector<std::string> params;  // one rendered line per VerbSpec param, in registry order
    std::vector<std::string> flags;   // one rendered line per core flag then verb flag, in order
    std::string text;                 // the full rendered plain-text help block (deterministic)
};

// Generate the help block for one registered verb, given the core flags honored by every verb
// (contract::Registry::core_flags()). A pure projection of the VerbSpec — the params/flags render in
// registry order so the output is deterministic + byte-assertable.
[[nodiscard]] VerbHelp render_verb_help(const contract::VerbSpec& verb,
                                        const std::vector<contract::FlagSpec>& core_flags);

// Look a verb up by its CLI command form ("context describe", "context session step",
// "context tilemap paint") in the ONE registry and render its help. Resolves ANY registered verb —
// stable, operational, or reserved — so a panel can surface an operational read verb like
// `context query`. Returns nullopt when no registered verb has that command.
[[nodiscard]] std::optional<VerbHelp> verb_help(const std::string& command);

// The whole user-facing contextual-help corpus: help for every STABLE + IMPLEMENTED verb, generated
// by iterating contract::Registry::instance().verbs() (R-HUX-004 per-verb help projected from the
// R-CLI-013 introspection). Operational (daemon-served) and reserved (unimplemented) verbs are
// contract-honest in `describe` but excluded from the user-facing help set.
[[nodiscard]] std::vector<VerbHelp> all_verb_help();

// Per-panel contextual help (R-HUX-010): a shipped editor panel plus the CLI verbs whose live help is
// relevant to authoring/observing through it. `related_commands` are CLI command forms resolved
// against the live registry, so the help CONTENT rots-if-drift (asserted); this thin mapping (which
// verbs a panel surfaces) is the only authored metadata — not a parallel doc.
struct PanelHelp
{
    std::string panel_id;                      // matches an a11y::registered_panels() id
    std::string title;                         // matches the panel's coverage-manifest title
    std::string summary;                       // one-line: what the panel is for
    std::vector<std::string> related_commands; // e.g. {"context query", "context validate"}
};

// The contextual help topics for every shipped editor panel (R-HUX-010 "help opens in-context for the
// shipped panels"). EXTENSION POINT: append a PanelHelp here when a new panel lands — the
// register-with-the-panel discipline extends to help. The gui-help-contextual ctest cross-checks this
// set against a11y::registered_panels(), so a panel added without a help topic (or a topic naming a
// phantom panel) fails the CEF-free default build, and gui-help-test_help_model asserts every
// related_command resolves to a real registered verb.
[[nodiscard]] std::vector<PanelHelp> panel_topics();

// Look one panel's contextual help up by id. nullopt when the panel has no registered topic.
[[nodiscard]] std::optional<PanelHelp> contextual_help(const std::string& panel_id);

} // namespace context::editor::gui::help
