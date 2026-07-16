// PackContentSource (R-ASSET-005 / R-ASSET-003): the shipped-build feed of the loading seam over a
// frozen v1 chunked pack (docs/chunk-pack-format.md §4). It parses the header + directory + string
// table ONCE in the constructor — the O(1) unitId->chunk index — then decodes each chunk LAZILY on
// load(), self-verifying its FNV-1a-64 content hash. Lazy per-chunk decode is the essence of
// streaming: the whole pack is never materialized, only the resident units are, so the memory-budget
// scheduler can hold a working set far smaller than the archive.
//
// This is the RUNTIME loader the a01 pack_reader.h header names as the a02 deliverable. It depends
// only on the frozen byte-layout constants (context_pack_format) + the canonical serializer — NOT on
// the editor writer / context_compose (L-24: no editor/derivation code in a RuntimeKernel build).

#pragma once

#include "context/runtime/content/content_source.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace context::runtime::content
{

class PackContentSource final : public ContentSource
{
public:
    // Parse `bytes` (a whole v1 pack stream) into the directory index. Takes ownership of a COPY of
    // the bytes so chunk regions stay valid for lazy decode. Total — never throws; a malformed pack
    // leaves ok() == false + error() set and an empty directory (load() then always fails).
    explicit PackContentSource(std::string bytes);

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::uint64_t engine_version() const noexcept { return engine_version_; }
    [[nodiscard]] const std::string& root_scene() const noexcept { return root_scene_; }

    [[nodiscard]] const std::vector<UnitDescriptor>& directory() const override { return directory_; }
    [[nodiscard]] bool load(std::uint64_t unit_id, LoadedUnit& out,
                            std::string* error) const override;
    [[nodiscard]] bool contains(std::uint64_t unit_id) const override;

private:
    // Parse header + directory + string table into the index. Returns true on success; on any
    // malformation records error() + leaves the directory empty (called once, from the constructor).
    bool build_index();

    // One directory entry retained for lazy chunk decode — offsets into `bytes_`, no decoded body.
    struct Entry
    {
        std::uint64_t unit_id = 0;
        std::uint64_t parent_unit = 0;
        bool is_root = false;
        bool is_sidecar = false;
        std::uint64_t entity_count = 0;
        std::uint64_t chunk_offset = 0;
        std::uint64_t chunk_length = 0;
        std::uint64_t content_hash = 0;
    };

    std::string bytes_;                                    // owned pack stream
    std::vector<UnitDescriptor> directory_;                // the cheap directory (parse order)
    std::unordered_map<std::uint64_t, Entry> index_;       // unitId -> lazy chunk entry (O(1))
    std::string root_scene_;
    std::uint64_t engine_version_ = 0;
    bool ok_ = false;
    std::string error_;
};

} // namespace context::runtime::content
