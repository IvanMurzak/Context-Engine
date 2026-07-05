// M2 exit criterion 6 — the DATA-MODEL SEAM CHECKLIST as an executable audit (ROADMAP §1 M2 Exit /
// issue #68): "the data-model seam set is IN the frozen M2 schema — M2 does not exit with any of them
// missing." One assertion per seam, exercising the real public surface, so a regression that quietly
// drops a seam turns this milestone gate red. The seams (ROADMAP §1 M2 "M2 data-model seams"):
//
//   1  nested-instance override id-path addressing + innermost-out precedence (L-35)
//   2  the composed write path: default-outermost / --at-instance / --edit-template (R-CLI-006)
//   3  per-component-payload schema versions + per-payload parse-time migration selection (L-32/L-37)
//   4  the package-migration execution contract: sandboxed-tier-only + migration-set hash (L-37/R-PKG-005)
//   5  override-path migration + orphan-override diagnostics (L-37/R-FILE-012)
//   6  the entity-ref value type ({"$entity": …}, id-path form) (L-34)
//   7  composed/runtime identity — deterministic, stable across re-derivation, ONE identity (L-37)
//   8  id-allocation + merge rules: duplicate-id diagnostic + re-key verb (L-33/R-FILE-012)
//   9  the schema vocabulary + units law (R-DATA-006)
//   10 binary-sidecar authoring rules: magic/version header, {"$sidecar": …} refs (L-33)
//   11 scene-level state as singleton components on the scene-root entity (inert / composable) (L-35)
//   12 runtime save-migration groundwork: per-component schemaVersion map + minimal runner (R-DATA-005)
//   13 the meta `platforms` reservation (L-36)
//   14 the advisory override-hygiene tooling: redundant-override query (L-35)

#include "context/cli/app.h"
#include "context/editor/assetdb/meta.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/json_pointer.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/merge/rekey.h"
#include "context/editor/migrate/migrate_document.h"
#include "context/editor/migrate/migration_set.h"
#include "context/editor/schema/vocabulary.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"
#include "context/editor/serializer/sidecar_ref.h"
#include "context/runtime/save/save_document.h"
#include "context/runtime/save/save_migration.h"

#include "m2_exit_test.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using context::cli::run;
using context::editor::contract::Envelope;
namespace assetdb = context::editor::assetdb;
namespace compose = context::editor::compose;
namespace merge = context::editor::merge;
namespace migrate = context::editor::migrate;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;
namespace save = context::runtime::save;
namespace fs = std::filesystem;

namespace
{

serializer::JsonValue parse(const std::string& json)
{
    serializer::CanonicalizeResult r = serializer::canonicalize(json);
    CHECK(r.is_json);
    return r.root;
}

const serializer::JsonValue* member(const serializer::JsonValue& obj, const std::string& key)
{
    for (const serializer::JsonMember& m : obj.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// A component object {type: <payload>} — the shape save entities + migrate payload sites use.
serializer::JsonValue one_component(const std::string& type, const std::string& payload_json)
{
    serializer::JsonValue obj;
    obj.type = serializer::JsonValue::Type::object;
    serializer::JsonMember m;
    m.key = type;
    m.value = parse(payload_json);
    obj.members.push_back(std::move(m));
    return obj;
}

void seed_hierarchy(const fs::path& dir)
{
    m2exit::write_file_raw(dir / "child.scene.json", R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [
    {"id": "ccccccccccccccc1", "name": "Light", "components": {"transform": {"position": [0, 0, 0]}}}
  ]})");
    m2exit::write_file_raw(dir / "mid.scene.json", R"({
  "$schema": "ctx:scene", "version": 1, "entities": [],
  "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "child.scene.json"}]})");
    m2exit::write_file_raw(dir / "root.scene.json", R"({
  "$schema": "ctx:scene", "version": 1, "entities": [],
  "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "mid.scene.json"}]})");
}

const std::string kFullC1 = "aaaaaaaaaaaaaaa1/bbbbbbbbbbbbbbb1/ccccccccccccccc1";
const std::string kPointer = "/components/transform/position";

const compose::ComposedEntity* find_entity(const compose::ComposedScene& scene,
                                           const std::vector<std::string>& id_path)
{
    for (const compose::ComposedEntity& e : scene.entities)
        if (e.id_path == id_path)
            return &e;
    return nullptr;
}

} // namespace

int main()
{
    // === Seam 1 — id-path overrides + innermost-out precedence (L-35) ============================
    {
        m2exit::MapResolver r;
        CHECK(r.add("child.scene.json", R"({"$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "ccccccccccccccc1", "name": "L",
            "components": {"transform": {"position": [0, 0, 0]}}}]})"));
        CHECK(r.add("mid.scene.json", R"({"$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "child.scene.json"}],
          "overrides": [{"path": ["bbbbbbbbbbbbbbb1", "ccccccccccccccc1"],
            "pointer": "/components/transform/position", "value": [1, 1, 1]}]})"));
        CHECK(r.add("root.scene.json", R"({"$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "mid.scene.json"}],
          "overrides": [{"path": ["aaaaaaaaaaaaaaa1", "bbbbbbbbbbbbbbb1", "ccccccccccccccc1"],
            "pointer": "/components/transform/position", "value": [2, 2, 2]}]})"));
        compose::ComposedScene s = compose::flatten("root.scene.json", r);
        CHECK(s.ok);
        const compose::ComposedEntity* e = find_entity(
            s, {"aaaaaaaaaaaaaaa1", "bbbbbbbbbbbbbbb1", "ccccccccccccccc1"});
        CHECK(e != nullptr);
        if (e != nullptr)
        {
            // The OUTERMOST (root) override wins over the mid override and the child template.
            const serializer::JsonValue* pos =
                compose::resolve_json_pointer(e->value, "/components/transform/position/0");
            CHECK(pos != nullptr && pos->int_value == 2);
        }
    }

    // === Seam 2 — the composed write path lands at all three targets (R-CLI-006) ==================
    {
        const fs::path dir = m2exit::make_temp_project("seam2");
        seed_hierarchy(dir);
        const Envelope outer = run({"set", "root.scene.json", "[9, 9, 9]", "--pointer", kPointer,
                                    "--id-path", kFullC1, "--project", dir.string()});
        CHECK(outer.ok() && outer.data().at("target").as_string() == "outermost");
        seed_hierarchy(dir); // reset
        const Envelope at = run({"set", "root.scene.json", "[3, 3, 3]", "--pointer", kPointer,
                                 "--id-path", kFullC1, "--at-instance", "aaaaaaaaaaaaaaa1",
                                 "--project", dir.string()});
        CHECK(at.ok() && at.data().at("target").as_string() == "at-instance");
        seed_hierarchy(dir); // reset
        const Envelope tmpl = run({"set", "root.scene.json", "[5, 5, 5]", "--pointer", kPointer,
                                   "--id-path", kFullC1, "--edit-template", "--project",
                                   dir.string()});
        CHECK(tmpl.ok() && tmpl.data().at("target").as_string() == "template");
        m2exit::remove_quiet(dir);
    }

    // === Seam 3 — componentVersions header + per-payload parse-time migration selection (L-37) ====
    {
        migrate::MigrationSet set;
        std::string problem;
        CHECK(set.register_component("test:sprite", 2, problem));
        migrate::MigrationStep step;
        step.component_type = "test:sprite";
        step.from_version = 1;
        step.transform = [](serializer::JsonValue& p) {
            for (serializer::JsonMember& m : p.members)
                if (m.key == "tint")
                    m.key = "color";
            return true;
        };
        CHECK(set.register_step(std::move(step), problem));

        // A v1-stamped payload is selected + migrated; a current-stamped one is left alone.
        serializer::JsonValue old_doc =
            parse(R"({"componentVersions": {"test:sprite": 1}, "c": {"test:sprite": {"tint": "red"}}})");
        const migrate::DocumentMigrationResult r1 = migrate::migrate_document(old_doc, set);
        CHECK(r1.ok && r1.changed);
        const serializer::JsonValue* c = member(old_doc, "c");
        const serializer::JsonValue* sprite = c != nullptr ? member(*c, "test:sprite") : nullptr;
        CHECK(sprite != nullptr && member(*sprite, "color") != nullptr);

        serializer::JsonValue cur_doc =
            parse(R"({"componentVersions": {"test:sprite": 2}, "c": {"test:sprite": {"color": "red"}}})");
        const migrate::DocumentMigrationResult r2 = migrate::migrate_document(cur_doc, set);
        CHECK(r2.ok && !r2.changed); // v == current: selection leaves it untouched
    }

    // === Seam 4 — package-migration contract: sandboxed-tier-only + migration-set hash (L-37) =====
    {
        migrate::MigrationSet set;
        std::string problem;
        CHECK(set.register_component("phys:body", 2, problem));
        const std::uint64_t hash_component_only = set.set_hash();
        migrate::MigrationStep pkg;
        pkg.component_type = "phys:body";
        pkg.from_version = 1;
        pkg.tier = migrate::MigrationTier::package_sandboxed;
        pkg.wasm_module = "phys_body_v1_v2.wasm";
        CHECK(set.register_step(std::move(pkg), problem));

        // The migration-set hash is content-sensitive — adding a step changes it, so it changes every
        // pass-1 derivation cache key (R-FILE-010: never a stale artifact under a new migration).
        CHECK(set.set_hash() != hash_component_only);

        // A package-tier migration is REFUSED in-process until the sandboxed WASM runner lands.
        serializer::JsonValue doc =
            parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"m": 1}}})");
        const migrate::DocumentMigrationResult r = migrate::migrate_document(doc, set);
        CHECK(!r.ok);
        bool saw_runner_unavailable = false;
        for (const migrate::MigrationDiagnostic& d : r.diagnostics)
            if (d.code == "migration.runner_unavailable")
                saw_runner_unavailable = true;
        CHECK(saw_runner_unavailable);
    }

    // === Seam 5 — override-path migration + orphan-override diagnostics (L-37/R-FILE-012) =========
    {
        migrate::MigrationSet set;
        std::string problem;
        CHECK(set.register_component("test:sprite", 2, problem));
        migrate::MigrationStep step;
        step.component_type = "test:sprite";
        step.from_version = 1;
        step.transform = [](serializer::JsonValue&) { return true; };
        step.map_path = [](std::string_view p) -> std::optional<std::string> {
            if (p == "/size")
                return std::nullopt; // removed in v2: an override addressing it orphans
            return std::string(p);
        };
        CHECK(set.register_step(std::move(step), problem));

        serializer::JsonValue doc = parse(R"({
          "componentVersions": {"test:sprite": 1},
          "entities": [{"components": {"test:sprite": {"size": 4}}}],
          "instances": [{"overrides": [{"path": "components/test:sprite/size", "value": 8}]}]})");
        const migrate::DocumentMigrationResult r = migrate::migrate_document(doc, set);
        CHECK(r.ok); // an orphan override is NON-blocking — data is preserved, not destroyed
        bool saw_orphan = false;
        for (const migrate::MigrationDiagnostic& d : r.diagnostics)
            if (d.code == "migration.orphan_override")
            {
                saw_orphan = true;
                CHECK(!d.blocking);
            }
        CHECK(saw_orphan);
    }

    // === Seam 6 — the entity-ref value type ({"$entity": …}, id-path form) (L-34) =================
    {
        m2exit::MapResolver r;
        CHECK(r.add("refs.scene.json", R"({"$schema": "ctx:scene", "version": 1, "entities": [
          {"id": "ccccccccccccccc1", "name": "A",
           "components": {"link": {"target": {"$entity": "ccccccccccccccc2"}}}},
          {"id": "ccccccccccccccc2", "name": "B", "components": {}}]})"));
        compose::ComposedScene s = compose::flatten("refs.scene.json", r);
        CHECK(s.ok);
        const compose::ComposedEntity* a = find_entity(s, {"ccccccccccccccc1"});
        CHECK(a != nullptr);
        if (a != nullptr)
        {
            // The {"$entity": <id-path>} value type flows through composition verbatim, and its
            // target is a real composed entity (an intra-file id-path ref, the v1 form).
            const serializer::JsonValue* ref =
                compose::resolve_json_pointer(a->value, "/components/link/target/$entity");
            CHECK(ref != nullptr && ref->string_value == "ccccccccccccccc2");
            CHECK(find_entity(s, {"ccccccccccccccc2"}) != nullptr);
        }
    }

    // === Seam 7 — composed/runtime identity: deterministic + stable + root-bound (L-37) ===========
    {
        const std::vector<std::string> id_path = {"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"};
        const std::uint64_t h1 = compose::identity_hash_of("root.scene.json", id_path);
        const std::uint64_t h2 = compose::identity_hash_of("root.scene.json", id_path);
        CHECK(h1 == h2);                                                    // deterministic
        CHECK(h1 != 0);                                                     // a real identity
        CHECK(compose::identity_hash_of("other.scene.json", id_path) != h1); // bound to the root scene
    }

    // === Seam 8 — id-allocation + merge rules: duplicate-id diagnostic + re-key (L-33/R-FILE-012) ==
    {
        serializer::JsonValue doc = parse(R"({"entities": [
          {"id": "aaaaaaaaaaaaaaa1", "name": "A"},
          {"id": "aaaaaaaaaaaaaaa1", "name": "B"}]})");
        std::vector<merge::DuplicateId> dups = merge::find_duplicate_ids(doc);
        CHECK(dups.size() == 1);
        if (dups.size() == 1)
            CHECK(dups[0].pointers.size() == 2);
        // Re-key the second holder; a fresh id is minted and the duplicate is gone.
        const merge::RekeyResult rk = merge::rekey_entity(doc, "/entities/1");
        CHECK(rk.ok);
        CHECK(rk.new_id != "aaaaaaaaaaaaaaa1");
        CHECK(merge::find_duplicate_ids(doc).empty());
    }

    // === Seam 9 — the schema vocabulary + units law (R-DATA-006) ==================================
    {
        // The x-ctx-* annotation keys are pinned (every kind schema shares one encoding).
        CHECK(schema::kKeySemanticType == "x-ctx-type");
        CHECK(schema::kKeyStorage == "x-ctx-storage");
        CHECK(schema::kKeyRef == "x-ctx-ref");
        CHECK(schema::kKeyUnits == "x-ctx-units"); // the units law annotation
        // A semantic type id round-trips, and check_semantic rejects a malformed instance.
        CHECK(schema::is_semantic_type_id("color"));
        const schema::SemanticType color = schema::semantic_type_from_id("color");
        CHECK(schema::semantic_type_id(color) == "color");
        const serializer::JsonValue bad = parse("[1, 2, 3]"); // a color is an object, not an array
        CHECK(!schema::check_semantic(color, bad).empty());
    }

    // === Seam 10 — binary-sidecar authoring rules ({"$sidecar": …} refs) (L-33) ===================
    {
        serializer::JsonValue doc = parse(R"({
          "tex": {"$sidecar": "albedo.png", "hash": "12345"},
          "bad": {"$sidecar": "n.png", "hash": "not-decimal"}})");
        std::vector<serializer::Diagnostic> diags;
        std::vector<serializer::SidecarRef> refs = serializer::collect_sidecar_refs(doc, diags);
        CHECK(refs.size() == 1);
        if (refs.size() == 1)
        {
            CHECK(refs[0].relpath == "albedo.png");
            CHECK(refs[0].hash == 12345);
        }
        CHECK(!diags.empty()); // the malformed ref is reported, not silently dropped
        const serializer::JsonValue* tex = member(doc, "tex");
        CHECK(tex != nullptr && serializer::is_sidecar_ref(*tex));
    }

    // === Seam 11 — scene-root singleton state: inert by default, composable opt-in (L-35) =========
    {
        m2exit::MapResolver r;
        CHECK(r.add("inert.scene.json", R"({"$schema": "ctx:scene", "version": 1, "entities": [],
          "root": {"components": {"physics": {"gravity": [0, -9.81, 0]}}}})"));
        CHECK(r.add("optin.scene.json", R"({"$schema": "ctx:scene", "version": 1, "entities": [],
          "root": {"composable": true, "components": {"weather": {"rain": true}}}})"));
        CHECK(r.add("host.scene.json", R"({"$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [
            {"id": "aaaaaaaaaaaaaaa1", "scene": "inert.scene.json"},
            {"id": "aaaaaaaaaaaaaaa2", "scene": "optin.scene.json"}]})"));
        compose::ComposedScene s = compose::flatten("host.scene.json", r);
        CHECK(s.ok);
        // Only the composable root composed under the host; the inert one stayed inert.
        CHECK(find_entity(s, {"aaaaaaaaaaaaaaa2", "$root"}) != nullptr);
        CHECK(find_entity(s, {"aaaaaaaaaaaaaaa1", "$root"}) == nullptr);
    }

    // === Seam 12 — runtime save-migration groundwork: schemaVersion map + minimal runner (R-DATA-005)
    {
        migrate::MigrationSet set;
        std::string problem;
        CHECK(set.register_component("test:sprite", 2, problem));
        migrate::MigrationStep step;
        step.component_type = "test:sprite";
        step.from_version = 1;
        step.transform = [](serializer::JsonValue& p) {
            for (serializer::JsonMember& m : p.members)
                if (m.key == "tint")
                    m.key = "color";
            return true;
        };
        CHECK(set.register_step(std::move(step), problem));

        save::SaveDocument doc;
        doc.component_versions = {{"test:sprite", 1}};
        save::SaveEntity entity;
        entity.identity = 0xabcdef0123456789ULL;
        entity.components = one_component("test:sprite", R"({"tint": "red"})");
        doc.entities = {std::move(entity)};
        const std::uint64_t identity_before = doc.entities[0].identity;

        const save::SaveMigrationResult r = save::migrate_save(doc, set);
        CHECK(r.ok && r.changed);
        const std::int64_t* stamped = doc.saved_version("test:sprite");
        CHECK(stamped != nullptr && *stamped == 2);       // header re-stamped by the runner
        CHECK(doc.entities[0].identity == identity_before); // composed identity survives the upgrade
        const serializer::JsonValue* payload = member(doc.entities[0].components, "test:sprite");
        CHECK(payload != nullptr && member(*payload, "color") != nullptr);
    }

    // === Seam 13 — the meta `platforms` reservation (L-36) =======================================
    {
        assetdb::AssetMeta meta;
        meta.guid = "0123456789abcdef0123456789abcdef";
        meta.kind = "ctx:scene";
        const std::string bytes = assetdb::serialize_meta(meta);
        // The reserved (empty) platforms block is written at creation and carried verbatim.
        CHECK(bytes.find("\"platforms\"") != std::string::npos);
        std::vector<std::string> problems;
        const std::optional<assetdb::AssetMeta> parsed = assetdb::parse_meta(bytes, problems);
        CHECK(parsed.has_value());
        if (parsed.has_value())
            CHECK(parsed->guid == meta.guid);
    }

    // === Seam 14 — advisory override-hygiene tooling: redundant-override query (L-35) =============
    {
        const fs::path dir = m2exit::make_temp_project("seam14");
        seed_hierarchy(dir);
        // Write an override equal to the template value ([0,0,0]) -> redundant (a no-op override).
        const Envelope w = run({"set", "root.scene.json", "[0, 0, 0]", "--pointer", kPointer,
                                "--id-path", kFullC1, "--project", dir.string()});
        CHECK(w.ok());
        const Envelope q = run({"query", "--overrides", "redundant", "root.scene.json", "--project",
                                dir.string()});
        CHECK(q.ok());
        CHECK(q.data().at("advisory").as_bool());
        CHECK(q.data().at("count").as_int() == 1);
        m2exit::remove_quiet(dir);
    }

    M2_EXIT_MAIN_END();
}
