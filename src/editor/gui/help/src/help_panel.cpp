// Help / getting-started panel: projects the help model into a headless, a11y-conformant uitree Panel.

#include "context/editor/gui/help/help_panel.h"

#include "context/editor/gui/help/help_model.h"
#include "context/editor/gui/uitree/node.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::help
{

namespace
{

// Keyboard-path commands for the two lists (R-CLI-001 CLI-completeness as structural accessibility):
// a focusable list row binds one of these, and the panel exposes it so it is reachable.
constexpr const char* kOpenSampleCommand = "help.open-sample";
constexpr const char* kOpenTopicCommand = "help.open-topic";

// A grep-safe id fragment: fold spaces/colons to '-' so a CLI command ("context session step")
// yields a unique, stable node-id suffix.
[[nodiscard]] std::string id_fragment(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
    {
        out += (c == ' ' || c == ':') ? '-' : c;
    }
    return out;
}

} // namespace

uitree::Panel HelpPanel::build_panel() const
{
    using uitree::Role;
    using uitree::UiNode;

    const std::vector<SampleRef> samples = getting_started_samples();
    const std::vector<PanelHelp> topics = panel_topics();

    uitree::Panel panel("help", "Help");
    if (!samples.empty())
    {
        panel.add_command(kOpenSampleCommand, "Open sample project");
    }
    if (!topics.empty())
    {
        panel.add_command(kOpenTopicCommand, "Open panel help");
    }

    UiNode root(Role::region, "help.panel");
    root.set_label("Help");

    root.add_child(UiNode(Role::heading, "help.heading").set_label("Help").set_text("Help"));
    root.add_child(
        UiNode(Role::status, "help.status")
            .set_label("Help mode")
            .set_text("Contextual help - generated from the live contract, offline (no network)."));

    // --- Getting started: the R-QA-006 human-onboarding samples (R-HUX-010) ----------------------
    root.add_child(UiNode(Role::heading, "help.getting-started.heading")
                       .set_label("Getting started")
                       .set_text("Getting started"));
    UiNode sample_list(Role::list, "help.getting-started.list");
    sample_list.set_label("Sample projects");
    for (const SampleRef& s : samples)
    {
        UiNode item(Role::listitem, "help.sample." + s.id);
        item.set_label(s.title);
        item.set_focusable(true);
        item.set_command(kOpenSampleCommand);
        item.set_text(s.title + " (" + s.path + ") - " + s.summary);
        sample_list.add_child(std::move(item));
    }
    root.add_child(std::move(sample_list));

    // --- Panel help: per-shipped-panel contextual help, each verb summary GENERATED from the live
    //     registry so the in-context help cannot drift from `context describe` (R-CLI-013). ---------
    root.add_child(UiNode(Role::heading, "help.topics.heading")
                       .set_label("Panel help")
                       .set_text("Panel help"));
    UiNode topic_list(Role::list, "help.topics.list");
    topic_list.set_label("Editor panels");
    for (const PanelHelp& t : topics)
    {
        UiNode item(Role::listitem, "help.topic." + t.panel_id);
        item.set_label(t.title);
        item.set_focusable(true);
        item.set_command(kOpenTopicCommand);
        item.set_text(t.title + " - " + t.summary);

        // Surface the live, generated help for each related verb IN the panel body: the verb's
        // summary is read from the ONE registry (help::verb_help), so the text a human sees here is
        // exactly the introspection `context <verb> --help` emits.
        for (const std::string& command : t.related_commands)
        {
            std::string line = command;
            if (const std::optional<VerbHelp> vh = verb_help(command))
            {
                line += ": " + vh->summary;
            }
            item.add_child(UiNode(Role::text, "help.topic." + t.panel_id + "." + id_fragment(command))
                               .set_text(line));
        }
        topic_list.add_child(std::move(item));
    }
    root.add_child(std::move(topic_list));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::help
