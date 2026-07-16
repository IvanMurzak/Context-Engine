// Per-target toolchain manifest (see toolchain_manifest.h).

#include "context/editor/build/toolchain_manifest.h"

namespace context::editor::build
{

const std::vector<ToolchainEntry>& toolchain_manifest()
{
    // Mirrors cmake/toolchain-versions.json § targets (L-42). Deterministic order (the manifest's key
    // order) so it is diff-stable + test-pinnable. The drift-guard test asserts this stays 1:1 with the
    // JSON's target keys + compiler ids.
    static const std::vector<ToolchainEntry> manifest = {
        {"linux-x86_64", "clang", "20.1", "strict"},
        {"macos-arm64", "apple-clang", "21.0", "advisory"},
        {"windows-x86_64", "msvc", "", "documented"},
        {"web-emscripten", "emscripten-clang", "", "documented"},
    };
    return manifest;
}

std::string toolchain_target_key(std::string_view build_target)
{
    // The CLI --target ids (import::platform_profiles ids) → the manifest triple keys. Kept here (not a
    // shared header) so the build module stays dependency-light; the drift guard pins both frozen sets.
    if (build_target == "linux")
        return "linux-x86_64";
    if (build_target == "macos")
        return "macos-arm64";
    if (build_target == "windows")
        return "windows-x86_64";
    if (build_target == "web")
        return "web-emscripten";
    return std::string();
}

const ToolchainEntry* resolve_toolchain(const std::vector<ToolchainEntry>& manifest,
                                        std::string_view build_target)
{
    const std::string key = toolchain_target_key(build_target);
    if (key.empty())
        return nullptr;
    for (const ToolchainEntry& entry : manifest)
        if (entry.target == key)
            return &entry;
    return nullptr;
}

} // namespace context::editor::build
