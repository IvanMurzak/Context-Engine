// D6 panel state contract (M9 e05b, design 04 §3): the versioned state blob every panel persists and
// restores across a rehome / tear-out / crash-restore / reload.
//
// THE PURITY RULE (D6): a panel is a pure function of (project state reached over the bridge, this
// state blob). Nothing else survives a panel's lifetime — there is no retainContext, no hidden
// per-panel singleton, no "the DOM still had it" carry-over. That is what makes tear-out and
// crash-restore reconstruct the SAME panel rather than an approximation of it, and it is why the blob
// is deliberately opaque to the host: the host persists and returns bytes, it never interprets them.
//
// THE MIGRATION RULE (D6): a blob whose schemaVersion does not match the version the panel declares
// today (Contribution::state.schema_version) is NOT migrated and NOT partially applied — the panel
// receives NULL state plus a diagnostic, and rebuilds from its defaults. Never a crash: every
// function here is total over arbitrary (including hostile or truncated) input.

#pragma once

#include "context/editor/contract/json.h"

#include <cstdint>
#include <optional>
#include <string>

namespace context::editor::gui::contract
{

using Json = context::editor::contract::Json;

// Grep-stable refusal codes for a persisted blob the panel cannot adopt (local codes, NOT R-CLI-008
// catalog codes — same rationale as registry.h).
inline constexpr const char* kErrStateSchemaMismatch = "gui.state_schema_mismatch";
inline constexpr const char* kErrStateMalformed = "gui.state_malformed";

// The on-disk member names of a persisted blob. Grep-stable so the TS side (e05d) and the host agree.
inline constexpr const char* kStateSchemaVersionKey = "schemaVersion";
inline constexpr const char* kStateDataKey = "data";

// One panel's versioned state: the schema version the data was written against, plus the opaque
// payload. `data` may be any JSON value — the host never looks inside it.
struct PanelState
{
    std::uint32_t schema_version = 0;
    Json data;
};

// The outcome of restoring a persisted blob for a panel that declares `expected_schema_version`.
// `ok == false` is the D6 "panel receives null state + a diagnostic" path — the caller hands the panel
// no state and surfaces `diagnostic`; it is NEVER an error the host propagates as a failure.
struct StateRestore
{
    bool ok = false;
    std::optional<PanelState> state; // engaged iff ok
    std::string code;                // empty when ok; else kErrState* above
    std::string diagnostic;          // empty when ok; else a human/AI-readable reason

    [[nodiscard]] static StateRestore accepted(PanelState state);
    [[nodiscard]] static StateRestore rejected(std::string code, std::string diagnostic);
};

// Serialize a panel state to the persisted blob shape:
//   {"schemaVersion": <n>, "data": <opaque>}
[[nodiscard]] Json persist_panel_state(const PanelState& state);

// Restore a persisted blob for a panel declaring `expected_schema_version`. Total — any input, any
// shape, no throw:
//   * a non-object / missing / non-integral / negative / out-of-range schemaVersion => malformed
//   * a well-formed blob whose version != expected                                  => mismatch
//   * otherwise                                                                     => accepted
// A missing `data` member is NOT a defect: it restores as JSON null (a panel is free to persist no
// payload while still versioning its shape).
[[nodiscard]] StateRestore restore_panel_state(std::uint32_t expected_schema_version,
                                               const Json& persisted);

} // namespace context::editor::gui::contract
