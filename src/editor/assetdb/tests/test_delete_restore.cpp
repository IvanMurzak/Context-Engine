// The M9 e2 DELETE / RESTORE engine operation (R-QA-013 heavy coverage, D10 write half): the
// quarantine-backed happy path, the reference refusal AND its producible sibling, the R-FILE-004
// dependency-safe removal order under R-QA-010 crash injection, idempotence + resume under partial
// apply, and the byte-identical undo round-trip the task's Definition of Done names.
//
// EVERY "X DID NOT HAPPEN" CLAIM HERE CARRIES A PRODUCIBLE SIBLING (the task's own rule): the
// referenced-refusal case is paired with the SAME fixture deleting cleanly once the reference is
// gone, and each never-overwrite refusal is paired with the write it does perform when the
// destination is genuinely free. A refusal test whose fixture could not have succeeded anyway
// proves nothing about the refusal.

#include "context/editor/assetdb/asset_database.h"

#include "context/editor/assetdb/meta.h"
#include "context/editor/assetdb/ref_heal.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/json_parse.h"

#include "assetdb_test.h"

#include <string>
#include <string_view>
#include <vector>

using namespace context::editor::assetdb;
namespace filesync = context::editor::filesync;
namespace schema = context::editor::schema;

namespace
{

constexpr std::string_view kGuidA = "00000000000000000000000000000aaa";
constexpr std::string_view kGuidB = "00000000000000000000000000000bbb";

// A sidecar with import settings + an unknown member: a restore must bring these back VERBATIM, not
// a freshly-serialized equivalent (the byte-identity claim is about bytes, not about semantics).
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

// A schema set with ONE reference-bearing kind, so a referring document is genuinely schema-bound —
// the only way find_referrers can see it (reference work is schema-driven).
const schema::SchemaSet& test_set()
{
    static const schema::SchemaSet set = []
    {
        schema::SchemaSet s;
        std::vector<std::string> problems;
        auto kind = schema::compile_kind_schema(R"({
            "$id": "test:scene",
            "version": 1,
            "type": "object",
            "properties": {
                "notes": {"description": "blessed"},
                "texture": {"x-ctx-ref": "ctx:texture"}
            }
        })",
                                                problems);
        if (kind.has_value() && problems.empty())
            s.add(std::move(*kind));
        return s;
    }();
    return set;
}

// An EMPTY schema set: nothing is schema-bound, so nothing is a visible referrer. This is the
// honest model of "the sweep cannot see it" — used to prove the delete proceeds there (and that the
// refusal above is caused by the reference, not by the fixture).
const schema::SchemaSet& empty_set()
{
    static const schema::SchemaSet set;
    return set;
}

[[nodiscard]] std::string ref_doc(std::string_view guid, std::string_view path)
{
    std::string doc = "{\"$schema\":\"test:scene\",\"version\":1,\"texture\":{";
    bool first = true;
    if (!guid.empty())
    {
        doc += "\"$ref\":\"" + std::string(guid) + "\"";
        first = false;
    }
    if (!path.empty())
    {
        if (!first)
            doc += ",";
        doc += "\"path\":\"" + std::string(path) + "\"";
    }
    doc += "}}\n";
    return doc;
}

} // namespace

int main()
{
    // The fixture's own precondition. A schema that silently failed to compile would make EVERY
    // reference refusal below pass for the wrong reason (nothing is schema-bound, so nothing is a
    // referrer) — the exact vacuity this suite's sibling-pairs exist to rule out, so it is asserted
    // rather than assumed.
    CHECK(test_set().latest("test:scene") != nullptr);
    CHECK(empty_set().latest("test:scene") == nullptr);

    // ================================ the happy path =============================================
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/art/hero.png", "PNGBYTES", kGuidA, "ctx:texture");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        CHECK(db.scan(fs, "proj").assets_indexed == 1);

        const DeleteResult r = db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set());
        CHECK(r.ok);
        CHECK(r.removed_asset);
        CHECK(r.removed_meta);
        CHECK(r.guid == kGuidA);
        // The token is the asset's OWN guid — deterministic, so a crashed re-run reuses the entry.
        CHECK(r.restore_token == std::string(kGuidA));

        // File + meta gone, index updated (the DoD's happy path, all three halves).
        CHECK(!fs.exists("proj/art/hero.png"));
        CHECK(!fs.exists(meta_path_for("proj/art/hero.png")));
        CHECK(db.find_by_path("proj/art/hero.png") == nullptr);
        CHECK(db.find_by_guid(kGuidA) == nullptr);

        // The bytes are QUARANTINED, not destroyed — and under a dot-segment path, so they are
        // outside the asset domain and a later scan can never re-index or re-key them.
        CHECK(fs.read(trash_asset_path(kGuidA)) == std::optional<std::string>("PNGBYTES"));
        CHECK(fs.exists(trash_entry_path(kGuidA)));
        CHECK(!is_asset_candidate(trash_asset_path(kGuidA)));
        AssetDatabase rescanned(guids);
        CHECK(rescanned.scan(fs, "proj").assets_indexed == 0);
    }

    // ======================= the undo round-trip: BYTE-IDENTICAL restore =========================
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/art/hero.png", "PNGBYTES");
        fs.write(meta_path_for("proj/art/hero.png"), kRichMeta); // import settings + unknown member
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        CHECK(db.scan(fs, "proj").assets_indexed == 1);

        const DeleteResult deleted = db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set());
        CHECK(deleted.ok);
        CHECK(!fs.exists("proj/art/hero.png"));

        const RestoreResult restored = db.restore_asset(fs, deleted.restore_token);
        CHECK(restored.ok);
        CHECK(restored.restored_asset);
        CHECK(restored.restored_meta);
        CHECK(restored.path == "proj/art/hero.png");
        CHECK(restored.guid == std::string(kGuidA));

        // BYTE-identical, both halves — compared as bytes, not as parsed values, so a sidecar
        // round-tripped through the serializer (which would lose `futureField`'s position, and could
        // lose the member entirely) fails here.
        CHECK(fs.read("proj/art/hero.png") == std::optional<std::string>("PNGBYTES"));
        CHECK(fs.read(meta_path_for("proj/art/hero.png")) == std::optional<std::string>(kRichMeta));
        // Identity is back in the index, and the quarantine entry is consumed.
        CHECK(db.find_by_path("proj/art/hero.png") != nullptr);
        CHECK(!fs.exists(trash_entry_path(deleted.restore_token)));
        CHECK(!fs.exists(trash_asset_path(deleted.restore_token)));

        // A SECOND restore of the same token is honestly refused — never a silent "ok" that would
        // tell the human a file came back when nothing happened.
        const RestoreResult again = db.restore_asset(fs, deleted.restore_token);
        CHECK(!again.ok);
        CHECK(has_diag(again.diagnostics, "asset.restore_missing"));
    }

    // ================== references: the REFUSAL and its producible sibling =======================
    {
        // (a) REFUSAL — a schema-bound document holds a $ref to this asset's GUID.
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/art/hero.png", "PNGBYTES", kGuidA, "ctx:texture");
        put_asset(fs, "proj/scenes/main.json", ref_doc(kGuidA, ""), kGuidB, "test:scene");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        CHECK(db.scan(fs, "proj").assets_indexed == 2);

        const DeleteResult refused = db.delete_asset(fs, "proj", "proj/art/hero.png", test_set());
        CHECK(!refused.ok);
        CHECK(has_diag(refused.diagnostics, "asset.delete_referenced"));
        // The diagnostic NAMES the referrer (path + pointer), which is the whole point of refusing
        // instead of dangling.
        bool named = false;
        for (const AssetDiagnostic& d : refused.diagnostics)
            if (d.code == "asset.delete_referenced" && d.other_path == "proj/scenes/main.json" &&
                d.message.find("/texture") != std::string::npos)
                named = true;
        CHECK(named);
        // Nothing moved: not one byte of a refused destructive operation lands.
        CHECK(fs.exists("proj/art/hero.png"));
        CHECK(fs.exists(meta_path_for("proj/art/hero.png")));
        CHECK(!fs.exists(trash_entry_path(kGuidA)));

        // (b) THE PRODUCIBLE SIBLING — the SAME fixture, same schema set, same asset: remove the
        // reference and the delete goes through. Without this half, (a) could be passing because
        // the fixture is undeletable for some unrelated reason.
        fs.write("proj/scenes/main.json", "{\"$schema\":\"test:scene\",\"version\":1}\n");
        const DeleteResult allowed = db.delete_asset(fs, "proj", "proj/art/hero.png", test_set());
        CHECK(allowed.ok);
        CHECK(allowed.removed_asset);
        CHECK(!fs.exists("proj/art/hero.png"));
    }
    {
        // A PATH-ONLY reference (L-34's unresolved form) blocks the delete too — otherwise a delete
        // racing the resolution pass would dangle a reference that was about to become
        // authoritative. Sibling: the same document pointing at a DIFFERENT path does not block.
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/art/hero.png", "PNGBYTES", kGuidA, "ctx:texture");
        put_asset(fs, "proj/scenes/main.json", ref_doc("", "proj/art/hero.png"), kGuidB, "test:scene");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        CHECK(db.scan(fs, "proj").assets_indexed == 2);
        CHECK(!db.delete_asset(fs, "proj", "proj/art/hero.png", test_set()).ok);

        fs.write("proj/scenes/main.json", ref_doc("", "proj/art/other.png"));
        CHECK(db.delete_asset(fs, "proj", "proj/art/hero.png", test_set()).ok);
    }
    {
        // WHAT THE SWEEP CANNOT SEE, proven rather than asserted: the identical referring document
        // under a schema set that binds NO kind is invisible, and the delete proceeds. This is the
        // honest half of decision 2 — deleting past an invisible reference dangles it, and the
        // existing asset.ref_dangling finding is what surfaces that afterwards.
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/art/hero.png", "PNGBYTES", kGuidA, "ctx:texture");
        put_asset(fs, "proj/scenes/main.json", ref_doc(kGuidA, ""), kGuidB, "test:scene");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        CHECK(db.scan(fs, "proj").assets_indexed == 2);
        CHECK(db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set()).ok);

        // ...and the dangle IS observable through the existing check, so the documented follow-up
        // is real machinery rather than a promise.
        AssetDatabase after(guids);
        (void)after.scan(fs, "proj");
        const auto parsed = context::editor::serializer::parse_json(*fs.read("proj/scenes/main.json"));
        CHECK(parsed.ok);
        const std::vector<RefFinding> findings =
            check_document_refs(parsed.root, test_set(), after);
        bool dangling = false;
        for (const RefFinding& f : findings)
            if (f.code == "asset.ref_dangling")
                dangling = true;
        CHECK(dangling);
    }

    // ================ idempotence + resume under partial apply (R-FILE-004) ======================
    {
        // Crash between the two source removals (asset gone, sidecar still there) — the state the
        // removal ORDER is chosen to produce. scan() calls it asset.meta_orphaned; a re-run of the
        // delete COMPLETES it.
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/art/hero.png", "PNGBYTES", kGuidA, "ctx:texture");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        (void)db.scan(fs, "proj");

        fs.crash_on_remove(meta_path_for("proj/art/hero.png"));
        bool crashed = false;
        try
        {
            (void)db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set());
        }
        catch (const filesync::SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);
        // The half-applied state, exactly as designed: asset gone, sidecar orphaned, bytes safe.
        CHECK(!fs.exists("proj/art/hero.png"));
        CHECK(fs.exists(meta_path_for("proj/art/hero.png")));
        CHECK(fs.exists(trash_asset_path(kGuidA)));

        // A fresh scan sees the recognised residue rather than a mystery.
        AssetDatabase observer(guids);
        const ScanResult scanned = observer.scan(fs, "proj");
        CHECK(scanned.assets_indexed == 0);
        CHECK(has_diag(scanned.diagnostics, "asset.meta_orphaned"));

        // Re-running the delete completes it, and still reports the restore handle.
        const DeleteResult resumed = db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set());
        CHECK(resumed.ok);
        CHECK(!resumed.removed_asset); // already gone
        CHECK(resumed.removed_meta);   // this call finished the job
        CHECK(resumed.restore_token == std::string(kGuidA));
        CHECK(!fs.exists(meta_path_for("proj/art/hero.png")));

        // And the interrupted delete is still fully RECOVERABLE — the DoD's "recoverable per
        // decision 1" clause, proven by restoring both halves after the crash + resume.
        const RestoreResult back = db.restore_asset(fs, resumed.restore_token);
        CHECK(back.ok);
        CHECK(fs.read("proj/art/hero.png") == std::optional<std::string>("PNGBYTES"));
        CHECK(fs.exists(meta_path_for("proj/art/hero.png")));
    }
    {
        // Crash BEFORE the source removal (quarantine written, project untouched): the project is
        // unchanged and a re-run redoes the whole order into the SAME entry — no second copy.
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/art/hero.png", "PNGBYTES", kGuidA, "ctx:texture");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        (void)db.scan(fs, "proj");

        fs.crash_on_remove("proj/art/hero.png");
        try
        {
            (void)db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set());
        }
        catch (const filesync::SimulatedCrash&)
        {
        }
        CHECK(fs.exists("proj/art/hero.png")); // the project never saw the delete
        CHECK(fs.exists(meta_path_for("proj/art/hero.png")));

        const std::size_t files_after_crash = fs.file_count();
        const DeleteResult rerun = db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set());
        CHECK(rerun.ok);
        CHECK(rerun.restore_token == std::string(kGuidA));
        // Two files left (asset + meta), three quarantine files gained nothing new: the re-run
        // landed in the same entry rather than leaking a second quarantine directory.
        CHECK(fs.file_count() == files_after_crash - 2);
    }
    {
        // The fully-converged tail: deleting an already-deleted path is ok and a no-op, so a caller
        // that retries can tell "already in the requested state" from "this call did the work".
        filesync::MemoryFileStore fs;
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        const DeleteResult converged = db.delete_asset(fs, "proj", "proj/art/gone.png", empty_set());
        CHECK(converged.ok);
        CHECK(!converged.removed_asset);
        CHECK(!converged.removed_meta);
        CHECK(converged.restore_token.empty());
    }

    // ============================= meta-less assets (unknown = not enforced) =====================
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/art/raw.bin", "RAWBYTES"); // no sidecar at all
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        (void)db.scan(fs, "proj");

        const DeleteResult r = db.delete_asset(fs, "proj", "proj/art/raw.bin", empty_set());
        CHECK(r.ok);
        CHECK(r.removed_asset);
        CHECK(!r.removed_meta); // there was none to remove
        CHECK(!r.restore_token.empty());
        CHECK(!fs.exists("proj/art/raw.bin"));

        const RestoreResult back = db.restore_asset(fs, r.restore_token);
        CHECK(back.ok);
        CHECK(back.restored_asset);
        CHECK(!back.restored_meta);
        CHECK(fs.read("proj/art/raw.bin") == std::optional<std::string>("RAWBYTES"));
        // A meta-less asset comes back meta-LESS: restoring a sidecar it never had would not be a
        // byte-identical restore, it would be a silent authoring change.
        CHECK(!fs.exists(meta_path_for("proj/art/raw.bin")));
    }

    // ================================== refusals =================================================
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/art/hero.png", "PNGBYTES", kGuidA, "ctx:texture");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        (void)db.scan(fs, "proj");

        // A sidecar is not an asset; nor is a dot-tree internal, nor atomic-write residue.
        CHECK(has_diag(db.delete_asset(fs, "proj", meta_path_for("proj/art/hero.png"), empty_set()).diagnostics,
                       "asset.delete_invalid"));
        CHECK(has_diag(db.delete_asset(fs, "proj", "proj/.editor/session.json", empty_set()).diagnostics,
                       "asset.delete_invalid"));
        CHECK(has_diag(db.delete_asset(fs, "proj", "", empty_set()).diagnostics, "asset.delete_invalid"));
        // Refused means UNTOUCHED — the sibling that makes the three claims above non-vacuous.
        CHECK(fs.exists(meta_path_for("proj/art/hero.png")));
        CHECK(db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set()).ok);
    }
    {
        // A malformed sidecar refuses rather than quarantining bytes under a token nobody can name.
        filesync::MemoryFileStore fs;
        fs.write("proj/art/hero.png", "PNGBYTES");
        fs.write(meta_path_for("proj/art/hero.png"), "{ not json");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        (void)db.scan(fs, "proj");

        const DeleteResult refused = db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set());
        CHECK(!refused.ok);
        CHECK(has_diag(refused.diagnostics, "asset.meta_invalid"));
        CHECK(fs.exists("proj/art/hero.png"));

        // Sibling: repair the sidecar and the same delete goes through.
        AssetMeta meta;
        meta.guid = std::string(kGuidA);
        fs.write(meta_path_for("proj/art/hero.png"), serialize_meta(meta));
        CHECK(db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set()).ok);
    }
    {
        // RESTORE never overwrites: a different file took the path while the asset was deleted.
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/art/hero.png", "PNGBYTES", kGuidA, "ctx:texture");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        (void)db.scan(fs, "proj");
        const DeleteResult deleted = db.delete_asset(fs, "proj", "proj/art/hero.png", empty_set());
        CHECK(deleted.ok);

        fs.write("proj/art/hero.png", "SOMEONE-ELSES-BYTES");
        const RestoreResult refused = db.restore_asset(fs, deleted.restore_token);
        CHECK(!refused.ok);
        CHECK(has_diag(refused.diagnostics, "asset.restore_destination_exists"));
        CHECK(fs.read("proj/art/hero.png") == std::optional<std::string>("SOMEONE-ELSES-BYTES"));
        CHECK(fs.exists(trash_asset_path(deleted.restore_token))); // the bytes are still safe

        // Sibling: clear the squatter and the SAME restore lands.
        fs.remove("proj/art/hero.png");
        const RestoreResult allowed = db.restore_asset(fs, deleted.restore_token);
        CHECK(allowed.ok);
        CHECK(fs.read("proj/art/hero.png") == std::optional<std::string>("PNGBYTES"));
    }
    {
        // An unknown / cleared token is refused honestly, never silently "ok".
        filesync::MemoryFileStore fs;
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        CHECK(has_diag(db.restore_asset(fs, "").diagnostics, "asset.restore_missing"));
        CHECK(has_diag(db.restore_asset(fs, kGuidA).diagnostics, "asset.restore_missing"));
    }

    ASSETDB_TEST_MAIN_END();
}
