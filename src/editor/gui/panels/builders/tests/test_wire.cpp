// The wire projection of the panel models (M9 e05d3): shape, hex-string hashes (a JSON number is a
// double — bits above 2^53 would vanish), canonical field values, the present:false minimal form,
// and determinism (identical model -> byte-identical dump). The Shell-side PARSE half + the full
// build->wire->parse round-trip live with the feeds (src/editor/shell/panels/tests/), which link
// both halves.

#include "context/editor/gui/panels/builders/wire.h"

#include "context/editor/serializer/json_tree.h"

#include "builders_test.h"

#include <cstdint>
#include <string>

namespace builders = context::editor::gui::panels::builders;
namespace scenetree = context::editor::gui::panels::scenetree;
namespace inspector = context::editor::gui::panels::inspector;
namespace serializer = context::editor::serializer;
using Json = context::editor::contract::Json;

namespace
{

[[nodiscard]] scenetree::SceneTreeModel small_tree()
{
    scenetree::SceneTreeModel model;
    model.root_scene = "root.scene.json";
    model.ok = true;
    model.entity_count = 1;
    scenetree::SceneTreeNode boundary;
    boundary.identity = "inst1";
    boundary.kind = scenetree::NodeKind::instance;
    boundary.display_name = "inst1";
    scenetree::SceneTreeNode child;
    child.identity = "inst1/ent1";
    child.kind = scenetree::NodeKind::entity;
    child.display_name = "Cam";
    child.identity_hash = 0xdeadbeef01ull;
    child.overridden = true;
    boundary.children.push_back(child);
    model.roots.push_back(boundary);
    return model;
}

} // namespace

int main()
{
    // --- scene tree: shape + the hex-hash discipline --------------------------------------------
    {
        const Json wire = builders::scene_tree_to_wire(small_tree());
        CHECK(wire.at("rootScene").as_string() == "root.scene.json");
        CHECK(wire.at("ok").as_bool());
        CHECK(wire.at("entityCount").as_int() == 1);
        CHECK(wire.at("roots").size() == 1u);
        const Json& boundary = wire.at("roots").at(std::size_t{0});
        CHECK(boundary.at("kind").as_string() == "instance");
        CHECK(boundary.at("identityHash").as_string() == "0000000000000000"); // no own composed entity
        const Json& child = boundary.at("children").at(std::size_t{0});
        CHECK(child.at("kind").as_string() == "entity");
        // The zero-padded 16-char format_stable_id form — the flatten/pack "identityHash" rendering.
        CHECK(child.at("identityHash").as_string() == "000000deadbeef01"); // lowercase hex STRING
        CHECK(child.at("overridden").as_bool());

        // Determinism: identical model -> byte-identical wire (the render-stability discipline).
        CHECK(builders::scene_tree_to_wire(small_tree()).dump(0) == wire.dump(0));
    }

    // --- inspector: canonical values + the present:false minimal form ---------------------------
    {
        inspector::InspectorModel model;
        model.has_entity = true;
        model.root_scene = "root.scene.json";
        model.id_path = {"inst1", "ent1"};
        model.identity = "inst1/ent1";
        model.identity_hash = 0xff00ffull;
        model.kind_id = "ctx:scene";
        inspector::InspectorField field;
        field.pointer = "/components/camera/fov";
        field.label = "fov";
        field.units = "rad";
        field.kind = inspector::WidgetKind::number;
        serializer::JsonValue value;
        value.type = serializer::JsonValue::Type::number;
        value.number_value = 1.5;
        field.value = value;
        field.overridden = true;
        field.editable = true;
        model.fields.push_back(field);
        model.override_count = 1;

        const Json wire = builders::inspector_to_wire(model);
        CHECK(wire.at("present").as_bool());
        CHECK(wire.at("identityHash").as_string() == "0000000000ff00ff");
        CHECK(wire.at("idPath").size() == 2u);
        CHECK(wire.at("overrideCount").as_int() == 1);
        const Json& wf = wire.at("fields").at(std::size_t{0});
        CHECK(wf.at("kind").as_string() == "number");
        CHECK(wf.at("value").as_string() == "1.5"); // the CANONICAL byte form, as a string
        CHECK(wf.at("units").as_string() == "rad");
        CHECK(wf.at("overridden").as_bool());
        CHECK(builders::inspector_to_wire(model).dump(0) == wire.dump(0)); // deterministic

        // No selection serializes as {present:false} ALONE — nothing to over-read.
        const Json none = builders::inspector_to_wire(inspector::InspectorModel{});
        CHECK(!none.at("present").as_bool());
        CHECK(none.size() == 1u);
    }

    BUILDERS_TEST_MAIN_END();
}
