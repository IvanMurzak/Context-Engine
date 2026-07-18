// The a18 tilemap write-path core tests (R-2D-003 GUI half / R-CLI-001 / L-33 / R-QA-013): the u32-LE
// cell codec, rect-fill expansion, batch cell edits (happy / last-wins / no-op / heal), the
// all-or-nothing failure paths (unknown layer / out-of-bounds cell / unknown tile), and the L-33
// sidecar-first commit executor — including canonical-fixpoint + family-coherence assertions (the
// "authored file diff is canonical and hot-reloads" DoD: the rewrite is canonical, the raw-byte hash
// moves so the L-22 watch->derive pipeline re-derives, and every "$sidecar" ref verifies).

#include "context/editor/tilemap/tilemap_edit.h"

#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/sidecar.h"
#include "context/editor/serializer/canonical.h"

#include "tilemap_edit_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor;
using namespace context::editor::tilemap;

namespace
{

constexpr const char* kOwnerPath = "tilemaps/map.tilemap.json";

// A minimal two-layer tilemap: layer A (id aaaaaaaaaaaaaa01) has one 4x4 chunk at the origin backed
// by "a.cells.bin"; layer B (id bbbbbbbbbbbbbb01) has one 2x2 chunk at (10, 10) whose sidecar is
// DANGLING (absent on disk) — the corpus-realistic state the heal pass covers. One tile-set spans
// global ids [1, 65).
[[nodiscard]] std::string fixture_json()
{
    return R"({
      "$schema": "ctx:tilemap",
      "tileSize": [1, 1],
      "tileSets": [
        {"id": "7777777777777701", "name": "terrain", "firstTileId": 1, "tileCount": 64,
         "atlas": {"$ref": "a7c3e5f1b9d20648", "path": "atlases/terrain.atlas.json"}}
      ],
      "layers": [
        {"id": "aaaaaaaaaaaaaa01", "name": "ground", "chunks": [
          {"region": [0, 0, 4, 4], "cells": {"$sidecar": "a.cells.bin", "hash": "1"}}
        ]},
        {"id": "bbbbbbbbbbbbbb01", "name": "props", "chunks": [
          {"region": [10, 10, 2, 2], "cells": {"$sidecar": "b.cells.bin", "hash": "1"}}
        ]}
      ]
    })";
}

struct Fixture
{
    filesync::MemoryFileStore fs;
    std::string owner_bytes;
    serializer::JsonValue doc;
};

// Stage the fixture into a fresh MemoryFileStore: canonical owner bytes + layer A's sidecar holding
// a known 4x4 grid (cell (1, 0) already 7 so the no-op path is exercisable). Layer B stays dangling.
[[nodiscard]] Fixture make_fixture()
{
    Fixture f;
    serializer::CanonicalizeResult canonical = serializer::canonicalize(fixture_json());
    CHECK(canonical.is_json);
    f.owner_bytes = canonical.bytes;
    f.doc = canonical.root;
    CHECK(f.fs.write(kOwnerPath, f.owner_bytes));

    std::vector<std::uint32_t> cells(16, 0U);
    cells[1] = 7U; // (x=1, y=0)
    CHECK(f.fs.write("tilemaps/a.cells.bin", filesync::encode_sidecar(encode_cell_payload(cells))));
    return f;
}

void test_codec()
{
    // Byte order is pinned LITTLE-ENDIAN (R-FILE-001: identical sidecar bytes on every platform).
    const std::string bytes = encode_cell_payload({0x01020304U});
    CHECK(bytes.size() == 4);
    CHECK(static_cast<unsigned char>(bytes[0]) == 0x04);
    CHECK(static_cast<unsigned char>(bytes[1]) == 0x03);
    CHECK(static_cast<unsigned char>(bytes[2]) == 0x02);
    CHECK(static_cast<unsigned char>(bytes[3]) == 0x01);

    const std::vector<std::uint32_t> grid = {0U, 1U, 0xffffffffU, 42U};
    CHECK(decode_cell_payload(encode_cell_payload(grid)) == grid);
    CHECK(decode_cell_payload("").empty());
    CHECK(decode_cell_payload("abc").empty()); // a trailing partial word is ignored
}

void test_fill_expansion()
{
    const std::vector<CellEdit> edits = expand_fill_rect(2, 3, 2, 2, 9U);
    CHECK(edits.size() == 4);
    // Row-major: (2,3), (3,3), (2,4), (3,4).
    CHECK(edits[0].x == 2 && edits[0].y == 3);
    CHECK(edits[1].x == 3 && edits[1].y == 3);
    CHECK(edits[2].x == 2 && edits[2].y == 4);
    CHECK(edits[3].x == 3 && edits[3].y == 4);
    CHECK(edits[0].tile == 9U);
    CHECK(expand_fill_rect(0, 0, 0, 5, 1U).empty());
    CHECK(expand_fill_rect(0, 0, 5, -1, 1U).empty());
}

void test_paint_happy_and_heal()
{
    Fixture f = make_fixture();
    const std::vector<CellEdit> edits = {{0, 0, 5U}, {3, 3, 6U}};
    const EditOutcome out =
        apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "aaaaaaaaaaaaaa01", edits);
    CHECK(out.ok);
    CHECK(out.cells_changed == 2);
    // Layer A's rewritten sidecar + layer B's HEALED (previously dangling) sidecar are both staged.
    CHECK(out.sidecars.size() == 2);
    CHECK(out.sidecars[0].path == "tilemaps/a.cells.bin");
    CHECK(out.sidecars[1].path == "tilemaps/b.cells.bin");

    // The painted grid: existing cell (1,0)=7 preserved, (0,0)=5 and (3,3)=6 written.
    const filesync::SidecarDecodeResult decoded = filesync::decode_sidecar(out.sidecars[0].bytes);
    CHECK(decoded.ok);
    const std::vector<std::uint32_t> cells = decode_cell_payload(decoded.payload);
    CHECK(cells.size() == 16);
    CHECK(cells[0] == 5U);
    CHECK(cells[1] == 7U);
    CHECK(cells[15] == 6U);

    // The healed sidecar is an all-empty 2x2 grid.
    const filesync::SidecarDecodeResult healed = filesync::decode_sidecar(out.sidecars[1].bytes);
    CHECK(healed.ok);
    CHECK(decode_cell_payload(healed.payload) == std::vector<std::uint32_t>(4, 0U));

    // The mutated owner is a canonical FIXPOINT and its hash members match the staged bytes.
    const serializer::CanonicalizeResult recanon = serializer::canonicalize(out.owner_bytes);
    CHECK(recanon.is_json);
    CHECK(recanon.bytes == out.owner_bytes);
    for (const StagedWrite& w : out.sidecars)
        CHECK(out.owner_bytes.find(filesync::format_sidecar_hash(w.raw_hash)) != std::string::npos);
}

void test_last_edit_wins_and_noop()
{
    Fixture f = make_fixture();
    // Same cell painted twice: the LAST stroke wins.
    const EditOutcome out = apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "aaaaaaaaaaaaaa01",
                                             {{2, 2, 3U}, {2, 2, 9U}});
    CHECK(out.ok);
    const filesync::SidecarDecodeResult decoded = filesync::decode_sidecar(out.sidecars[0].bytes);
    const std::vector<std::uint32_t> cells = decode_cell_payload(decoded.payload);
    CHECK(cells[2 * 4 + 2] == 9U);

    // A paint writing the already-present value is a NO-OP: nothing dirty for that chunk (only the
    // dangling layer-B sidecar is healed).
    const EditOutcome noop =
        apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "aaaaaaaaaaaaaa01", {{1, 0, 7U}});
    CHECK(noop.ok);
    CHECK(noop.cells_changed == 0);
    CHECK(noop.sidecars.size() == 1);
    CHECK(noop.sidecars[0].path == "tilemaps/b.cells.bin");
}

void test_failure_paths()
{
    Fixture f = make_fixture();

    const EditOutcome no_layer =
        apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "ffffffffffffff01", {{0, 0, 1U}});
    CHECK(!no_layer.ok);
    CHECK(no_layer.error_code == kTilemapLayerNotFoundCode);

    const EditOutcome oob =
        apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "aaaaaaaaaaaaaa01", {{99, 99, 1U}});
    CHECK(!oob.ok);
    CHECK(oob.error_code == kTilemapCellOutOfBoundsCode);

    const EditOutcome bad_tile =
        apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "aaaaaaaaaaaaaa01", {{0, 0, 1000U}});
    CHECK(!bad_tile.ok);
    CHECK(bad_tile.error_code == kTilemapTileUnknownCode);

    // All-or-nothing: one bad cell in the batch refuses the WHOLE gesture — nothing is staged.
    const EditOutcome mixed = apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "aaaaaaaaaaaaaa01",
                                               {{0, 0, 2U}, {99, 99, 2U}});
    CHECK(!mixed.ok);
    CHECK(mixed.sidecars.empty());
    CHECK(mixed.owner_bytes.empty());

    // Erase (tile 0) is always a known tile.
    const EditOutcome erase =
        apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "aaaaaaaaaaaaaa01", {{1, 0, 0U}});
    CHECK(erase.ok);
    CHECK(erase.cells_changed == 1);
}

void test_commit_writes_coherent_family()
{
    Fixture f = make_fixture();
    const std::uint64_t owner_hash_before = filesync::content_hash(f.owner_bytes);

    const EditOutcome out = apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "aaaaaaaaaaaaaa01",
                                             expand_fill_rect(0, 0, 2, 2, 4U));
    CHECK(out.ok);
    const CommitResult commit = commit_edit(f.fs, ".", kOwnerPath, out);
    CHECK(commit.ok);

    // The owner on disk holds the staged canonical bytes; its raw hash MOVED (the L-22 watch->derive
    // hot-reload trigger is exactly this raw-byte change).
    const std::optional<std::string> owner_now = f.fs.read(kOwnerPath);
    CHECK(owner_now.has_value());
    CHECK(*owner_now == out.owner_bytes);
    CHECK(commit.owner_raw_hash == filesync::content_hash(*owner_now));
    CHECK(commit.owner_raw_hash != owner_hash_before);

    // Every "$sidecar" ref in the committed owner verifies against disk — a fully coherent family
    // (scan + verify are the same passes the reconcile/derivation pipeline runs on hot reload).
    const filesync::SidecarScan scan = filesync::scan_sidecar_refs(".", kOwnerPath, *owner_now);
    CHECK(scan.owner_parsed);
    CHECK(scan.refs.size() == 2);
    CHECK(scan.diagnostics.empty());
    CHECK(filesync::verify_sidecar_refs(f.fs, kOwnerPath, scan.refs).empty());

    // Committing a TAMPERED family is refused by the L-33 planner (hash_mismatch) — the write path
    // can never durably author a lying reference.
    EditOutcome tampered = apply_cell_edits(f.fs, ".", kOwnerPath, f.doc, "aaaaaaaaaaaaaa01",
                                            {{1, 1, 8U}});
    CHECK(tampered.ok);
    CHECK(!tampered.sidecars.empty());
    tampered.sidecars[0].bytes += "corruption";
    const CommitResult refused = commit_edit(f.fs, ".", kOwnerPath, tampered);
    CHECK(!refused.ok);
    CHECK(!refused.error_code.empty());
}

} // namespace

int main()
{
    test_codec();
    test_fill_expansion();
    test_paint_happy_and_heal();
    test_last_edit_wins_and_noop();
    test_failure_paths();
    test_commit_writes_coherent_family();
    TILEMAP_TEST_MAIN_END();
}
