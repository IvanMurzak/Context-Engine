// The M2 compose node (L-35 wired per the graph's node discipline): valid scene documents retain
// a composition view; each pass re-flattens the scenes it touched PLUS their transitive dependents
// (the graph's first cross-file dependency edges); composed() surfaces the flattened output with
// closure-aware L-31 stability; a failing ingest retains the last-good composed view (R-FILE-003).
// (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/compose/json_pointer.h"
#include "context/editor/derivation/derivation_graph.h"
#include "context/editor/schema/kind_schema.h"

#include "derivation_test.h"

#include <string>
#include <string_view>
#include <vector>

using context::editor::derivation::ComposedView;
using context::editor::derivation::DerivationGraph;
using context::editor::derivation::DerivePassResult;
using context::editor::filesync::ChangeType;
using context::editor::filesync::ReconcileChange;
namespace compose = context::editor::compose;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;

namespace
{

ReconcileChange change(std::string path, ChangeType type)
{
    ReconcileChange c;
    c.path = std::move(path);
    c.type = type;
    return c;
}

// Fixtures validate against the engine ctx:scene schema (the graph runs with the validate node
// wired), exercising the frozen M2 composition members end-to-end.
const char* kChild = R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [
    {"id": "ccccccccccccccc1", "name": "Light",
     "components": {"transform": {"position": [0, 0, 0]}}},
    {"id": "ccccccccccccccc2", "name": "Prop",
     "components": {"transform": {"position": [7, 7, 7]}}}
  ]
})";

const char* kChildMoved = R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [
    {"id": "ccccccccccccccc1", "name": "Light",
     "components": {"transform": {"position": [0, 0, 0]}}},
    {"id": "ccccccccccccccc2", "name": "Prop",
     "components": {"transform": {"position": [9, 9, 9]}}}
  ]
})";

// Same canonical form as kChildMoved (whitespace/key-order noise only).
const char* kChildMovedNoise = R"({
  "version": 1,   "$schema": "ctx:scene",
  "entities": [
    {"components": {"transform": {"position": [0, 0, 0]}}, "name": "Light",
     "id": "ccccccccccccccc1"},
    {"components": {"transform": {"position": [9,   9, 9]}}, "name": "Prop",
     "id": "ccccccccccccccc2"}
  ]
})";

// Fails ctx:scene validation ("entities" must be an array).
const char* kChildBroken = R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": {"broken": true}
})";

const char* kRoot = R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [],
  "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"}],
  "overrides": [
    {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"],
     "pointer": "/components/transform/position", "value": [5, 5, 5]}
  ]
})";

[[nodiscard]] std::int64_t position_x(const compose::ComposedEntity& entity)
{
    const serializer::JsonValue* x =
        compose::resolve_json_pointer(entity.value, "/components/transform/position/0");
    CHECK(x != nullptr);
    return x == nullptr ? -1 : x->int_value;
}

} // namespace

int main()
{
    // --- happy path: two scenes derive; both get composed views; the override applied ------------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        graph.apply(change("child.scene.json", ChangeType::created), kChild);
        graph.apply(change("root.scene.json", ChangeType::created), kRoot);
        DerivePassResult pass = graph.run_pass();
        CHECK(pass.nodes_derived == 2);
        CHECK(pass.scenes_composed == 2);

        std::optional<ComposedView> child = graph.composed("child.scene.json");
        CHECK(child.has_value());
        CHECK(child->scene->ok);
        CHECK(child->scene->entities.size() == 2); // the degenerate self-flatten
        CHECK(child->generation == pass.generation);
        CHECK(child->stable);

        std::optional<ComposedView> root = graph.composed("root.scene.json");
        CHECK(root.has_value());
        CHECK(root->scene->ok);
        CHECK(root->scene->entities.size() == 2);
        CHECK(position_x(root->scene->entities[0]) == 5); // the override won over the template
        CHECK(position_x(root->scene->entities[1]) == 7); // the untouched sibling flowed through
        CHECK(root->scene->entities[0].id_path ==
              (std::vector<std::string>{"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"}));

        // Composed identity is stable across a full graph rebuild (L-37: re-derivation-proof).
        const std::uint64_t identity = root->scene->entities[0].identity_hash;
        DerivationGraph rebuilt({}, nullptr, &schema::engine_schemas());
        rebuilt.apply(change("root.scene.json", ChangeType::created), kRoot);
        rebuilt.apply(change("child.scene.json", ChangeType::created), kChild);
        (void)rebuilt.run_pass();
        std::optional<ComposedView> again = rebuilt.composed("root.scene.json");
        CHECK(again.has_value());
        CHECK(again->scene->entities[0].identity_hash == identity);

        // Non-scene paths never compose.
        CHECK(!graph.composed("missing.scene.json").has_value());
    }

    // --- template-edit fan-out: editing the child re-flattens the dependent root ------------------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        graph.apply(change("child.scene.json", ChangeType::created), kChild);
        graph.apply(change("root.scene.json", ChangeType::created), kRoot);
        (void)graph.run_pass();

        // Stability: while the child edit is PENDING, the root's composed view is provisional
        // (its transitive template closure has queued work).
        graph.apply(change("child.scene.json", ChangeType::modified), kChildMoved);
        std::optional<ComposedView> provisional = graph.composed("root.scene.json");
        CHECK(provisional.has_value());
        CHECK(!provisional->stable);

        DerivePassResult pass = graph.run_pass();
        CHECK(pass.nodes_derived == 1);
        CHECK(pass.scenes_composed == 2); // the child AND its dependent root re-flattened

        std::optional<ComposedView> root = graph.composed("root.scene.json");
        CHECK(root.has_value());
        CHECK(root->stable);
        CHECK(root->generation == pass.generation);
        CHECK(position_x(root->scene->entities[0]) == 5); // the override still wins
        CHECK(position_x(root->scene->entities[1]) == 9); // the template edit fanned out

        // Memoization: a formatting-only re-ingest of the child derives to the SAME canonical
        // form — nothing recomposes, the composed generation is untouched (L-22 discipline).
        const std::uint64_t composed_gen = root->generation;
        graph.apply(change("child.scene.json", ChangeType::modified), kChildMovedNoise);
        DerivePassResult noise = graph.run_pass();
        CHECK(noise.nodes_skipped == 1);
        CHECK(noise.scenes_composed == 0);
        std::optional<ComposedView> after = graph.composed("root.scene.json");
        CHECK(after.has_value());
        CHECK(after->generation == composed_gen);
    }

    // --- last-good retention: a validation-failing child ingest leaves composition untouched ------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        graph.apply(change("child.scene.json", ChangeType::created), kChild);
        graph.apply(change("root.scene.json", ChangeType::created), kRoot);
        (void)graph.run_pass();

        graph.apply(change("child.scene.json", ChangeType::modified), kChildBroken);
        DerivePassResult pass = graph.run_pass();
        CHECK(pass.nodes_invalid == 1);
        CHECK(pass.scenes_composed == 0);

        std::optional<ComposedView> root = graph.composed("root.scene.json");
        CHECK(root.has_value());
        CHECK(root->scene->ok);
        CHECK(root->scene->entities.size() == 2); // the last-good flatten survived (R-FILE-003)
        CHECK(position_x(root->scene->entities[1]) == 7);
    }

    // --- removal: the template disappears; the dependent re-flattens with missing_scene -----------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        graph.apply(change("child.scene.json", ChangeType::created), kChild);
        graph.apply(change("root.scene.json", ChangeType::created), kRoot);
        (void)graph.run_pass();

        graph.apply(change("child.scene.json", ChangeType::removed), "");
        DerivePassResult pass = graph.run_pass();
        CHECK(pass.nodes_removed == 1);
        CHECK(pass.scenes_composed == 1); // only the dependent root re-flattened

        CHECK(!graph.composed("child.scene.json").has_value());
        std::optional<ComposedView> root = graph.composed("root.scene.json");
        CHECK(root.has_value());
        CHECK(!root->scene->ok);
        CHECK(root->scene->entities.empty());
        bool missing = false;
        for (const compose::ComposeDiagnostic& d : root->scene->diagnostics)
            missing = missing || d.code == "compose.missing_scene";
        CHECK(missing);
    }

    // --- a compose-participating scene works with NO validator wired (the M1-compat path) ---------
    {
        DerivationGraph graph; // no schema set: validation off, composition still on
        graph.apply(change("child.scene.json", ChangeType::created), kChild);
        graph.apply(change("root.scene.json", ChangeType::created), kRoot);
        DerivePassResult pass = graph.run_pass();
        CHECK(pass.scenes_composed == 2);
        std::optional<ComposedView> root = graph.composed("root.scene.json");
        CHECK(root.has_value());
        CHECK(position_x(root->scene->entities[0]) == 5);
    }

    // --- non-scene JSON derives normally and never composes ---------------------------------------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        graph.apply(change("project.json", ChangeType::created),
                    R"({"$schema": "ctx:project", "version": 1, "engine": "context",
                        "name": "demo", "scene": "scenes/main.scene.json"})");
        DerivePassResult pass = graph.run_pass();
        CHECK(pass.nodes_derived == 1);
        CHECK(pass.scenes_composed == 0);
        CHECK(!graph.composed("project.json").has_value());
    }

    DERIVATION_TEST_MAIN_END();
}
