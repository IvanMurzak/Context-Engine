// Contextual-help model: pure projections of the ONE contract registry into human-readable help
// (see help_model.h). No CEF, no network — everything is derived from contract::Registry::instance()
// and the committed samples/ corpus.

#include "context/editor/gui/help/help_model.h"

#include "context/editor/contract/registry.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::help
{

namespace
{

// Render one param as a single deterministic line: "name (type[, required]) — description".
[[nodiscard]] std::string render_param(const contract::ParamSpec& p)
{
    std::string line = p.name + " (" + p.value_type;
    if (p.required)
    {
        line += ", required";
    }
    line += ")";
    if (!p.description.empty())
    {
        line += " - " + p.description;
    }
    return line;
}

// Render one flag as a single deterministic line: "--name <type> — description [reserved]
// [deprecated: removed in X]". The reserved/deprecation markers are live-contract fields (R-CLI-010),
// so they stay in lockstep with introspection.
[[nodiscard]] std::string render_flag(const contract::FlagSpec& f)
{
    std::string line = "--" + f.name + " <" + f.value_type + ">";
    if (!f.description.empty())
    {
        line += " - " + f.description;
    }
    if (f.reserved)
    {
        line += " [reserved]";
    }
    if (f.deprecated)
    {
        line += " [deprecated: removed in " + f.removed_in + "]";
    }
    return line;
}

} // namespace

std::vector<SampleRef> getting_started_samples()
{
    // The R-QA-006 human-onboarding set (R-HUX-010 getting-started references): the readable,
    // tutorial-shaped RUNNABLE projects in the committed corpus — the two sample GAMES plus the RPG.
    // Ordered simplest-first. Each `path` is asserted to exist by the gui-help-getting-started ctest.
    return {
        {"platformer-2d", "2D Platformer", "samples/platformer-2d",
         "A small but real 2D platformer: an input-driven run+jump player, platforms and a pushable "
         "crate, landing particles, jump/coin sounds, and sprite-sheet animation."},
        {"roll-3d", "3D Roller", "samples/roll-3d",
         "A small but real 3D game: a player-controlled rolling ball, dynamic boulders and a ramp, "
         "impact particles and spatialized audio, and a skeletal-animated flag prop."},
        {"topdown-rpg", "Top-down RPG", "samples/topdown-rpg",
         "A tiny top-down RPG applying the three structural override kinds, with a three-way merge "
         "corpus (clean + conflicting edits) to learn convergence by example."},
    };
}

VerbHelp render_verb_help(const contract::VerbSpec& verb,
                          const std::vector<contract::FlagSpec>& core_flags)
{
    VerbHelp help;
    help.command = verb.cli_command();
    help.summary = verb.summary;
    help.rpc_method = verb.rpc_method;
    help.mcp_tool = verb.mcp_tool;

    for (const contract::ParamSpec& p : verb.params)
    {
        help.params.push_back(render_param(p));
    }
    // Core flags are honored by EVERY verb (R-CLI-007), then the verb-specific flags — the same
    // union `describe`/`--help` present, in the same order.
    for (const contract::FlagSpec& f : core_flags)
    {
        help.flags.push_back(render_flag(f));
    }
    for (const contract::FlagSpec& f : verb.flags)
    {
        help.flags.push_back(render_flag(f));
    }

    std::ostringstream text;
    text << help.command << "\n" << help.summary << "\n\n";
    text << "RPC method: " << help.rpc_method << " | MCP tool: " << help.mcp_tool << "\n\n";
    text << "Parameters:\n";
    if (help.params.empty())
    {
        text << "  (none)\n";
    }
    else
    {
        for (const std::string& p : help.params)
        {
            text << "  " << p << "\n";
        }
    }
    text << "\nFlags:\n";
    for (const std::string& f : help.flags)
    {
        text << "  " << f << "\n";
    }
    help.text = text.str();
    return help;
}

std::optional<VerbHelp> verb_help(const std::string& command)
{
    const contract::Registry& reg = contract::Registry::instance();
    for (const contract::VerbSpec& v : reg.verbs())
    {
        if (v.cli_command() == command)
        {
            return render_verb_help(v, reg.core_flags());
        }
    }
    return std::nullopt;
}

std::vector<VerbHelp> all_verb_help()
{
    const contract::Registry& reg = contract::Registry::instance();
    std::vector<VerbHelp> out;
    for (const contract::VerbSpec& v : reg.verbs())
    {
        // The user-facing help corpus is the stable, implemented one-shot surface — the verbs a
        // human actually runs. Operational (daemon-served) and reserved (unimplemented) verbs stay
        // out of the help set (they remain contract-honest in `describe`).
        if (v.stability == "stable" && v.implemented)
        {
            out.push_back(render_verb_help(v, reg.core_flags()));
        }
    }
    return out;
}

std::vector<PanelHelp> panel_topics()
{
    // One topic per shipped editor panel (id + title match a11y::registered_panels() /
    // coverage.manifest.jsonl). `related_commands` are live registry commands — the
    // gui-help-test_help_model ctest asserts each resolves, and gui-help-contextual asserts this set
    // == the registered panels.
    return {
        {"placeholder", "Context Editor",
         "The editor host. Start here — scaffold a runnable project, then inspect the contract.",
         {"context new", "context describe"}},
        {"builtin.scene-tree", "Scene Tree",
         "Observe the composed derived-world hierarchy (instances and overrides are visible).",
         {"context query", "context validate"}},
        {"builtin.inspector", "Inspector",
         "Inspect and edit the selected entity's composed fields; edits write file overrides.",
         {"context set", "context query"}},
        {"builtin.viewport", "Viewport",
         "The read-only render observer of the current scene.",
         {"context query", "context describe"}},
        {"builtin.playbar", "Play Bar",
         "Drive a play-in-editor session: create, seed, step, and inspect the deterministic state.",
         {"context session new", "context session step"}},
        {"builtin.problems", "Problems",
         "The diagnostics list — post-merge convergence and validation findings, keyboard-navigable.",
         {"context validate"}},
        {"builtin.tilemap-painter", "Tilemap Painter",
         "Paint and fill tilemap cells; the same write path the tilemap verbs run (GUI is sugar).",
         {"context tilemap paint", "context tilemap fill"}},
        {"builtin.viewport-edit", "Viewport Edit",
         "In-context override editing via gizmos; the gesture commit runs the same composed write.",
         {"context set"}},
        {"builtin.help", "Help",
         "This panel: getting-started pointers and per-panel help generated from the live contract.",
         {"context describe"}},
    };
}

std::optional<PanelHelp> contextual_help(const std::string& panel_id)
{
    for (PanelHelp& topic : panel_topics())
    {
        if (topic.panel_id == panel_id)
        {
            return std::move(topic);
        }
    }
    return std::nullopt;
}

} // namespace context::editor::gui::help
