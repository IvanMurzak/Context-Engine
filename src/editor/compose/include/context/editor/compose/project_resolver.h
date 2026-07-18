// A disk-backed composed-write resolver (L-35 / R-CLI-006): lazily reads + canonicalizes +
// composition-parses project scene files under a project root on demand, caching the raw parsed tree
// (for the write splice) and the composition view (for the id-path walk). This is the ONE resolver
// shared by the CLI `context set` / `context query --overrides` path (src/cli/set_command.cpp) AND
// the GUI in-context viewport override-editing path (src/editor/gui/viewport/, R-HUX-006), so both
// drive the SAME compose::plan_write over the SAME on-disk read — never a parallel write path.
//
// The R-SEC-008 jail is enforced lexically here (a scene / instance path escaping the project root
// simply does not resolve); the native FileStore re-enforces it TOCTOU-safely on the actual write.
// The cache is snapshot-per-instance: after a write to disk, construct a FRESH resolver to observe
// the new bytes (the CLI + the GUI both make a resolver per operation).

#pragma once

#include "context/editor/compose/compose_write.h" // WriteResolver
#include "context/editor/compose/scene_model.h"    // SceneDoc
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/serializer/json_tree.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace context::editor::compose
{

// The lazy project scene resolver behind `context set`. Reads + canonicalizes + composition-parses
// scene files under `root` on demand, caching the raw parsed tree and the composition view.
class ProjectSceneResolver final : public WriteResolver
{
public:
    explicit ProjectSceneResolver(std::filesystem::path root);

    [[nodiscard]] const SceneDoc* resolve(std::string_view path) const override;
    [[nodiscard]] const serializer::JsonValue* tree(std::string_view path) const override;

private:
    struct Entry
    {
        bool has_tree = false;
        serializer::JsonValue tree;
        bool has_doc = false;
        SceneDoc doc;
    };

    [[nodiscard]] const Entry& load(const std::string& path) const;

    filesync::NativeFileStore store_;
    mutable std::map<std::string, Entry, std::less<>> cache_;
};

} // namespace context::editor::compose
