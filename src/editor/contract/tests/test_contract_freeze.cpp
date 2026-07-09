// Contract-freeze gate tests (R-CLI-004 / R-CLI-009): the whole-surface additive-only invariant at
// protocolMajor == 1. Generalizes the error-catalog additive-only gate (test_error_catalog) to the
// entire STABLE registry surface. Coverage (R-QA-013 happy + failure/additive):
//   * The frozen v1 snapshot is a SUBSET of the live surface (no silent removal/rename/retype) — green.
//   * TEETH: a synthetic removed/renamed/retyped signature IS detected (a breaking change fails CI).
//   * ADDITIVE: a synthetic NEW live signature keeps the gate green (an additive change passes CI).
//   * DEPRECATION carve-out: a deprecated-but-still-present entry stays in the surface, so the gate
//     keeps passing across its migration window (removal is only sanctioned via baseline promotion).

#include "context/editor/contract/contract_freeze.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/registry.h"
#include "contract_test.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace context::editor::contract;

int main()
{
    const Registry& reg = Registry::instance();
    const std::vector<std::string> live = live_frozen_surface(reg);

    // The freeze only makes sense once protocolMajor is bumped.
    CHECK(kProtocolMajor == 1);

    // --- 1. SUBSET invariant: every frozen v1 signature still exists in the live surface -----------
    {
        const std::vector<std::string> missing = missing_from_surface(frozen_v1_surface(), live);
        for (const std::string& m : missing)
            std::fprintf(stderr, "contract-freeze violation: frozen entry removed/renamed/retyped: %s\n",
                         m.c_str());
        CHECK(missing.empty());
    }

    // The snapshot is non-trivial (it actually froze the surface, not an empty list).
    CHECK(frozen_v1_surface().size() >= 40);

    // --- 2. TEETH: a breaking change (removed / renamed / retyped) IS detected --------------------
    {
        // A removed frozen entry the live surface no longer serves.
        std::vector<std::string> baseline = frozen_v1_surface();
        baseline.push_back("verb:/never-shipped-verb");
        const std::vector<std::string> missing = missing_from_surface(baseline, live);
        CHECK(missing.size() == 1);
        CHECK(missing.front() == "verb:/never-shipped-verb");
    }
    {
        // A RETYPED core flag: the same name with a different type is a different signature, so the
        // original signature goes missing (retype == breaking). `--json` is a real frozen bool flag.
        CHECK(std::find(live.begin(), live.end(), "coreFlag:json:bool") != live.end());
        std::vector<std::string> mutated = live;
        std::replace(mutated.begin(), mutated.end(), std::string("coreFlag:json:bool"),
                     std::string("coreFlag:json:string"));
        const std::vector<std::string> missing = missing_from_surface(frozen_v1_surface(), mutated);
        CHECK(std::find(missing.begin(), missing.end(), "coreFlag:json:bool") != missing.end());
    }

    // --- 3. ADDITIVE change passes: a NEW live signature does not trip the gate --------------------
    {
        std::vector<std::string> augmented = live;
        augmented.push_back("verb:/brand-new-additive-verb");
        augmented.push_back("error:brand.new.additive.code");
        const std::vector<std::string> missing = missing_from_surface(frozen_v1_surface(), augmented);
        CHECK(missing.empty()); // additive-only: adding entries never removes a frozen one
    }

    // --- 4. DEPRECATION carve-out: a deprecated entry stays in the surface (keeps its id) ----------
    // A deprecated verb keeps its stable rpc_method / mcp_tool until removal (registry.h R-CLI-010),
    // so its signatures remain in the live surface for the whole migration window — the gate keeps
    // passing. Removal is sanctioned ONLY after the deprecation window, by promoting the entry out of
    // frozen_v1_surface() (like baseline_v0_codes). We assert the mechanism: marking a live verb
    // deprecated does NOT change the identity/method signatures the freeze locks.
    {
        const VerbSpec* describe_verb = reg.find_verb("", "", "describe");
        CHECK(describe_verb != nullptr);
        if (describe_verb != nullptr)
        {
            VerbSpec copy = *describe_verb; // a deprecation edits metadata, never the stable ids
            copy.deprecated = true;
            copy.removed_in = "2.0.0";
            CHECK(copy.rpc_method == describe_verb->rpc_method); // id stable across the lifecycle
            CHECK(copy.mcp_tool == describe_verb->mcp_tool);
            CHECK(copy.key() == describe_verb->key());
            // All three frozen signatures for this verb are still present in the live surface.
            CHECK(std::find(live.begin(), live.end(), "verb:" + copy.key()) != live.end());
            CHECK(std::find(live.begin(), live.end(), "rpcMethod:" + copy.rpc_method) != live.end());
            CHECK(std::find(live.begin(), live.end(), "mcpTool:" + copy.mcp_tool) != live.end());
        }
    }

    CONTRACT_TEST_MAIN_END();
}
