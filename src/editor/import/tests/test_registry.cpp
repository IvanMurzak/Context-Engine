// Importer registry: extension routing (case-insensitive), collision refusal, ordering.

#include "context/editor/import/importer_registry.h"
#include "context/editor/import/importers/png_importer.h"

#include "import_test.h"

#include <memory>

using namespace context::editor::import;

int main()
{
    ImporterRegistry registry = default_importer_registry();
    CHECK(registry.size() == 3);

    // Extension routing (with + without dot handling via path).
    CHECK(registry.importer_for_path("assets/hero.png") != nullptr);
    CHECK(registry.importer_for_path("assets/hero.png")->id() == "png");
    CHECK(registry.importer_for_path("sfx/jump.wav")->id() == "wav");
    CHECK(registry.importer_for_path("models/tree.gltf")->id() == "gltf");
    CHECK(registry.importer_for_path("models/tree.glb")->id() == "gltf"); // both route to gltf

    // Case-insensitive.
    CHECK(registry.importer_for_path("HERO.PNG") != nullptr);
    CHECK(registry.importer_for_extension(".PnG")->id() == "png");

    // Unknown / extensionless -> no importer (never a silent wrong route).
    CHECK(registry.importer_for_path("notes.txt") == nullptr);
    CHECK(registry.importer_for_path("Makefile") == nullptr);
    CHECK(registry.importer_for_path(".gitignore") == nullptr); // dotfile, not an extension
    CHECK(registry.importer_for_extension(".xyz") == nullptr);

    // Registration order is preserved (deterministic tooling iteration).
    const auto all = registry.importers();
    CHECK(all.size() == 3);
    CHECK(all[0]->id() == "png");
    CHECK(all[1]->id() == "wav");
    CHECK(all[2]->id() == "gltf");

    // Collision is REFUSED, not last-writer-wins, and leaves the registry unchanged (atomic).
    {
        const RegisterResult result = registry.register_importer(std::make_unique<PngImporter>());
        CHECK(!result.ok);
        CHECK(result.conflict_extension == ".png");
        CHECK(result.conflict_importer == "png");
        CHECK(registry.size() == 3); // unchanged
    }

    // A fresh, disjoint registry accepts a single importer.
    {
        ImporterRegistry fresh;
        const RegisterResult result = fresh.register_importer(std::make_unique<PngImporter>());
        CHECK(result.ok);
        CHECK(fresh.size() == 1);
        // A null registration is a no-op failure, not a crash.
        CHECK(!fresh.register_importer(nullptr).ok);
        CHECK(fresh.size() == 1);
    }

    IMPORT_TEST_MAIN_END();
}
