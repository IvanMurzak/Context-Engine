// `context tilemap paint|fill` — the tilemap cell-authoring CLI verbs (see tilemap_command.h).

#include "context/cli/tilemap_command.h"

#include "context/cli/wire_client.h" // parse_u64 (the canonical decimal-u64 flag parse)
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/editor/tilemap/tilemap_edit.h"

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
namespace filesync = editor::filesync;
namespace serializer = editor::serializer;
namespace tilemap = editor::tilemap;

namespace
{

[[nodiscard]] std::optional<std::string> flag(const std::map<std::string, std::string>& flags,
                                              const std::string& name)
{
    const auto it = flags.find(name);
    if (it == flags.end())
        return std::nullopt;
    return it->second;
}

[[nodiscard]] std::optional<std::string> bound_value(const std::map<std::string, std::string>& bound,
                                                     const std::string& name)
{
    const auto it = bound.find(name);
    if (it == bound.end() || it->second.empty())
        return std::nullopt;
    return it->second;
}

// The integer value of a parsed JSON node, when it is an integer literal; nullopt otherwise.
[[nodiscard]] std::optional<std::int64_t> as_int(const serializer::JsonValue& v)
{
    if (v.type == serializer::JsonValue::Type::integer)
        return v.int_value;
    if (v.type == serializer::JsonValue::Type::unsigned_integer &&
        v.uint_value <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(v.uint_value);
    return std::nullopt;
}

// Parse the `paint` <cells> positional: a JSON array of [x, y, tileId] integer triples with tileId
// in u32 range. Returns nullopt (with `error` set) on any malformed entry.
[[nodiscard]] std::optional<std::vector<tilemap::CellEdit>> parse_cells(const std::string& text,
                                                                        std::string& error)
{
    serializer::ParseResult parsed = serializer::parse_json(text);
    if (!parsed.ok || parsed.root.type != serializer::JsonValue::Type::array)
    {
        error = "the <cells> argument must be a JSON array of [x, y, tileId] triples, e.g. "
                "'[[0, 0, 1], [1, 0, 2]]'";
        return std::nullopt;
    }
    std::vector<tilemap::CellEdit> edits;
    edits.reserve(parsed.root.elements.size());
    for (const serializer::JsonValue& entry : parsed.root.elements)
    {
        if (entry.type != serializer::JsonValue::Type::array || entry.elements.size() != 3)
        {
            error = "each <cells> entry must be an [x, y, tileId] triple";
            return std::nullopt;
        }
        const std::optional<std::int64_t> x = as_int(entry.elements[0]);
        const std::optional<std::int64_t> y = as_int(entry.elements[1]);
        const std::optional<std::int64_t> tile = as_int(entry.elements[2]);
        if (!x || !y || !tile || *tile < 0 || *tile > static_cast<std::int64_t>(UINT32_MAX))
        {
            error = "each [x, y, tileId] component must be an integer (tileId in u32 range; 0 = "
                    "empty)";
            return std::nullopt;
        }
        edits.push_back(tilemap::CellEdit{*x, *y, static_cast<std::uint32_t>(*tile)});
    }
    if (edits.empty())
    {
        error = "the <cells> array is empty — nothing to paint";
        return std::nullopt;
    }
    return edits;
}

// Parse the `fill` --rect flag: four comma-separated integers "x,y,width,height" (positive extent).
[[nodiscard]] bool parse_rect(const std::string& text, std::int64_t out[4])
{
    std::istringstream stream(text);
    std::string part;
    std::size_t i = 0;
    while (std::getline(stream, part, ','))
    {
        if (i >= 4 || part.empty())
            return false;
        try
        {
            std::size_t used = 0;
            out[i] = std::stoll(part, &used);
            if (used != part.size())
                return false;
        }
        catch (...)
        {
            return false;
        }
        ++i;
    }
    return i == 4 && out[2] > 0 && out[3] > 0;
}

// Shared tail of both verbs: canonical-load the owner, run the ONE write-path core, honor
// --dry-run / --if-match, and shape the result envelope.
[[nodiscard]] Envelope apply_and_report(const std::string& path, const std::string& layer,
                                        const std::vector<tilemap::CellEdit>& edits,
                                        const std::map<std::string, std::string>& flags)
{
    if (!filesync::is_inside_jail(".", path))
        return Envelope::failure("path.jail_violation", "the tilemap path `" + path +
                                                            "` escapes the project root (R-SEC-008)");

    const std::string project = flag(flags, "project").value_or(".");
    filesync::NativeFileStore store(project);

    const std::optional<std::string> owner_bytes = store.read(path);
    if (!owner_bytes)
        return Envelope::failure("file.not_found", "the tilemap `" + path +
                                                       "` does not exist under project `" +
                                                       project + "`");

    // --if-match CAS: guard the owner's CURRENT raw bytes (R-FILE-004 / R-CLI-006 convention).
    if (const std::optional<std::string> if_match = flag(flags, "if-match"))
    {
        const std::optional<std::uint64_t> expected = parse_u64(*if_match);
        if (!expected)
            return Envelope::failure("usage.invalid",
                                     "--if-match takes a decimal raw-byte content hash; got `" +
                                         *if_match + "`");
        const std::uint64_t actual = filesync::content_hash(*owner_bytes);
        if (actual != *expected)
            return Envelope::failure("cas.mismatch",
                                     "the --if-match raw-byte hash (" + std::to_string(*expected) +
                                         ") does not match the tilemap's current bytes (" +
                                         std::to_string(actual) + ") — re-read and retry");
    }

    const serializer::CanonicalizeResult canonical = serializer::canonicalize(*owner_bytes);
    if (!canonical.is_json)
        return Envelope::failure("file.parse_error",
                                 "the tilemap `" + path + "` is not a JSON document");

    const tilemap::EditOutcome edit =
        tilemap::apply_cell_edits(store, ".", path, canonical.root, layer, edits);
    if (!edit.ok)
        return Envelope::failure(edit.error_code, edit.error_message);

    Json sidecars = Json::array();
    for (const tilemap::StagedWrite& w : edit.sidecars)
    {
        Json entry = Json::object();
        entry.set("path", Json(w.path));
        // 64-bit hashes as decimal STRINGS (they exceed 2^53 — the rawHash/canonicalHash convention).
        entry.set("rawHash", Json(std::to_string(w.raw_hash)));
        sidecars.push_back(std::move(entry));
    }

    Json data = Json::object();
    data.set("file", Json(path));
    data.set("layer", Json(layer));
    data.set("cellsChanged", Json(static_cast<std::uint64_t>(edit.cells_changed)));
    data.set("rawHash", Json(std::to_string(filesync::content_hash(edit.owner_bytes))));
    data.set("canonicalHash", Json(std::to_string(serializer::canonical_hash_of(edit.owner_bytes))));
    data.set("sidecars", std::move(sidecars));

    const bool dry_run = flags.find("dry-run") != flags.end();
    data.set("dryRun", Json(dry_run));
    if (dry_run)
    {
        data.set("applied", Json(false));
        data.set("note", Json(std::string("dry-run: the staged rewrite + resulting hashes are "
                                          "computed; no bytes were written")));
        return Envelope::success(std::move(data));
    }

    const tilemap::CommitResult committed = tilemap::commit_edit(store, ".", path, edit);
    if (!committed.ok)
        return Envelope::failure(committed.error_code, committed.error_message);
    data.set("applied", Json(true));
    return Envelope::success(std::move(data));
}

} // namespace

Envelope run_tilemap(const std::string& verb, const std::map<std::string, std::string>& bound,
                     const std::map<std::string, std::string>& flags)
{
    const std::optional<std::string> path = bound_value(bound, "path");
    const std::optional<std::string> layer = flag(flags, "layer");

    if (verb == "paint")
    {
        const std::optional<std::string> cells_text = bound_value(bound, "cells");
        if (!path || !cells_text)
            return Envelope::failure("usage.missing_argument",
                                     "usage: context tilemap paint <path> <cells> --layer "
                                     "<layerId> — <cells> is a JSON array of [x, y, tileId] "
                                     "triples (tileId 0 = empty/erase)");
        if (!layer || layer->empty())
            return Envelope::failure("usage.missing_argument",
                                     "context tilemap paint requires --layer <layerId> (the stable "
                                     "id of the layer to paint)");
        std::string error;
        const std::optional<std::vector<tilemap::CellEdit>> edits = parse_cells(*cells_text, error);
        if (!edits)
            return Envelope::failure("usage.invalid", error);
        return apply_and_report(*path, *layer, *edits, flags);
    }

    if (verb == "fill")
    {
        const std::optional<std::string> tile_text = bound_value(bound, "tile");
        if (!path || !tile_text)
            return Envelope::failure("usage.missing_argument",
                                     "usage: context tilemap fill <path> <tile> --layer <layerId> "
                                     "--rect x,y,width,height (tile 0 = empty/erase)");
        if (!layer || layer->empty())
            return Envelope::failure("usage.missing_argument",
                                     "context tilemap fill requires --layer <layerId> (the stable "
                                     "id of the layer to fill)");
        const std::optional<std::string> rect_text = flag(flags, "rect");
        if (!rect_text)
            return Envelope::failure("usage.missing_argument",
                                     "context tilemap fill requires --rect x,y,width,height (cell "
                                     "coordinates, positive extent)");
        const std::optional<std::uint64_t> tile = parse_u64(*tile_text);
        if (!tile || *tile > UINT32_MAX)
            return Envelope::failure("usage.invalid",
                                     "<tile> must be a u32 global tile id (0 = empty); got `" +
                                         *tile_text + "`");
        std::int64_t rect[4] = {0, 0, 0, 0};
        if (!parse_rect(*rect_text, rect))
            return Envelope::failure("usage.invalid",
                                     "--rect takes four comma-separated integers "
                                     "x,y,width,height with a positive extent; got `" + *rect_text +
                                         "`");
        const std::vector<tilemap::CellEdit> edits = tilemap::expand_fill_rect(
            rect[0], rect[1], rect[2], rect[3], static_cast<std::uint32_t>(*tile));
        return apply_and_report(*path, *layer, edits, flags);
    }

    return Envelope::failure("usage.invalid",
                             "unknown tilemap verb `" + verb + "` (expected paint or fill)");
}

} // namespace context::cli
