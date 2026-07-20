// D6 panel state contract tests (M9 e05b, design 04 §3): versioned blob round-trips, the
// schemaVersion-mismatch path yields NULL state + a diagnostic (never a crash), and every malformed /
// hostile shape is handled totally — happy path, edge cases, and failure paths (R-QA-013).

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/panel_state.h"

#include "contract_test.h"

#include <cstdint>
#include <string>

using namespace context::editor::gui::contract;

int main()
{
    // --- happy path: a versioned blob round-trips through persist -> restore ----------------------
    {
        PanelState state;
        state.schema_version = 1;
        Json data = Json::object();
        data.set("scrollTop", Json(42));
        data.set("selectedId", Json("aaaaaaaaaaaaaaa1"));
        state.data = data;

        const Json blob = persist_panel_state(state);
        CHECK(blob.is_object());
        CHECK(blob.at(kStateSchemaVersionKey).as_int() == 1);
        CHECK(blob.at(kStateDataKey).at("scrollTop").as_int() == 42);

        const StateRestore restored = restore_panel_state(1, blob);
        CHECK(restored.ok);
        CHECK(restored.state.has_value());
        CHECK(restored.code.empty());
        CHECK(restored.diagnostic.empty());
        CHECK(restored.state->schema_version == 1);
        CHECK(restored.state->data.at("selectedId").as_string() == "aaaaaaaaaaaaaaa1");
        // The blob is OPAQUE to the host: it survives byte-for-byte.
        CHECK(restored.state->data.dump() == data.dump());
    }

    // --- the D6 migration rule: a version mismatch yields NULL state + a diagnostic ---------------
    {
        PanelState old_state;
        old_state.schema_version = 1;
        old_state.data = Json("whatever the v1 panel wrote");

        const StateRestore restored = restore_panel_state(2, persist_panel_state(old_state));
        CHECK(!restored.ok);
        CHECK(!restored.state.has_value()); // the panel receives NULL state and rebuilds from defaults
        CHECK(restored.code == kErrStateSchemaMismatch);
        CHECK(!restored.diagnostic.empty());
        // The diagnostic names BOTH versions so the mismatch is actionable.
        CHECK(restored.diagnostic.find('1') != std::string::npos);
        CHECK(restored.diagnostic.find('2') != std::string::npos);
    }

    // --- a NEWER blob than the panel understands is equally refused (downgrade is not migration) ---
    {
        PanelState future;
        future.schema_version = 99;
        const StateRestore restored = restore_panel_state(1, persist_panel_state(future));
        CHECK(!restored.ok);
        CHECK(restored.code == kErrStateSchemaMismatch);
    }

    // --- versioning a shape with no payload is legal: a missing "data" restores as JSON null -------
    {
        Json blob = Json::object();
        blob.set(kStateSchemaVersionKey, Json(7));
        const StateRestore restored = restore_panel_state(7, blob);
        CHECK(restored.ok);
        CHECK(restored.state.has_value());
        CHECK(restored.state->data.is_null());
    }

    // --- malformed shapes: TOTAL over arbitrary input, always a diagnostic, never a crash ----------
    {
        // not an object
        for (const Json& hostile : {Json(), Json(true), Json(5), Json("nope"), Json::array()})
        {
            const StateRestore r = restore_panel_state(1, hostile);
            CHECK(!r.ok);
            CHECK(r.code == kErrStateMalformed);
            CHECK(!r.state.has_value());
            CHECK(!r.diagnostic.empty());
        }

        // an object with no schemaVersion at all
        Json no_version = Json::object();
        no_version.set(kStateDataKey, Json("orphan payload"));
        CHECK(restore_panel_state(1, no_version).code == kErrStateMalformed);

        // schemaVersion of the wrong TYPE
        for (const Json& bad : {Json("1"), Json(true), Json::array(), Json()})
        {
            Json blob = Json::object();
            blob.set(kStateSchemaVersionKey, bad);
            CHECK(restore_panel_state(1, blob).code == kErrStateMalformed);
        }

        // a non-integral / negative / out-of-range schemaVersion is REFUSED, never truncated into a
        // version that happens to match (a truncating cast is how a corrupt blob would sneak past).
        for (const double bad : {1.5, -1.0, -0.0001, 4294967296.0, 1e300})
        {
            Json blob = Json::object();
            blob.set(kStateSchemaVersionKey, Json(bad));
            const StateRestore r = restore_panel_state(1, blob);
            CHECK(!r.ok);
            CHECK(r.code == kErrStateMalformed);
        }
    }

    // --- version 0 parses cleanly but can never match a panel (the registry refuses a 0 declaration)
    {
        Json blob = Json::object();
        blob.set(kStateSchemaVersionKey, Json(0));
        const StateRestore r = restore_panel_state(1, blob);
        CHECK(!r.ok);
        CHECK(r.code == kErrStateSchemaMismatch); // well-formed, just not ours
    }

    // --- the boundary version restores cleanly (no off-by-one at the uint32 ceiling) ---------------
    {
        const std::uint32_t max_version = 4294967295U;
        PanelState state;
        state.schema_version = max_version;
        const StateRestore r = restore_panel_state(max_version, persist_panel_state(state));
        CHECK(r.ok);
        CHECK(r.state->schema_version == max_version);
    }

    // --- "on EVERY panel": every roster entry's declared version round-trips its own blob ----------
    {
        for (const Contribution& c : builtin_contributions())
        {
            PanelState state;
            state.schema_version = c.state.schema_version;
            state.data = Json::object();

            const StateRestore mine = restore_panel_state(c.state.schema_version,
                                                          persist_panel_state(state));
            CHECK(mine.ok);
            CHECK(mine.state->schema_version == c.state.schema_version);

            // and a blob from a DIFFERENT version of the same panel is refused, not misapplied
            PanelState stale;
            stale.schema_version = c.state.schema_version + 1;
            const StateRestore theirs = restore_panel_state(c.state.schema_version,
                                                            persist_panel_state(stale));
            CHECK(!theirs.ok);
            CHECK(theirs.code == kErrStateSchemaMismatch);
        }
    }

    GUI_CONTRACT_TEST_MAIN_END();
}
