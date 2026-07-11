// Error-catalog tests: the ADDITIVE-ONLY invariant (R-CLI-008) is the headline — the frozen v0
// baseline must remain a subset of the live catalog forever. Plus exit-code-table coverage and
// per-entry well-formedness (happy + failure paths, R-QA-013).

#include "context/editor/contract/error_catalog.h"
#include "contract_test.h"

#include <algorithm>
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

    // --- R-SEC-007: the scope-denial codes map to the permission class (exit 6) ------------------
    // Promoted from bridge-local strings into the catalog so a scope-denied RPC returns exit-class 6
    // instead of the generic error (1). Both are on the frozen baseline (additive-only holds above).
    {
        const ErrorCode* denied = find_code("scope.denied");
        CHECK(denied != nullptr);
        CHECK(denied->exit_code == 6);       // permission class
        CHECK(denied->retriable == false);   // a bare retry cannot grant scope
        CHECK(denied->origin == "R-SEC-007");

        const ErrorCode* insufficient = find_code("scope.insufficient");
        CHECK(insufficient != nullptr);
        CHECK(insufficient->exit_code == 6);

        CHECK(exit_code_for("scope.denied") == 6);
        CHECK(exit_code_for("scope.insufficient") == 6);

        // Both were promoted onto the frozen v0 baseline (so removing/renaming them later trips the
        // additive-only check just like every other shipped code).
        const std::vector<std::string>& base = baseline_v0_codes();
        CHECK(std::find(base.begin(), base.end(), "scope.denied") != base.end());
        CHECK(std::find(base.begin(), base.end(), "scope.insufficient") != base.end());
    }

    // --- R-DATA-005 save-groundwork codes (M2 issue #66) map to the validation class -------------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds because baseline is a subset of the live catalog).
    {
        for (const char* code : {"save.malformed", "save.unknown_component",
                                 "save.back_compat_exceeded", "save.format_unsupported"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-DATA-005");
        }
    }

    // --- R-LANG-002 M3 TS-toolchain codes (issue #83 + the #85 typecheck follow-up) --------------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds because baseline is a subset of the live catalog). These are the codes src/runtime/ts
    // emits by string: the esbuild transpile/bundle failures AND the tsgo semantic-typecheck failure
    // (ts.type_error, issue #85 — the one esbuild cannot catch because it strips types). Assert the
    // registry agrees on class + provenance.
    {
        for (const char* code : {"ts.transpile_failed", "ts.bundle_failed", "ts.type_error"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-LANG-002");
        }
    }

    // --- R-OBS-005 TS runtime-error code (task 4b, issue #94) — the RUN-tier sibling of the two
    // build-tier codes above. Additive-only new row (NOT on the frozen v0 baseline). It carries the
    // TS-source-mapped stack trace for an authored-TS throw in the V8 host.
    {
        const ErrorCode* entry = find_code("ts.runtime_error");
        CHECK(entry != nullptr);
        CHECK(entry->exit_code == 5);      // validation class
        CHECK(entry->retriable == false);  // deterministic — a bare retry re-throws
        CHECK(entry->origin == "R-OBS-005");
    }

    // --- R-CLI-015 subscription protocol (issue #98) — the unknown-subId failure class -----------
    // Additive-only new row (NOT on the frozen v0 baseline). unsubscribe/ack of a stale subId map to
    // the not-found class; a bare retry cannot help (re-subscribe for a fresh subId).
    {
        const ErrorCode* entry = find_code("subscription.unknown_sub");
        CHECK(entry != nullptr);
        CHECK(entry->exit_code == 3);     // not-found class
        CHECK(entry->retriable == false); // deterministic
        CHECK(entry->origin == "R-CLI-015");
    }

    // --- R-SEC-005 engine-driven install codes (issue #100) --------------------------------------
    // Additive-only new rows (NOT on the frozen v0 baseline). The pin/integrity/completeness refusals
    // are deterministic validation-class; the native-tier scripts gate + the reserved R-SEC-011
    // consent code are permission-class. These strings are the source-of-truth in
    // src/editor/pkg/codes.h (context::editor::pkg::kInstall*Code / kConsentRequiredCode).
    {
        for (const char* code :
             {"install.version_unpinned", "install.integrity_mismatch", "install.lockfile_incomplete",
              "install.fetch_failed"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);     // validation class
            CHECK(entry->retriable == false); // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SEC-005");
        }

        const ErrorCode* scripts = find_code("install.scripts_required");
        CHECK(scripts != nullptr);
        CHECK(scripts->exit_code == 6);      // permission class (native-tier consent gate)
        CHECK(scripts->retriable == false);  // a bare retry cannot grant consent
        CHECK(scripts->origin == "R-SEC-005");

        const ErrorCode* consent = find_code("consent_required");
        CHECK(consent != nullptr);
        CHECK(consent->exit_code == 6);      // permission class
        CHECK(consent->retriable == false);  // a bare retry cannot grant; resume is out-of-band (v2)
        CHECK(consent->origin == "R-SEC-011");
    }

    // --- R-SEC-006 importer subprocess-sandbox failure code (issue #72) --------------------------
    // Additive-only new row (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). Emitted by run_subprocess (isolated_runner) when the unprivileged importer child fails
    // to spawn, is killed by its per-OS sandbox primitive (seccomp-bpf), or exits without a result:
    // an internal-class fail-closed (a bare retry cannot repair a broken sandbox).
    {
        const ErrorCode* entry = find_code("import.subprocess_failed");
        CHECK(entry != nullptr);
        CHECK(entry->exit_code == 1);      // internal class (fail-closed)
        CHECK(entry->retriable == false);
        CHECK(entry->origin == "R-SEC-006");
    }

    // --- R-OBS-005 interactive CDP debug-attach codes (task 4b, issue #94) ------------------------
    // Additive-only new rows (NOT on the frozen v0 baseline). attach_failed is an internal-class
    // failure (the inspector could not be created/connected); unsupported is unimplemented-class (no
    // V8 backend in this build). Both deterministic (a bare retry cannot conjure a backend).
    {
        const ErrorCode* attach = find_code("debug.attach_failed");
        CHECK(attach != nullptr);
        CHECK(attach->exit_code == 1);      // internal class
        CHECK(attach->retriable == false);
        CHECK(attach->origin == "R-OBS-005");

        const ErrorCode* unsupported = find_code("debug.unsupported");
        CHECK(unsupported != nullptr);
        CHECK(unsupported->exit_code == 8); // unimplemented / reserved-surface class
        CHECK(unsupported->retriable == false);
        CHECK(unsupported->origin == "R-OBS-005");
    }

    // --- M5-F1 native viewport panel reserved codes (issue #164) ---------------------------------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). The reserved viewport.* block the observer viewport mints: adapter_absent is
    // unimplemented-class (the GPU adapter capability is absent in this environment); surface_unavailable
    // + render_failed are internal-class fail-closed. All deterministic (a bare retry cannot conjure a
    // GPU / surface / successful readback). The strings are the source-of-truth in
    // src/editor/gui/viewport/viewport_model.h (context::editor::gui::viewport::kViewport*Code).
    {
        const ErrorCode* adapter = find_code("viewport.adapter_absent");
        CHECK(adapter != nullptr);
        CHECK(adapter->exit_code == 8); // unimplemented / capability-absent class
        CHECK(adapter->retriable == false);
        CHECK(adapter->origin == "R-HEAD-002");

        for (const char* code : {"viewport.surface_unavailable", "viewport.render_failed"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 1);     // internal class (fail-closed)
            CHECK(entry->retriable == false); // deterministic — a bare retry cannot help
        }
        CHECK(find_code("viewport.surface_unavailable")->origin == "R-UI-007");
        CHECK(find_code("viewport.render_failed")->origin == "R-REND-002");
    }

    CONTRACT_TEST_MAIN_END();
}
