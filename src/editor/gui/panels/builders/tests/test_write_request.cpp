// The clean->compose write-request conversion (M9 e05d3): every target mode maps member-for-member,
// and the model-based convenience constructor keeps its L-35 outermost default. This conversion is
// what every kernel-side gateway routes through — a drifted member here would silently retarget
// authored writes, so it is pinned field by field.

#include "context/editor/gui/panels/builders/inspector_builder.h"

#include "context/editor/serializer/json_tree.h"

#include "builders_test.h"

#include <string>
#include <vector>

namespace builders = context::editor::gui::panels::builders;
namespace inspector = context::editor::gui::panels::inspector;
namespace compose = context::editor::compose;
namespace serializer = context::editor::serializer;

namespace
{

[[nodiscard]] serializer::JsonValue jstr(const std::string& s)
{
    serializer::JsonValue v;
    v.type = serializer::JsonValue::Type::string;
    v.string_value = s;
    return v;
}

} // namespace

int main()
{
    // --- the outermost default (the inspector panel's commit shape) -----------------------------
    {
        inspector::OverrideWriteRequest clean;
        clean.root_scene = "root.scene.json";
        clean.id_path = {"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"};
        clean.pointer = "/components/camera/fov";
        clean.value = jstr("v");

        const compose::WriteRequest converted = builders::to_write_request(clean);
        CHECK(converted.root_scene == clean.root_scene);
        CHECK(converted.id_path == clean.id_path);
        CHECK(converted.pointer == clean.pointer);
        CHECK(converted.value.type == serializer::JsonValue::Type::string);
        CHECK(converted.value.string_value == "v");
        CHECK(converted.target == compose::WriteTarget::outermost);
        CHECK(converted.at_instance.empty());
    }

    // --- the two retargets (the viewport's edit-target surface) ---------------------------------
    {
        inspector::OverrideWriteRequest clean;
        clean.root_scene = "root.scene.json";
        clean.id_path = {"e1"};
        clean.pointer = "/name";
        clean.target = inspector::OverrideWriteTarget::defining_template;
        CHECK(builders::to_write_request(clean).target == compose::WriteTarget::defining_template);

        clean.target = inspector::OverrideWriteTarget::at_instance;
        clean.at_instance = {"aaaaaaaaaaaaaaa1"};
        const compose::WriteRequest converted = builders::to_write_request(clean);
        CHECK(converted.target == compose::WriteTarget::at_instance);
        CHECK(converted.at_instance == clean.at_instance);
    }

    // --- the model-based convenience constructor keeps its L-35 outermost semantics -------------
    {
        inspector::InspectorModel model;
        model.root_scene = "root.scene.json";
        model.id_path = {"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"};
        model.has_entity = true;

        const compose::WriteRequest request =
            builders::override_write_request(model, "/components/camera/fov", jstr("1.5"));
        CHECK(request.root_scene == model.root_scene);
        CHECK(request.id_path == model.id_path);
        CHECK(request.pointer == "/components/camera/fov");
        CHECK(request.target == compose::WriteTarget::outermost);
    }

    BUILDERS_TEST_MAIN_END();
}
