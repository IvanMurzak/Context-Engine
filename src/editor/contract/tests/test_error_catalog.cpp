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

    // --- M5-F5 play-in-editor playbar reserved codes (issue #166) --------------------------------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). The reserved play.* block the playbar mints: not_running is usage-class (a control issued
    // with no live session); session_failed + step_failed are internal-class fail-closed; hot_reload_
    // failed is validation-class. All deterministic (a bare retry cannot help). The strings are the
    // source-of-truth in src/editor/gui/playbar/playbar_model.h (context::editor::gui::playbar::kPlay*Code).
    {
        const ErrorCode* not_running = find_code("play.not_running");
        CHECK(not_running != nullptr);
        CHECK(not_running->exit_code == 2); // usage class
        CHECK(not_running->retriable == false);
        CHECK(not_running->origin == "R-PLAY-001");

        for (const char* code : {"play.session_failed", "play.step_failed"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 1);     // internal class (fail-closed)
            CHECK(entry->retriable == false); // deterministic
        }
        CHECK(find_code("play.session_failed")->origin == "R-PLAY-001");
        CHECK(find_code("play.step_failed")->origin == "R-PLAY-002");

        const ErrorCode* hot_reload = find_code("play.hot_reload_failed");
        CHECK(hot_reload != nullptr);
        CHECK(hot_reload->exit_code == 5); // validation class
        CHECK(hot_reload->retriable == false);
        CHECK(hot_reload->origin == "R-PLAY-003");
    }

    // --- M6 P1 physics3d codes (issue #174) — the first filled F0a-reserved M6 domain block ------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). The physics3d.* fail-closed refusals the rigid-body package mints: dead-entity and
    // missing-component-set ops are usage-class; invalid shape / mass / step are validation-class.
    // All deterministic (a bare retry cannot repair an invalid body description). The strings are
    // the source-of-truth in src/packages/physics3d/.../errors.h (context::packages::physics3d).
    {
        for (const char* code : {"physics3d.invalid_entity", "physics3d.missing_component"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 2);      // usage class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SYS-001");
        }
        for (const char* code :
             {"physics3d.invalid_shape", "physics3d.invalid_mass", "physics3d.invalid_step"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SYS-001");
        }
    }

    // --- M6 P2 physics2d codes (issue #176) — the second filled F0a-reserved M6 domain block ------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). The physics2d.* fail-closed refusals the Box2D-class 2D rigid-body package mints,
    // mirroring the physics3d block: dead-entity and missing-component-set ops are usage-class;
    // invalid shape / mass / step are validation-class. All deterministic (a bare retry cannot repair
    // an invalid body description). The strings are the source-of-truth in
    // src/packages/physics2d/.../errors.h (context::packages::physics2d).
    {
        for (const char* code : {"physics2d.invalid_entity", "physics2d.missing_component"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 2);      // usage class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-2D-002");
        }
        for (const char* code :
             {"physics2d.invalid_shape", "physics2d.invalid_mass", "physics2d.invalid_step"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-2D-002");
        }
    }

    // --- M6 P4 particle codes (issue #178) — the third filled F0a-reserved M6 domain block --------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). The particle.* fail-closed refusals the particle-system package mints, mirroring the
    // physics blocks: dead-entity and missing-component ops are usage-class; invalid config / step are
    // validation-class. All deterministic (a bare retry cannot repair an invalid emitter description).
    // The strings are the source-of-truth in
    // src/packages/particles/.../errors.h (context::packages::particles). The COSMETIC observer path
    // (R-SIM-001) is off the sim path and mints no codes.
    {
        for (const char* code : {"particle.invalid_entity", "particle.missing_component"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 2);      // usage class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SYS-003");
        }
        for (const char* code : {"particle.invalid_config", "particle.invalid_step"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SYS-003");
        }
    }

    // --- M6 P5 spline codes (issue #182) — the F0a-reserved M6 spline domain block ----------------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). The spline.* fail-closed refusals the spline package mints, mirroring the physics /
    // particle / anim blocks: dead-entity and missing-component ops are usage-class; invalid path
    // (empty/malformed set or out-of-range index) / duplicate / step are validation-class. All
    // deterministic (a bare retry cannot repair an invalid path set). The strings are the
    // source-of-truth in src/packages/spline/.../errors.h (context::packages::spline). The
    // tooling/geometry DISPLAY observer path (R-SIM-001) is off the sim path and mints no codes.
    {
        for (const char* code : {"spline.invalid_entity", "spline.missing_component"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 2);      // usage class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SYS-004");
        }
        for (const char* code :
             {"spline.invalid_path", "spline.duplicate_component", "spline.invalid_step"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SYS-004");
        }
    }

    // --- M6 P6 audio codes (issue #184) — the F0a-reserved M6 audio domain block -----------------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). Audio is ENTIRELY a presentation observer (R-SIM-001): it mints no sim-path codes, so
    // unlike the sim packages it has no missing-component/step refusals. invalid_entity (a dead entity
    // handle to a spatialize op) is usage-class; invalid_bus / invalid_event are validation-class;
    // device_unavailable is internal-class fail-closed (the device could not init — the SIM is
    // unaffected). All deterministic (a bare retry cannot repair a bad bus graph or conjure a device).
    // The strings are the source-of-truth in src/packages/audio/.../errors.h (context::packages::audio).
    {
        const ErrorCode* entity = find_code("audio.invalid_entity");
        CHECK(entity != nullptr);
        CHECK(entity->exit_code == 2);      // usage class
        CHECK(entity->retriable == false);  // deterministic — a bare retry cannot help
        CHECK(entity->origin == "R-SYS-006");

        for (const char* code : {"audio.invalid_bus", "audio.invalid_event"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SYS-006");
        }

        const ErrorCode* device = find_code("audio.device_unavailable");
        CHECK(device != nullptr);
        CHECK(device->exit_code == 1);      // internal class (fail-closed; the sim is unaffected)
        CHECK(device->retriable == false);
        CHECK(device->origin == "R-SYS-006");
    }

    // --- M6-F0a deterministic-build attestation codes (issue #170) -------------------------------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). The determinism.attestation_* fail-closed codes the deterministic build PRODUCES when it
    // cannot verify determinism from the applied flags: all validation-class, deterministic (a bare
    // retry cannot repair a build's flags). The strings are the source-of-truth in
    // src/runtime/determinism/.../attestation.h (context::runtime::determinism::kAttestation*).
    {
        for (const char* code :
             {"determinism.attestation_fastmath_forbidden", "determinism.attestation_strict_fp_missing",
              "determinism.attestation_flags_unverified"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SIM-005");
        }
    }

    // --- M6 X1 sim.gc codes (issue #188) — the F0a-reserved JS-tier GC-discipline block ----------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). GC touches the JS heap ONLY (logical sim state is unreachable from the collector), so
    // every refusal is fail-closed with the sim unaffected. unavailable = internal-class capability
    // absence (the stub JS backend — the audio.device_unavailable precedent); invalid_budget =
    // validation-class (a rejected window request); window_failed = internal-class (the VM refused
    // the window/query). All deterministic (a bare retry cannot conjure a VM or fix a NaN budget).
    // The strings are the source-of-truth in src/runtime/js/.../gc_errors.h (context::runtime::js).
    {
        for (const char* code : {"sim.gc.unavailable", "sim.gc.window_failed"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 1);      // internal class (fail-closed; the sim is unaffected)
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-SIM-008");
        }

        const ErrorCode* budget = find_code("sim.gc.invalid_budget");
        CHECK(budget != nullptr);
        CHECK(budget->exit_code == 5);      // validation class
        CHECK(budget->retriable == false);  // deterministic — a bare retry cannot help
        CHECK(budget->origin == "R-SIM-008");
    }

    // --- M6 X2 net.* codes (issue X2) — the F0a-reserved replication / state-sync block ------------
    // Additive-only new rows (NOT on the frozen v0 baseline — the additive-only check above still
    // holds). The netsync harness's fail-closed refusals over the L-48 replication metadata:
    // invalid_net_id / duplicate_net_id (a rejected replication registration) + snapshot_component_
    // mismatch (a malformed inbound snapshot) are validation-class; authority_conflict (an inbound
    // delta targeting a replica-owned entity) is usage-class. All deterministic (a bare retry cannot
    // repair a bad identity, a size-mismatched payload, or an authority conflict). The strings are the
    // source-of-truth in src/runtime/netsync/.../errors.h (context::runtime::netsync).
    {
        for (const char* code :
             {"net.invalid_net_id", "net.duplicate_net_id", "net.snapshot_component_mismatch"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-NET-001");
        }

        const ErrorCode* conflict = find_code("net.authority_conflict");
        CHECK(conflict != nullptr);
        CHECK(conflict->exit_code == 2);      // usage class (authority arbitration)
        CHECK(conflict->retriable == false);  // deterministic — a bare retry cannot help
        CHECK(conflict->origin == "R-NET-001");
    }

    // --- M7 T5 ui.* codes (issue #223) — the runtime-UI CLI drive/assert domain block --------------
    // Additive-only new rows at the catalog tail (NOT on the frozen v0 baseline — the additive-only
    // check above still holds). The `context ui …` verbs' fail-closed refusals: scene_not_found +
    // node_not_found (a missing scene file / author-named node) are not-found class; scene_invalid +
    // assertion_failed (a malformed scene / a false assertion) are validation-class; invalid_event (a
    // malformed `ui send`) is usage-class. All deterministic (a bare retry cannot conjure a missing
    // file/node or make a false assertion true). The strings are the source-of-truth in
    // src/packages/ui/.../errors.h (context::packages::ui).
    {
        for (const char* code : {"ui.scene_not_found", "ui.node_not_found"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 3);      // not-found class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-UI-006");
        }
        for (const char* code : {"ui.scene_invalid", "ui.assertion_failed"})
        {
            const ErrorCode* entry = find_code(code);
            CHECK(entry != nullptr);
            CHECK(entry->exit_code == 5);      // validation class
            CHECK(entry->retriable == false);  // deterministic — a bare retry cannot help
            CHECK(entry->origin == "R-UI-006");
        }
        const ErrorCode* invalid_event = find_code("ui.invalid_event");
        CHECK(invalid_event != nullptr);
        CHECK(invalid_event->exit_code == 2); // usage class
        CHECK(invalid_event->retriable == false);
        CHECK(invalid_event->origin == "R-UI-006");
    }

    CONTRACT_TEST_MAIN_END();
}
