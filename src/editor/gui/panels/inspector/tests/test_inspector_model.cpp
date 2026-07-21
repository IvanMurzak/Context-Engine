// Inspector model tests (M5-F3): the schema -> editable-field derivation (R-CLI-005 introspection
// intersected with the composed entity), the L-35 override-provenance marking, the immutable /id +
// L-32 notes exclusion, and the override-write ENVELOPE routed through the REAL `context set` write
// path (compose::plan_write) to assert it lands an outermost override entry. Happy + edge + failure
// coverage (R-QA-013).

#include "context/editor/gui/panels/inspector/inspector_model.h"

#include "context/editor/gui/panels/builders/inspector_builder.h"

#include "context/editor/compose/compose_write.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

#include "inspector_test.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::inspector;
// The kernel-typed builders moved to context_gui_panel_builders (M9 e05d3, D10); the tests reach
// them through their new home while the panel library under test stays boundary-clean.
using context::editor::gui::panels::builders::build_inspector_model;
using context::editor::gui::panels::builders::override_write_request;
namespace compose = context::editor::compose;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

namespace
{

[[nodiscard]] JsonValue parse(const char* json)
{
    serializer::CanonicalizeResult r = serializer::canonicalize(json);
    CHECK(r.is_json);
    return r.root;
}

// A WriteResolver over an in-memory map of scene documents (the write counterpart of test_flatten's
// resolver — mirrors test_compose_write.cpp's MapWriteResolver). IS-A SceneResolver, so it also
// drives flatten().
class MapWriteResolver final : public compose::WriteResolver
{
public:
    void add(const std::string& path, const char* json)
    {
        JsonValue tree = parse(json);
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, tree);
        CHECK(doc.has_value());
        trees_[path] = std::move(tree);
        docs_[path] = std::move(*doc);
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        const auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const JsonValue* tree(std::string_view path) const override
    {
        const auto it = trees_.find(std::string(path));
        return it == trees_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, JsonValue, std::less<>> trees_;
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

const std::string kInst = "aaaaaaaaaaaaaaa1"; // root -> child instance id
const std::string kEnt = "ccccccccccccccc1";  // the composed entity id

// A two-level world: root instances child under kInst and OVERRIDES the child camera's fov; the
// child authors a full transform + camera on entity kEnt.
void seed(MapWriteResolver& r)
{
    r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Cam",
         "components": {
           "transform": {"position": [1, 2, 3]},
           "camera": {"fov": 1.0, "near": 0.1, "far": 500.0}
         }}
      ]})");
    r.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [],
      "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"}],
      "overrides": [
        {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"],
         "pointer": "/components/camera/fov", "value": 1.4, "base": 1.0}
      ]})");
}

[[nodiscard]] const compose::ComposedEntity* find_entity(const compose::ComposedScene& scene,
                                                         const std::vector<std::string>& id_path)
{
    for (const compose::ComposedEntity& e : scene.entities)
    {
        if (e.id_path == id_path)
        {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

int main()
{
    // --- the contribution id + edit command are the pinned R-EDIT-001 / R-A11Y-001 identities -------
    // (kept in test_inspector_panel; here we drive the model.)

    MapWriteResolver r;
    seed(r);
    const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);

    const compose::ComposedEntity* entity = find_entity(scene, {kInst, kEnt});
    CHECK(entity != nullptr);

    const schema::KindSchema* scene_schema = schema::engine_schemas().latest("ctx:scene");
    CHECK(scene_schema != nullptr);

    const InspectorModel model = build_inspector_model(*entity, *scene_schema, "root.scene.json");

    // --- the model carries the L-35 identity + the driving kind --------------------------------------
    CHECK(model.has_entity);
    CHECK(model.root_scene == "root.scene.json");
    CHECK(model.id_path == std::vector<std::string>({kInst, kEnt}));
    CHECK(model.identity == kInst + "/" + kEnt);
    CHECK(model.kind_id == "ctx:scene");

    // --- the schema-driven fields: name + the present component leaves, immutable /id + notes gone ---
    CHECK(find_field(model, "/name") != nullptr);
    CHECK(find_field(model, "/components/transform/position") != nullptr);
    CHECK(find_field(model, "/components/camera/fov") != nullptr);
    CHECK(find_field(model, "/components/camera/near") != nullptr);
    CHECK(find_field(model, "/components/camera/far") != nullptr);
    CHECK(model.fields.size() == 5);
    // The immutable identity field is never an editable row (L-37).
    CHECK(find_field(model, "/id") == nullptr);
    // Containers + the L-32 notes annotation are not editable leaf rows.
    CHECK(find_field(model, "/components") == nullptr);
    CHECK(find_field(model, "/components/transform") == nullptr);
    CHECK(find_field(model, "/notes") == nullptr);

    // --- widget kinds + units come from the schema (R-CLI-005) --------------------------------------
    const InspectorField* name = find_field(model, "/name");
    CHECK(name->kind == WidgetKind::text);
    CHECK(name->editable);
    const InspectorField* position = find_field(model, "/components/transform/position");
    CHECK(position->kind == WidgetKind::json); // an array -> a JSON-literal edit
    CHECK(position->units == "m");
    const InspectorField* fov = find_field(model, "/components/camera/fov");
    CHECK(fov->kind == WidgetKind::number);
    CHECK(fov->units == "rad");

    // --- L-35 override provenance is marked per field (only fov is overridden here) ------------------
    CHECK(fov->overridden);
    CHECK(!find_field(model, "/components/camera/near")->overridden);
    CHECK(!name->overridden);
    CHECK(model.override_count == 1);

    // --- the composed value flows through: fov reflects the override, not the template value ---------
    CHECK(fov->value.type == JsonValue::Type::number);
    CHECK(fov->value.number_value == 1.4);

    // --- the override-write ENVELOPE routes through the REAL context set write path (plan_write) -----
    // Editing camera/near lands an OUTERMOST override entry in the root scene (L-35 default), addressed
    // by the entity's full id-path — the single source of truth, not a parallel writer.
    {
        const compose::WriteRequest request =
            override_write_request(model, "/components/camera/near", parse("0.25"));
        CHECK(request.root_scene == "root.scene.json");
        CHECK(request.id_path == std::vector<std::string>({kInst, kEnt}));
        CHECK(request.pointer == "/components/camera/near");
        CHECK(request.target == compose::WriteTarget::outermost);

        const compose::WritePlan plan = compose::plan_write(request, r);
        CHECK(plan.ok);
        CHECK(plan.file == "root.scene.json");                 // the OUTERMOST instancing scene
        CHECK(plan.target == compose::WriteTarget::outermost);
        CHECK(plan.pointer == "/overrides/1/value");           // appended after the existing fov override
        CHECK(plan.base_recorded);                             // near is authored in the child template

        // The new override entry addresses the entity by the full id-path.
        const JsonValue* path = compose::resolve_json_pointer(plan.document, "/overrides/1/path");
        CHECK(path != nullptr && path->type == JsonValue::Type::array && path->elements.size() == 2);
    }

    // --- editing an already-overridden field UPDATES the existing entry (idempotent, not a 2nd one) --
    {
        const compose::WriteRequest request =
            override_write_request(model, "/components/camera/fov", parse("1.9"));
        const compose::WritePlan plan = compose::plan_write(request, r);
        CHECK(plan.ok);
        CHECK(plan.file == "root.scene.json");
        CHECK(plan.pointer == "/overrides/0/value"); // the EXISTING fov override entry, updated in place
    }

    // --- an empty (no-entity) model is the no-selection state ---------------------------------------
    {
        InspectorModel empty;
        CHECK(!empty.has_entity);
        CHECK(empty.fields.empty());
        CHECK(find_field(empty, "/name") == nullptr);
    }

    INSPECTOR_TEST_MAIN_END();
}
