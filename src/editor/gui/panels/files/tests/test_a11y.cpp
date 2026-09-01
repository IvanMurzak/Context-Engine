// The Files panel's per-panel a11y scan + keyboard-only navigation assertion (R-A11Y-001 /
// R-EDIT-001), headless on the default matrix (no CEF). Mirrors
// gui/panels/scenetree/tests/test_a11y.cpp. Asserts EVERY node the panel renders has a keyboard path
// and no accessibility violation, across empty / populated / nested / focused states.

#include "context/editor/gui/panels/files/files_model.h"
#include "context/editor/gui/panels/files/files_panel.h"
#include "context/editor/gui/uitree/panel.h"

#include "files_test.h"

#include <cstddef>
#include <string>

using namespace context::editor::gui::panels::files;
namespace uitree = context::editor::gui::uitree;

namespace
{

// Total node count in the view model (each node becomes exactly one focusable treeitem).
[[nodiscard]] std::size_t node_count(const FileNode& node)
{
    std::size_t n = 1;
    for (const FileNode& child : node.children)
    {
        n += node_count(child);
    }
    return n;
}

[[nodiscard]] std::size_t node_count(const FilesModel& model)
{
    std::size_t n = 0;
    for (const FileNode& root : model.roots)
    {
        n += node_count(root);
    }
    return n;
}

void assert_a11y_clean(const FilesModel& model)
{
    FilesPanel panel;
    panel.set_model(model);
    const uitree::Panel ui = panel.build_panel();

    CHECK(uitree::audit_a11y(ui).empty());
    // Keyboard-only navigation reaches every rendered node, files AND directories alike
    // (R-A11Y-001 complete keyboard nav) — only FILE rows also carry the select command.
    CHECK(uitree::focus_order(ui).size() == node_count(model));
}

} // namespace

int main()
{
    // Empty project: an a11y-clean panel with no dangling command and no focusable rows.
    {
        FilesModel model;
        assert_a11y_clean(model);
    }

    // A nested project: a directory holding a meta'd asset + a plain file, plus a top-level file.
    {
        FilesModel model;
        model.file_count = 3;

        FileNode dir;
        dir.identity = "textures";
        dir.display_name = "textures";
        dir.kind = FileNodeKind::directory;

        FileNode asset;
        asset.identity = "textures/wall.tex.json";
        asset.display_name = "wall.tex.json";
        asset.kind = FileNodeKind::file;
        asset.guid = "abc123";
        asset.asset_kind = "ctx:texture";
        dir.children.push_back(asset);

        FileNode plain;
        plain.identity = "textures/notes.txt";
        plain.display_name = "notes.txt";
        plain.kind = FileNodeKind::file;
        dir.children.push_back(plain);

        FileNode readme;
        readme.identity = "README.md";
        readme.display_name = "README.md";
        readme.kind = FileNodeKind::file;

        model.roots.push_back(dir);
        model.roots.push_back(readme);

        CHECK(find_node(model, "textures/wall.tex.json") != nullptr);
        CHECK(find_node(model, "textures/wall.tex.json")->asset_kind == "ctx:texture");

        assert_a11y_clean(model);

        // The kind annotation is visible in the rendered surface (row_label).
        FilesPanel panel;
        panel.set_model(model);
        const std::string html = uitree::render_html(panel.build_panel());
        CHECK(html.find("(folder)") != std::string::npos);
        CHECK(html.find("(ctx:texture)") != std::string::npos);
    }

    // A directory-only project (no candidate file anywhere): still a11y-clean — no orphan command.
    {
        FilesModel model;
        model.file_count = 0;
        FileNode empty_dir;
        empty_dir.identity = "empty";
        empty_dir.display_name = "empty";
        empty_dir.kind = FileNodeKind::directory;
        model.roots.push_back(empty_dir);
        assert_a11y_clean(model);
    }

    // The c1/D3 focus state renders cleanly too (no new node, just a status-line annotation).
    {
        FilesModel model;
        FilesPanel panel;
        panel.set_model(model);
        panel.set_focused(true);
        CHECK(uitree::audit_a11y(panel.build_panel()).empty());
    }

    FILES_TEST_MAIN_END();
}
