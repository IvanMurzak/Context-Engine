// The a18 GUI<->CLI tilemap-authoring PARITY gate (R-2D-003 GUI half / R-CLI-001 / L-20; ctest
// `tilemap-paint-parity`): the SAME edits driven through (a) the tile-painting GUI model's L-20
// gesture lifecycle (begin/extend/commit-at-gesture-end) and (b) the REAL `context tilemap
// paint|fill` CLI verbs (in-process cli::run over the shipped verb grammar), on two identical
// staged copies of the committed platformer-2d sample, must produce BYTE-IDENTICAL authored files —
// owner JSON and every cell sidecar ("the GUI is sugar over the same write path", proven, not
// assumed). Then the DoD's hot-reload half: the authored diff is canonical (validate green +
// migrate --dry-run reports a fixpoint), the owner's raw-byte hash MOVED (the L-22 watch->derive
// trigger), and a FRESH model re-derives the painted cells from the files alone.

#include "context/cli/app.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/sidecar.h"
#include "context/editor/gui/panels/tilemap/tilemap_paint_model.h"
#include "context/editor/gui/panels/tilemap/tilemap_paint_panel.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/tilemap/tilemap_edit.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace context::editor;
using context::cli::run;
using context::editor::contract::Envelope;
using gui::panels::tilemap::CommitOutcome;
using gui::panels::tilemap::TilemapPaintModel;
using gui::panels::tilemap::Tool;
namespace tme = context::editor::tilemap;

namespace
{

int g_failures = 0;

void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

constexpr const char* kTilemapRel = "tilemaps/level-1.tilemap.json";
constexpr const char* kLayerId = "8888888888888801"; // the platformer ground layer's stable id

[[nodiscard]] std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Stage a fresh copy of the committed platformer-2d sample into a unique temp dir.
[[nodiscard]] fs::path stage_platformer(const char* tag)
{
    const auto stamp = static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffffffLL);
    const fs::path dir = fs::temp_directory_path()
                         / ("ctx-tilemap-parity-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(fs::path(CONTEXT_SAMPLES_DIR) / "platformer-2d", dir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    return dir;
}

void remove_quiet(const fs::path& p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

// Every authored artifact the edits produce, for the byte-identity comparison.
[[nodiscard]] std::vector<std::string> artifact_paths()
{
    return {kTilemapRel, "tilemaps/tilemaps/level-1.ground.0.cells.bin"};
}

} // namespace

int main()
{
    const fs::path stage_gui = stage_platformer("gui");
    const fs::path stage_cli = stage_platformer("cli");

    const std::string owner_before = read_file(stage_gui / kTilemapRel);
    CHECK(!owner_before.empty());
    const std::uint64_t owner_hash_before = filesync::content_hash(owner_before);

    // --- (a) the GUI path: the L-20 gesture lifecycle over the paint model -----------------------
    {
        filesync::NativeFileStore store(stage_gui.string());
        TilemapPaintModel model;
        CHECK(model.open(store, ".", kTilemapRel));
        CHECK(model.active_layer_id() == kLayerId);

        // A paint stroke over two cells with tile 2.
        CHECK(model.select_tile(2));
        model.set_tool(Tool::paint);
        model.begin_gesture_at_cell(0, 0);
        model.extend_gesture_to_cell(1, 1);
        const CommitOutcome painted = model.commit_gesture(store, ".");
        CHECK(painted.ok);
        CHECK(painted.cells_changed == 2);

        // A 3x3 fill with tile 3 (anchor -> drag, the rect expands at gesture end).
        CHECK(model.select_tile(3));
        model.set_tool(Tool::fill);
        model.begin_gesture_at_cell(2, 2);
        model.extend_gesture_to_cell(4, 4);
        const CommitOutcome filled = model.commit_gesture(store, ".");
        CHECK(filled.ok);
        CHECK(filled.cells_changed == 9);
    }

    // --- (b) the CLI path: the REAL shipped verbs, in-process over the identical stage -----------
    {
        const Envelope painted =
            run({"tilemap", "paint", kTilemapRel, "[[0, 0, 2], [1, 1, 2]]", "--layer", kLayerId,
                 "--project", stage_cli.string()});
        if (!painted.ok())
            std::fprintf(stderr, "tilemap paint failed: %s\n", painted.dump(0).c_str());
        CHECK(painted.ok());
        const Envelope filled = run({"tilemap", "fill", kTilemapRel, "3", "--layer", kLayerId,
                                     "--rect", "2,2,3,3", "--project", stage_cli.string()});
        if (!filled.ok())
            std::fprintf(stderr, "tilemap fill failed: %s\n", filled.dump(0).c_str());
        CHECK(filled.ok());
    }

    // --- PARITY: every authored artifact is byte-identical across the two paths ------------------
    for (const std::string& rel : artifact_paths())
    {
        const std::string gui_bytes = read_file(stage_gui / rel);
        const std::string cli_bytes = read_file(stage_cli / rel);
        CHECK(!gui_bytes.empty());
        if (gui_bytes != cli_bytes)
            std::fprintf(stderr, "parity break in %s (gui %zu bytes, cli %zu bytes)\n", rel.c_str(),
                         gui_bytes.size(), cli_bytes.size());
        CHECK(gui_bytes == cli_bytes);
    }

    // --- the DoD hot-reload half over the GUI stage ----------------------------------------------
    const std::string owner_after = read_file(stage_gui / kTilemapRel);
    CHECK(filesync::content_hash(owner_after) != owner_hash_before); // the L-22 watch trigger fires

    // The authored diff is CANONICAL: a re-canonicalize is a fixpoint, `validate` stays green, and
    // `migrate --dry-run` reports zero canonicalization drift.
    const serializer::CanonicalizeResult recanon = serializer::canonicalize(owner_after);
    CHECK(recanon.is_json);
    CHECK(recanon.bytes == owner_after);
    const Envelope validated = run({"validate", stage_gui.string()});
    CHECK(validated.ok());
    const Envelope migrated =
        run({"migrate", (stage_gui / "tilemaps").string(), "--dry-run"});
    CHECK(migrated.ok());
    if (migrated.ok())
    {
        CHECK(migrated.data().at("canonicalized").as_number() == 0.0);
        CHECK(migrated.data().at("failed").as_number() == 0.0);
    }

    // Every "$sidecar" ref verifies against disk (the previously-DANGLING corpus ref was healed by
    // the first commit), and a FRESH model re-derives the painted cells from the files alone — the
    // authored state hot-reloads into a new session with no in-memory carry-over (L-20).
    {
        filesync::NativeFileStore store(stage_gui.string());
        const filesync::SidecarScan scan =
            filesync::scan_sidecar_refs(".", kTilemapRel, owner_after);
        CHECK(scan.owner_parsed);
        CHECK(!scan.refs.empty());
        CHECK(scan.diagnostics.empty());
        CHECK(filesync::verify_sidecar_refs(store, kTilemapRel, scan.refs).empty());

        TilemapPaintModel fresh;
        CHECK(fresh.open(store, ".", kTilemapRel));
        const std::optional<std::string> sidecar =
            store.read("tilemaps/tilemaps/level-1.ground.0.cells.bin");
        CHECK(sidecar.has_value());
        const filesync::SidecarDecodeResult decoded = filesync::decode_sidecar(*sidecar);
        CHECK(decoded.ok);
        const std::vector<std::uint32_t> cells = tme::decode_cell_payload(decoded.payload);
        CHECK(cells.size() == 64); // the 8x8 chunk
        CHECK(cells[0] == 2U);     // (0, 0) painted
        CHECK(cells[1 * 8 + 1] == 2U);
        CHECK(cells[2 * 8 + 2] == 3U); // the fill rect
        CHECK(cells[4 * 8 + 4] == 3U);
        CHECK(cells[7 * 8 + 7] == 0U); // untouched cells stay empty
    }

    remove_quiet(stage_gui);
    remove_quiet(stage_cli);
    return g_failures == 0 ? 0 : 1;
}
