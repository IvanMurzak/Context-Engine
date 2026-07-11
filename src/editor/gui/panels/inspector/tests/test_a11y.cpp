// The inspector panel's per-panel a11y scan + keyboard-only navigation assertion (R-A11Y-001 /
// R-EDIT-001), headless on the default matrix (no CEF). This is the M5-F3 half of the a11y-harness
// coverage the M5-F6 harness reconciles; its ctest name is registered in the defensive coverage
// fragment src/editor/gui/a11y/coverage/inspector.json. Asserts EVERY editable widget the panel
// renders has a keyboard path and no accessibility violation, across no-selection / populated /
// overridden / all-readonly worlds.

#include "context/editor/gui/panels/inspector/inspector_model.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/gui/uitree/panel.h"

#include "context/editor/serializer/json_tree.h"

#include "inspector_test.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::inspector;
namespace serializer = context::editor::serializer;
namespace uitree = context::editor::gui::uitree;
using serializer::JsonValue;

namespace
{

[[nodiscard]] JsonValue jstr(const std::string& s)
{
    JsonValue v;
    v.type = JsonValue::Type::string;
    v.string_value = s;
    return v;
}
[[nodiscard]] JsonValue jbool(bool b)
{
    JsonValue v;
    v.type = JsonValue::Type::boolean;
    v.boolean_value = b;
    return v;
}

[[nodiscard]] InspectorField field(const std::string& pointer, WidgetKind kind, JsonValue value,
                                   bool overridden = false)
{
    InspectorField f;
    f.pointer = pointer;
    f.label = pointer;
    f.kind = kind;
    f.value = std::move(value);
    f.overridden = overridden;
    f.editable = kind != WidgetKind::readonly;
    return f;
}

// Editable-field count = the number of focusable widgets the panel must render (one keyboard stop each).
[[nodiscard]] std::size_t editable_count(const InspectorModel& model)
{
    std::size_t n = 0;
    for (const InspectorField& f : model.fields)
    {
        if (f.editable)
        {
            ++n;
        }
    }
    return n;
}

// The a11y + keyboard-nav gate for one model: zero violations AND every editable widget reachable.
void assert_a11y_clean(const InspectorModel& model)
{
    InspectorPanel panel;
    panel.set_model(model, 0);
    const uitree::Panel ui = panel.build_panel();

    CHECK(uitree::audit_a11y(ui).empty());
    // Keyboard-only navigation reaches every editable widget (R-A11Y-001 complete keyboard nav).
    CHECK(uitree::focus_order(ui).size() == editable_count(model));
}

} // namespace

int main()
{
    // No selection: an a11y-clean placeholder with no dangling command and no focusable widget.
    {
        InspectorModel model; // has_entity == false
        assert_a11y_clean(model);
    }

    // Populated world: mixed widget kinds (text / checkbox / json) + a readonly field.
    {
        InspectorModel model;
        model.root_scene = "root.scene.json";
        model.id_path = {"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"};
        model.identity = "aaaaaaaaaaaaaaa1/ccccccccccccccc1";
        model.kind_id = "ctx:scene";
        model.has_entity = true;
        model.fields.push_back(field("/name", WidgetKind::text, jstr("Cam")));
        model.fields.push_back(field("/components/light/enabled", WidgetKind::toggle, jbool(false)));
        model.fields.push_back(field("/components/transform/position", WidgetKind::json,
                                     jstr("[1,2,3]"), /*overridden=*/true));
        model.fields.push_back(field("/note", WidgetKind::readonly, jstr("baked")));
        assert_a11y_clean(model);

        // The overridden marker is visible in the rendered surface.
        InspectorPanel panel;
        panel.set_model(model, 0);
        const std::string html = uitree::render_html(panel.build_panel());
        CHECK(html.find("(overridden)") != std::string::npos);
    }

    // All-readonly world: no editable field -> the panel exposes NO command (no unreachable-command).
    {
        InspectorModel model;
        model.identity = "x";
        model.kind_id = "ctx:scene";
        model.has_entity = true;
        model.fields.push_back(field("/note", WidgetKind::readonly, jstr("baked")));
        assert_a11y_clean(model);

        InspectorPanel panel;
        panel.set_model(model, 0);
        const uitree::Panel ui = panel.build_panel();
        CHECK(!ui.has_command(InspectorPanel::kEditCommand)); // no dangling command
        CHECK(uitree::focus_order(ui).empty());
    }

    // An entity with NO fields at all (e.g. only immutable identity) is still a11y-clean.
    {
        InspectorModel model;
        model.identity = "y";
        model.kind_id = "ctx:scene";
        model.has_entity = true;
        assert_a11y_clean(model);
    }

    // The registered default (no-selection) panel — exactly what the M5-F6 harness scans — is clean.
    {
        const uitree::Panel ui = InspectorPanel{}.build_panel();
        CHECK(uitree::audit_a11y(ui).empty());
    }

    INSPECTOR_TEST_MAIN_END();
}
