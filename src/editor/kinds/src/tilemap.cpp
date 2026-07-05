// Tilemap content-kind semantics — see tilemap.h.

#include "context/editor/kinds/tilemap.h"

#include "json_access.h" // shared member() / array_member() over the serializer tree

#include <limits>
#include <string>

namespace context::editor::kinds
{

using serializer::JsonValue;

namespace
{

// A lossless integer read of a JSON number (integer / in-range unsigned). False otherwise.
bool as_ll(const JsonValue& v, long long& out)
{
    switch (v.type)
    {
    case JsonValue::Type::integer:
        out = v.int_value;
        return true;
    case JsonValue::Type::unsigned_integer:
        if (v.uint_value >
            static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
            return false;
        out = static_cast<long long>(v.uint_value);
        return true;
    default:
        return false;
    }
}

// True when `v` is a JSON numeric literal (integer or unsigned). The schema's region items are
// `{"type": "integer"}` with no range bound, and carrier_matches admits an unsigned above INT64_MAX
// for it — so a numeric literal that overflows as_ll is a real magnitude defect, not the non-numeric
// shape error the schema's items check already owns.
bool is_json_number(const JsonValue& v) noexcept
{
    return v.type == JsonValue::Type::integer || v.type == JsonValue::Type::unsigned_integer;
}

std::string ptr_index(std::string_view base, std::size_t i)
{
    return std::string(base) + "/" + std::to_string(i);
}

} // namespace

std::size_t tilemap_chunk_bytes(long long width, long long height) noexcept
{
    if (width <= 0 || height <= 0)
        return 0;
    const auto w = static_cast<unsigned long long>(width);
    const auto h = static_cast<unsigned long long>(height);
    const auto cap = static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max());
    if (w > cap / h)
        return static_cast<std::size_t>(cap);
    const unsigned long long cells = w * h;
    if (cells > cap / kTilemapBytesPerCell)
        return static_cast<std::size_t>(cap);
    return static_cast<std::size_t>(cells * kTilemapBytesPerCell);
}

std::vector<KindDiagnostic> analyze_tilemap(const JsonValue& doc,
                                            const std::map<std::string, std::size_t>* sidecar_sizes)
{
    std::vector<KindDiagnostic> out;

    // --- stable-id uniqueness across tileSets and across layers (L-33) --------------------------
    const auto check_id_dupes = [&](const char* array_key) {
        const JsonValue* arr = array_member(doc, array_key);
        if (arr == nullptr)
            return;
        std::vector<std::string> seen;
        for (std::size_t i = 0; i < arr->elements.size(); ++i)
        {
            const JsonValue* id = member(arr->elements[i], "id");
            if (id == nullptr || id->type != JsonValue::Type::string)
                continue; // shape errors are schema::validate_document's job
            bool dup = false;
            for (const std::string& s : seen)
                dup = dup || s == id->string_value;
            if (dup)
                out.push_back({"tilemap.id_duplicate",
                               ptr_index(std::string("/") + array_key, i) + "/id",
                               "duplicate stable id \"" + id->string_value +
                                   "\" (L-33 ids are unique within a collection)"});
            else
                seen.push_back(id->string_value);
        }
    };
    check_id_dupes("tileSets");
    check_id_dupes("layers");

    // --- per-chunk region sanity + the L-33 split-nudge -----------------------------------------
    const JsonValue* layers = array_member(doc, "layers");
    if (layers == nullptr)
        return out;
    for (std::size_t li = 0; li < layers->elements.size(); ++li)
    {
        const std::string layer_ptr = ptr_index("/layers", li);
        const JsonValue* chunks = array_member(layers->elements[li], "chunks");
        if (chunks == nullptr)
            continue;
        for (std::size_t ci = 0; ci < chunks->elements.size(); ++ci)
        {
            const std::string chunk_ptr = ptr_index(layer_ptr + "/chunks", ci);
            const JsonValue& chunk = chunks->elements[ci];
            const JsonValue* region = member(chunk, "region");
            if (region == nullptr || region->type != JsonValue::Type::array ||
                region->elements.size() != 4)
                continue; // wrong arity / not an array — the schema's i32x4 items check owns it
            long long w = 0;
            long long h = 0;
            const bool w_ok = as_ll(region->elements[2], w);
            const bool h_ok = as_ll(region->elements[3], h);
            if (!w_ok || !h_ok)
            {
                // A width/height that IS a schema-accepted integer (region items carry no range
                // bound, and carrier_matches passes an unsigned above INT64_MAX) but overflows
                // long long is a genuine region defect — not the non-numeric shape error the schema
                // owns — so raise region_invalid rather than silently skipping the chunk (which
                // would also swallow the chunk_oversize check meant for pathological sizes).
                if ((!w_ok && is_json_number(region->elements[2])) ||
                    (!h_ok && is_json_number(region->elements[3])))
                    out.push_back({"tilemap.region_invalid", chunk_ptr + "/region",
                                   "chunk region width and height must be a representable integer"});
                continue;
            }
            if (w <= 0 || h <= 0)
            {
                out.push_back({"tilemap.region_invalid", chunk_ptr + "/region",
                               "chunk region width and height must be positive"});
                continue;
            }
            std::size_t bytes = tilemap_chunk_bytes(w, h);
            // Prefer the sidecar's MEASURED payload size when the caller supplied one.
            if (sidecar_sizes != nullptr)
            {
                const JsonValue* cells = member(chunk, "cells");
                const JsonValue* relpath = cells != nullptr ? member(*cells, "$sidecar") : nullptr;
                if (relpath != nullptr && relpath->type == JsonValue::Type::string)
                    if (const auto it = sidecar_sizes->find(relpath->string_value);
                        it != sidecar_sizes->end())
                        bytes = it->second;
            }
            if (bytes > kTilemapSplitCeilingBytes)
                out.push_back({"tilemap.chunk_oversize", chunk_ptr,
                               "chunk cell payload (" + std::to_string(bytes) +
                                   " bytes) exceeds the ~1 MB split-nudge ceiling — split the "
                                   "region (L-33)"});
        }
    }
    return out;
}

} // namespace context::editor::kinds
