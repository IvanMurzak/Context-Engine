// The KERNEL-SIDE wire projection of the boundary-clean panel models (M9 e05d3, D10/D18): serialize
// a SceneTreeModel / InspectorModel to the contract::Json the daemon's `editor.scene-tree` /
// `editor.inspect` verbs answer with. The Shell-side feeds (src/editor/shell/panels/) parse this
// shape back into the SAME model structs — the parsers live THERE (the ProblemsFeed discipline: the
// client side owns its wire tolerance), the serializers live HERE (only the kernel side may link
// compose/schema, and only this side ever HOLDS a built model to serialize). Round-trip is asserted
// by the feed tests, which link both halves.
//
// Two wire rules the shapes below encode:
//   * u64 identity/CAS hashes cross as lowercase HEX STRINGS ("identityHash"), never JSON numbers —
//     contract::Json numbers are doubles, and a hash above 2^53 would silently lose bits. The hex
//     form is compose::format_stable_id's zero-padded 16-char rendering — byte-identical to the
//     "identityHash" the composed scene JSON (compose/flatten.cpp) and the pack chunk bodies emit.
//   * inspector field VALUES cross as their canonical serialization ("value", a JSON string) —
//     serializer::JsonValue and contract::Json are different DOMs, and the canonical byte form
//     (R-FILE-001) is the engine's one value identity, so the round-trip is exact by construction.

#pragma once

#include "context/editor/contract/json.h"

#include "context/editor/gui/panels/inspector/inspector_model.h"
#include "context/editor/gui/panels/scenetree/scene_tree_model.h"

namespace context::editor::gui::panels::builders
{

// NB the FULL qualification below: inside gui::panels::builders a bare `contract::` resolves to the
// GUI extension contract (context::editor::gui::contract), not the wire-JSON module.
namespace wire_json = context::editor::contract;

// {"rootScene", "ok", "entityCount", "roots": [{"identity", "identityHash", "displayName",
//  "kind": "entity"|"instance", "overridden", "children": [...]}]}. Deterministic.
[[nodiscard]] wire_json::Json scene_tree_to_wire(const scenetree::SceneTreeModel& model);

// {"present", "rootScene", "idPath": [...], "identity", "identityHash", "kindId", "overrideCount",
//  "fields": [{"pointer", "label", "description", "units", "kind", "value", "overridden",
//  "editable"}]}. `kind` is the WidgetKind token (text/number/toggle/json/readonly); `value` is the
// field's canonical serialization. A model with `has_entity == false` serializes as
// {"present": false} alone. Deterministic.
[[nodiscard]] wire_json::Json inspector_to_wire(const inspector::InspectorModel& model);

} // namespace context::editor::gui::panels::builders
