// Deprecation-lifecycle conformance tests (R-CLI-010): the deprecation metadata + policy machinery
// that ACTIVATES at the M3 contract freeze, so the frozen 1.0 surface can evolve after protocolMajor
// bumps to 1 without silently breaking agents. This lands the MECHANISM (per-entry deprecated /
// removedIn on verbs, methods, tools, and flags; a written policy section in `describe`), NOT the
// protocolMajor bump and NOT the negotiation behavior (v1 stays hard-fail-on-mismatch).
//
// Coverage (R-QA-013 happy + edge + failure/no-op):
//   * No real surface is deprecated at the freeze — the machinery is inert (no live deprecations).
//   * `describe` carries the R-CLI-010 deprecationPolicy section with the right shape + inactive state.
//   * An EXAMPLE (test-only) deprecated verb/flag advertises deprecated + removedIn AND keeps its
//     stable method-id across the lifecycle — the guarantee R-CLI-010 makes.

#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"
#include "contract_test.h"

#include <string>
#include <vector>

using namespace context::editor::contract;

namespace
{
// Collect every flag object (core flags + per-verb flags) `describe` emits, to assert none of the
// REAL surface is deprecated at the freeze.
void check_no_flag_deprecated(const Json& flags_array)
{
    for (std::size_t i = 0; i < flags_array.size(); ++i)
    {
        const Json& f = flags_array.at(i);
        CHECK(f.contains("deprecated"));    // the field is ALWAYS present (R-CLI-010 metadata)
        CHECK(!f.at("deprecated").as_bool()); // ...and false: no live deprecations at freeze
        CHECK(!f.contains("removedIn"));     // removedIn appears only when deprecated
    }
}
} // namespace

int main()
{
    const Registry& reg = Registry::instance();

    // --- 1. No REAL verb/flag is deprecated at the M3 freeze (the machinery is inert) ------------
    for (const VerbSpec& v : reg.verbs())
    {
        CHECK(!v.deprecated);
        CHECK(v.removed_in.empty());
    }
    for (const FlagSpec& f : reg.core_flags())
    {
        CHECK(!f.deprecated);
        CHECK(f.removed_in.empty());
    }

    const Json describe = reg.describe();
    const Json& contract = describe.at("contract");

    // Every describe verb entry carries `deprecated:false` and NO removedIn (no live deprecation),
    // and each verb's own flag list is clean too.
    const Json& d_verbs = contract.at("verbs");
    for (std::size_t i = 0; i < d_verbs.size(); ++i)
    {
        const Json& v = d_verbs.at(i);
        CHECK(v.contains("deprecated"));
        CHECK(!v.at("deprecated").as_bool());
        CHECK(!v.contains("removedIn"));
        check_no_flag_deprecated(v.at("flags"));
    }
    check_no_flag_deprecated(contract.at("coreFlags"));

    // The compact rpc/mcp/cli surfaces carry the same inert `deprecated:false` (one property, every
    // surface — R-CLI-009 parity).
    for (const char* section : {"rpcMethods", "mcpTools"})
    {
        const Json& arr = contract.at(section);
        for (std::size_t i = 0; i < arr.size(); ++i)
        {
            CHECK(arr.at(i).contains("deprecated"));
            CHECK(!arr.at(i).at("deprecated").as_bool());
        }
    }

    // --- 2. describe carries the R-CLI-010 deprecationPolicy section, INACTIVE at protocolMajor=0 --
    {
        const Json& dp = contract.at("deprecationPolicy");
        CHECK(dp.is_object());
        CHECK(dp.at("requirement").as_string() == "R-CLI-010");
        // Inert at the freeze-entry surface: protocolMajor is still 0, so the lifecycle is inactive.
        CHECK(!dp.at("active").as_bool());
        CHECK(contract.at("protocol").at("protocolMajor").as_int() == 0); // task keeps protocolMajor 0
        // A defined migration window of N minors (kDeprecationMinMinors).
        CHECK(dp.at("minMinorsBeforeRemoval").as_int() ==
              static_cast<std::int64_t>(kDeprecationMinMinors));
        CHECK(dp.at("minMinorsBeforeRemoval").as_int() >= 1);
        // The method-id stability guarantee is advertised.
        CHECK(dp.at("methodIdStability").as_string() == "stable-across-lifecycle");
        // The per-entry field names + the entities the policy applies to.
        CHECK(dp.at("perEntryFields").is_array());
        CHECK(dp.at("perEntryFields").size() == 2); // deprecated + removedIn
        CHECK(dp.at("appliesTo").is_array());
        CHECK(dp.at("appliesTo").size() == 5); // verb / rpcMethod / mcpTool / flag / capability
        CHECK(!dp.at("compatibilityWindow").as_string().empty());
        CHECK(!dp.at("note").as_string().empty());
    }

    // --- 3. An EXAMPLE (test-only) deprecated entry advertises deprecated + removedIn AND keeps its
    //        stable method-id across the lifecycle (the core R-CLI-010 guarantee). These specs are
    //        NOT registered — no real surface is deprecated yet — so this exercises the projection the
    //        first genuine deprecation will flow through (same helper `describe` uses). ----------------
    {
        VerbSpec v;
        v.noun = "legacy";
        v.verb = "thing";
        v.summary = "Example (test-only) verb — not part of the real surface.";
        v.rpc_method = "legacy.thing";           // the R-CLI-004 stable method-id
        v.mcp_tool = "context_legacy_thing";
        const std::string id_before = v.rpc_method;
        const std::string tool_before = v.mcp_tool;

        // Undeprecated projection: field present + false, no removedIn.
        {
            const Json e = verb_describe_json(v);
            CHECK(e.at("deprecated").as_bool() == false);
            CHECK(!e.contains("removedIn"));
        }

        // Transition into the lifecycle: mark deprecated + set the removal version. The stable ids
        // MUST NOT change — a client's stored id keeps resolving for the whole compatibility window.
        v.deprecated = true;
        v.removed_in = "2.0.0";
        CHECK(v.rpc_method == id_before);  // stable across the lifecycle
        CHECK(v.mcp_tool == tool_before);

        const Json e = verb_describe_json(v);
        CHECK(e.at("deprecated").as_bool());
        CHECK(e.contains("removedIn"));
        CHECK(e.at("removedIn").as_string() == "2.0.0");
        CHECK(e.at("rpcMethod").as_string() == "legacy.thing"); // id unchanged in the projection
        CHECK(e.at("mcpTool").as_string() == "context_legacy_thing");
        CHECK(e.at("verb").as_string() == "thing"); // identity intact

        // A deprecated FLAG advertises the same lifecycle fields through the shared projection.
        FlagSpec f;
        f.name = "old-flag";
        f.value_type = "bool";
        f.description = "Example (test-only) deprecated flag.";
        f.deprecated = true;
        f.removed_in = "2.0.0";
        const Json fe = flag_describe_json(f);
        CHECK(fe.at("name").as_string() == "old-flag"); // the grammar slot (name) stays stable
        CHECK(fe.at("deprecated").as_bool());
        CHECK(fe.contains("removedIn"));
        CHECK(fe.at("removedIn").as_string() == "2.0.0");

        // A non-deprecated flag: field present + false, no removedIn (the common case).
        FlagSpec live;
        live.name = "live-flag";
        live.value_type = "bool";
        const Json le = flag_describe_json(live);
        CHECK(le.at("deprecated").as_bool() == false);
        CHECK(!le.contains("removedIn"));
    }

    CONTRACT_TEST_MAIN_END();
}
