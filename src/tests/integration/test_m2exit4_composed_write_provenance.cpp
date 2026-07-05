// M2 exit criterion 4 — a composed-entity write lands in the CORRECT FILE/LEVEL and reports its
// PROVENANCE CHAIN (ROADMAP §1 M2 Exit / issue #68; R-CLI-006 / L-35, write path #64). Driven
// end-to-end through the real `context set` verb grammar over a nested root->mid->child hierarchy on
// disk, then read back through flatten's provenance surface:
//   write side  — the three write targets each land in the RIGHT file at the RIGHT level:
//                 default-outermost -> the outermost (root) scene; --at-instance -> the mid scene;
//                 --edit-template -> the defining (child) scene, editing the template in place;
//   read side   — after two overrides at different levels, the provenance chain for the overridden
//                 pointer is winning-value-first: outermost override (level 0), then the mid-level
//                 override (level 1), then the defining template (level 2) — every contributor visible.
//
// R-QA-013: happy path (3 write targets + provenance chain) + failure path (--edit-template and
// --at-instance are mutually exclusive).

#include "context/cli/app.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"

#include "m2_exit_test.h"

#include <string>
#include <vector>

using context::cli::run;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
namespace compose = context::editor::compose;
namespace fs = std::filesystem;

namespace
{

const std::string kInstA = "aaaaaaaaaaaaaaa1";
const std::string kInstB = "bbbbbbbbbbbbbbb1";
const std::string kEntC1 = "ccccccccccccccc1";
const std::string kFullC1 = "aaaaaaaaaaaaaaa1/bbbbbbbbbbbbbbb1/ccccccccccccccc1";
const std::string kPointer = "/components/transform/position";

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
    // --- write side: default-outermost lands in the OUTERMOST (root) scene at level 0 -------------
    {
        const fs::path dir = m2exit::make_temp_project("outermost");
        seed_hierarchy(dir);
        const Envelope env = run({"set", "root.scene.json", "[9, 9, 9]", "--pointer", kPointer,
                                  "--id-path", kFullC1, "--project", dir.string()});
        CHECK(env.ok());
        CHECK(env.data().at("file").as_string() == "root.scene.json");
        CHECK(env.data().at("target").as_string() == "outermost");
        // The override with a recorded base landed in root.scene.json, addressed by the full id-path.
        const Json doc = Json::parse(m2exit::read_file(dir / "root.scene.json"));
        CHECK(doc.at("overrides").size() == 1);
        CHECK(doc.at("overrides").at(std::size_t{0}).at("path").size() == 3);
        m2exit::remove_quiet(dir);
    }

    // --- write side: --at-instance lands in the MID scene (a mid-level override) -------------------
    {
        const fs::path dir = m2exit::make_temp_project("atinstance");
        seed_hierarchy(dir);
        const Envelope env = run({"set", "root.scene.json", "[3, 3, 3]", "--pointer", kPointer,
                                  "--id-path", kFullC1, "--at-instance", kInstA, "--project",
                                  dir.string()});
        CHECK(env.ok());
        CHECK(env.data().at("file").as_string() == "mid.scene.json");
        CHECK(env.data().at("target").as_string() == "at-instance");
        // The override lands mid-level, addressed by the SUFFIX path [bbb1, ccc1] (not the full id).
        const Json mid = Json::parse(m2exit::read_file(dir / "mid.scene.json"));
        CHECK(mid.at("overrides").at(std::size_t{0}).at("path").size() == 2);
        // The outermost scene was NOT touched.
        const Json root = Json::parse(m2exit::read_file(dir / "root.scene.json"));
        CHECK(!root.contains("overrides"));
        m2exit::remove_quiet(dir);
    }

    // --- write side: --edit-template edits the DEFINING (child) scene in place ---------------------
    {
        const fs::path dir = m2exit::make_temp_project("template");
        seed_hierarchy(dir);
        const Envelope env = run({"set", "root.scene.json", "[5, 5, 5]", "--pointer", kPointer,
                                  "--id-path", kFullC1, "--edit-template", "--project",
                                  dir.string()});
        CHECK(env.ok());
        CHECK(env.data().at("file").as_string() == "child.scene.json");
        CHECK(env.data().at("target").as_string() == "template");
        // The child template value itself changed; no override entry was written anywhere.
        const Json child = Json::parse(m2exit::read_file(dir / "child.scene.json"));
        CHECK(child.at("entities")
                  .at(std::size_t{0})
                  .at("components")
                  .at("transform")
                  .at("position")
                  .at(std::size_t{0})
                  .as_int() == 5);
        const Json root = Json::parse(m2exit::read_file(dir / "root.scene.json"));
        CHECK(!root.contains("overrides"));
        m2exit::remove_quiet(dir);
    }

    // --- failure path: --edit-template and --at-instance are mutually exclusive --------------------
    {
        const fs::path dir = m2exit::make_temp_project("mutex");
        seed_hierarchy(dir);
        const Envelope env = run({"set", "root.scene.json", "[3, 3, 3]", "--pointer", kPointer,
                                  "--id-path", kFullC1, "--edit-template", "--at-instance", kInstA,
                                  "--project", dir.string()});
        CHECK(!env.ok());
        CHECK(env.error().has_value() && env.error()->code == "usage.invalid");
        m2exit::remove_quiet(dir);
    }

    // --- read side: the provenance chain reports EVERY contributor, winning-value-first -----------
    {
        const fs::path dir = m2exit::make_temp_project("provenance");
        seed_hierarchy(dir);
        // An outermost override (root, level 0) AND a mid-level override (level 1) on the SAME
        // pointer, both authored through the real `context set` verb.
        const Envelope outer = run({"set", "root.scene.json", "[2, 2, 2]", "--pointer", kPointer,
                                    "--id-path", kFullC1, "--project", dir.string()});
        CHECK(outer.ok());
        const Envelope mid = run({"set", "root.scene.json", "[1, 1, 1]", "--pointer", kPointer,
                                  "--id-path", kFullC1, "--at-instance", kInstA, "--project",
                                  dir.string()});
        CHECK(mid.ok());

        m2exit::FileSceneResolver resolver;
        CHECK(resolver.load(dir) == 3);
        compose::ComposedScene scene = compose::flatten("root.scene.json", resolver);
        CHECK(scene.ok);
        const compose::ComposedEntity* light = find_entity(scene, {kInstA, kInstB, kEntC1});
        CHECK(light != nullptr);
        if (light != nullptr)
        {
            const std::vector<compose::ProvenanceEntry> chain =
                compose::provenance_for(*light, kPointer);
            CHECK(chain.size() == 3);
            if (chain.size() == 3)
            {
                using Source = compose::ProvenanceEntry::Source;
                // [0] the winning outermost override (root, level 0).
                CHECK(chain[0].source == Source::override_value);
                CHECK(chain[0].file == "root.scene.json");
                CHECK(chain[0].level == 0);
                // [1] the mid-level override (level 1).
                CHECK(chain[1].source == Source::override_value);
                CHECK(chain[1].file == "mid.scene.json");
                CHECK(chain[1].level == 1);
                // [2] the defining template value (child, level 2).
                CHECK(chain[2].source == Source::template_value);
                CHECK(chain[2].file == "child.scene.json");
                CHECK(chain[2].level == 2);
            }
        }
        m2exit::remove_quiet(dir);
    }

    M2_EXIT_MAIN_END();
}
