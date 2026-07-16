// EditorContentSource (R-FILE-009 / L-24): the play-in-editor feed of the loading seam. In-editor,
// EditorKernel hands RuntimeKernel its live derived content units in memory (no pack, no file
// parse) — this source models that seam over an explicit set of LoadedUnits. It is the SAME loading
// seam PackContentSource implements, so a unit loaded live and the same unit loaded from a shipped
// pack materialize identically (editor==build fidelity — proven by the feed-parity test).
//
// Units are mutable via replace_unit(): re-deriving a unit in the editor (a hot reload — the
// watch->hash->re-derive pipeline, R-PLAY-003 / L-22) swaps its content here, and the loader's
// invalidate() path re-materializes it + invalidates stale handles (L-24 handle invalidation).

#pragma once

#include "context/runtime/content/content_source.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace context::runtime::content
{

class EditorContentSource final : public ContentSource
{
public:
    EditorContentSource() = default;
    explicit EditorContentSource(std::vector<LoadedUnit> units);

    // Add or replace a unit (the derivation feed / a hot-reload re-derive). Rebuilds the directory
    // row for the unit; the loader picks up the new content on its next load()/invalidate().
    void replace_unit(LoadedUnit unit);

    // Drop a unit from the derived set entirely (e.g. an entity subtree deleted in the editor).
    // Returns true if a unit was removed.
    bool remove_unit(std::uint64_t unit_id);

    [[nodiscard]] const std::vector<UnitDescriptor>& directory() const override { return directory_; }
    [[nodiscard]] bool load(std::uint64_t unit_id, LoadedUnit& out,
                            std::string* error) const override;
    [[nodiscard]] bool contains(std::uint64_t unit_id) const override;

private:
    void rebuild_directory();

    std::unordered_map<std::uint64_t, LoadedUnit> units_;
    std::vector<UnitDescriptor> directory_;
};

} // namespace context::runtime::content
