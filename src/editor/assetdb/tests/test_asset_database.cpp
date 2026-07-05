// Asset-database index tests: lazy meta-only scan (R-FILE-011(e)), duplicate-GUID + orphan +
// malformed diagnostics, meta creation, and the REAL RefTargetResolver wire through
// validate_document (wrong-kind rejection via meta lookup — R-DATA-006).

#include "context/editor/assetdb/asset_database.h"

#include "context/editor/filesync/file_store.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/schema/validator.h"
#include "context/editor/serializer/json_parse.h"

#include "assetdb_test.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace context::editor::assetdb;
namespace filesync = context::editor::filesync;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;

namespace
{

// A delegating store that counts read() calls per path — the R-FILE-011(e) laziness proof: the
// index build touches sidecar bytes only, never asset payloads.
class CountingFileStore final : public filesync::FileStore
{
public:
    explicit CountingFileStore(filesync::FileStore& inner) : inner_(&inner) {}

    [[nodiscard]] bool exists(std::string_view path) const override
    {
        return inner_->exists(path);
    }
    [[nodiscard]] std::optional<std::string> read(std::string_view path) const override
    {
        ++reads_[std::string(path)];
        return inner_->read(path);
    }
    [[nodiscard]] std::optional<filesync::FileStat> stat(std::string_view path) const override
    {
        return inner_->stat(path);
    }
    [[nodiscard]] std::vector<std::string> list(std::string_view dir) const override
    {
        return inner_->list(dir);
    }
    bool write(std::string_view path, std::string_view data) override
    {
        return inner_->write(path, data);
    }
    bool rename(std::string_view from, std::string_view to) override
    {
        return inner_->rename(from, to);
    }
    bool remove(std::string_view path) override { return inner_->remove(path); }
    void fsync(std::string_view path) override { inner_->fsync(path); }

    [[nodiscard]] std::size_t reads_of(std::string_view path) const
    {
        const auto it = reads_.find(std::string(path));
        return it == reads_.end() ? 0 : it->second;
    }

private:
    filesync::FileStore* inner_;
    mutable std::map<std::string, std::size_t> reads_;
};

void put_asset(filesync::FileStore& fs, std::string_view path, std::string_view bytes,
               std::string_view guid, std::string_view kind)
{
    fs.write(path, bytes);
    AssetMeta meta;
    meta.guid = std::string(guid);
    meta.kind = std::string(kind);
    fs.write(meta_path_for(path), serialize_meta(meta));
}

bool has_diag(const std::vector<AssetDiagnostic>& diags, std::string_view code,
              std::string_view path = "")
{
    for (const AssetDiagnostic& d : diags)
        if (d.code == code && (path.empty() || d.path == path))
            return true;
    return false;
}

constexpr std::string_view kGuidScene = "00000000000000000000000000000aaa";
constexpr std::string_view kGuidMesh = "00000000000000000000000000000bbb";
constexpr std::string_view kGuidTexture = "00000000000000000000000000000ccc";

} // namespace

int main()
{
    // --- candidate filter -------------------------------------------------------------------------
    CHECK(is_asset_candidate("proj/scenes/level1.json"));
    CHECK(is_asset_candidate("tex.png"));
    CHECK(!is_asset_candidate("proj/scenes/level1.json.meta.json")); // sidecars are not assets
    CHECK(!is_asset_candidate("proj/.editor/index"));                // engine-internal tree
    CHECK(!is_asset_candidate("a/.git/config"));                     // dot-segment anywhere
    CHECK(!is_asset_candidate(".hidden"));
    CHECK(!is_asset_candidate("proj/scenes/level1.json.tmp"));       // atomic-write residue
    CHECK(!is_asset_candidate("proj/scenes/level1.json.tmp.abc123"));
    CHECK(is_asset_candidate("proj/deploy.tmpl.yaml")); // NOT residue (the lexical marker is exact)

    // --- scan: happy path + the resolver over the index -------------------------------------------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/scenes/level1.json",
                  "{\"$schema\": \"ctx:scene\", \"version\": 1}\n", kGuidScene, "ctx:scene");
        put_asset(fs, "proj/meshes/rock.bin", "BINARY", kGuidMesh, "ctx:mesh");

        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        CountingFileStore counting(fs);
        const ScanResult scan = db.scan(counting, "proj");
        CHECK(scan.assets_indexed == 2);
        CHECK(scan.diagnostics.empty());
        CHECK(db.size() == 2);

        const AssetRecord* by_guid = db.find_by_guid(kGuidScene);
        CHECK(by_guid != nullptr);
        CHECK(by_guid->path == "proj/scenes/level1.json");
        CHECK(by_guid->kind == "ctx:scene");
        const AssetRecord* by_path = db.find_by_path("proj/meshes/rock.bin");
        CHECK(by_path != nullptr);
        CHECK(by_path->guid == kGuidMesh);
        CHECK(db.find_by_guid("00000000000000000000000000000fff") == nullptr);
        CHECK(db.find_by_path("proj/no/such.json") == nullptr);

        // The typed-reference meta lookup (the PR #48 seam, now real).
        CHECK(db.kind_of(kGuidMesh).has_value());
        CHECK(*db.kind_of(kGuidMesh) == "ctx:mesh");
        CHECK(!db.kind_of("00000000000000000000000000000fff").has_value()); // unknown GUID

        // R-FILE-011(e) laziness: the index build read sidecars ONLY — never asset payloads.
        CHECK(counting.reads_of("proj/scenes/level1.json") == 0);
        CHECK(counting.reads_of("proj/meshes/rock.bin") == 0);
        CHECK(counting.reads_of("proj/scenes/level1.json.meta.json") == 1);
        CHECK(counting.reads_of("proj/meshes/rock.bin.meta.json") == 1);
    }

    // --- scan: kind not recorded => not enforced (nullopt), per the seam contract -----------------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/raw.bin", "BYTES", kGuidMesh, "");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        CHECK(db.scan(fs, "proj").assets_indexed == 1);
        CHECK(!db.kind_of(kGuidMesh).has_value());
    }

    // --- scan: duplicate GUID between two LIVE assets (deterministic first-path-wins) --------------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/a/first.json", "{}\n", kGuidScene, "ctx:scene");
        put_asset(fs, "proj/b/copy.json", "{}\n", kGuidScene, "ctx:scene");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        const ScanResult scan = db.scan(fs, "proj");
        CHECK(scan.assets_indexed == 1);
        CHECK(has_diag(scan.diagnostics, "asset.guid_duplicate", "proj/b/copy.json"));
        const AssetRecord* winner = db.find_by_guid(kGuidScene);
        CHECK(winner != nullptr);
        CHECK(winner->path == "proj/a/first.json"); // lexicographically-first keeps the identity
        CHECK(db.find_by_path("proj/b/copy.json") == nullptr);
        // The diagnostic names both claimants and the contested GUID.
        for (const AssetDiagnostic& d : scan.diagnostics)
            if (d.code == "asset.guid_duplicate")
            {
                CHECK(d.other_path == "proj/a/first.json");
                CHECK(d.guid == kGuidScene);
            }
    }

    // --- scan ignores sidecars OUTSIDE the asset domain (dot-trees, sidecar-of-a-sidecar) ---------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/real.json", "{}\n", kGuidScene, "ctx:scene");
        put_asset(fs, "proj/.editor/cache/level.json", "{}\n", kGuidMesh, ""); // dot-tree pair
        // A hand-made sidecar-of-a-sidecar must not turn the real sidecar into an indexed asset.
        fs.write("proj/real.json.meta.json.meta.json",
                 "{\"guid\": \"00000000000000000000000000000ccc\"}\n");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        const ScanResult scan = db.scan(fs, "proj");
        CHECK(scan.assets_indexed == 1); // only the real asset
        CHECK(scan.diagnostics.empty()); // out-of-domain sidecars are not diagnosed either
        CHECK(db.find_by_guid(kGuidMesh) == nullptr);                  // dot-tree pair not indexed
        CHECK(db.find_by_path("proj/real.json.meta.json") == nullptr); // a sidecar is never an asset
        CHECK(db.find_by_guid(kGuidTexture) == nullptr);
    }

    // --- scan: orphaned meta (asset gone) — diagnosed, never indexed, never auto-removed ----------
    {
        filesync::MemoryFileStore fs;
        AssetMeta meta;
        meta.guid = std::string(kGuidScene);
        meta.kind = "ctx:scene";
        fs.write("proj/gone.json.meta.json", serialize_meta(meta));
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        const ScanResult scan = db.scan(fs, "proj");
        CHECK(scan.assets_indexed == 0);
        CHECK(has_diag(scan.diagnostics, "asset.meta_orphaned", "proj/gone.json.meta.json"));
        CHECK(!db.kind_of(kGuidScene).has_value()); // a gone asset enforces nothing
        CHECK(fs.exists("proj/gone.json.meta.json")); // deletion cleanup is NOT an auto-write
    }

    // --- scan: malformed meta — diagnosed and skipped ----------------------------------------------
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/broken.json", "{}\n");
        fs.write("proj/broken.json.meta.json", "not json");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        const ScanResult scan = db.scan(fs, "proj");
        CHECK(scan.assets_indexed == 0);
        CHECK(has_diag(scan.diagnostics, "asset.meta_invalid", "proj/broken.json.meta.json"));
    }

    // --- ensure_metas: meta creation (R-FILE-003 write #1), kind sniffing, skip rules, idempotence -
    {
        filesync::MemoryFileStore fs;
        fs.write("proj/scenes/new.json", "{\"$schema\": \"ctx:scene\", \"version\": 1}\n");
        fs.write("proj/textures/new.png", "PNGBYTES");
        fs.write("proj/notes.json", "{\"no\": \"header\"}\n"); // JSON without $schema
        fs.write("proj/.editor/index", "internal");            // never gets a sidecar
        put_asset(fs, "proj/scenes/old.json", "{}\n", kGuidScene, "ctx:scene"); // already has one

        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");
        const HealResult created = db.ensure_metas(fs, "proj");
        CHECK(created.actions.size() == 3);
        CHECK(created.diagnostics.empty());
        CHECK(fs.exists("proj/scenes/new.json.meta.json"));
        CHECK(fs.exists("proj/textures/new.png.meta.json"));
        CHECK(fs.exists("proj/notes.json.meta.json"));
        CHECK(!fs.exists("proj/.editor/index.meta.json"));
        CHECK(!fs.exists("proj/scenes/old.json.meta.json.meta.json")); // no sidecar sidecars

        // Kind was sniffed from the canonical-JSON header; unknown elsewhere.
        const AssetRecord* scene = db.find_by_path("proj/scenes/new.json");
        CHECK(scene != nullptr);
        CHECK(scene->kind == "ctx:scene");
        CHECK(is_guid(scene->guid));
        const AssetRecord* texture = db.find_by_path("proj/textures/new.png");
        CHECK(texture != nullptr);
        CHECK(texture->kind.empty());
        const AssetRecord* headerless = db.find_by_path("proj/notes.json");
        CHECK(headerless != nullptr);
        CHECK(headerless->kind.empty());

        // The created sidecar parses and its GUID matches the index (idempotent write surface).
        std::vector<std::string> problems;
        const auto meta = parse_meta(*fs.read("proj/scenes/new.json.meta.json"), problems);
        CHECK(meta.has_value());
        CHECK(meta->guid == scene->guid);

        // Idempotent: a second pass creates nothing.
        const HealResult again = db.ensure_metas(fs, "proj");
        CHECK(again.actions.empty());
    }

    // --- the REAL resolver wire: wrong-kind rejection through validate_document -------------------
    {
        filesync::MemoryFileStore fs;
        put_asset(fs, "proj/meshes/rock.json", "{}\n", kGuidMesh, "ctx:mesh");
        put_asset(fs, "proj/textures/wood.json", "{}\n", kGuidTexture, "ctx:texture");
        SequenceGuidGenerator guids;
        AssetDatabase db(guids);
        db.scan(fs, "proj");

        schema::SchemaSet set;
        std::vector<std::string> problems;
        auto kind = schema::compile_kind_schema(R"({
            "$id": "test:scene",
            "version": 1,
            "type": "object",
            "properties": {
                "notes": {"description": "blessed"},
                "mesh": {"x-ctx-ref": "ctx:mesh"}
            }
        })",
                                                problems);
        CHECK(problems.empty());
        CHECK(kind.has_value());
        set.add(std::move(*kind));

        auto validate = [&](std::string_view doc)
        {
            auto parsed = serializer::parse_json(doc);
            CHECK(parsed.ok);
            return schema::validate_document(parsed.root, doc, set, &db);
        };
        auto has_code = [](const schema::ValidationReport& r, std::string_view code)
        {
            for (const auto& d : r.diagnostics)
                if (d.code == code)
                    return true;
            return false;
        };

        // Right kind: silent.
        const auto ok = validate("{\"$schema\": \"test:scene\", \"version\": 1, \"mesh\": "
                                 "{\"$ref\": \"00000000000000000000000000000bbb\"}}");
        CHECK(ok.ok);
        CHECK(!has_code(ok, "schema.ref_wrong_kind"));

        // Wrong kind: the meta lookup rejects it (a texture in a mesh slot).
        const auto wrong = validate("{\"$schema\": \"test:scene\", \"version\": 1, \"mesh\": "
                                    "{\"$ref\": \"00000000000000000000000000000ccc\"}}");
        CHECK(!wrong.ok);
        CHECK(has_code(wrong, "schema.ref_wrong_kind"));

        // Unknown GUID: the validator stays silent BY CONTRACT (unknown = not enforced there);
        // check_document_refs owns dangling-$ref reporting (test_ref_heal proves it).
        const auto unknown = validate("{\"$schema\": \"test:scene\", \"version\": 1, \"mesh\": "
                                      "{\"$ref\": \"00000000000000000000000000000fff\"}}");
        CHECK(unknown.ok);
        CHECK(!has_code(unknown, "schema.ref_wrong_kind"));
    }

    ASSETDB_TEST_MAIN_END();
}
