// Input-bindings content-kind semantics (R-SYS-007 / L-45): the REFERENTIAL rules the ctx:input-bindings
// schema shape cannot express — action-id and context-id uniqueness, and every binding's `action`
// resolving to a declared action. The schema (src/editor/schema/, engine_schemas()) pins the SHAPE; this
// pins the document's internal consistency, which is what makes the authored bindings safe for the input
// package to load into a runtime action map + context stack (src/packages/input/).

#pragma once

#include "context/editor/kinds/diagnostic.h"
#include "context/editor/serializer/json_tree.h"

#include <vector>

namespace context::editor::kinds
{

// Semantic analysis of a parsed ctx:input-bindings document (BEYOND schema validation — the schema
// pins the SHAPE, this pins the referential-integrity rules the dialect cannot):
//   - input_bindings.duplicate_action  — two actions share an `id`;
//   - input_bindings.duplicate_context — two contexts share an `id`;
//   - input_bindings.binding_unknown_action — a binding's `action` names no declared action.
// Deterministic; diagnostics are emitted in document order. A schema-invalid document (missing/mistyped
// members) is skipped gracefully — never crashes — because schema::validate_document is the gate for
// shape; this pass only adds the referential rules on top.
[[nodiscard]] std::vector<KindDiagnostic> analyze_input_bindings(const serializer::JsonValue& doc);

} // namespace context::editor::kinds
