// Scene-tree view-model builder: flat composed entities -> nested hierarchy by L-35 id-path.
// Moved VERBATIM from context_gui_panel_scenetree (M9 e05d3) so the panel library stays
// boundary-clean; the weaving logic itself is unchanged.

#include "context/editor/gui/panels/builders/scene_tree_builder.h"

#include "context/editor/schema/json_access.h" // schema::find_member — the shared JSON-tree accessor
#include "context/editor/serializer/json_tree.h"

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace context::editor::gui::panels::builders
{

namespace
{

namespace serializer = context::editor::serializer;

using scenetree::NodeKind;
using scenetree::SceneTreeModel;
using scenetree::SceneTreeNode;

// The value of a string member `key` on an object JsonValue, or nullptr when absent / not a string
// (schema::find_member plus the string-type filter).
[[nodiscard]] const std::string* string_member(const serializer::JsonValue& value, const char* key)
{
    const serializer::JsonValue* m = schema::find_member(value, key);
    return (m != nullptr && m->type == serializer::JsonValue::Type::string) ? &m->string_value
                                                                            : nullptr;
}

// True iff any pointer of `entity` was touched by an L-35 override contributor (so it is visibly
// overridden in the tree). A pointer with only a template contributor is not an override.
[[nodiscard]] bool has_override_contributor(const compose::ComposedEntity& entity)
{
    for (const compose::FieldProvenance& fp : entity.field_provenance)
    {
        for (const compose::ProvenanceEntry& pe : fp.chain)
        {
            if (pe.source == compose::ProvenanceEntry::Source::override_value)
            {
                return true;
            }
        }
    }
    return false;
}

// A mutable build node with a STABLE address (stored in a std::deque) so parent nodes can hold child
// pointers while the tree is still growing. Converted to the value-based SceneTreeNode at the end.
struct BuildNode
{
    std::vector<std::string> path;
    const compose::ComposedEntity* entity = nullptr; // nullptr => synthetic instance boundary
    std::vector<BuildNode*> children;                // ordered by first appearance
};

class TreeBuilder
{
public:
    // Ensure a node exists for `path` (creating every missing ancestor as a synthetic boundary) and
    // return it. Ancestors are created first, so parents always precede children.
    BuildNode* ensure(const std::vector<std::string>& path)
    {
        const std::string key = join_identity(path);
        auto it = index_.find(key);
        if (it != index_.end())
        {
            return it->second;
        }

        nodes_.push_back(BuildNode{path, nullptr, {}});
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

[[nodiscard]] SceneTreeNode finalize(const BuildNode& b)
{
    SceneTreeNode node;
    node.identity = join_identity(b.path);
    if (b.entity != nullptr)
    {
        node.kind = NodeKind::entity;
        node.identity_hash = b.entity->identity_hash;
        node.overridden = has_override_contributor(*b.entity);
        const std::string* name = string_member(b.entity->value, "name");
        node.display_name = (name != nullptr && !name->empty())
                                ? *name
                                : (b.path.empty() ? std::string() : b.path.back());
    }
    else
    {
        node.kind = NodeKind::instance;
        node.display_name = b.path.empty() ? std::string() : b.path.back();
    }
    for (const BuildNode* child : b.children)
    {
        node.children.push_back(finalize(*child));
    }
    return node;
}

} // namespace

std::string join_identity(const std::vector<std::string>& id_path)
{
    std::string out;
    for (std::size_t i = 0; i < id_path.size(); ++i)
    {
        if (i != 0)
        {
            out += '/';
        }
        out += id_path[i];
    }
    return out;
}

SceneTreeModel build_scene_tree(const compose::ComposedScene& scene)
{
    SceneTreeModel model;
    model.root_scene = scene.root_path;
    model.ok = scene.ok;
    model.entity_count = scene.entities.size();

    TreeBuilder builder;
    for (const compose::ComposedEntity& entity : scene.entities)
    {
        if (entity.id_path.empty())
        {
            continue; // an entity with no id-path cannot be placed (defensive; flatten never emits one)
        }
        builder.ensure(entity.id_path)->entity = &entity;
    }

    for (const BuildNode* root : builder.roots())
    {
        model.roots.push_back(finalize(*root));
    }
    return model;
}

} // namespace context::editor::gui::panels::builders
