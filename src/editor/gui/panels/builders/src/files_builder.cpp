// File-tree view-model builder: the project's flat, asset-candidate-filtered path list -> a nested
// hierarchy by path segment (see files_builder.h). Mirrors scene_tree_builder.cpp's TreeBuilder
// shape, substituting path segments for L-35 id-path segments.

#include "context/editor/gui/panels/builders/files_builder.h"

#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::gui::panels::builders
{

namespace
{

using files::FileNode;
using files::FileNodeKind;
using files::FilesModel;

// Split a normalized ('/'-separated) project-relative path into its segments. Never returns an
// empty segment: files_builder is fed paths from filesync::FileStore::list / assetdb, which never
// emit a leading, trailing, or doubled '/'.
[[nodiscard]] std::vector<std::string> split_path(std::string_view path)
{
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        if (end > start)
        {
            segments.emplace_back(path.substr(start, end - start));
        }
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return segments;
}

[[nodiscard]] std::string join_path(const std::vector<std::string>& segments)
{
    std::string out;
    for (std::size_t i = 0; i < segments.size(); ++i)
    {
        if (i != 0)
        {
            out += '/';
        }
        out += segments[i];
    }
    return out;
}

// A mutable build node with a STABLE address (stored in a std::deque) so parent nodes can hold child
// pointers while the tree is still growing — the SAME shape scene_tree_builder.cpp's BuildNode uses.
struct BuildNode
{
    std::vector<std::string> path;
    bool is_file = false; // false => a directory (a synthetic grouping node, or one with no file yet)
    std::string guid;
    std::string asset_kind;
    std::vector<BuildNode*> children; // ordered by first appearance
};

class TreeBuilder
{
public:
    // Ensure a node exists for `path` (creating every missing ancestor as a directory) and return
    // it. Ancestors are created first, so parents always precede children.
    BuildNode* ensure(const std::vector<std::string>& path)
    {
        const std::string key = join_path(path);
        auto it = index_.find(key);
        if (it != index_.end())
        {
            return it->second;
        }

        nodes_.push_back(BuildNode{path, false, "", "", {}});
        BuildNode* node = &nodes_.back();
        index_.emplace(key, node);

        if (path.size() <= 1)
        {
            roots_.push_back(node);
        }
        else
        {
            std::vector<std::string> parent_path(path.begin(), path.end() - 1);
            ensure(parent_path)->children.push_back(node);
        }
        return node;
    }

    [[nodiscard]] const std::vector<BuildNode*>& roots() const noexcept { return roots_; }

private:
    std::deque<BuildNode> nodes_; // stable addresses
    std::map<std::string, BuildNode*> index_;
    std::vector<BuildNode*> roots_;
};

[[nodiscard]] FileNode finalize(const BuildNode& b)
{
    FileNode node;
    node.identity = join_path(b.path);
    node.display_name = b.path.empty() ? std::string() : b.path.back();
    node.kind = b.is_file ? FileNodeKind::file : FileNodeKind::directory;
    if (b.is_file)
    {
        node.guid = b.guid;
        node.asset_kind = b.asset_kind;
    }
    for (const BuildNode* child : b.children)
    {
        node.children.push_back(finalize(*child));
    }
    return node;
}

} // namespace

FilesModel build_files_model(const std::vector<std::string>& paths, const assetdb::AssetDatabase& db)
{
    FilesModel model;

    TreeBuilder builder;
    for (const std::string& path : paths)
    {
        if (!assetdb::is_asset_candidate(path))
        {
            continue; // a sidecar, atomic-write residue, or a dot-segment tool-internal path
        }
        const std::vector<std::string> segments = split_path(path);
        if (segments.empty())
        {
            continue; // defensive; is_asset_candidate already refuses the paths that would land here
        }
        BuildNode* file_node = builder.ensure(segments);
        file_node->is_file = true;
        if (const assetdb::AssetRecord* record = db.find_by_path(path))
        {
            file_node->guid = record->guid;
            file_node->asset_kind = record->kind;
        }
        ++model.file_count;
    }

    for (const BuildNode* root : builder.roots())
    {
        model.roots.push_back(finalize(*root));
    }
    return model;
}

} // namespace context::editor::gui::panels::builders
