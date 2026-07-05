// Tilemap content kind (R-2D-003): schema-fixture validation against the REAL engine_schemas()
// registration (valid + every invalid class), the L-33 split-nudge + region/id semantics, the
// canonical round-trip, and the binary-sidecar day-one round-trip through the PR #54 codec.
// (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/kinds/tilemap.h"

#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/sidecar.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/schema/validator.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/sidecar_ref.h"

#include "kinds_test.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace kinds = context::editor::kinds;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;
namespace filesync = context::editor::filesync;

namespace
{

serializer::JsonValue parse(std::string_view text)
{
    serializer::ParseResult p = serializer::parse_json(text);
    CHECK(p.ok);
    return p.root;
}

schema::ValidationReport validate(std::string_view text)
{
    serializer::ParseResult p = serializer::parse_json(text);
    CHECK(p.ok);
    return schema::validate_document(p.root, text, schema::engine_schemas());
}

bool has(const schema::ValidationReport& r, std::string_view code)
{
    for (const schema::ValidationDiagnostic& d : r.diagnostics)
        if (d.code == code)
            return true;
    return false;
}

// A minimal, schema-valid tilemap: one atlas tile-set, one layer, one 16x16 chunk whose cells live
// in a binary sidecar.
constexpr std::string_view kValidTilemap = R"({
  "$schema": "ctx:tilemap",
  "version": 1,
  "tileSize": [1, 1],
  "tileSets": [
    {"id": "a1b2c3d4e5f60718", "name": "ground", "atlas": {"$ref": "9f8e7d6c5b4a3928", "path": "atlases/ground.atlas.json"}, "firstTileId": 1, "tileCount": 64}
  ],
  "layers": [
    {"id": "0f1e2d3c4b5a6978", "name": "base", "opacity": 1, "visible": true, "chunks": [
      {"region": [0, 0, 16, 16], "cells": {"$sidecar": "layers/base/0.bin", "hash": "12345678901234567890"}}
    ]}
  ]
})";

// One chunk of the given region (schema-valid skeleton) — the split-nudge / region-sanity subject.
std::string tilemap_one_chunk(int x, int y, int w, int h)
{
    return std::string("{\"$schema\":\"ctx:tilemap\",\"version\":1,\"tileSize\":[1,1],"
                       "\"tileSets\":[],\"layers\":[{\"id\":\"l1\",\"name\":\"b\",\"chunks\":[{"
                       "\"region\":[") +
           std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(w) + "," +
           std::to_string(h) + "],\"cells\":{\"$sidecar\":\"c.bin\",\"hash\":\"1\"}}]}]}";
}

} // namespace

int main()
{
    // --- schema validation: happy path -----------------------------------------------------------
    {
        schema::ValidationReport r = validate(kValidTilemap);
        CHECK(r.schema_bound);
        CHECK(r.schema_id == "ctx:tilemap");
        CHECK(r.version == 1);
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }

    // --- schema validation: every invalid class --------------------------------------------------
    {
        // Missing a required root property (tileSets).
        schema::ValidationReport r = validate(R"({"$schema": "ctx:tilemap", "version": 1,
            "tileSize": [1, 1], "layers": []})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.required_missing"));
    }
    {
        // cells is not the pinned {$sidecar, hash} shape → the sidecar day-one consumer rejects it.
        schema::ValidationReport r = validate(R"({"$schema": "ctx:tilemap", "version": 1,
            "tileSize": [1, 1], "tileSets": [], "layers": [
              {"id": "l1", "name": "b", "chunks": [{"region": [0,0,4,4], "cells": "oops.bin"}]}]})");
        CHECK(!r.ok);
        CHECK(has(r, "sidecar.ref_malformed"));
    }
    {
        // region carries the wrong arity (i32x4 pins exactly four cell coordinates).
        schema::ValidationReport r = validate(R"({"$schema": "ctx:tilemap", "version": 1,
            "tileSize": [1, 1], "tileSets": [], "layers": [
              {"id": "l1", "name": "b", "chunks": [
                {"region": [0,0,4], "cells": {"$sidecar": "c.bin", "hash": "1"}}]}]})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.storage_arity"));
    }
    {
        // An undeclared property (additionalProperties: false everywhere).
        schema::ValidationReport r = validate(R"({"$schema": "ctx:tilemap", "version": 1,
            "tileSize": [1, 1], "tileSets": [], "layers": [], "bogus": 7})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.unknown_property"));
    }

    // --- canonical round-trip (R-FILE-001 fixpoint) ---------------------------------------------
    {
        serializer::CanonicalizeResult once = serializer::canonicalize(kValidTilemap);
        CHECK(once.is_json);
        // Canonicalizing canonical output is byte-identical.
        CHECK(once.bytes == serializer::canonicalize(once.bytes).bytes);
        // The canonical form still validates.
        CHECK(validate(once.bytes).ok);
    }

    // --- tilemap_chunk_bytes: the split-nudge arithmetic ----------------------------------------
    {
        CHECK(kinds::tilemap_chunk_bytes(512, 512) == kinds::kTilemapSplitCeilingBytes); // exactly 1 MiB
        CHECK(kinds::tilemap_chunk_bytes(512, 513) == 512ull * 513ull * 4ull);
        CHECK(kinds::tilemap_chunk_bytes(0, 16) == 0);
        CHECK(kinds::tilemap_chunk_bytes(16, -1) == 0);
    }

    // --- analyze_tilemap: split-nudge at the ceiling (estimate path) -----------------------------
    {
        // 512x512 packs to EXACTLY the ceiling → no nudge (the ceiling is inclusive).
        CHECK(!kinds::has_code(kinds::analyze_tilemap(parse(tilemap_one_chunk(0, 0, 512, 512))),
                               "tilemap.chunk_oversize"));
        // One cell over the ceiling → nudge.
        std::vector<kinds::KindDiagnostic> over =
            kinds::analyze_tilemap(parse(tilemap_one_chunk(0, 0, 512, 513)));
        CHECK(kinds::has_code(over, "tilemap.chunk_oversize"));
        CHECK(over.front().pointer == "/layers/0/chunks/0");
        // A small chunk is fine.
        CHECK(kinds::analyze_tilemap(parse(tilemap_one_chunk(0, 0, 32, 32))).empty());
    }

    // --- analyze_tilemap: region sanity ---------------------------------------------------------
    {
        std::vector<kinds::KindDiagnostic> d =
            kinds::analyze_tilemap(parse(tilemap_one_chunk(0, 0, 0, 16)));
        CHECK(kinds::has_code(d, "tilemap.region_invalid"));
        CHECK(d.front().pointer == "/layers/0/chunks/0/region");
    }

    // --- analyze_tilemap: stable-id uniqueness (L-33) -------------------------------------------
    {
        // Two layers sharing an id.
        std::vector<kinds::KindDiagnostic> d = kinds::analyze_tilemap(parse(R"({
            "$schema": "ctx:tilemap", "version": 1, "tileSize": [1,1], "tileSets": [],
            "layers": [{"id": "dup", "name": "a", "chunks": []},
                       {"id": "dup", "name": "b", "chunks": []}]})"));
        CHECK(kinds::has_code(d, "tilemap.id_duplicate"));
    }
    {
        // Two tile-sets sharing an id.
        std::vector<kinds::KindDiagnostic> d = kinds::analyze_tilemap(parse(R"({
            "$schema": "ctx:tilemap", "version": 1, "tileSize": [1,1],
            "tileSets": [{"id": "t", "atlas": {"$ref": "g"}, "firstTileId": 1},
                         {"id": "t", "atlas": {"$ref": "g2"}, "firstTileId": 2}],
            "layers": []})"));
        CHECK(kinds::has_code(d, "tilemap.id_duplicate"));
    }

    // --- analyze_tilemap: the MEASURED sidecar size overrides the estimate ----------------------
    {
        serializer::JsonValue doc = parse(kValidTilemap); // a tiny 16x16 chunk (estimate = 1024 B)
        std::map<std::string, std::size_t> sizes;
        // The real sidecar on disk is 2 MiB → nudge, even though the region estimate is tiny.
        sizes["layers/base/0.bin"] = 2u * 1024u * 1024u;
        CHECK(kinds::has_code(kinds::analyze_tilemap(doc, &sizes), "tilemap.chunk_oversize"));
        // A small measured size beats a huge region estimate (no nudge).
        serializer::JsonValue big = parse(tilemap_one_chunk(0, 0, 4096, 4096)); // estimate = 64 MiB
        std::map<std::string, std::size_t> small;
        small["c.bin"] = 10;
        CHECK(!kinds::has_code(kinds::analyze_tilemap(big, &small), "tilemap.chunk_oversize"));
    }

    // --- the binary-sidecar day-one round-trip (PR #54 codec) -----------------------------------
    {
        // Pack a tiny cell grid (4 u32 tile ids) as the chunk's sidecar payload.
        std::string payload;
        const std::uint32_t ids[4] = {1, 2, 65, 66};
        for (std::uint32_t id : ids)
            for (int b = 0; b < 4; ++b)
                payload.push_back(static_cast<char>((id >> (8 * b)) & 0xFF));

        // Encode through the versioned sidecar codec and prove it decodes back byte-for-byte.
        const std::string encoded = filesync::encode_sidecar(payload);
        const filesync::SidecarDecodeResult decoded = filesync::decode_sidecar(encoded);
        CHECK(decoded.ok);
        CHECK(decoded.version == filesync::sidecar_format_version);
        CHECK(std::string(decoded.payload) == payload);

        // The authored "hash" is the whole-file raw-byte hash, as a canonical decimal string.
        const std::uint64_t raw = filesync::content_hash(encoded);
        const std::string hash_str = filesync::format_sidecar_hash(raw);
        CHECK(serializer::parse_hash_string(hash_str) == raw);

        // Author a tilemap referencing that sidecar with the real hash — it validates, and the cells
        // value is a well-formed sidecar reference the collector recognizes.
        const std::string doc = std::string("{\"$schema\":\"ctx:tilemap\",\"version\":1,"
                                            "\"tileSize\":[1,1],\"tileSets\":[],\"layers\":[{"
                                            "\"id\":\"l1\",\"name\":\"b\",\"chunks\":[{"
                                            "\"region\":[0,0,2,2],\"cells\":{\"$sidecar\":"
                                            "\"layers/l1/0.bin\",\"hash\":\"") +
                                 hash_str + "\"}}]}]}";
        schema::ValidationReport r = validate(doc);
        CHECK(r.ok);
        CHECK(!has(r, "sidecar.ref_malformed"));

        serializer::JsonValue tree = parse(doc);
        std::vector<serializer::Diagnostic> refs_diag;
        std::vector<serializer::SidecarRef> refs =
            serializer::collect_sidecar_refs(tree, refs_diag);
        CHECK(refs.size() == 1);
        CHECK(refs.front().relpath == "layers/l1/0.bin");
        CHECK(refs.front().hash == raw);
        CHECK(refs_diag.empty());

        // The measured payload is far under the ceiling → no nudge.
        std::map<std::string, std::size_t> sizes;
        sizes["layers/l1/0.bin"] = encoded.size();
        CHECK(kinds::analyze_tilemap(tree, &sizes).empty());
    }

    KINDS_TEST_MAIN_END();
}
