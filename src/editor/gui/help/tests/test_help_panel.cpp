// The Help / getting-started panel renders the getting-started samples + per-panel help, and its body
// carries the LIVE, generated verb summaries (so the in-context help is the contract, not a copy).

#include "context/editor/gui/help/help_model.h"
#include "context/editor/gui/help/help_panel.h"
#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"

#include "help_test.h"

#include <optional>
#include <string>
#include <vector>

using namespace context::editor::gui::help;
namespace uitree = context::editor::gui::uitree;

int main()
{
    HelpPanel panel;
    const uitree::Panel ui = panel.build_panel();

    // Identity + structure.
    CHECK(ui.id() == "help");
    CHECK(ui.title() == "Help");
    CHECK(ui.has_root());
    CHECK(ui.has_command("help.open-sample"));
    CHECK(ui.has_command("help.open-topic"));

    const std::string html = uitree::render_html(ui);

    // Getting-started section references every R-QA-006 human-onboarding sample by title + path.
    CHECK(html.find("Getting started") != std::string::npos);
    for (const SampleRef& s : getting_started_samples())
    {
        CHECK(html.find(s.title) != std::string::npos);
        CHECK(html.find(s.path) != std::string::npos);
    }

    // Panel-help section names every shipped panel topic.
    CHECK(html.find("Panel help") != std::string::npos);
    for (const PanelHelp& t : panel_topics())
    {
        CHECK(html.find(t.title) != std::string::npos);
    }

    // The in-context help body carries LIVE verb summaries generated from the registry — assert the
    // rendered HTML contains the actual `context describe` summary the contract emits (not a copy).
    const std::optional<VerbHelp> describe = verb_help("context describe");
    CHECK(describe.has_value());
    CHECK(html.find(describe->summary) != std::string::npos);

    // Deterministic: identical registry + corpus state renders byte-identically (stable re-render).
    HelpPanel panel2;
    CHECK(uitree::render_html(panel2.build_panel()) == html);

    HELP_TEST_MAIN_END();
}
