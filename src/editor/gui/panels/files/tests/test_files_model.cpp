// Files view-model tests (M9 e1): build_files_model's asset-candidate filtering (sidecars +
// tool-internal paths never become rows), the directory/file nesting by path segment, the
// guid/assetKind population from a live AssetDatabase index, the "unknown = not enforced" state for
// an un-meta'd file, and find_node.

#include "context/editor/gui/panels/files/files_model.h"

#include "context/editor/gui/panels/builders/files_builder.h"

#include "context/editor/assetdb/asset_database.h"
#include "context/editor/filesync/file_store.h"

#include "files_test.h"

#include <string>
#include <string_view>
#include <vector>

using namespace context::editor::gui::panels::files;
using context::editor::gui::panels::builders::build_files_model;
namespace assetdb = context::editor::assetdb;
namespace filesync = context::editor::filesync;

namespace
{

void put_asset(filesync::FileStore& fs, std::string_view path, std::string_view guid,
               std::string_view kind)
{
    fs.write(path, "content");
    assetdb::AssetMeta meta;
    meta.guid = std::string(guid);
    meta.kind = std::string(kind);
    fs.write(assetdb::meta_path_for(path), assetdb::serialize_meta(meta));
}

constexpr std::string_view kGuidTexture = "00000000000000000000000000000aaa";

} // namespace

int main()
{
    // --- find_node over a hand-built forest -------------------------------------------------------
    {
        FilesModel model;
        FileNode dir;
        dir.identity = "textures";
        dir.kind = FileNodeKind::directory;
        FileNode leaf;
        leaf.identity = "textures/wall.png";
        leaf.kind = FileNodeKind::file;
        dir.children.push_back(leaf);
        model.roots.push_back(dir);

        CHECK(find_node(model, "textures") != nullptr);
        CHECK(find_node(model, "textures/wall.png") != nullptr);
        CHECK(find_node(model, "textures/wall.png")->kind == FileNodeKind::file);
        CHECK(find_node(model, "does-not-exist") == nullptr);
    }

    // --- build_files_model: candidate filtering + nesting + the live guid/kind index ---------------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/textures/wall.tex.json", kGuidTexture, "ctx:texture");
        fs.write("proj/README.md", "hello"); // a plain file with no meta sidecar yet
        fs.write("proj/.editor/index", "internal"); // tool-internal — must never surface

        assetdb::SequenceGuidGenerator guids;
        assetdb::AssetDatabase db(guids);
        CHECK(db.scan(fs, "proj").assets_indexed == 1);

        // The RAW listing, including the sidecar and the dot-tree file — the same unfiltered input
        // kernel_server.cpp hands the builder in production.
        const std::vector<std::string> paths = fs.list("proj");
        const FilesModel model = build_files_model(paths, db);

        CHECK(model.file_count == 2); // wall.tex.json + README.md; the sidecar/.editor entry excluded

        const FileNode* texdir = find_node(model, "proj/textures");
        CHECK(texdir != nullptr);
        CHECK(texdir->kind == FileNodeKind::directory);
        CHECK(texdir->guid.empty()); // a directory carries no identity of its own

        const FileNode* tex = find_node(model, "proj/textures/wall.tex.json");
        CHECK(tex != nullptr);
        CHECK(tex->kind == FileNodeKind::file);
        CHECK(tex->guid == kGuidTexture);
        CHECK(tex->asset_kind == "ctx:texture");
        CHECK(tex->display_name == "wall.tex.json");

        const FileNode* readme = find_node(model, "proj/README.md");
        CHECK(readme != nullptr);
        CHECK(readme->kind == FileNodeKind::file);
        CHECK(readme->guid.empty());     // no meta sidecar yet — unknown, not a defect
        CHECK(readme->asset_kind.empty());

        // The sidecar itself, and the dot-tree file, never became rows anywhere in the tree.
        CHECK(find_node(model, "proj/textures/wall.tex.json.meta.json") == nullptr);
        CHECK(find_node(model, "proj/.editor") == nullptr);
        CHECK(find_node(model, "proj/.editor/index") == nullptr);
    }

    FILES_TEST_MAIN_END();
}
