// The KERNEL-SIDE inspector model builder + write-request construction (M9 e05d3, D10/D18): derives
// the boundary-clean InspectorModel from a composed entity's kind schema (R-CLI-005 introspection)
// intersected with its composed value, and converts the panel's boundary-clean OverrideWriteRequest
// into the compose::WriteRequest the `context set` write path (compose::plan_write) consumes. Split
// OUT of context_gui_panel_inspector (owner ruling 2026-07-20) so the panel library carries NO
// compose::/schema:: type on its public link interface — the kernel-typed halves live here, linked
// by the daemon / the disk-backed gateway / the in-process M5 harnesses / tests.

#pragma once

#include "context/editor/compose/compose_write.h" // compose::WriteRequest
#include "context/editor/compose/flatten.h"       // compose::ComposedEntity
#include "context/editor/schema/kind_schema.h"    // schema::KindSchema

#include "context/editor/gui/panels/inspector/inspector_model.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h" // inspector::OverrideWriteRequest

#include <string>

namespace context::editor::gui::panels::builders
{

// Resolve `identity` — the join_identity selection key (scene_tree_builder.h) — back to its composed
// entity, or nullptr when no entity carries that id-path. The inverse lookup every identity-keyed
// consumer routes through (the daemon's `editor.inspect`, e09's wire write gateway): the join is
// injective, so the lookup splits the key once and compares segment vectors — no per-candidate
// string building. Total and deterministic.
[[nodiscard]] const compose::ComposedEntity*
find_entity_by_identity(const compose::ComposedScene& scene, const std::string& identity);

// Build the inspector model for a composed entity, driven by its kind schema's R-CLI-005
// introspection (schema::introspection_json) intersected with the entity's composed value. Only the
// entity's PRESENT component fields are surfaced; the immutable identity fields (/id, /$schema,
// /version — L-37) and the L-32 `notes` annotation fields are excluded. Total and deterministic.
[[nodiscard]] inspector::InspectorModel build_inspector_model(const compose::ComposedEntity& entity,
                                                              const schema::KindSchema& kind_schema,
                                                              const std::string& root_scene);

// The L-35 override-write envelope for setting `pointer` to `value` on the model's entity: a
// `compose::WriteRequest` targeting the OUTERMOST instancing scene (L-35 default), addressed by the
// entity's id-path. This is the request the `context set` write path (compose::plan_write) consumes —
// the inspector never writes a parallel path. Pure: it constructs the request, it does not apply it.
[[nodiscard]] compose::WriteRequest override_write_request(const inspector::InspectorModel& model,
                                                           const std::string& pointer,
                                                           serializer::JsonValue value);

// Convert the panel's boundary-clean OverrideWriteRequest (inspector_panel.h) into the
// compose::WriteRequest the write path consumes. Member-for-member — the clean struct mirrors the
// compose one exactly (root_scene / id_path / pointer / value / target / at_instance), so every
// gateway implementation on the kernel side of the seam converts through this ONE function rather
// than each re-deriving the mapping. Pure and total.
[[nodiscard]] compose::WriteRequest to_write_request(const inspector::OverrideWriteRequest& request);

} // namespace context::editor::gui::panels::builders
