// Audio-event content-kind semantics (R-SYS-006 / L-46): the REFERENTIAL rule the ctx:audio-event
// schema shape cannot express — the OPTIONAL 3D spatialization block's attenuation range must be
// consistent (maxDistance strictly greater than minDistance, both non-negative). The schema
// (src/editor/schema/, engine_schemas()) pins the SHAPE; this pins the cross-field consistency the
// small dialect (which has no field-comparison keyword) cannot. The event's `bus` reference resolves
// against a SEPARATE ctx:audio-bus file, so it is a cross-file reference not checkable within one
// document (like an anim-graph clip naming a DCC asset) — validated at asset-database bind time (L-36).

#pragma once

#include "context/editor/kinds/diagnostic.h"
#include "context/editor/serializer/json_tree.h"

#include <vector>

namespace context::editor::kinds
{

// Semantic analysis of a parsed ctx:audio-event document (BEYOND schema validation):
//   - audio_event.invalid_attenuation — the spatial block's maxDistance is not strictly greater than
//     minDistance, or a distance is negative (an inverted / degenerate attenuation curve).
// Deterministic; a schema-invalid document (missing/mistyped members) is skipped gracefully — never
// crashes — because schema::validate_document is the gate for shape; this pass only adds the
// cross-field consistency rule on top. A non-spatial event (no `spatial` block) yields no diagnostics.
[[nodiscard]] std::vector<KindDiagnostic> analyze_audio_event(const serializer::JsonValue& doc);

} // namespace context::editor::kinds
