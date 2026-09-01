// Files panel tests (M9 e1): the DAEMON-BACKED selection (write requests out through the
// SelectionGateway, rendered selection in through `apply_selection`), directory rows being
// unselectable, the c1/D3 focus render, deterministic re-render, and a11y/keyboard reachability.
//
// ⚠ THE GATEWAY DOUBLE IS DELIBERATELY NO MORE CAPABLE THAN THE DAEMON (mirrors
// test_scene_tree_panel.cpp) — it answers with the selection the daemon would then hold and
// `nullopt` when it refuses; it never reaches into the panel.

#include "context/editor/gui/panels/files/files_panel.h"

#include "context/editor/gui/panels/files/files_model.h"
#include "context/editor/gui/uitree/panel.h"

#include "context/editor/bridge/event_stream.h"

#include "files_test.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::files;
namespace bridge = context::editor::bridge;
namespace uitree = context::editor::gui::uitree;

namespace
{

// A root directory with two files, plus one top-level file.
[[nodiscard]] FilesModel standard_model()
{
    FilesModel model;
    model.ok = true;
    model.file_count = 3;

    FileNode dir;
    dir.identity = "textures";
    dir.display_name = "textures";
    dir.kind = FileNodeKind::directory;

    FileNode wall;
    wall.identity = "textures/wall.tex.json";
    wall.display_name = "wall.tex.json";
    wall.kind = FileNodeKind::file;
    wall.asset_kind = "ctx:texture";
    dir.children.push_back(wall);

    FileNode floor_tex;
    floor_tex.identity = "textures/floor.tex.json";
    floor_tex.display_name = "floor.tex.json";
    floor_tex.kind = FileNodeKind::file;
    dir.children.push_back(floor_tex);

    FileNode readme;
    readme.identity = "README.md";
    readme.display_name = "README.md";
    readme.kind = FileNodeKind::file;

    model.roots.push_back(dir);
    model.roots.push_back(readme);
    return model;
}

class RecordingGateway final : public SelectionGateway
{
public:
    std::vector<std::vector<std::string>> requests;
    std::vector<std::string> selection;
    bool refuse = false;

    std::optional<std::vector<std::string>>
    request_selection(const std::vector<std::string>& ids) override
    {
        requests.push_back(ids);
        if (refuse)
        {
            return std::nullopt;
        }
        selection = ids;
        return selection;
    }
};

[[nodiscard]] std::size_t count(const std::string& haystack, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
    {
        ++n;
    }
    return n;
}

} // namespace

int main()
{
    CHECK(std::string(FilesPanel::kContributionId) == "builtin.files");

    // --- select WRITES, then renders the DAEMON'S ANSWER -----------------------------------------
    {
        RecordingGateway gateway;
        FilesPanel panel(&gateway);
        panel.set_model(standard_model());

        FileSelection last;
        int notifications = 0;
        panel.add_selection_listener(
            [&](const FileSelection& s)
            {
                last = s;
                ++notifications;
            });

        CHECK(panel.select("textures/wall.tex.json"));
        CHECK(gateway.requests.size() == 1);
        CHECK(gateway.requests[0] == std::vector<std::string>{"textures/wall.tex.json"});
        CHECK(notifications == 1);
        CHECK(last.identity == "textures/wall.tex.json");
        CHECK(panel.selection().identity == "textures/wall.tex.json");

        // Re-selecting the same row: no churn.
        CHECK(panel.select("textures/wall.tex.json"));
        CHECK(gateway.requests.size() == 2);
        CHECK(notifications == 1);

        // A DIRECTORY row is not selectable — refused locally, never sent to the daemon.
        CHECK(!panel.select("textures"));
        CHECK(gateway.requests.size() == 2);

        // An unknown identity is a dead click.
        CHECK(!panel.select("ghost"));
        CHECK(gateway.requests.size() == 2);

        // A daemon REFUSAL leaves the rendered selection exactly where it was.
        gateway.refuse = true;
        CHECK(!panel.select("README.md"));
        CHECK(panel.selection().identity == "textures/wall.tex.json");
        CHECK(notifications == 1);

        // Clearing is a write too.
        gateway.refuse = false;
        CHECK(panel.clear_selection());
        CHECK(gateway.requests.back().empty());
        CHECK(notifications == 2);
        CHECK(last.identity.empty());
    }

    // --- with NO gateway the panel cannot change a selection it does not own ----------------------
    {
        FilesPanel panel; // the a11y harness's default-constructed shape
        panel.set_model(standard_model());
        int notifications = 0;
        panel.add_selection_listener([&](const FileSelection&) { ++notifications; });

        CHECK(!panel.select("README.md"));
        CHECK(!panel.clear_selection());
        CHECK(panel.selection().identity.empty());
        CHECK(notifications == 0);

        // ...but it still RENDERS whatever the daemon says.
        CHECK(panel.apply_selection({"README.md"}));
        CHECK(panel.selection().identity == "README.md");
        CHECK(notifications == 1);
    }

    // --- the daemon's multi-id selection renders its FIRST id (single-select panel) ---------------
    {
        FilesPanel panel;
        panel.set_model(standard_model());
        CHECK(panel.apply_selection({"README.md", "textures/wall.tex.json"}));
        CHECK(panel.selection().identity == "README.md");

        // An id with no row here is still adopted — a path is its own stable identity, no hash to
        // re-resolve; it simply renders unmarked until a model containing it arrives.
        CHECK(panel.apply_selection({"not-in-this-view"}));
        CHECK(panel.selection().identity == "not-in-this-view");
        CHECK(uitree::render_html(panel.build_panel()).find("(selected)") == std::string::npos);

        CHECK(!panel.apply_selection({"not-in-this-view"})); // restated -> no churn
    }

    // --- c1/D3: focus is rendered in the status line ------------------------------------------------
    {
        FilesPanel panel;
        panel.set_model(standard_model());
        CHECK(!panel.focused());
        CHECK(uitree::render_html(panel.build_panel()).find("focused") == std::string::npos);

        panel.set_focused(true);
        CHECK(panel.focused());
        CHECK(uitree::render_html(panel.build_panel()).find("focused") != std::string::npos);

        panel.set_focused(false);
        CHECK(!panel.focused());
    }

    // --- build_panel is a11y-conformant + keyboard-reachable; directories carry no command ----------
    {
        FilesPanel panel;
        panel.set_model(standard_model());
        const uitree::Panel ui = panel.build_panel();

        CHECK(uitree::audit_a11y(ui).empty());
        const std::vector<std::string> order = uitree::focus_order(ui);
        CHECK(order.size() == 4); // textures (dir) + 2 files under it + README.md
        CHECK(ui.has_command(kSelectCommand));
    }

    // --- selection is visible in the rendered tree; directory rows never carry "(selected)" ---------
    {
        FilesPanel panel;
        panel.set_model(standard_model());
        CHECK(panel.apply_selection({"textures/wall.tex.json"}));
        const std::string html = uitree::render_html(panel.build_panel());
        CHECK(count(html, "(selected)") == 1);
        CHECK(html.find("role=\"tree\"") != std::string::npos);
    }

    // --- deterministic (stable) re-render ----------------------------------------------------------
    {
        FilesPanel panel;
        panel.set_model(standard_model());
        CHECK(panel.apply_selection({"README.md"}));
        const std::string first = uitree::render_html(panel.build_panel());
        const std::string second = uitree::render_html(panel.build_panel());
        CHECK(first == second);
    }

    // --- R-BRIDGE-008: derivation.settled advances generation + records stability -------------------
    {
        FilesPanel panel;
        panel.set_model(standard_model());
        CHECK(panel.generation() == 0);
        CHECK(panel.stability() == bridge::Stability::stable);

        panel.on_derivation_settled(7, bridge::Stability::settling);
        CHECK(panel.generation() == 7);
        CHECK(panel.stability() == bridge::Stability::settling);
        const std::string settling = uitree::render_html(panel.build_panel());
        CHECK(settling.find("settling") != std::string::npos);
        CHECK(settling.find("generation 7") != std::string::npos);
    }

    // --- an empty model renders an a11y-clean panel with no exposed command -------------------------
    {
        FilesPanel panel; // no model set
        const uitree::Panel ui = panel.build_panel();
        CHECK(uitree::audit_a11y(ui).empty());
        CHECK(!ui.has_command(kSelectCommand));
        CHECK(uitree::focus_order(ui).empty());
    }

    // --- a directory-only tree exposes no command (nothing to bind it to) ---------------------------
    {
        FilesModel model;
        model.file_count = 0;
        FileNode dir;
        dir.identity = "empty";
        dir.display_name = "empty";
        dir.kind = FileNodeKind::directory;
        model.roots.push_back(dir);

        FilesPanel panel;
        panel.set_model(model);
        const uitree::Panel ui = panel.build_panel();
        CHECK(uitree::audit_a11y(ui).empty());        // no orphan/unreachable command
        CHECK(!ui.has_command(kSelectCommand));
        CHECK(uitree::focus_order(ui).size() == 1);    // the directory row is still a focus stop
    }

    FILES_TEST_MAIN_END();
}
