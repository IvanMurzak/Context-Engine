// The built-in editor-panel registry (see registry.h). Append-only: a new M5 panel adds one entry
// here and one line to coverage.manifest.jsonl.

#include "context/editor/gui/a11y/registry.h"

#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/gui/uitree/panel.h"

namespace context::editor::gui::a11y
{

std::vector<RegisteredPanel> registered_panels()
{
    std::vector<RegisteredPanel> panels;

    // M5-F0b — the built-in placeholder panel the CEF editor host boots (gui/uitree/builtin.h).
    panels.push_back(RegisteredPanel{"placeholder", &uitree::make_placeholder_panel});

    // --- M5 fan-out panels append their RegisteredPanel BELOW (one entry each). Keep in lockstep
    //     with coverage.manifest.jsonl — tools/a11y_scan.py cross-checks the two on every PR. ---

    return panels;
}

} // namespace context::editor::gui::a11y
