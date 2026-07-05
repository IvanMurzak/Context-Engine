// The importer registry — extension routing + collision-refusing registration.

#include "context/editor/import/importer_registry.h"

#include "context/editor/import/importers/gltf_importer.h"
#include "context/editor/import/importers/png_importer.h"
#include "context/editor/import/importers/wav_importer.h"

#include <string>

namespace context::editor::import
{
namespace
{
void ascii_lower(std::string& s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
}

// The lowercase extension of a source path (with the dot), or "" when it has none. A leading-dot
// basename ("./.gitignore" -> ".gitignore") is treated as extensionless (a dotfile, not a type).
std::string extension_of(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t start = (slash == std::string_view::npos) ? 0 : slash + 1;
    const std::string_view name = path.substr(start);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    std::string ext(name.substr(dot));
    ascii_lower(ext);
    return ext;
}
} // namespace

RegisterResult ImporterRegistry::register_importer(std::unique_ptr<Importer> importer)
{
    RegisterResult result;
    if (importer == nullptr)
        return result; // ok=false — registering nothing

    std::vector<std::string> exts = importer->extensions();
    for (std::string& e : exts)
        ascii_lower(e);

    // Atomic: refuse if ANY extension is already claimed, leaving the registry unchanged.
    for (const std::string& e : exts)
    {
        for (const auto& [claimed, owner] : by_extension_)
        {
            if (claimed == e)
            {
                result.ok = false;
                result.conflict_extension = e;
                result.conflict_importer = std::string(owner->id());
                return result;
            }
        }
    }

    const Importer* raw = importer.get();
    importers_.push_back(std::move(importer));
    for (const std::string& e : exts)
        by_extension_.emplace_back(e, raw);
    result.ok = true;
    return result;
}

const Importer* ImporterRegistry::importer_for_path(std::string_view source_path) const
{
    const std::string ext = extension_of(source_path);
    if (ext.empty())
        return nullptr;
    return importer_for_extension(ext);
}

const Importer* ImporterRegistry::importer_for_extension(std::string_view ext) const
{
    std::string needle(ext);
    ascii_lower(needle);
    for (const auto& [claimed, owner] : by_extension_)
        if (claimed == needle)
            return owner;
    return nullptr;
}

std::vector<const Importer*> ImporterRegistry::importers() const
{
    std::vector<const Importer*> out;
    out.reserve(importers_.size());
    for (const std::unique_ptr<Importer>& importer : importers_)
        out.push_back(importer.get());
    return out;
}

ImporterRegistry default_importer_registry()
{
    ImporterRegistry registry;
    // Fixed registration order (deterministic tooling iteration). Extensions are disjoint, so none of
    // these collide; a package-supplied importer that later claims a taken extension is refused.
    registry.register_importer(std::make_unique<PngImporter>());
    registry.register_importer(std::make_unique<WavImporter>());
    registry.register_importer(std::make_unique<GltfImporter>());
    return registry;
}

} // namespace context::editor::import
