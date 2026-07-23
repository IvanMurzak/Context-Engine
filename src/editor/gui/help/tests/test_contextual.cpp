// The "help opens in-context for the shipped panels" contract (R-HUX-010 DoD): the contextual-help
// topic set (help::panel_topics()) must name the SAME panels the editor SHIPS — the built-in roster
// (gui/contract/builtin_roster.h). A panel shipped without a help topic (or a topic naming a phantom
// panel) is a coverage gap — the same register-with-the-panel discipline the a11y coverage manifest
// enforces, applied to help.
//
// ⚠ THE ROSTER, NOT a11y::registered_panels() — CORRECTED BY M9 e06d. The two sets were identical
// until a `ContentType::local` panel existed (Settings: editor-core renders it from the kit, so it has
// no C++ factory and cannot appear in the scanned set). Keying this gate off the SCAN would have made
// help optional for precisely the renderer-modelled panels, and silently: the gate would still pass.
// The roster is what "shipped" means, so the roster is what help must cover.

// help_model.h FIRST, deliberately: it names `contract::VerbSpec` meaning the EDITOR contract
// (context::editor::contract), and pulling gui/contract in ahead of it would put a nearer
// `context::editor::gui::contract` in scope for its own header — which resolves to the wrong
// namespace and fails to compile. Include order is load-bearing here, not cosmetic.
#include "context/editor/gui/help/help_model.h"

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/extension.h"

#include "help_test.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace context::editor::gui;

int main()
{
    // The panels the editor SHIPS (the single built-in roster, M9 e05b).
    std::set<std::string> registered_ids;
    for (const contract::Contribution& c : contract::builtin_contributions())
    {
        registered_ids.insert(c.id);
    }
    CHECK(!registered_ids.empty());

    // The panels help DOCUMENTS (contextual topics), no duplicate ids.
    std::set<std::string> topic_ids;
    for (const help::PanelHelp& t : help::panel_topics())
    {
        CHECK(topic_ids.insert(t.panel_id).second); // a duplicate topic id is itself a defect
    }
    CHECK(!topic_ids.empty());

    // The contract: every shipped panel has a help topic AND every topic names a real panel.
    if (topic_ids != registered_ids)
    {
        std::vector<std::string> only_registered;
        std::set_difference(registered_ids.begin(), registered_ids.end(), topic_ids.begin(),
                            topic_ids.end(), std::back_inserter(only_registered));
        std::vector<std::string> only_topics;
        std::set_difference(topic_ids.begin(), topic_ids.end(), registered_ids.begin(),
                            registered_ids.end(), std::back_inserter(only_topics));
        for (const std::string& id : only_registered)
        {
            std::fprintf(stderr,
                         "gui-help-contextual: panel %s is on the built-in roster but has NO help "
                         "topic in help::panel_topics() (add one in the same PR)\n",
                         id.c_str());
        }
        for (const std::string& id : only_topics)
        {
            std::fprintf(stderr,
                         "gui-help-contextual: help topic %s is not a shipped panel "
                         "(gui/contract/src/builtin_roster.cpp) - remove or fix it\n",
                         id.c_str());
        }
    }
    CHECK(topic_ids == registered_ids);

    // Every shipped panel resolves an in-context help topic (the "help opens in-context" DoD).
    for (const std::string& id : registered_ids)
    {
        CHECK(help::contextual_help(id).has_value());
    }

    HELP_TEST_MAIN_END();
}
