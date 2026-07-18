// The DoD assertion (R-HUX-010): the editor's per-verb help is GENERATED from live introspection and
// MATCHES it. For every registered verb, render_verb_help() is cross-checked field-by-field against
// contract::verb_describe_json() — the exact projection `context <verb> --help` returns (src/cli:
// app.cpp) — so the in-editor help can never drift from the contract. Also asserts the user-facing
// corpus is exactly the stable+implemented registry surface, and that per-panel contextual help
// references only real registered verbs.

#include "context/editor/gui/help/help_model.h"

#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"

#include "help_test.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace context::editor::gui::help;
namespace contract = context::editor::contract;

namespace
{

[[nodiscard]] bool flag_line_present(const std::vector<std::string>& flags, const std::string& name)
{
    const std::string needle = "--" + name + " <";
    for (const std::string& line : flags)
    {
        if (line.rfind(needle, 0) == 0)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool corpus_has(const std::vector<VerbHelp>& corpus, const std::string& command)
{
    for (const VerbHelp& vh : corpus)
    {
        if (vh.command == command)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    const contract::Registry& reg = contract::Registry::instance();
    const std::vector<contract::FlagSpec>& core = reg.core_flags();
    CHECK(!core.empty());
    CHECK(!reg.verbs().empty());

    std::size_t stable_impl = 0;

    // === 1. Every verb's generated help is a faithful projection of its LIVE introspection ==========
    for (const contract::VerbSpec& v : reg.verbs())
    {
        const VerbHelp vh = render_verb_help(v, core);
        // verb_describe_json IS what `context <verb> --help` emits (src/cli/app.cpp) — the ground
        // truth for "help matches live introspection".
        const contract::Json dj = contract::verb_describe_json(v);

        CHECK(vh.command == v.cli_command());
        CHECK(vh.command == dj.at("command").as_string());
        CHECK(vh.summary == v.summary);
        CHECK(vh.summary == dj.at("summary").as_string());
        CHECK(vh.rpc_method == dj.at("rpcMethod").as_string());
        CHECK(vh.mcp_tool == dj.at("mcpTool").as_string());

        // Params: generated 1:1 from the introspection, in registry order, name + description carried.
        CHECK(vh.params.size() == v.params.size());
        CHECK(dj.at("params").size() == v.params.size());
        for (std::size_t i = 0; i < v.params.size(); ++i)
        {
            CHECK(vh.params[i].find(v.params[i].name) != std::string::npos);
            if (!v.params[i].description.empty())
            {
                CHECK(vh.params[i].find(v.params[i].description) != std::string::npos);
            }
        }

        // Flags: the core set (honored by EVERY verb) then the verb-specific flags. describe emits
        // only the verb-specific flags per entry, so the help set is core + that.
        CHECK(vh.flags.size() == core.size() + v.flags.size());
        CHECK(dj.at("flags").size() == v.flags.size());
        for (const contract::FlagSpec& f : core)
        {
            CHECK(flag_line_present(vh.flags, f.name));
        }
        for (const contract::FlagSpec& f : v.flags)
        {
            CHECK(flag_line_present(vh.flags, f.name));
        }

        // The rendered block carries the command + summary verbatim.
        CHECK(vh.text.find(vh.command) != std::string::npos);
        CHECK(vh.text.find(vh.summary) != std::string::npos);

        // verb_help(command) resolves the SAME verb by its CLI form.
        const auto by_cmd = verb_help(v.cli_command());
        CHECK(by_cmd.has_value());
        CHECK(by_cmd->summary == vh.summary);
        CHECK(by_cmd->text == vh.text);

        if (v.stability == "stable" && v.implemented)
        {
            ++stable_impl;
        }
    }

    // === 2. The user-facing corpus == the stable+implemented registry surface, generated live =======
    const std::vector<VerbHelp> corpus = all_verb_help();
    CHECK(stable_impl > 0);
    CHECK(corpus.size() == stable_impl);
    for (const VerbHelp& vh : corpus)
    {
        const auto rv = verb_help(vh.command);
        CHECK(rv.has_value());
        CHECK(rv->summary == vh.summary);
    }
    // Spot checks: describe/set are surfaced; an operational verb (query) is resolvable for panel
    // help but excluded from the user-facing corpus; an unknown command resolves to nothing.
    CHECK(corpus_has(corpus, "context describe"));
    CHECK(corpus_has(corpus, "context set"));
    CHECK(verb_help("context query").has_value());
    CHECK(!corpus_has(corpus, "context query"));
    CHECK(!verb_help("context definitely-not-a-verb").has_value());

    // === 3. Per-panel contextual help references only REAL registered verbs =========================
    const std::vector<PanelHelp> topics = panel_topics();
    CHECK(!topics.empty());
    for (const PanelHelp& t : topics)
    {
        CHECK(!t.panel_id.empty());
        CHECK(!t.title.empty());
        CHECK(!t.summary.empty());
        CHECK(!t.related_commands.empty());
        for (const std::string& cmd : t.related_commands)
        {
            CHECK(verb_help(cmd).has_value()); // rots if a referenced verb is renamed/removed
        }
    }
    CHECK(contextual_help("builtin.scene-tree").has_value());
    CHECK(contextual_help("builtin.help").has_value());
    CHECK(!contextual_help("no-such-panel").has_value());

    HELP_TEST_MAIN_END();
}
