// The "help opens in-context for the shipped panels" contract (R-HUX-010 DoD): the contextual-help
// topic set (help::panel_topics()) must name the SAME panels the a11y harness scans
// (a11y::registered_panels()). A panel registered without a help topic (or a topic naming a phantom
// panel) is a coverage gap — the same register-with-the-panel discipline the a11y coverage manifest
// enforces, applied to help. Links context_gui_a11y to read the live registered-panel set.

#include "context/editor/gui/a11y/registry.h"
#include "context/editor/gui/help/help_model.h"

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
    // The panels the a11y harness SCANS.
    std::set<std::string> registered_ids;
    for (const a11y::RegisteredPanel& rp : a11y::registered_panels())
    {
        registered_ids.insert(rp.id);
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
                         "gui-help-contextual: panel %s is registered but has NO help topic in "
                         "help::panel_topics() (add one in the same PR)\n",
                         id.c_str());
        }
        for (const std::string& id : only_topics)
        {
            std::fprintf(stderr,
                         "gui-help-contextual: help topic %s is not a registered panel "
                         "(a11y::registered_panels()) — remove or fix it\n",
                         id.c_str());
        }
    }
    CHECK(topic_ids == registered_ids);

    // Every registered panel resolves an in-context help topic (the "help opens in-context" DoD).
    for (const std::string& id : registered_ids)
    {
        CHECK(help::contextual_help(id).has_value());
    }

    HELP_TEST_MAIN_END();
}
