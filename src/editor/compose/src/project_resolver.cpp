// The disk-backed composed-write resolver — see project_resolver.h. Extracted verbatim from the CLI
// `context set` command (src/cli/set_command.cpp) so the CLI and the GUI viewport override editor
// resolve project scenes identically before driving the shared compose::plan_write.

#include "context/editor/compose/project_resolver.h"

#include "context/editor/filesync/path_jail.h"
#include "context/editor/serializer/canonical.h"

#include <optional>
#include <string>
#include <utility>

namespace context::editor::compose
{

ProjectSceneResolver::ProjectSceneResolver(std::filesystem::path root) : store_(std::move(root)) {}

const SceneDoc* ProjectSceneResolver::resolve(std::string_view path) const
{
    const Entry& e = load(std::string(path));
    return e.has_doc ? &e.doc : nullptr;
}

const serializer::JsonValue* ProjectSceneResolver::tree(std::string_view path) const
{
    const Entry& e = load(std::string(path));
    return e.has_tree ? &e.tree : nullptr;
}

const ProjectSceneResolver::Entry& ProjectSceneResolver::load(const std::string& path) const
{
    if (const auto it = cache_.find(path); it != cache_.end())
    {
        return it->second;
    }
    Entry entry;
    // The path must stay within the project root (R-SEC-008); an escaping instance/scene path
    // resolves to nothing (the write path then reports write_target_not_found / file.not_found).
    if (filesync::is_inside_jail(".", path))
    {
        if (const std::optional<std::string> bytes = store_.read(path))
        {
            serializer::CanonicalizeResult canonical = serializer::canonicalize(*bytes);
            if (canonical.is_json)
            {
                if (std::optional<SceneDoc> doc = build_scene_doc(path, canonical.root))
                {
                    entry.has_doc = true;
                    entry.doc = std::move(*doc);
                }
                entry.has_tree = true;
                entry.tree = std::move(canonical.root);
            }
        }
    }
    const auto [inserted, _] = cache_.emplace(path, std::move(entry));
    return inserted->second;
}

} // namespace context::editor::compose
