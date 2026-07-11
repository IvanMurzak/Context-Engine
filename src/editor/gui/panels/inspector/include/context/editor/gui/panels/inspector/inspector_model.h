// The inspector view model (M5-F3, R-EDIT-001 / R-DATA-002 / L-35 / R-CLI-005): the schema-driven,
// CEF-free editable projection of the selected composed entity the inspector panel renders. Kept
// separate from the panel's uitree rendering (mirroring scene_tree_model / scene_tree_panel) so the
// schema -> editable-field derivation is asserted on its own, and so the write-envelope construction
// (an L-35 override write through the `context set` path) is a pure, testable function.
//
// The fields are derived from the entity's kind schema via the R-CLI-005 introspection surface
// (schema::introspection_json) intersected with the entity's composed value — only the components the
// entity actually carries are surfaced, each field carrying its declared type + units + L-35
// override provenance. This is an observer+edit surface: it reads the composed derived world and
// commits edits back through the ONE `context set` write path (no parallel writer) — see
// inspector_panel.h.

#pragma once

#include "context/editor/compose/compose_write.h" // compose::WriteRequest
#include "context/editor/compose/flatten.h"       // compose::ComposedEntity
#include "context/editor/schema/kind_schema.h"    // schema::KindSchema
#include "context/editor/serializer/json_tree.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::gui::panels::inspector
{

// How one field is edited in the inspector (drives the uitree widget the panel renders).
enum class WidgetKind
{
    text,     // a string field -> a textbox
    number,   // a number/integer field (SI units) -> a textbox
    toggle,   // a boolean field -> a checkbox
    json,     // a composite (array / union) field edited as a JSON literal -> a textbox
    readonly, // a non-editable field (e.g. a binary x-ctx-sidecar) -> inert text
};

// One editable field of the selected entity. `pointer` is the entity-RELATIVE JSON pointer (the
// exact `compose::WriteRequest::pointer` the override write uses, e.g. `/name`,
// `/components/camera/fov`, `/components/transform/position`). `value` is the current COMPOSED value
// (overrides applied). `overridden` marks a field an L-35 override contributor touched, so overrides
// are visible in the inspector, not hidden.
struct InspectorField
{
    std::string pointer;                 // entity-relative JSON pointer (the write-request pointer)
    std::string label;                   // the field's leaf name (the accessible name / grep key)
    std::string description;             // the schema `description` (may be empty)
    std::string units;                   // x-ctx-units (e.g. "m", "rad"; may be empty)
    WidgetKind kind = WidgetKind::text;  // how the field is edited
    serializer::JsonValue value;         // the current composed value at `pointer`
    bool overridden = false;             // an L-35 override contributor touched this pointer
    bool editable = true;                // false for a readonly field
};

// The inspector view model for one selected composed entity: its L-35 identity + the schema-driven
// editable field set. `has_entity` is false for the no-selection state (the panel renders an
// a11y-clean placeholder). Deterministic — field order is the schema's introspection field order.
struct InspectorModel
{
    std::string root_scene;              // the addressing (root) scene the write targets (L-35)
    std::vector<std::string> id_path;    // the L-35 id-path to the composed entity
    std::string identity;                // id_path joined with '/', the selection key
    std::uint64_t identity_hash = 0;     // the L-37 composed identity hash
    std::string kind_id;                 // the driving kind ($schema), e.g. "ctx:scene"
    bool has_entity = false;             // false => no selection
    std::vector<InspectorField> fields;  // the editable component fields present on the entity
    std::size_t override_count = 0;      // how many fields carry an L-35 override
};

// Build the inspector model for a composed entity, driven by its kind schema's R-CLI-005
// introspection (schema::introspection_json) intersected with the entity's composed value. Only the
// entity's PRESENT component fields are surfaced; the immutable identity fields (/id, /$schema,
// /version — L-37) and the L-32 `notes` annotation fields are excluded. Total and deterministic.
[[nodiscard]] InspectorModel build_inspector_model(const compose::ComposedEntity& entity,
                                                   const schema::KindSchema& kind_schema,
                                                   const std::string& root_scene);

// The L-35 override-write envelope for setting `pointer` to `value` on the model's entity: a
// `compose::WriteRequest` targeting the OUTERMOST instancing scene (L-35 default), addressed by the
// entity's id-path. This is the request the `context set` write path (compose::plan_write) consumes —
// the inspector never writes a parallel path. Pure: it constructs the request, it does not apply it.
[[nodiscard]] compose::WriteRequest override_write_request(const InspectorModel& model,
                                                          const std::string& pointer,
                                                          serializer::JsonValue value);

// The field whose `pointer` equals `pointer`; nullptr when absent.
[[nodiscard]] const InspectorField* find_field(const InspectorModel& model,
                                               const std::string& pointer);

} // namespace context::editor::gui::panels::inspector
