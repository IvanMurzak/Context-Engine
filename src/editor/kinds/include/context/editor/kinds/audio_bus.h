// Audio-bus content-kind semantics (R-SYS-006 / L-46): the REFERENTIAL rules the ctx:audio-bus schema
// shape cannot express — bus-id uniqueness, every `parent` resolving to a declared bus, and the parent
// graph being ACYCLIC. The schema (src/editor/schema/, engine_schemas()) pins the SHAPE; this pins the
// mix graph's internal consistency, which is what makes the authored bus graph safe for the
// presentation-observer audio package (src/packages/audio/) to compile into a runtime mix tree.

#pragma once

#include "context/editor/kinds/diagnostic.h"
#include "context/editor/serializer/json_tree.h"

#include <vector>

namespace context::editor::kinds
{

// Semantic analysis of a parsed ctx:audio-bus document (BEYOND schema validation — the schema pins the
// SHAPE, this pins the referential-integrity rules the dialect cannot):
//   - audio_bus.duplicate_bus     — two buses share an `id`;
//   - audio_bus.parent_unknown    — a bus `parent` names no declared bus;
//   - audio_bus.parent_cycle      — the parent chain from a bus loops back onto itself.
// Deterministic; diagnostics are emitted in document order. A schema-invalid document (missing/mistyped
// members) is skipped gracefully — never crashes — because schema::validate_document is the gate for
// shape; this pass only adds the referential rules on top.
[[nodiscard]] std::vector<KindDiagnostic> analyze_audio_bus(const serializer::JsonValue& doc);

} // namespace context::editor::kinds
