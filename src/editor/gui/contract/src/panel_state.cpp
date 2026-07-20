// D6 panel state contract implementation (see panel_state.h). Every function here is TOTAL: a
// truncated, wrong-shaped, or hostile blob yields a diagnostic, never an exception and never a crash.

#include "context/editor/gui/contract/panel_state.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace context::editor::gui::contract
{

StateRestore StateRestore::accepted(PanelState state)
{
    StateRestore r;
    r.ok = true;
    r.state = std::move(state);
    return r;
}

StateRestore StateRestore::rejected(std::string code, std::string diagnostic)
{
    StateRestore r;
    r.ok = false;
    r.code = std::move(code);
    r.diagnostic = std::move(diagnostic);
    return r;
}

Json persist_panel_state(const PanelState& state)
{
    Json out = Json::object();
    out.set(kStateSchemaVersionKey, Json(static_cast<std::uint64_t>(state.schema_version)));
    out.set(kStateDataKey, state.data);
    return out;
}

StateRestore restore_panel_state(std::uint32_t expected_schema_version, const Json& persisted)
{
    const std::string expected = std::to_string(expected_schema_version);

    if (!persisted.is_object())
    {
        return StateRestore::rejected(kErrStateMalformed,
                                      "persisted panel state is not a JSON object (got " +
                                          persisted.dump() + ")");
    }
    if (!persisted.contains(kStateSchemaVersionKey))
    {
        return StateRestore::rejected(kErrStateMalformed,
                                      "persisted panel state has no \"" +
                                          std::string(kStateSchemaVersionKey) + "\" member");
    }

    const Json& version = persisted.at(kStateSchemaVersionKey);
    if (!version.is_number())
    {
        return StateRestore::rejected(kErrStateMalformed,
                                      "persisted \"" + std::string(kStateSchemaVersionKey) +
                                          "\" is not a number (got " + version.dump() + ")");
    }

    // Reject a non-integral / negative / out-of-range version rather than silently truncating it into
    // a version that happens to match — a truncating cast is exactly how a corrupt blob would sneak
    // past the mismatch check below.
    const double raw = version.as_number();
    if (raw < 0.0 || raw > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        static_cast<double>(static_cast<std::int64_t>(raw)) != raw)
    {
        return StateRestore::rejected(
            kErrStateMalformed, "persisted \"" + std::string(kStateSchemaVersionKey) +
                                    "\" is not a whole number in [0, 2^32) (got " + version.dump() +
                                    ")");
    }

    const std::uint32_t found = static_cast<std::uint32_t>(version.as_int());
    if (found != expected_schema_version)
    {
        // D6: no migration, no partial adoption — the panel rebuilds from defaults with a diagnostic.
        return StateRestore::rejected(kErrStateSchemaMismatch,
                                      "persisted panel state declares schemaVersion " +
                                          std::to_string(found) + " but the panel writes version " +
                                          expected + "; the panel receives null state");
    }

    PanelState state;
    state.schema_version = found;
    // A missing "data" member restores as JSON null — versioning a shape without a payload is legal.
    state.data = persisted.contains(kStateDataKey) ? persisted.at(kStateDataKey) : Json();
    return StateRestore::accepted(std::move(state));
}

} // namespace context::editor::gui::contract
