// Binary-sidecar authoring rules (L-33 / R-FILE-001/003/004, R-SEC-008, R-QA-010/013):
//   * header codec: encode/decode round-trip, version acceptance, bad-magic / truncated /
//     unsupported-version failures;
//   * the R-FILE-001 sidecar hash rule: raw-byte hash == canonical hash BY CONSTRUCTION (the magic
//     can never parse as JSON), proven through serializer::canonicalize;
//   * $sidecar resolution: owner-relative joins, and the R-SEC-008 jail refusing absolute paths
//     and `..` escapes;
//   * verification diagnostics: dangling ref, hash mismatch, header failures — and the orphan sweep;
//   * sidecar-FIRST write plans + owned-satellite move plans, executed through the R-FILE-004
//     intent-logged WriteQueue, with crash points between every durable step (R-QA-010 fault
//     seams) asserting resume-or-diagnose;
//   * the sidecar-aware reconcile pipeline: a changed/removed sidecar dirties its registered
//     owner(s) with a synthetic via_sidecar change.

#include "context/editor/filesync/sidecar.h"

#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/intent_log.h"
#include "context/editor/filesync/reconciler.h"
#include "context/editor/filesync/watcher.h"
#include "context/editor/serializer/canonical.h"
#include "context/kernel/platform.h"
#include "filesync_test.h"

#include <string>
#include <vector>

using namespace context::editor::filesync;

namespace
{

const std::string kKey = "unit-test-project-hmac-key-000002";

bool has_diag(const std::vector<SidecarDiagnostic>& diags, const std::string& code)
{
    for (const SidecarDiagnostic& d : diags)
        if (d.code == code)
            return true;
    return false;
}

bool has_recovery_code(const std::vector<Diagnostic>& diags, const std::string& code)
{
    for (const Diagnostic& d : diags)
        if (d.code == code)
            return true;
    return false;
}

// An owner document referencing `relpath` with the raw-byte hash of `sidecar_bytes`.
std::string owner_json(const std::string& relpath, const std::string& sidecar_bytes)
{
    return std::string{"{\"tiles\": {\"$sidecar\": \""} + relpath + "\", \"hash\": \"" +
           format_sidecar_hash(content_hash(sidecar_bytes)) + "\"}}";
}

// A two-ref owner document.
std::string owner_json2(const std::string& rel_a, const std::string& bytes_a,
                        const std::string& rel_b, const std::string& bytes_b)
{
    return std::string{"{\"a\": {\"$sidecar\": \""} + rel_a + "\", \"hash\": \"" +
           format_sidecar_hash(content_hash(bytes_a)) + "\"}, \"b\": {\"$sidecar\": \"" + rel_b +
           "\", \"hash\": \"" + format_sidecar_hash(content_hash(bytes_b)) + "\"}}";
}

bool has_change_path(const std::vector<ReconcileChange>& changes, const std::string& path)
{
    for (const ReconcileChange& change : changes)
        if (change.path == path)
            return true;
    return false;
}

} // namespace

int main()
{
    // =============================================================================================
    // A. codec: round-trip + header versioning (happy / edge / failure)
    // =============================================================================================
    {
        const std::string payload = std::string{"\x00\x01\xFF raw \n bytes", 16};
        const std::string encoded = encode_sidecar(payload);
        CHECK(encoded.size() == sidecar_header_size + payload.size());
        CHECK(is_sidecar_bytes(encoded));

        const SidecarDecodeResult decoded = decode_sidecar(encoded);
        CHECK(decoded.ok);
        CHECK(decoded.version == sidecar_format_version);
        CHECK(decoded.payload == payload);

        // An empty payload is a valid sidecar (header only). The input is named because the
        // result's `payload` view is only valid while the decoded bytes live (sidecar.h).
        const std::string header_only = encode_sidecar("");
        const SidecarDecodeResult empty = decode_sidecar(header_only);
        CHECK(empty.ok);
        CHECK(empty.payload.empty());
    }
    {
        // Failure paths: not a sidecar at all, truncated header, unsupported versions.
        CHECK(!is_sidecar_bytes(""));
        CHECK(!is_sidecar_bytes("{\"json\": true}"));
        CHECK(!is_sidecar_bytes("CTXSIDE")); // 7 bytes — shorter than the magic

        CHECK(decode_sidecar("not a sidecar").error_code == "sidecar.bad_magic");
        CHECK(decode_sidecar("").error_code == "sidecar.truncated"); // empty is a magic prefix
        CHECK(decode_sidecar("CTXS").error_code == "sidecar.truncated"); // proper magic prefix
        CHECK(decode_sidecar(std::string{sidecar_magic, sizeof sidecar_magic} + "\x01")
                  .error_code == "sidecar.truncated"); // magic ok, version cut short
        // A first byte off the magic is foreign, even at full header length.
        CHECK(decode_sidecar(std::string_view{"XTXSIDE\0____", 12}).error_code ==
              "sidecar.bad_magic");

        CHECK(decode_sidecar(encode_sidecar("p", 0)).error_code == "sidecar.unsupported_version");
        const SidecarDecodeResult newer =
            decode_sidecar(encode_sidecar("p", sidecar_format_version + 1));
        CHECK(!newer.ok);
        CHECK(newer.error_code == "sidecar.unsupported_version");
        CHECK(newer.version == sidecar_format_version + 1); // the offending version is reported
    }

    // =============================================================================================
    // B. R-FILE-001 sidecar hash rule: raw == canonical, by construction (serializer integration)
    // =============================================================================================
    {
        // Even a payload that IS valid JSON text cannot make the sidecar parse as JSON — the magic
        // ('C' + an embedded NUL) is unparseable, so canonicalize takes its non-JSON path: bytes
        // pass through verbatim and the canonical hash equals the raw-byte hash.
        for (const std::string& payload : {std::string{"{\"valid\": \"json\"}"},
                                           std::string{"\x89PNG\r\n binary"}, std::string{}})
        {
            const std::string encoded = encode_sidecar(payload);
            const auto canon = context::editor::serializer::canonicalize(encoded);
            CHECK(!canon.is_json);
            CHECK(canon.bytes == encoded); // no canonicalization pass for binaries
            CHECK(canon.canonical_hash == content_hash(encoded)); // raw ≡ canonical (L-33)
        }
    }

    // =============================================================================================
    // C. resolution + the R-SEC-008 jail on sidecar paths
    // =============================================================================================
    {
        // Owner-relative joins.
        CHECK(*resolve_sidecar_path("proj", "proj/scenes/level.json", "tiles.bin") ==
              "proj/scenes/tiles.bin");
        CHECK(*resolve_sidecar_path("proj", "proj/scenes/level.json", "meshes/rock.bin") ==
              "proj/scenes/meshes/rock.bin");
        // A `..` that stays inside the root is legal...
        CHECK(*resolve_sidecar_path("proj", "proj/scenes/level.json", "../shared/blob.bin") ==
              "proj/shared/blob.bin");
        // ...but escaping the root is a validation error (R-SEC-008).
        CHECK(!resolve_sidecar_path("proj", "proj/scenes/level.json", "../../evil.bin"));
        CHECK(!resolve_sidecar_path("proj", "proj/level.json", "../evil.bin"));
        // Absolute paths are refused outright, in both separator styles + drive-letter form.
        CHECK(!resolve_sidecar_path("proj", "proj/level.json", "/etc/passwd"));
        CHECK(!resolve_sidecar_path("proj", "proj/level.json", "\\windows\\system32"));
        CHECK(!resolve_sidecar_path("proj", "proj/level.json", "C:/secrets.bin"));
        CHECK(!resolve_sidecar_path("proj", "proj/level.json", "c:\\secrets.bin"));
        CHECK(!resolve_sidecar_path("proj", "proj/level.json", ""));
    }

    // =============================================================================================
    // D. scan: parse + collect + resolve in one pass
    // =============================================================================================
    {
        const std::string tiles = encode_sidecar("TILES");
        const std::string scene = owner_json("tiles.bin", tiles);
        const SidecarScan scan = scan_sidecar_refs("proj", "proj/scenes/level.json", scene);
        CHECK(scan.owner_parsed);
        CHECK(scan.diagnostics.empty());
        CHECK(scan.refs.size() == 1);
        CHECK(scan.refs[0].resolved_path == "proj/scenes/tiles.bin");
        CHECK(scan.refs[0].ref.hash == content_hash(tiles));
    }
    {
        // Non-JSON owner: no refs, not an error here (the parse diagnostic is derivation's).
        const SidecarScan scan = scan_sidecar_refs("proj", "proj/blob.bin", "not json at all");
        CHECK(!scan.owner_parsed);
        CHECK(scan.refs.empty());
        CHECK(scan.diagnostics.empty());
    }
    {
        // A malformed ref and a jail-escaping ref each surface as diagnostics, never as refs.
        const std::string scene = R"({
            "bad": {"$sidecar": "x.bin"},
            "escape": {"$sidecar": "../../evil.bin", "hash": "5"}
        })";
        const SidecarScan scan = scan_sidecar_refs("proj", "proj/scenes/level.json", scene);
        CHECK(scan.owner_parsed);
        CHECK(scan.refs.empty());
        CHECK(has_diag(scan.diagnostics, "sidecar.ref_malformed"));
        CHECK(has_diag(scan.diagnostics, "path.jail_violation"));
    }

    // =============================================================================================
    // E. verification: dangling / header failures / hash mismatch (R-FILE-003 diagnostics)
    // =============================================================================================
    {
        MemoryFileStore fs;
        const std::string good = encode_sidecar("GOOD");
        const std::string scene = owner_json2("good.bin", good, "gone.bin", "whatever");
        fs.write("proj/good.bin", good);
        // "gone.bin" is deliberately absent.

        const SidecarScan scan = scan_sidecar_refs("proj", "proj/level.json", scene);
        CHECK(scan.refs.size() == 2);
        const auto diags = verify_sidecar_refs(fs, "proj/level.json", scan.refs);
        CHECK(diags.size() == 1);
        CHECK(diags[0].code == "sidecar.dangling_ref");
        CHECK(diags[0].sidecar == "proj/gone.bin");
    }
    {
        MemoryFileStore fs;
        const std::string real = encode_sidecar("REAL");
        // The owner declares the hash of DIFFERENT bytes than what is on disk.
        const std::string scene = owner_json("tiles.bin", encode_sidecar("DECLARED"));
        fs.write("proj/tiles.bin", real);
        const SidecarScan scan = scan_sidecar_refs("proj", "proj/level.json", scene);
        const auto diags = verify_sidecar_refs(fs, "proj/level.json", scan.refs);
        CHECK(diags.size() == 1);
        CHECK(diags[0].code == "sidecar.hash_mismatch");
    }
    {
        MemoryFileStore fs;
        // On-disk bytes with no magic, and a newer-versioned sidecar: header codes surface.
        const std::string no_magic = "plain bytes";
        const std::string newer = encode_sidecar("P", sidecar_format_version + 1);
        const std::string scene = owner_json2("a.bin", no_magic, "b.bin", newer);
        fs.write("proj/a.bin", no_magic);
        fs.write("proj/b.bin", newer);
        const SidecarScan scan = scan_sidecar_refs("proj", "proj/level.json", scene);
        const auto diags = verify_sidecar_refs(fs, "proj/level.json", scan.refs);
        CHECK(has_diag(diags, "sidecar.bad_magic"));
        CHECK(has_diag(diags, "sidecar.unsupported_version"));
    }

    // =============================================================================================
    // F. index + orphan sweep
    // =============================================================================================
    {
        SidecarIndex index;
        index.set_owner_refs("proj/a.json", {"proj/s1.bin", "proj/s2.bin"});
        index.set_owner_refs("proj/b.json", {"proj/s2.bin"});
        CHECK(index.is_sidecar("proj/s1.bin"));
        CHECK(index.owners_of("proj/s2.bin").size() == 2);
        CHECK(index.sidecars_of("proj/a.json").size() == 2);

        // Replacing an owner's refs drops the stale reverse edges.
        index.set_owner_refs("proj/a.json", {"proj/s3.bin"});
        CHECK(!index.is_sidecar("proj/s1.bin"));
        CHECK(index.owners_of("proj/s2.bin").size() == 1);

        index.remove_owner("proj/b.json");
        CHECK(!index.is_sidecar("proj/s2.bin"));
        CHECK(index.owner_count() == 1);
    }
    {
        MemoryFileStore fs;
        SidecarIndex index;
        const std::string referenced = encode_sidecar("REF");
        const std::string orphan = encode_sidecar("ORPHAN");
        fs.write("proj/ref.bin", referenced);
        fs.write("proj/orphan.bin", orphan);
        fs.write("proj/plain.txt", "not a sidecar");
        fs.write("proj/.editor/intent/x.tmp", "control residue");
        index.set_owner_refs("proj/level.json", {"proj/ref.bin"});

        const auto diags = find_orphaned_sidecars(fs, "proj", index);
        CHECK(diags.size() == 1);
        CHECK(diags[0].code == "sidecar.orphaned");
        CHECK(diags[0].sidecar == "proj/orphan.bin");
    }

    // =============================================================================================
    // G. sidecar-first family write: plan shape, refusals, and the R-QA-010 crash window
    // =============================================================================================
    {
        MemoryFileStore fs;
        const std::string s1 = encode_sidecar("S1");
        const std::string s2 = encode_sidecar("S2");
        const std::string scene = owner_json2("s1.bin", s1, "meshes/s2.bin", s2);

        const SidecarPlan plan = plan_sidecar_family_write(
            fs, "proj", "proj/scenes/level.json", scene,
            {{"proj/scenes/s1.bin", s1}, {"proj/scenes/meshes/s2.bin", s2}});
        CHECK(plan.ok);
        CHECK(plan.steps.size() == 3);
        // Sidecar-FIRST: the owner write is the LAST step (L-33 write order).
        CHECK(plan.steps[0].path == "proj/scenes/s1.bin");
        CHECK(plan.steps[1].path == "proj/scenes/meshes/s2.bin");
        CHECK(plan.steps[2].path == "proj/scenes/level.json");
        for (const PlannedWrite& step : plan.steps)
            CHECK(step.kind == WriteKind::write);

        // Executes cleanly through the intent-logged queue.
        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);
        CHECK(queue.execute("op-family", plan.steps));
        CHECK(*fs.read("proj/scenes/level.json") == scene);
        CHECK(log.pending().empty());

        // And the written family verifies clean end-to-end.
        const SidecarScan scan = scan_sidecar_refs("proj", "proj/scenes/level.json", scene);
        CHECK(verify_sidecar_refs(fs, "proj/scenes/level.json", scan.refs).empty());
    }
    {
        // Refusals: unreferenced staged sidecar; staged-hash mismatch; dangling ref; non-JSON owner.
        MemoryFileStore fs;
        const std::string s1 = encode_sidecar("S1");

        const SidecarPlan unreferenced = plan_sidecar_family_write(
            fs, "proj", "proj/level.json", owner_json("s1.bin", s1),
            {{"proj/s1.bin", s1}, {"proj/extra.bin", encode_sidecar("X")}});
        CHECK(!unreferenced.ok);
        CHECK(has_diag(unreferenced.diagnostics, "sidecar.orphaned"));

        const SidecarPlan lying = plan_sidecar_family_write(
            fs, "proj", "proj/level.json", owner_json("s1.bin", s1),
            {{"proj/s1.bin", encode_sidecar("DIFFERENT")}});
        CHECK(!lying.ok);
        CHECK(has_diag(lying.diagnostics, "sidecar.hash_mismatch"));

        const SidecarPlan dangling = plan_sidecar_family_write(
            fs, "proj", "proj/level.json", owner_json("s1.bin", s1), {});
        CHECK(!dangling.ok);
        CHECK(has_diag(dangling.diagnostics, "sidecar.dangling_ref"));

        const SidecarPlan not_json =
            plan_sidecar_family_write(fs, "proj", "proj/level.json", "not json", {});
        CHECK(!not_json.ok);
        CHECK(has_diag(not_json.diagnostics, "file.parse_error"));

        // Staged bytes that are not a readable sidecar REFUSE with the decode code: the daemon
        // never authors a "sidecar" it cannot itself read (headerless bytes would also evade the
        // is_sidecar_bytes merge classifier). Coherent hashes cannot save either family.
        const std::string headerless = "raw bytes without the magic";
        const SidecarPlan no_header = plan_sidecar_family_write(
            fs, "proj", "proj/level.json", owner_json("s1.bin", headerless),
            {{"proj/s1.bin", headerless}});
        CHECK(!no_header.ok);
        CHECK(has_diag(no_header.diagnostics, "sidecar.bad_magic"));

        const std::string future = encode_sidecar("P", sidecar_format_version + 1);
        const SidecarPlan future_version = plan_sidecar_family_write(
            fs, "proj", "proj/level.json", owner_json("s1.bin", future), {{"proj/s1.bin", future}});
        CHECK(!future_version.ok);
        CHECK(has_diag(future_version.diagnostics, "sidecar.unsupported_version"));

        // A ref satisfied by an already-durable on-disk sidecar needs no staging.
        fs.write("proj/s1.bin", s1);
        const SidecarPlan on_disk =
            plan_sidecar_family_write(fs, "proj", "proj/level.json", owner_json("s1.bin", s1), {});
        CHECK(on_disk.ok);
        CHECK(on_disk.steps.size() == 1);
        CHECK(on_disk.steps[0].path == "proj/level.json");
    }
    {
        // THE L-33 crash window: the process dies between the sidecar write and the owner write.
        // The sidecar is durable, the owner is NOT (never a referencing JSON without its sidecar);
        // restart RESUMES the op to completion (R-FILE-004).
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);

        const std::string s1 = encode_sidecar("S1");
        const std::string scene = owner_json("s1.bin", s1);
        const SidecarPlan plan = plan_sidecar_family_write(fs, "proj", "proj/level.json", scene,
                                                           {{"proj/s1.bin", s1}});
        CHECK(plan.ok);

        fs.crash_on_rename_to("proj/level.json"); // die at the owner's atomic commit point
        bool crashed = false;
        try
        {
            (void)queue.execute("op-crash", plan.steps);
        }
        catch (const SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);
        CHECK(*fs.read("proj/s1.bin") == s1);      // the sidecar IS durable
        CHECK(!fs.read("proj/level.json"));        // the referencing JSON is NOT
        CHECK(log.pending().size() == 1);          // the intent entry survived the crash

        // Restart: recovery resumes the owner write; the family is complete and coherent.
        const auto diags = queue.recover();
        CHECK(diags.empty());
        CHECK(*fs.read("proj/level.json") == scene);
        CHECK(log.pending().empty());
        const SidecarScan scan = scan_sidecar_refs("proj", "proj/level.json", scene);
        CHECK(verify_sidecar_refs(fs, "proj/level.json", scan.refs).empty());
    }
    {
        // Crash BEFORE anything landed (at the first sidecar's rename): recovery replays the whole
        // family, still sidecar-first.
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);

        const std::string s1 = encode_sidecar("S1");
        const std::string scene = owner_json("s1.bin", s1);
        const SidecarPlan plan = plan_sidecar_family_write(fs, "proj", "proj/level.json", scene,
                                                           {{"proj/s1.bin", s1}});
        fs.crash_on_rename_to("proj/s1.bin");
        bool crashed = false;
        try
        {
            (void)queue.execute("op-crash-early", plan.steps);
        }
        catch (const SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);
        CHECK(!fs.read("proj/s1.bin"));
        CHECK(!fs.read("proj/level.json"));
        CHECK(queue.recover().empty());
        CHECK(*fs.read("proj/s1.bin") == s1);
        CHECK(*fs.read("proj/level.json") == scene);
    }
    {
        // Moved-on CAS during recovery: after the crash, an external writer changed the owner.
        // Recovery must NOT clobber it — a filesync.intent.cas diagnostic names the op instead.
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);

        const std::string s1 = encode_sidecar("S1");
        const std::string scene = owner_json("s1.bin", s1);
        const SidecarPlan plan = plan_sidecar_family_write(fs, "proj", "proj/level.json", scene,
                                                           {{"proj/s1.bin", s1}});
        fs.crash_on_rename_to("proj/level.json");
        try
        {
            (void)queue.execute("op-moved-on", plan.steps);
        }
        catch (const SimulatedCrash&)
        {
        }
        fs.write("proj/level.json", "{\"externally\": \"edited\"}"); // the world moved on

        const auto diags = queue.recover();
        CHECK(has_recovery_code(diags, "filesync.intent.cas"));
        CHECK(*fs.read("proj/level.json") == "{\"externally\": \"edited\"}"); // not clobbered
        CHECK(log.pending().size() == 1); // the op stays reported, never silently dropped
    }

    // =============================================================================================
    // H. owned-satellite move: plan shape, execution, refusals, and removal-tail crash windows
    // =============================================================================================
    {
        MemoryFileStore fs;
        const std::string tiles = encode_sidecar("TILES");
        const std::string rock = encode_sidecar("ROCK");
        const std::string scene = owner_json2("tiles.bin", tiles, "meshes/rock.bin", rock);
        fs.write("proj/a/level.json", scene);
        fs.write("proj/a/tiles.bin", tiles);
        fs.write("proj/a/meshes/rock.bin", rock);

        const SidecarPlan plan = plan_owner_move(fs, "proj", "proj/a/level.json",
                                                 "proj/b/level.json");
        CHECK(plan.ok);
        CHECK(plan.diagnostics.empty());
        // Dependency-safe order: dest sidecars -> dest owner -> remove src owner -> remove src
        // sidecars. The satellites keep their owner-relative relpaths at the destination.
        CHECK(plan.steps.size() == 6);
        CHECK(plan.steps[0].path == "proj/b/tiles.bin");
        CHECK(plan.steps[0].kind == WriteKind::write);
        CHECK(plan.steps[1].path == "proj/b/meshes/rock.bin");
        CHECK(plan.steps[2].path == "proj/b/level.json");
        CHECK(plan.steps[2].kind == WriteKind::write);
        CHECK(plan.steps[3].path == "proj/a/level.json");
        CHECK(plan.steps[3].kind == WriteKind::remove);
        CHECK(plan.steps[4].path == "proj/a/tiles.bin");
        CHECK(plan.steps[4].kind == WriteKind::remove);
        CHECK(plan.steps[5].path == "proj/a/meshes/rock.bin");

        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);
        CHECK(queue.execute("op-move", plan.steps));

        // The whole family moved; the owner bytes are VERBATIM; refs verify at the destination.
        CHECK(!fs.read("proj/a/level.json"));
        CHECK(!fs.read("proj/a/tiles.bin"));
        CHECK(!fs.read("proj/a/meshes/rock.bin"));
        CHECK(*fs.read("proj/b/level.json") == scene);
        const SidecarScan scan = scan_sidecar_refs("proj", "proj/b/level.json", scene);
        CHECK(scan.refs.size() == 2);
        CHECK(verify_sidecar_refs(fs, "proj/b/level.json", scan.refs).empty());
    }
    {
        // A same-directory RENAME: the satellites' resolved paths do not change, so they stay in
        // place and only the owner moves (still referenced by the unchanged relpaths).
        MemoryFileStore fs;
        const std::string tiles = encode_sidecar("TILES");
        const std::string scene = owner_json("tiles.bin", tiles);
        fs.write("proj/a/level.json", scene);
        fs.write("proj/a/tiles.bin", tiles);

        const SidecarPlan plan =
            plan_owner_move(fs, "proj", "proj/a/level.json", "proj/a/level2.json");
        CHECK(plan.ok);
        CHECK(plan.steps.size() == 2);
        CHECK(plan.steps[0].path == "proj/a/level2.json");
        CHECK(plan.steps[1].path == "proj/a/level.json");
        CHECK(plan.steps[1].kind == WriteKind::remove);
    }
    {
        // Refusals + advisory paths.
        MemoryFileStore fs;
        const std::string tiles = encode_sidecar("TILES");
        fs.write("proj/a/level.json", owner_json("tiles.bin", tiles));
        fs.write("proj/a/tiles.bin", tiles);

        // Destination exists.
        fs.write("proj/b/level.json", "occupied");
        const SidecarPlan occupied =
            plan_owner_move(fs, "proj", "proj/a/level.json", "proj/b/level.json");
        CHECK(!occupied.ok);
        CHECK(has_diag(occupied.diagnostics, "usage.invalid"));
        CHECK(fs.remove("proj/b/level.json"));

        // Source missing.
        const SidecarPlan missing =
            plan_owner_move(fs, "proj", "proj/gone.json", "proj/b/level.json");
        CHECK(!missing.ok);
        CHECK(has_diag(missing.diagnostics, "file.not_found"));

        // Endpoint escaping the jail.
        const SidecarPlan escape =
            plan_owner_move(fs, "proj", "proj/a/level.json", "proj/../outside.json");
        CHECK(!escape.ok);
        CHECK(has_diag(escape.diagnostics, "path.jail_violation"));

        // A carried satellite escaping the jail AT THE DESTINATION: "../shared/t.bin" is legal
        // from proj/a/ (resolves inside) but escapes when the owner moves to the project root.
        const std::string shared = encode_sidecar("SHARED");
        fs.write("proj/shared/t.bin", shared);
        fs.write("proj/a/uses_shared.json", owner_json("../shared/t.bin", shared));
        const SidecarPlan dest_escape =
            plan_owner_move(fs, "proj", "proj/a/uses_shared.json", "proj/root_level.json");
        CHECK(!dest_escape.ok);
        CHECK(has_diag(dest_escape.diagnostics, "path.jail_violation"));

        // Same-path move: a no-op plan.
        const SidecarPlan noop =
            plan_owner_move(fs, "proj", "proj/a/level.json", "proj/a/level.json");
        CHECK(noop.ok);
        CHECK(noop.steps.empty());

        // A pre-existing DANGLING ref: the move proceeds owner-only for that satellite, with the
        // finding surfaced (the move neither fixes nor worsens the inconsistency).
        fs.write("proj/a/dangler.json", owner_json("missing.bin", "never written"));
        const SidecarPlan dangling =
            plan_owner_move(fs, "proj", "proj/a/dangler.json", "proj/b/dangler.json");
        CHECK(dangling.ok);
        CHECK(has_diag(dangling.diagnostics, "sidecar.dangling_ref"));
        CHECK(dangling.steps.size() == 2); // dest owner write + src owner remove only

        // An unparseable owner moves alone, flagged advisory.
        fs.write("proj/a/opaque.dat", "not json");
        const SidecarPlan opaque =
            plan_owner_move(fs, "proj", "proj/a/opaque.dat", "proj/b/opaque.dat");
        CHECK(opaque.ok);
        CHECK(has_diag(opaque.diagnostics, "file.parse_error"));
        CHECK(opaque.steps.size() == 2);
    }
    {
        // SHARED satellite: when the (derived, best-effort) index knows ANOTHER owner still
        // references a satellite, the move COPIES it — the dest gets its copy, the src stays put
        // for the sibling — so the daemon's own move never turns a sibling's valid ref dangling.
        MemoryFileStore fs;
        const std::string tiles = encode_sidecar("SHARED-TILES");
        const std::string scene_a = owner_json("tiles.bin", tiles);
        const std::string scene_b = owner_json("tiles.bin", tiles);
        fs.write("proj/a/x.json", scene_a);
        fs.write("proj/a/y.json", scene_b);
        fs.write("proj/a/tiles.bin", tiles);

        SidecarIndex index;
        index.set_owner_refs("proj/a/x.json", {"proj/a/tiles.bin"});
        index.set_owner_refs("proj/a/y.json", {"proj/a/tiles.bin"});

        const SidecarPlan plan =
            plan_owner_move(fs, "proj", "proj/a/x.json", "proj/b/x.json", &index);
        CHECK(plan.ok);
        CHECK(plan.diagnostics.empty());
        CHECK(plan.steps.size() == 3); // dest satellite COPY + dest owner + src owner remove —
        CHECK(plan.steps[0].path == "proj/b/tiles.bin"); // and NO src satellite remove
        CHECK(plan.steps[0].kind == WriteKind::write);
        CHECK(plan.steps[1].path == "proj/b/x.json");
        CHECK(plan.steps[2].path == "proj/a/x.json");
        CHECK(plan.steps[2].kind == WriteKind::remove);

        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);
        CHECK(queue.execute("op-shared-move", plan.steps));

        // BOTH owners verify clean: the mover against its dest copy, the sibling against the
        // still-present src satellite.
        CHECK(*fs.read("proj/a/tiles.bin") == tiles);
        const SidecarScan moved = scan_sidecar_refs("proj", "proj/b/x.json", scene_a);
        CHECK(verify_sidecar_refs(fs, "proj/b/x.json", moved.refs).empty());
        const SidecarScan sibling = scan_sidecar_refs("proj", "proj/a/y.json", scene_b);
        CHECK(verify_sidecar_refs(fs, "proj/a/y.json", sibling.refs).empty());

        // Once the mover is re-registered at its destination, the sibling is the SOLE registered
        // owner of the src satellite — a later move of the sibling carries it normally.
        index.set_owner_refs("proj/b/x.json", {"proj/b/tiles.bin"});
        index.remove_owner("proj/a/x.json");
        const SidecarPlan solo =
            plan_owner_move(fs, "proj", "proj/a/y.json", "proj/c/y.json", &index);
        CHECK(solo.ok);
        CHECK(solo.steps.size() == 4); // dest satellite + dest owner + src owner + src satellite
        CHECK(solo.steps[3].path == "proj/a/tiles.bin");
        CHECK(solo.steps[3].kind == WriteKind::remove);
    }
    {
        // Crash in the REMOVAL tail (after every dest write): both copies exist mid-crash — the
        // acceptable mid-state (never a dangling ref) — and recovery completes the removals.
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);

        const std::string tiles = encode_sidecar("TILES");
        const std::string scene = owner_json("tiles.bin", tiles);
        fs.write("proj/a/level.json", scene);
        fs.write("proj/a/tiles.bin", tiles);

        const SidecarPlan plan =
            plan_owner_move(fs, "proj", "proj/a/level.json", "proj/b/level.json");
        CHECK(plan.ok);

        fs.crash_on_remove("proj/a/level.json"); // die entering the removal tail
        bool crashed = false;
        try
        {
            (void)queue.execute("op-move-crash", plan.steps);
        }
        catch (const SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);
        CHECK(*fs.read("proj/b/level.json") == scene); // dest complete...
        CHECK(*fs.read("proj/b/tiles.bin") == tiles);
        CHECK(*fs.read("proj/a/level.json") == scene); // ...src not yet removed

        CHECK(queue.recover().empty()); // resume: removals re-applied
        CHECK(!fs.read("proj/a/level.json"));
        CHECK(!fs.read("proj/a/tiles.bin"));
        CHECK(*fs.read("proj/b/level.json") == scene);
    }
    {
        // Moved-on removal CAS: after the crash an external writer REPLACED a src sidecar; the
        // recovery pass must not delete the new bytes — diagnostic instead (L-25: no rollback).
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);

        const std::string tiles = encode_sidecar("TILES");
        const std::string scene = owner_json("tiles.bin", tiles);
        fs.write("proj/a/level.json", scene);
        fs.write("proj/a/tiles.bin", tiles);

        const SidecarPlan plan =
            plan_owner_move(fs, "proj", "proj/a/level.json", "proj/b/level.json");
        fs.crash_on_remove("proj/a/tiles.bin");
        try
        {
            (void)queue.execute("op-move-cas", plan.steps);
        }
        catch (const SimulatedCrash&)
        {
        }
        fs.write("proj/a/tiles.bin", encode_sidecar("REWRITTEN"));

        const auto diags = queue.recover();
        CHECK(has_recovery_code(diags, "filesync.intent.cas"));
        CHECK(*fs.read("proj/a/tiles.bin") == encode_sidecar("REWRITTEN")); // preserved
        CHECK(log.pending().size() == 1); // reported, not silently dropped
    }

    // =============================================================================================
    // I. sidecar-aware reconcile: a changed/removed sidecar dirties its registered owner(s)
    // =============================================================================================
    {
        MemoryFileStore fs;
        FakeWatcher watcher;
        context::kernel::ManualClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(fs, watcher, clock, tasks, "proj", "proj/.editor/index");

        const std::string tiles_v1 = encode_sidecar("V1");
        const std::string scene = owner_json("tiles.bin", tiles_v1);
        fs.write("proj/level.json", scene);
        fs.write("proj/tiles.bin", tiles_v1);

        // Track both files, then register the ownership (as the parse layer would).
        (void)rec.crawl(false);
        rec.set_sidecar_refs("proj/level.json", {"proj/tiles.bin"});

        // An external edit of the SIDECAR dirties the owner too: the synthetic change carries the
        // owner's (unchanged) hash and names the sidecar that triggered it.
        fs.write("proj/tiles.bin", encode_sidecar("V2"));
        watcher.emit("proj/tiles.bin", ChangeKind::modified);
        const auto changes = rec.reconcile_hints();
        CHECK(changes.size() == 2);
        CHECK(changes[0].path == "proj/tiles.bin");
        CHECK(changes[0].via_sidecar.empty()); // the direct change
        CHECK(changes[1].path == "proj/level.json");
        CHECK(changes[1].type == ChangeType::modified);
        CHECK(changes[1].via_sidecar == "proj/tiles.bin"); // the synthetic owner-dirty
        CHECK(changes[1].content_hash == content_hash(scene));

        // When the OWNER changed in the same pass, no duplicate synthetic change is emitted.
        fs.write("proj/tiles.bin", encode_sidecar("V3"));
        fs.write("proj/level.json", scene + "\n");
        watcher.emit("proj/tiles.bin", ChangeKind::modified);
        watcher.emit("proj/level.json", ChangeKind::modified);
        const auto both = rec.reconcile_hints();
        CHECK(both.size() == 2); // one per file, nothing synthetic

        // A REMOVED sidecar also dirties the owner (its ref is now dangling — re-derive will
        // diagnose), through the crawl path as well as the hint path.
        CHECK(fs.remove("proj/tiles.bin"));
        const auto crawl_changes = rec.crawl(false);
        CHECK(crawl_changes.size() == 2);
        CHECK(has_change_path(crawl_changes, "proj/tiles.bin"));
        bool owner_dirtied = false;
        for (const ReconcileChange& change : crawl_changes)
            if (change.path == "proj/level.json" && change.via_sidecar == "proj/tiles.bin")
                owner_dirtied = true;
        CHECK(owner_dirtied);

        // A reconciled OWNER removal drops its registrations: later sidecar changes dirty nothing.
        fs.write("proj/tiles.bin", encode_sidecar("V4"));
        (void)rec.crawl(false); // re-track the sidecar
        CHECK(fs.remove("proj/level.json"));
        (void)rec.crawl(false); // reconciles the owner removal -> registrations dropped
        CHECK(!rec.sidecar_index().is_sidecar("proj/tiles.bin"));
        fs.write("proj/tiles.bin", encode_sidecar("V5"));
        watcher.emit("proj/tiles.bin", ChangeKind::modified);
        const auto after = rec.reconcile_hints();
        CHECK(after.size() == 1); // just the sidecar — no ghost owner change
        CHECK(after[0].via_sidecar.empty());
    }

    FILESYNC_TEST_MAIN_END();
}
