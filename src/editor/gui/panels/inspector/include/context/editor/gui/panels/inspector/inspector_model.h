// The inspector view model (M5-F3 / M9 e05d3, R-EDIT-001 / R-DATA-002 / L-35 / R-CLI-005): the
// schema-driven, CEF-free editable projection of the selected composed entity the inspector panel
// renders. BOUNDARY-CLEAN by construction (D10/D18, owner ruling 2026-07-20): no compose::/schema::
// type appears here — only plain data plus serializer::JsonValue (the value DOM, deliberately NOT a
// kernel-internal module) — so the panel library is Shell-hostable under the D10 shell-boundary
// gate. The kernel-typed builder (the schema -> editable-field derivation over R-CLI-005
// introspection) and the compose::WriteRequest construction live on the kernel side of the wire:
// gui/panels/builders/inspector_builder.h (context_gui_panel_builders).
//
// This is an observer+edit surface: it reads the composed derived world (as data) and commits edits
// back through the ONE `context set` write path (no parallel writer) — see inspector_panel.h.

#pragma once

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

// The field whose `pointer` equals `pointer`; nullptr when absent.
[[nodiscard]] const InspectorField* find_field(const InspectorModel& model,
                                               const std::string& pointer);

} // namespace context::editor::gui::panels::inspector
