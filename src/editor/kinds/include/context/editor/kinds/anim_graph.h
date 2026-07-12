// Anim-graph content-kind semantics (R-SYS-008): the REFERENTIAL rules the ctx:anim-graph schema shape
// cannot express — state-id uniqueness, the initial state resolving, and every transition target
// resolving to a declared state. The schema (src/editor/schema/, engine_schemas()) pins the SHAPE; this
// pins the graph's internal consistency, which is what makes the authored graph safe for the animation
// package to compile into a runtime AnimGraph and evaluate.

#pragma once

#include "context/editor/kinds/diagnostic.h"
#include "context/editor/serializer/json_tree.h"

#include <vector>

namespace context::editor::kinds
{

// Semantic analysis of a parsed ctx:anim-graph document (BEYOND schema validation — the schema pins
// the SHAPE, this pins the referential-integrity rules the dialect cannot):
//   - anim_graph.duplicate_state — two states share an `id`;
//   - anim_graph.initial_unknown — the `initial` state id names no declared state;
//   - anim_graph.transition_unknown_target — a transition `to` names no declared state.
// Deterministic; diagnostics are emitted in document order. A schema-invalid document (missing/mistyped
// members) is skipped gracefully — never crashes — because schema::validate_document is the gate for
// shape; this pass only adds the referential rules on top.
[[nodiscard]] std::vector<KindDiagnostic> analyze_anim_graph(const serializer::JsonValue& doc);

} // namespace context::editor::kinds
