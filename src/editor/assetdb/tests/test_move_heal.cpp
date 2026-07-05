// Move/rename + raw-move healing matrices (R-QA-013 heavy coverage): tool-driven moves, raw-FS
// moves, crash windows around the R-FILE-004 meta-first write order (R-QA-010 fault injection),
// duplicate-GUID observability, ambiguity refusal, and idempotent re-runs under partial apply.

#include "context/editor/assetdb/asset_database.h"

#include "context/editor/filesync/file_store.h"

#include "assetdb_test.h"

#include <string>
#include <string_view>
#include <vector>

using namespace context::editor::assetdb;
namespace filesync = context::editor::filesync;

namespace
{

void put_asset(filesync::FileStore& fs, std::string_view path, std::string_view bytes,
               std::string_view guid, std::string_view kind)
{
    fs.write(path, bytes);
    AssetMeta meta;
    meta.guid = std::string(guid);
    meta.kind = std::string(kind);
    fs.write(meta_path_for(path), serialize_meta(meta));
}

bool has_diag(const std::vector<AssetDiagnostic>& diags, std::string_view code)
{
    for (const AssetDiagnostic& d : diags)
        if (d.code == code)
            return true;
    return false;
}

bool has_action(const std::vector<HealAction>& actions, std::string_view action,
                std::string_view asset_path = "")
{
    for (const HealAction& a : actions)
        if (a.action == action && (asset_path.empty() || a.asset_path == asset_path))
            return true;
    return false;
}

[[nodiscard]] std::string guid_at(const filesync::FileStore& fs, std::string_view asset_path)
{
    const auto bytes = fs.read(meta_path_for(asset_path));
    if (!bytes.has_value())
        return "";
    std::vector<std::string> problems;
    const auto meta = parse_meta(*bytes, problems);
    return meta.has_value() ? meta->guid : "";
}

constexpr std::string_view kGuidA = "00000000000000000000000000000aaa";
constexpr std::string_view kGuidB = "00000000000000000000000000000bbb";

// A sidecar with custom import settings + an unknown member: moves must carry it VERBATIM.
constexpr std::string_view kRichMeta =
    "{\n"
    "  \"$schema\": \"ctx:meta\",\n"
    "  \"futureField\": true,\n"
    "  \"guid\": \"00000000000000000000000000000aaa\",\n"
    "  \"importSettings\": {\n"
    "    \"compression\": \"bc7\"\n"
    "  },\n"
    "  \"kind\": \"ctx:texture\",\n"
    "  \"platforms\": {},\n"
    "  \"version\": 1\n"
    "}\n";

} // namespace

int main()
{
    // ============================== tool-driven move/rename ======================================

    // --- happy move: identity + sidecar bytes travel; source side fully gone ----------------------
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/a/tex.png", "PIXELS");
        fs.write("proj/a/tex.png.meta.json", kRichMeta);
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");

        const MoveResult moved = db.move_asset(fs, "proj/a/tex.png", "proj/b/tex.png");
        CHECK(moved.ok);
        CHECK(moved.guid == kGuidA);
        CHECK(moved.diagnostics.empty());
        CHECK(fs.exists("proj/b/tex.png"));
        CHECK(*fs.read("proj/b/tex.png") == "PIXELS");
        CHECK(!fs.exists("proj/a/tex.png"));
        CHECK(!fs.exists("proj/a/tex.png.meta.json"));
        // Sidecar bytes travel VERBATIM: import settings AND unknown members survive (L-36).
        CHECK(*fs.read("proj/b/tex.png.meta.json") == kRichMeta);
        // The index followed the move.
        CHECK(db.find_by_path("proj/a/tex.png") == nullptr);
        const AssetRecord* rec = db.find_by_guid(kGuidA);
        CHECK(rec != nullptr);
        CHECK(rec->path == "proj/b/tex.png");
        CHECK(rec->kind == "ctx:texture");
    }

    // --- rename in place (same directory) — the same engine operation -----------------------------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/s/old.json", "{}\n", kGuidA, "ctx:scene");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const MoveResult renamed = db.move_asset(fs, "proj/s/old.json", "proj/s/new.json");
        CHECK(renamed.ok);
        CHECK(renamed.guid == kGuidA);
        CHECK(guid_at(fs, "proj/s/new.json") == kGuidA); // identity survives the rename
        CHECK(!fs.exists("proj/s/old.json.meta.json"));
    }

    // --- moving a meta-less asset mints identity at move time -------------------------------------
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/loose.json", "{\"$schema\": \"ctx:scene\", \"version\": 1}\n");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const MoveResult moved = db.move_asset(fs, "proj/loose.json", "proj/kept.json");
        CHECK(moved.ok);
        CHECK(is_guid(moved.guid));
        CHECK(guid_at(fs, "proj/kept.json") == moved.guid);
        const AssetRecord* rec = db.find_by_path("proj/kept.json");
        CHECK(rec != nullptr);
        CHECK(rec->kind == "ctx:scene"); // sniffed from the $schema header at mint time
    }

    // --- failure paths: missing source, occupied destination, malformed requests ------------------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/a.json", "A\n", kGuidA, "");
        put_asset(fs, "proj/b.json", "B\n", kGuidB, "");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");

        const MoveResult missing = db.move_asset(fs, "proj/nope.json", "proj/x.json");
        CHECK(!missing.ok);
        CHECK(has_diag(missing.diagnostics, "asset.move_source_missing"));

        // CAS-honesty: a DIFFERENT asset occupies the destination — refused, nothing written.
        const MoveResult occupied = db.move_asset(fs, "proj/a.json", "proj/b.json");
        CHECK(!occupied.ok);
        CHECK(has_diag(occupied.diagnostics, "asset.move_destination_exists"));
        CHECK(*fs.read("proj/a.json") == "A\n");
        CHECK(*fs.read("proj/b.json") == "B\n");
        CHECK(guid_at(fs, "proj/a.json") == kGuidA);
        CHECK(guid_at(fs, "proj/b.json") == kGuidB);

        // Sidecar paths are not movable subjects.
        const MoveResult meta_move =
            db.move_asset(fs, "proj/a.json.meta.json", "proj/c.json.meta.json");
        CHECK(!meta_move.ok);
        CHECK(has_diag(meta_move.diagnostics, "asset.move_invalid"));

        // A no-op move (from == to) converges trivially.
        const MoveResult noop = db.move_asset(fs, "proj/a.json", "proj/a.json");
        CHECK(noop.ok);
        CHECK(noop.guid == kGuidA);

        // A malformed-but-present source sidecar is NEVER re-keyed or discarded: the move refuses
        // with asset.meta_invalid and leaves both files untouched (repair — by hand or from git —
        // is the user's call, R-FILE-003).
        fs.write("proj/broken.json", "B2\n");
        fs.write("proj/broken.json.meta.json", "not json");
        const MoveResult broken = db.move_asset(fs, "proj/broken.json", "proj/elsewhere.json");
        CHECK(!broken.ok);
        CHECK(has_diag(broken.diagnostics, "asset.meta_invalid"));
        CHECK(!fs.exists("proj/elsewhere.json"));
        CHECK(*fs.read("proj/broken.json") == "B2\n");
        CHECK(*fs.read("proj/broken.json.meta.json") == "not json"); // the bytes survive

        // The malformed-DESTINATION twin: a byte-identical destination with an unparseable
        // sidecar must NOT be mistaken for our own window-B residue (that window is defined by
        // a MISSING destination sidecar) — refused, the unparseable bytes survive.
        fs.write("proj/lookalike.json", "A\n"); // byte-identical to a.json
        fs.write("proj/lookalike.json.meta.json", "corrupt dest");
        const MoveResult dest_broken = db.move_asset(fs, "proj/a.json", "proj/lookalike.json");
        CHECK(!dest_broken.ok);
        CHECK(has_diag(dest_broken.diagnostics, "asset.meta_invalid"));
        CHECK(*fs.read("proj/lookalike.json.meta.json") == "corrupt dest"); // bytes survive
        CHECK(fs.exists("proj/a.json"));
        CHECK(guid_at(fs, "proj/a.json") == kGuidA); // source untouched

        // An orphaned sidecar squatting at the destination (asset file gone) holds some asset's
        // identity — never overwritten by the write order, refused instead.
        AssetMeta squatter;
        squatter.guid = "00000000000000000000000000000ddd";
        fs.write("proj/free.json.meta.json", serialize_meta(squatter)); // no proj/free.json
        const MoveResult squat = db.move_asset(fs, "proj/a.json", "proj/free.json");
        CHECK(!squat.ok);
        CHECK(has_diag(squat.diagnostics, "asset.move_destination_exists"));
        CHECK(guid_at(fs, "proj/free.json") == "00000000000000000000000000000ddd"); // survives
        CHECK(fs.exists("proj/a.json"));

        // Non-candidate endpoints are refused: dot-tree internals and temp-residue shapes are
        // outside the asset domain (the index could never see the resulting pair).
        const MoveResult into_dot = db.move_asset(fs, "proj/a.json", "proj/.editor/a.json");
        CHECK(!into_dot.ok);
        CHECK(has_diag(into_dot.diagnostics, "asset.move_invalid"));
        const MoveResult onto_tmp = db.move_asset(fs, "proj/a.json", "proj/a2.json.tmp");
        CHECK(!onto_tmp.ok);
        CHECK(has_diag(onto_tmp.diagnostics, "asset.move_invalid"));
        CHECK(fs.exists("proj/a.json")); // and nothing moved
    }

    // --- converged state with a MALFORMED destination sidecar: residue-clearing must not clobber --
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/a/l.json.meta.json", kRichMeta); // source sidecar left behind (file gone)
        fs.write("proj/b/l.json", "{}\n");              // destination file present
        fs.write("proj/b/l.json.meta.json", "corrupt"); // malformed destination sidecar
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const MoveResult conv = db.move_asset(fs, "proj/a/l.json", "proj/b/l.json");
        CHECK(!conv.ok);
        CHECK(has_diag(conv.diagnostics, "asset.meta_invalid"));
        CHECK(*fs.read("proj/b/l.json.meta.json") == "corrupt"); // bytes survive
        CHECK(fs.exists("proj/a/l.json.meta.json"));             // source residue untouched
    }

    // ================== crash windows around the meta-first order (R-QA-010) =====================

    // --- window A: crash BEFORE the destination file lands (rename to `to` throws) -----------------
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/a/lvl.json", "{}\n");
        fs.write("proj/a/lvl.json.meta.json", kRichMeta);
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");

        fs.crash_on_rename_to("proj/b/lvl.json");
        bool crashed = false;
        try
        {
            (void)db.move_asset(fs, "proj/a/lvl.json", "proj/b/lvl.json");
        }
        catch (const filesync::SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);
        // GUID identity is UNTOUCHED at the source: the observed mid-state is safe (R-FILE-004).
        CHECK(fs.exists("proj/a/lvl.json"));
        CHECK(guid_at(fs, "proj/a/lvl.json") == kGuidA);
        CHECK(!fs.exists("proj/b/lvl.json"));
        CHECK(!fs.exists("proj/b/lvl.json.meta.json"));

        // Re-run converges (idempotent + re-runnable under partial apply).
        AssetDatabase db2(guids);
        db2.scan(fs, "proj");
        const MoveResult rerun = db2.move_asset(fs, "proj/a/lvl.json", "proj/b/lvl.json");
        CHECK(rerun.ok);
        CHECK(rerun.guid == kGuidA);
        CHECK(!fs.exists("proj/a/lvl.json"));
        CHECK(!fs.exists("proj/a/lvl.json.meta.json"));
        CHECK(*fs.read("proj/b/lvl.json.meta.json") == kRichMeta);
    }

    // --- window B: destination file landed, crash BEFORE the destination sidecar ------------------
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/a/lvl.json", "{}\n");
        fs.write("proj/a/lvl.json.meta.json", kRichMeta);
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");

        fs.crash_on_rename_to("proj/b/lvl.json.meta.json");
        bool crashed = false;
        try
        {
            (void)db.move_asset(fs, "proj/a/lvl.json", "proj/b/lvl.json");
        }
        catch (const filesync::SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);
        // Mid-state: both files, ONE sidecar — identity still uniquely at the source (L-36).
        CHECK(fs.exists("proj/a/lvl.json"));
        CHECK(fs.exists("proj/b/lvl.json"));
        CHECK(guid_at(fs, "proj/a/lvl.json") == kGuidA);
        CHECK(!fs.exists("proj/b/lvl.json.meta.json"));

        // Re-run detects its own residue (same bytes, sidecar-less destination) and converges.
        AssetDatabase db2(guids);
        db2.scan(fs, "proj");
        const MoveResult rerun = db2.move_asset(fs, "proj/a/lvl.json", "proj/b/lvl.json");
        CHECK(rerun.ok);
        CHECK(rerun.guid == kGuidA);
        CHECK(!fs.exists("proj/a/lvl.json"));
        CHECK(!fs.exists("proj/a/lvl.json.meta.json"));
        CHECK(guid_at(fs, "proj/b/lvl.json") == kGuidA);
    }

    // --- window C: destination COMPLETE (file + sidecar), crash before source removal -------------
    {
        filesync::MemoryFileStore fs;
        // Constructed post-step-4 state: the duplicate-GUID window the meta-first order creates.
        fs.write("proj/a/lvl.json", "{}\n");
        fs.write("proj/a/lvl.json.meta.json", kRichMeta);
        fs.write("proj/b/lvl.json", "{}\n");
        fs.write("proj/b/lvl.json.meta.json", kRichMeta);

        // The window is OBSERVABLE and diagnosed, never silent (R-FILE-003 honesty).
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        const ScanResult scan = db.scan(fs, "proj");
        CHECK(has_diag(scan.diagnostics, "asset.guid_duplicate"));

        // Re-running the verb recognizes its own identity at the destination and converges.
        const MoveResult rerun = db.move_asset(fs, "proj/a/lvl.json", "proj/b/lvl.json");
        CHECK(rerun.ok);
        CHECK(rerun.guid == kGuidA);
        CHECK(!fs.exists("proj/a/lvl.json"));
        CHECK(!fs.exists("proj/a/lvl.json.meta.json"));
        CHECK(guid_at(fs, "proj/b/lvl.json") == kGuidA);
        // Converged clean: a fresh scan sees exactly one asset, no diagnostics.
        AssetDatabase db2(guids);
        const ScanResult after = db2.scan(fs, "proj");
        CHECK(after.assets_indexed == 1);
        CHECK(after.diagnostics.empty());
    }

    // --- window D: only the source SIDECAR remains (crash between the two source removals) --------
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/a/lvl.json.meta.json", kRichMeta); // orphaned source sidecar
        fs.write("proj/b/lvl.json", "{}\n");
        fs.write("proj/b/lvl.json.meta.json", kRichMeta);

        // Path 1 — re-running the verb converges.
        {
            filesync::MemoryFileStore fs2;
            fs2.write("proj/a/lvl.json.meta.json", kRichMeta);
            fs2.write("proj/b/lvl.json", "{}\n");
            fs2.write("proj/b/lvl.json.meta.json", kRichMeta);
            SequenceGuidGenerator guids;
            AssetDatabase db(guids);
            db.scan(fs2, "proj");
            const MoveResult rerun = db.move_asset(fs2, "proj/a/lvl.json", "proj/b/lvl.json");
            CHECK(rerun.ok);
            CHECK(rerun.guid == kGuidA);
            CHECK(!fs2.exists("proj/a/lvl.json.meta.json"));
        }

        // Path 2 — the healing pass removes the residue (rule 0: GUID already live elsewhere).
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const HealResult healed = db.heal_moves(fs, "proj");
        CHECK(has_action(healed.actions, "meta-residue-removed"));
        CHECK(!fs.exists("proj/a/lvl.json.meta.json"));
        CHECK(guid_at(fs, "proj/b/lvl.json") == kGuidA);
        // Idempotent: nothing left on the second pass.
        const HealResult again = db.heal_moves(fs, "proj");
        CHECK(again.actions.empty());
        CHECK(again.diagnostics.empty());
    }

    // ========================= raw-FS moves (the watcher healing path) ===========================

    // --- raw move of asset + sidecar together: identity stable, nothing to write ------------------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/old/level.json", "{}\n", kGuidA, "ctx:scene");
        // The user did `mv old new` — both files moved.
        fs.rename("proj/old/level.json", "proj/new/level.json");
        fs.rename("proj/old/level.json.meta.json", "proj/new/level.json.meta.json");

        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        const ScanResult scan = db.scan(fs, "proj");
        CHECK(scan.assets_indexed == 1);
        CHECK(scan.diagnostics.empty());
        const AssetRecord* rec = db.find_by_guid(kGuidA);
        CHECK(rec != nullptr);
        CHECK(rec->path == "proj/new/level.json"); // same GUID at the new path: a MOVE, not a swap
        const HealResult healed = db.heal_moves(fs, "proj");
        CHECK(healed.actions.empty()); // nothing to heal
        CHECK(healed.diagnostics.empty());
    }

    // --- raw move of the asset ONLY (sidecar left behind): healed by unique basename match --------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/old/level.json", "{}\n", kGuidA, "ctx:scene");
        fs.write("proj/old/level.json.meta.json", kRichMeta); // enrich to prove verbatim carry
        fs.rename("proj/old/level.json", "proj/new/level.json"); // meta NOT moved

        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const HealResult healed = db.heal_moves(fs, "proj");
        CHECK(has_action(healed.actions, "meta-moved", "proj/new/level.json"));
        CHECK(healed.diagnostics.empty());
        CHECK(!fs.exists("proj/old/level.json.meta.json"));
        CHECK(*fs.read("proj/new/level.json.meta.json") == kRichMeta); // verbatim (settings kept)
        const AssetRecord* rec = db.find_by_guid(kGuidA);
        CHECK(rec != nullptr);
        CHECK(rec->path == "proj/new/level.json");
        // Idempotent: a second pass finds nothing.
        CHECK(db.heal_moves(fs, "proj").actions.empty());
    }

    // --- raw rename in place (basename changed): healed as the directory's sole pair --------------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/s/alpha.json", "{}\n", kGuidA, "ctx:scene");
        fs.rename("proj/s/alpha.json", "proj/s/beta.json"); // meta NOT renamed

        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const HealResult healed = db.heal_moves(fs, "proj");
        CHECK(has_action(healed.actions, "meta-moved", "proj/s/beta.json"));
        CHECK(guid_at(fs, "proj/s/beta.json") == kGuidA); // identity survived the raw rename
        CHECK(!fs.exists("proj/s/alpha.json.meta.json"));
    }

    // --- ambiguity: two same-basename orphans, one newcomer — refused, never guessed ---------------
    {
        filesync::MemoryFileStore fs;
        AssetMeta meta_a;
        meta_a.guid = std::string(kGuidA);
        fs.write("proj/a/x.json.meta.json", serialize_meta(meta_a)); // orphan 1
        AssetMeta meta_b;
        meta_b.guid = std::string(kGuidB);
        fs.write("proj/b/x.json.meta.json", serialize_meta(meta_b)); // orphan 2
        fs.write("proj/c/x.json", "{}\n");                           // one newcomer

        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const HealResult healed = db.heal_moves(fs, "proj");
        CHECK(healed.actions.empty()); // no writes on ambiguity
        CHECK(has_diag(healed.diagnostics, "asset.heal_ambiguous"));
        CHECK(fs.exists("proj/a/x.json.meta.json")); // both orphans untouched
        CHECK(fs.exists("proj/b/x.json.meta.json"));
        CHECK(!fs.exists("proj/c/x.json.meta.json"));
    }

    // --- an orphaned DOT-TREE sidecar never pairs onto a genuine newcomer (out of domain) ----------
    {
        filesync::MemoryFileStore fs;
        AssetMeta internal;
        internal.guid = std::string(kGuidA);
        fs.write("proj/.editor/cache/level.json.meta.json", serialize_meta(internal)); // orphan
        fs.write("proj/scenes/level.json", "{}\n"); // genuine newcomer, same basename
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const HealResult healed = db.heal_moves(fs, "proj");
        CHECK(healed.actions.empty());     // no identity injection from engine-internal residue
        CHECK(healed.diagnostics.empty()); // out of domain means out of diagnostics too
        CHECK(fs.exists("proj/.editor/cache/level.json.meta.json")); // untouched
        CHECK(!fs.exists("proj/scenes/level.json.meta.json"));
        const HealResult created = db.ensure_metas(fs, "proj");
        CHECK(has_action(created.actions, "meta-created", "proj/scenes/level.json"));
        CHECK(guid_at(fs, "proj/scenes/level.json") != kGuidA); // fresh identity, not injected
    }

    // --- pure delete: orphan reported, sidecar NOT auto-removed (enumerated write surface) ---------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/gone.json", "{}\n", kGuidA, "ctx:scene");
        fs.remove("proj/gone.json"); // raw delete, sidecar left

        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const HealResult healed = db.heal_moves(fs, "proj");
        CHECK(healed.actions.empty());
        CHECK(has_diag(healed.diagnostics, "asset.meta_orphaned"));
        CHECK(fs.exists("proj/gone.json.meta.json")); // cleanup belongs to `validate --fix`
    }

    // --- heal BEFORE create: a healable newcomer keeps its GUID; a truly-new file mints fresh ------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/old/level.json", "{}\n", kGuidA, "ctx:scene");
        fs.rename("proj/old/level.json", "proj/new/level.json"); // raw move, meta left
        fs.write("proj/new/brand.json", "{}\n");                 // genuinely new asset

        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const HealResult healed = db.heal_moves(fs, "proj");
        CHECK(has_action(healed.actions, "meta-moved", "proj/new/level.json"));
        const HealResult created = db.ensure_metas(fs, "proj");
        CHECK(has_action(created.actions, "meta-created", "proj/new/brand.json"));
        CHECK(guid_at(fs, "proj/new/level.json") == kGuidA); // healed: OLD identity
        const std::string fresh = guid_at(fs, "proj/new/brand.json");
        CHECK(is_guid(fresh));
        CHECK(fresh != kGuidA); // minted: NEW identity
    }

    ASSETDB_TEST_MAIN_END();
}
