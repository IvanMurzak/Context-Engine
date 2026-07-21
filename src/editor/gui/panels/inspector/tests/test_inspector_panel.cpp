// Inspector panel tests (M5-F3): the schema-driven widget tree (a11y-conformant + keyboard-reachable),
// the L-20 gesture-end commit through the `context set` write path (via an in-memory gateway), and the
// L-30 rebase-or-drop policy under a concurrent writer — REBASE when an unrelated field moved, DROP
// loudly (never a silent overwrite) when THIS field path collided. Happy + edge + failure (R-QA-013).

#include "context/editor/gui/panels/inspector/inspector_panel.h"

#include "context/editor/gui/panels/inspector/inspector_model.h"
#include "context/editor/gui/uitree/panel.h"

#include "context/editor/serializer/json_tree.h"

#include "inspector_test.h"

#include <cstdint>
#include <functional>
#include <map>
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
[[nodiscard]] JsonValue jnum(double d)
{
    JsonValue v;
    v.type = JsonValue::Type::number;
    v.number_value = d;
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

// A model with a text, a checkbox, a JSON (overridden) editable field + one readonly field.
[[nodiscard]] InspectorModel make_model()
{
    InspectorModel m;
    m.root_scene = "root.scene.json";
    m.id_path = {"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"};
    m.identity = "aaaaaaaaaaaaaaa1/ccccccccccccccc1";
    m.kind_id = "ctx:scene";
    m.has_entity = true;
    m.fields.push_back(field("/name", WidgetKind::text, jstr("Cam")));
    m.fields.push_back(field("/components/light/enabled", WidgetKind::toggle, jbool(true)));
    m.fields.push_back(
        field("/components/camera/fov", WidgetKind::number, jnum(1.4), /*overridden=*/true));
    m.fields.push_back(field("/note", WidgetKind::readonly, jstr("baked")));
    m.override_count = 1;
    return m;
}

// An in-memory gateway over a single mutable target file: a raw hash + the current composed value per
// pointer. A concurrent writer is simulated by `on_first_attempt`, fired once inside the FIRST
// attempt() (before its CAS check) — it advances the file hash and, optionally, mutates a field.
class FakeGateway final : public OverrideWriteGateway
{
public:
    mutable std::uint64_t file_hash = 100;
    mutable std::map<std::string, JsonValue> field_values;
    mutable int attempts = 0;
    std::function<void()> on_first_attempt;
    mutable bool fired = false;

    WriteAttempt attempt(const OverrideWriteRequest& request,
                         std::uint64_t expected_raw_hash) const override
    {
        ++attempts;
        if (on_first_attempt && !fired)
        {
            fired = true;
            on_first_attempt();
        }
        WriteAttempt a;
        if (expected_raw_hash == file_hash)
        {
            file_hash += 1; // the write advances the file's raw bytes
            field_values[request.pointer] = request.value;
            a.applied = true;
            a.file = "root.scene.json";
            a.pointer = "/overrides/0/value";
            a.raw_hash = file_hash;
        }
        else
        {
            a.cas_mismatch = true;
            a.code = "cas.mismatch";
            a.raw_hash = file_hash;
        }
        return a;
    }

    FieldState read(const std::string&, const std::vector<std::string>&,
                    const std::string& pointer) const override
    {
        FieldState s;
        s.present = true;
        s.raw_hash = file_hash;
        const auto it = field_values.find(pointer);
        if (it != field_values.end())
        {
            s.value = it->second;
        }
        return s;
    }
};

[[nodiscard]] std::size_t count(const std::string& haystack, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
    {
        ++n;
    }
    return n;
}

} // namespace

int main()
{
    // --- the pinned R-EDIT-001 / R-A11Y-001 identities ----------------------------------------------
    CHECK(std::string(InspectorPanel::kContributionId) == "builtin.inspector");
    CHECK(std::string(InspectorPanel::kEditCommand) == "inspector.edit");

    // --- build_panel is a11y-conformant + keyboard-reachable (the primary CI assertion) -------------
    {
        InspectorPanel panel;
        panel.set_model(make_model(), 100);
        const uitree::Panel ui = panel.build_panel();

        CHECK(uitree::audit_a11y(ui).empty()); // no a11y violations by construction

        // Every EDITABLE field is a focusable widget (readonly /note is not) -> a complete keyboard
        // path for every edit (R-A11Y-001 / R-CLI-001).
        const std::vector<std::string> order = uitree::focus_order(ui);
        CHECK(order.size() == 3);
        CHECK(ui.has_command(InspectorPanel::kEditCommand));

        const std::string html = uitree::render_html(ui);
        CHECK(html.find("role=\"checkbox\"") != std::string::npos); // the toggle widget
        CHECK(html.find("role=\"textbox\"") != std::string::npos);  // text / number / json widgets
        CHECK(count(html, "(overridden)") == 1);                    // the fov field is L-35 overridden
    }

    // --- stage_edit validates the field (unknown / readonly rejected) -------------------------------
    {
        InspectorPanel panel;
        panel.set_model(make_model(), 100);
        CHECK(!panel.stage_edit("/ghost", jstr("x")));               // unknown field
        CHECK(!panel.stage_edit("/note", jstr("x")));                // readonly field
        CHECK(panel.stage_edit("/name", jstr("NewCam")));            // editable field
        CHECK(panel.has_staged_edit());
        CHECK(panel.staged_pointer() == "/name");
        panel.discard_edit();
        CHECK(!panel.has_staged_edit());
    }

    // --- commit with no gateway / no staged edit is a no-op -----------------------------------------
    {
        InspectorPanel panel;
        panel.set_model(make_model(), 100);
        CHECK(panel.stage_edit("/name", jstr("NewCam")));
        CHECK(panel.commit().status == CommitResult::Status::none); // no gateway
    }

    // --- happy path: a CAS-guarded commit applies the override write --------------------------------
    {
        FakeGateway gw;
        gw.field_values["/name"] = jstr("Cam");
        InspectorPanel panel(&gw);
        panel.set_model(make_model(), 100);

        CommitResult seen;
        panel.add_commit_listener([&](const CommitResult& r) { seen = r; });

        CHECK(panel.stage_edit("/name", jstr("NewCam")));
        const CommitResult result = panel.commit();
        CHECK(result.status == CommitResult::Status::applied);
        CHECK(result.ok());
        CHECK(result.file == "root.scene.json");
        CHECK(result.written_pointer == "/overrides/0/value");
        CHECK(gw.attempts == 1);
        CHECK(!panel.has_staged_edit());     // the gesture consumed
        CHECK(panel.base_raw_hash() == 101); // the CAS token advanced to the new file hash
        CHECK(seen.status == CommitResult::Status::applied); // the R-HUX-011 loop event fired
    }

    // --- L-30 REBASE: a concurrent writer touched an UNRELATED field -> rebase onto the new state ---
    {
        FakeGateway gw;
        gw.field_values["/components/camera/near"] = jnum(0.1); // this field's base value
        // The concurrent writer advances the file + edits /name (NOT camera/near).
        gw.on_first_attempt = [&]() {
            gw.file_hash = 101;
            gw.field_values["/name"] = jstr("Hijacked");
        };

        InspectorPanel panel(&gw);
        InspectorModel model = make_model();
        model.fields.push_back(field("/components/camera/near", WidgetKind::number, jnum(0.1)));
        panel.set_model(model, 100);

        CHECK(panel.stage_edit("/components/camera/near", jnum(0.25)));
        const CommitResult result = panel.commit();
        CHECK(result.status == CommitResult::Status::rebased); // untouched field path -> rebased
        CHECK(result.ok());
        CHECK(gw.attempts == 2);             // first (stale) + the rebased attempt
        CHECK(!panel.has_staged_edit());
    }

    // --- L-30 DROP: a concurrent writer touched THIS field path -> dropped loudly, never overwrite --
    {
        FakeGateway gw;
        gw.field_values["/components/camera/near"] = jnum(0.1); // base
        // The concurrent writer advances the file AND changes the SAME field the gesture edits.
        gw.on_first_attempt = [&]() {
            gw.file_hash = 101;
            gw.field_values["/components/camera/near"] = jnum(0.9);
        };

        InspectorPanel panel(&gw);
        InspectorModel model = make_model();
        model.fields.push_back(field("/components/camera/near", WidgetKind::number, jnum(0.1)));
        panel.set_model(model, 100);

        CommitResult seen;
        panel.add_commit_listener([&](const CommitResult& r) { seen = r; });

        CHECK(panel.stage_edit("/components/camera/near", jnum(0.25)));
        const CommitResult result = panel.commit();
        CHECK(result.status == CommitResult::Status::dropped); // collided field -> loud drop
        CHECK(!result.ok());
        CHECK(result.code == "cas.mismatch"); // reuses the existing catalog code (no new mint)
        CHECK(!result.message.empty());       // the loud L-30 diagnostic
        CHECK(gw.attempts == 1);              // never a second (overwriting) attempt after the collision
        CHECK(!panel.has_staged_edit());      // the gesture was dropped, not left dangling
        CHECK(seen.status == CommitResult::Status::dropped); // the drop is a visible event (R-HUX-011)
    }

    // --- write-path refusal (e.g. an unresolved target) surfaces as an error, not a drop -----------
    {
        FakeGateway gw;
        // A gateway that refuses with a compose error on the first attempt (never applies).
        struct RefusingGateway final : public OverrideWriteGateway
        {
            WriteAttempt attempt(const OverrideWriteRequest&, std::uint64_t) const override
            {
                WriteAttempt a;
                a.code = "compose.write_target_not_found";
                a.message = "no such composed entity";
                return a; // neither applied nor cas_mismatch
            }
            FieldState read(const std::string&, const std::vector<std::string>&,
                            const std::string&) const override
            {
                return FieldState{};
            }
        } refusing;

        InspectorPanel panel(&refusing);
        panel.set_model(make_model(), 100);
        CHECK(panel.stage_edit("/name", jstr("NewCam")));
        const CommitResult result = panel.commit();
        CHECK(result.status == CommitResult::Status::error);
        CHECK(result.code == "compose.write_target_not_found");
        CHECK(panel.has_staged_edit()); // a refusal keeps the gesture for the caller to fix
    }

    // --- the no-selection model renders an a11y-clean placeholder with no command -------------------
    {
        InspectorPanel panel;
        const uitree::Panel ui = panel.build_panel();
        CHECK(uitree::audit_a11y(ui).empty());
        CHECK(!ui.has_command(InspectorPanel::kEditCommand));
        CHECK(uitree::focus_order(ui).empty());
        CHECK(uitree::render_html(ui).find("No entity selected") != std::string::npos);
    }

    // --- deterministic (stable) re-render: identical state -> byte-identical HTML --------------------
    {
        InspectorPanel panel;
        panel.set_model(make_model(), 100);
        const std::string first = uitree::render_html(panel.build_panel());
        const std::string second = uitree::render_html(panel.build_panel());
        CHECK(first == second);
    }

    INSPECTOR_TEST_MAIN_END();
}
