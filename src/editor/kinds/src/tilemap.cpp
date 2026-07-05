// Tilemap content-kind semantics — see tilemap.h.

#include "context/editor/kinds/tilemap.h"

#include <limits>
#include <string>

namespace context::editor::kinds
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

const JsonValue* member(const JsonValue& object, std::string_view key)
{
    if (object.type != JsonValue::Type::object)
        return nullptr;
    for (const JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// The array value at object[key], or nullptr when absent / not an array.
const JsonValue* array_member(const JsonValue& object, std::string_view key)
{
    const JsonValue* v = member(object, key);
    return (v != nullptr && v->type == JsonValue::Type::array) ? v : nullptr;
}

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
            long long w = 0;
            long long h = 0;
            const bool region_ok = region != nullptr && region->type == JsonValue::Type::array &&
                                   region->elements.size() == 4 &&
                                   as_ll(region->elements[2], w) && as_ll(region->elements[3], h);
            if (!region_ok)
                continue; // the schema's i32x4 arity check owns malformed regions
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
