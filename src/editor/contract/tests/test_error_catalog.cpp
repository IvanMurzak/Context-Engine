// Error-catalog tests: the ADDITIVE-ONLY invariant (R-CLI-008) is the headline — the frozen v0
// baseline must remain a subset of the live catalog forever. Plus exit-code-table coverage and
// per-entry well-formedness (happy + failure paths, R-QA-013).

#include "context/editor/contract/error_catalog.h"
#include "contract_test.h"

#include <set>
#include <string>
#include <vector>

using namespace context::editor::contract;

int main()
{
    // --- ADDITIVE-ONLY (R-CLI-008, CI-enforced) ------------------------------------------------
    // Every code frozen into the v0 baseline MUST still exist in the live catalog. Removing or
    // renaming a shipped code fails here — that is the additive-only enforcement point.
    {
        const std::vector<std::string> missing =
            missing_from_catalog(baseline_v0_codes(), catalog());
        for (const std::string& m : missing)
            std::fprintf(stderr, "additive-only violation: baseline code removed/renamed: %s\n",
                         m.c_str());
        CHECK(missing.empty());
    }

    // A synthetic removed code IS detected (proves the check has teeth, not a tautology).
    {
        std::vector<std::string> synthetic_baseline = baseline_v0_codes();
        synthetic_baseline.push_back("code.that.was.never.shipped");
        const std::vector<std::string> missing =
            missing_from_catalog(synthetic_baseline, catalog());
        CHECK(missing.size() == 1);
        CHECK(missing.front() == "code.that.was.never.shipped");
    }

    // --- catalog well-formedness ----------------------------------------------------------------
    {
        std::set<std::string> seen;
        CHECK(!catalog().empty());
        for (const ErrorCode& e : catalog())
        {
            CHECK(!e.code.empty());
            CHECK(!e.message.empty());
            CHECK(!e.origin.empty());
            CHECK(e.exit_code >= 1 && e.exit_code <= 8); // within the fixed exit-code table
            CHECK(seen.insert(e.code).second);           // no duplicate codes
        }
    }

    // --- lookup + exit-code table ---------------------------------------------------------------
    {
        const ErrorCode* jail = find_code("path.jail_violation");
        CHECK(jail != nullptr);
        CHECK(jail->exit_code == 6);      // permission class
        CHECK(jail->retriable == false);

        const ErrorCode* cas = find_code("cas.mismatch");
        CHECK(cas != nullptr);
        CHECK(cas->retriable == true);    // a CAS retry is meaningful
        CHECK(cas->exit_code == 4);       // conflict class

        CHECK(find_code("no.such.code") == nullptr);
        CHECK(exit_code_for("path.jail_violation") == 6);
        CHECK(exit_code_for("no.such.code") == 1); // unknown => generic-error exit
    }

    CONTRACT_TEST_MAIN_END();
}
