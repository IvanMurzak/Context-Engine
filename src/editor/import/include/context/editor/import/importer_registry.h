// The importer registry (R-ASSET-001 / R-PKG-*). Routes a source path to the importer that claims
// its extension. First-party importers register here; package-supplied importers join the same
// registry when the package system lands (same contract, same isolation).

#pragma once

#include "context/editor/import/importer.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::import
{

// The result of a registration attempt — collision is reported, never silently last-writer-wins (two
// importers claiming one extension is an authoring error the package loader must surface, R-CLI-007).
struct RegisterResult
{
    bool ok = false;
    std::string conflict_extension; // the extension already claimed (when !ok)
    std::string conflict_importer;  // the importer id that already claims it (when !ok)
};

class ImporterRegistry
{
public:
    // Register `importer`, claiming each of its extensions. Fails (registering nothing) if ANY of its
    // extensions is already claimed — atomic, so a collision leaves the registry unchanged. The
    // collision result MUST be surfaced (a dropped collision is an authoring error, R-CLI-007), so
    // ignoring it is a defect — hence [[nodiscard]] (matches every other query on this class).
    [[nodiscard]] RegisterResult register_importer(std::unique_ptr<Importer> importer);

    // The importer for a source path (routed by lowercased extension), or nullptr when none claims
    // it. `.gltf`/`.glb` both route to the glTF importer via its extension list.
    [[nodiscard]] const Importer* importer_for_path(std::string_view source_path) const;

    // The importer that claims `ext` (lowercase, with dot), or nullptr.
    [[nodiscard]] const Importer* importer_for_extension(std::string_view ext) const;

    // Every registered importer, in registration order (deterministic iteration for tooling/tests).
    [[nodiscard]] std::vector<const Importer*> importers() const;

    [[nodiscard]] std::size_t size() const noexcept { return importers_.size(); }

private:
    std::vector<std::unique_ptr<Importer>> importers_;
    std::vector<std::pair<std::string, const Importer*>> by_extension_; // ext -> importer (order-kept)
};

// The first-party importer set for v1: png + wav + gltf, registered in a fixed order. This is the
// registry EditorKernel boots with before the package system contributes more.
[[nodiscard]] ImporterRegistry default_importer_registry();

} // namespace context::editor::import
