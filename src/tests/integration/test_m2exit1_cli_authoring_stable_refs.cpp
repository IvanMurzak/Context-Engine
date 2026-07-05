// M2 exit criterion 1 — author a scene from the CLI, save/reload with STABLE REFERENCES (ROADMAP §1
// M2 Exit / issue #68): a GUID (composed identity, L-37) and an `$entity` reference (L-34) survive a
// restart. "Restart" is re-derivation from the files that ARE the truth (L-19): a second, independent
// FileSceneResolver over the same directory reproduces byte-identical composed identities — exactly
// what a rebooted daemon does when it re-reads the project from disk.
//
//   Leg A (real shipped binary, cross-process): `context new` authors a runnable scene as a REAL
//          separate process; the scaffolded scene re-derives to a stable composed identity twice.
//   Leg B (composed hierarchy, CLI-authored write): a nested root->mid->child instance hierarchy
//          carrying an `$entity` ref is authored with a real `context set` composed override (write
//          path #64); after a save/reload both the composed GUIDs and the `$entity` ref survive
//          byte-identically, and the CLI-authored override is present in the reloaded composition.
//
// R-QA-013: happy path (stable identity + ref across reload, override survives) + edge (identity is
// root-scene-bound) + failure-path control (a different root scene yields a different GUID).

#include "context/cli/app.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/stable_id.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

#include "m2_exit_test.h"
#include "process_util.h"

#include <string>
#include <vector>

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

using context::cli::run;
using context::editor::contract::Envelope;
namespace compose = context::editor::compose;
namespace serializer = context::editor::serializer;
namespace fs = std::filesystem;

namespace
{

// The nested instance hierarchy (mirrors the write-path #64 fixtures): root instances mid, mid
// instances child; child holds two entities, and c1 carries an `$entity` ref to its sibling c2.
const std::string kInstA = "aaaaaaaaaaaaaaa1"; // root -> mid
const std::string kInstB = "bbbbbbbbbbbbbbb1"; // mid -> child
const std::string kEntC1 = "ccccccccccccccc1"; // child entity (overridden + ref source)
const std::string kEntC2 = "ccccccccccccccc2"; // child entity (ref target)
const std::string kFullC1 = "aaaaaaaaaaaaaaa1/bbbbbbbbbbbbbbb1/ccccccccccccccc1";

void seed_hierarchy(const fs::path& dir)
{
    // child: c1 (Light) links to c2 (Prop) via an `$entity` id-path ref (L-34). The ref is authored
    // content that must survive the round-trip verbatim.
    m2exit::write_file_raw(dir / "child.scene.json", R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [
    {"id": "ccccccccccccccc1", "name": "Light",
     "components": {"transform": {"position": [0, 0, 0]},
                    "link": {"target": {"$entity": "ccccccccccccccc2"}}}},
    {"id": "ccccccccccccccc2", "name": "Prop", "components": {"transform": {"position": [7, 7, 7]}}}
  ]
})");
    m2exit::write_file_raw(dir / "mid.scene.json", R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [],
  "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "child.scene.json"}]
})");
    m2exit::write_file_raw(dir / "root.scene.json", R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [],
  "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "mid.scene.json"}]
})");
}

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
    const std::string bin = CONTEXT_BINARY;

    // ============================================================================================
    // Leg A — the REAL shipped `context` binary authors a runnable scene across a process boundary,
    //          and the authored file is a canonical fixpoint (a save/reload re-reads identical truth).
    // ============================================================================================
    {
        const fs::path project = m2exit::make_temp_project("newbin");
        ctest_proc::Process proc = ctest_proc::spawn(bin, {"new", project.string()});
        CHECK(ctest_proc::valid(proc));
        int code = -1;
        const bool done = ctest_proc::wait_for(proc, 30000, code);
        if (!done)
            ctest_proc::kill(proc);
        ctest_proc::release(proc);
        CHECK(done);
        CHECK(code == 0);
        CHECK(fs::exists(project / "project.json"));
        CHECK(fs::exists(project / "scenes" / "main.scene.json"));

        // The CLI authored a well-formed ctx:scene, and it is a canonical-JSON fixpoint (R-FILE-001):
        // re-canonicalizing the bytes is byte-identical, so a save/reload re-reads identical truth
        // (L-19). The composed-identity stability that survives a restart is proven in Leg B.
        const std::string scene_bytes = m2exit::read_file(project / "scenes" / "main.scene.json");
        const serializer::CanonicalizeResult parsed = serializer::canonicalize(scene_bytes);
        CHECK(parsed.is_json);
        CHECK(parsed.bytes == scene_bytes);
        m2exit::remove_quiet(project);
    }

    // ============================================================================================
    // Leg B — a CLI-authored composed override + an `$entity` ref survive save/reload byte-exactly.
    // ============================================================================================
    {
        const fs::path project = m2exit::make_temp_project("compose");
        seed_hierarchy(project);

        // Author a composed override through the REAL `context set` verb grammar (write path #64):
        // the outermost (root) scene records an override of c1's transform position.
        const Envelope wrote =
            run({"set", "root.scene.json", "[2, 2, 2]", "--pointer",
                 "/components/transform/position", "--id-path", kFullC1, "--project",
                 project.string()});
        CHECK(wrote.ok());
        CHECK(wrote.data().at("applied").as_bool());
        CHECK(wrote.data().at("file").as_string() == "root.scene.json");

        // Save/reload #1 — re-derive from the files (the CLI-authored override now lives in
        // root.scene.json on disk).
        m2exit::FileSceneResolver r1;
        CHECK(r1.load(project) == 3);
        compose::ComposedScene c1scene = compose::flatten("root.scene.json", r1);
        CHECK(c1scene.ok);
        CHECK(c1scene.diagnostics.empty());

        const compose::ComposedEntity* light = find_entity(c1scene, {kInstA, kInstB, kEntC1});
        const compose::ComposedEntity* prop = find_entity(c1scene, {kInstA, kInstB, kEntC2});
        CHECK(light != nullptr);
        CHECK(prop != nullptr);

        // The CLI-authored override survived the save/reload: the composed value is [2,2,2].
        const serializer::JsonValue* pos =
            compose::resolve_json_pointer(light->value, "/components/transform/position/0");
        CHECK(pos != nullptr && pos->int_value == 2);

        // The `$entity` ref (L-34) survived the round-trip verbatim, and its target is a real
        // composed entity (the reference resolves).
        const serializer::JsonValue* ref =
            compose::resolve_json_pointer(light->value, "/components/link/target/$entity");
        CHECK(ref != nullptr);
        CHECK(ref->string_value == kEntC2);

        const std::uint64_t light_guid = light->identity_hash;
        const std::uint64_t prop_guid = prop->identity_hash;
        const std::string light_guid_str = compose::format_stable_id(light_guid);

        // Restart — a SECOND independent resolver re-derives the same files (a rebooted daemon).
        m2exit::FileSceneResolver r2;
        CHECK(r2.load(project) == 3);
        compose::ComposedScene c2scene = compose::flatten("root.scene.json", r2);
        CHECK(c2scene.entities.size() == c1scene.entities.size());

        const compose::ComposedEntity* light2 = find_entity(c2scene, {kInstA, kInstB, kEntC1});
        const compose::ComposedEntity* prop2 = find_entity(c2scene, {kInstA, kInstB, kEntC2});
        CHECK(light2 != nullptr && prop2 != nullptr);

        // The GUIDs (composed identity, L-37) are byte-identical across the restart.
        CHECK(light2->identity_hash == light_guid);
        CHECK(prop2->identity_hash == prop_guid);
        CHECK(compose::format_stable_id(light2->identity_hash) == light_guid_str);

        // The `$entity` ref is byte-identical across the restart.
        const serializer::JsonValue* ref2 =
            compose::resolve_json_pointer(light2->value, "/components/link/target/$entity");
        CHECK(ref2 != nullptr && ref2->string_value == kEntC2);

        // The override, too, is stable across the restart.
        const serializer::JsonValue* pos2 =
            compose::resolve_json_pointer(light2->value, "/components/transform/position/0");
        CHECK(pos2 != nullptr && pos2->int_value == 2);

        // Identity is bound to the ROOT scene (L-37): the same id-path under a different root scene
        // is a DIFFERENT GUID — the failure-path control that proves the hash is not path-only.
        CHECK(compose::identity_hash_of("root.scene.json", light->id_path) == light_guid);
        CHECK(compose::identity_hash_of("other.scene.json", light->id_path) != light_guid);

        m2exit::remove_quiet(project);
    }

    M2_EXIT_MAIN_END();
}
