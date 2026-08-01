// ctest `editor-shell-test_package_grants` - the persisted per-package grant store, the install-consent
// surface over it, and the DERIVED daemon scope (M9 e13c-4, design 08 section 2-3 / 04 section 3+5).
//
// WHAT THIS SUITE IS FOR. This module is the sandbox CAPABILITY BOUNDARY: a silently-passing assertion
// here means third-party package code holds scopes nobody granted. So three disciplines bind every
// case below, each of which has cost this repo a round when it was skipped:
//
//   (1) ASSERT THE POSITIVE ARTIFACT, NEVER AN ABSENCE. "the write did not happen" is satisfied for
//       free by a request that never arrived, a fixture that failed to write, or an unrelated error.
//       Every refusal here pins the grep-stable CODE it came back with (`scope.denied`,
//       `kErrGrantsEntryInvalid`, ...) and every grant pins the EXACT capability list, never a
//       `!empty()` / `>= 1` bound - a threshold cannot tell the feature working from a fallback branch
//       that also lands inside it.
//
//   (2) EVERY REFUSAL IS PAIRED WITH THE POSITIVE COUNTERPART IN THE SAME FIXTURE FAMILY. A suite that
//       only proves denial passes just as happily against a build that refuses EVERYTHING - i.e.
//       against a broken editor - so each denial sits next to a case where the SAME path, with the
//       grant present, produces the effect. The dispatcher block is the sharpest instance: the
//       un-granted session's `set` refusal is meaningless without the granted session's `set` NOT
//       being refused two lines below it.
//
//   (3) THE FIXTURE MUST BE ABLE TO DISCRIMINATE. A package declaring NO capabilities cannot tell
//       "grants enforced" from "grants ignored" - both deny. `two_contribution_manifest` therefore
//       declares THREE capabilities on one contribution and ONE on its sibling, and every grant case
//       grants a strict SUBSET, so both the permitted and the refused half are observable and the
//       per-contribution clamp is visible rather than inferred.
//
// WHAT IS DELIBERATELY ELSEWHERE. The store SCAN, the manifest -> `Contribution` mapping and the mount
// provenance are `test_package_store.cpp`'s; the allowlist, the sub-cap and the lazy attach are
// `test_package_sessions.cpp`'s. What is HERE is only the grant: where it is recorded, what it is
// clamped to, and what it changes about the two enforcement points.

#include "context/editor/shell/package_grants.h"

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/scope.h"
#include "context/editor/client/client.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/registry.h"
#include "context/editor/gui/contract/sandbox.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/package_sessions.h"
#include "context/editor/shell/package_store.h"

#include "mock_channel.h"
#include "shell_test.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
namespace bridge = context::editor::bridge;
namespace client = context::editor::client;
namespace contract = context::editor::contract;
namespace gc = context::editor::gui::contract;
namespace shell = context::editor::shell;

using contract::Json;
using shell::BridgeResult;
using shell::PackageConsentRequest;
using shell::PackageGrantHost;
using shell::PackageGrantStore;
using shell::GrantDiagnostic;

// ------------------------------------------------------------------------------------- the fixtures

// THE DISCRIMINATING MANIFEST (discipline 3). Two contributions with DIFFERENT declarations:
//
//   * `<id>.panel`  declares read_query + ui_events + file_write
//   * `<id>.viewer` declares read_query ONLY
//
// So a package granted `file_write` must end up with the scope on `.panel` and NOT on `.viewer` - a
// difference no single-contribution fixture can express, and precisely the shape that would make a
// union-grant bug (clamping against the package's union rather than each contribution's own
// declaration) red the registry for `.viewer` while looking correct at the package level.
[[nodiscard]] std::string two_contribution_manifest(const std::string& id)
{
    return R"({
  "id": ")" + id + R"(",
  "version": "2.0.1",
  "contributions": [
    {
      "id": ")" + id + R"(.panel",
      "kind": "panel",
      "title": "Hello Panel",
      "contractVersion": 2,
      "content": { "type": "iframe", "entry": "context-ext://)" + id + R"(/panel.html" },
      "capabilities": [ "read_query", "ui_events", "file_write" ]
    },
    {
      "id": ")" + id + R"(.viewer",
      "kind": "panel",
      "title": "Hello Viewer",
      "contractVersion": 2,
      "content": { "type": "iframe", "entry": "context-ext://)" + id + R"(/viewer.html" },
      "capabilities": [ "read_query" ]
    }
  ]
})";
}

// A package declaring NOTHING beyond the baseline - the control for "there is nothing to consent to".
[[nodiscard]] std::string bare_manifest(const std::string& id)
{
    return R"({
  "id": ")" + id + R"(",
  "version": "0.1.0",
  "contributions": [
    {
      "id": ")" + id + R"(.panel",
      "kind": "panel",
      "title": "Bare",
      "contractVersion": 2,
      "content": { "type": "iframe", "entry": "context-ext://)" + id + R"(/panel.html" }
    }
  ]
})";
}

std::filesystem::path stage_package(const std::filesystem::path& store, const std::string& dir_name,
                                    const std::string& manifest)
{
    const std::filesystem::path root = store / dir_name;
    shelltest::write_file(root / shell::kPackageManifestFileName, manifest);
    shelltest::write_file(root / "panel.html", "<!DOCTYPE html><title>hi</title>");
    shelltest::write_file(root / "viewer.html", "<!DOCTYPE html><title>hi</title>");
    return root;
}

// NO `holds(list, token)` MEMBERSHIP HELPER EXISTS HERE, DELIBERATELY. Every capability assertion in
// this suite compares the WHOLE list against an exact expected vector instead, because a membership
// check is satisfied by a SUPERSET - and a superset is precisely the failure this module exists to
// prevent (a package holding a scope nobody granted). The one place a set-shaped answer is unavoidable
// is `ScopeSet`, and there each case asserts the absent scopes explicitly alongside the present one.

[[nodiscard]] bool has_code(const std::vector<GrantDiagnostic>& diagnostics, const char* code)
{
    for (const GrantDiagnostic& diagnostic : diagnostics)
    {
        if (diagnostic.error_code == code)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] const gc::Contribution* contribution_for(const shell::PackageStoreScan& scan,
                                                       const std::string& id)
{
    for (const shell::InstalledPackage& package : scan.packages)
    {
        for (const gc::Contribution& contribution : package.contributions)
        {
            if (contribution.id == id)
            {
                return &contribution;
            }
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------- 1. the vocabulary is PAIRED
//
// `capability_supported` (extension.h) is the closed vocabulary; `kCapabilityVocabulary`
// (package_grants.cpp) is this module's ordered copy of it. A token added to the first without the
// second would be ACCEPTED by the manifest parser and silently unrepresentable in a grant - an
// operator could consent to it and no package could ever hold it. Driven through the real
// record/read path, so it is the STORE's own answer rather than a list compared to a list.

void test_every_supported_capability_is_recordable()
{
    const char* every[] = {gc::kCapabilityReadQuery, gc::kCapabilityFileWrite,
                           gc::kCapabilitySessionControl, gc::kCapabilityBuildInstall,
                           gc::kCapabilityUiEvents};
    for (const char* token : every)
    {
        CHECK(gc::capability_supported(token));
        PackageGrantStore store;
        std::vector<GrantDiagnostic> diagnostics;
        store.record("hello-panel", {token}, diagnostics);
        // THE POSITIVE ARTIFACT: the exact list came back, not merely "something was recorded".
        CHECK(store.granted_capabilities("hello-panel") == std::vector<std::string>{token});
        CHECK(store.granted("hello-panel", token));
        CHECK(diagnostics.empty());
    }

    // ...and the deny half, in the same fixture family: a token OUTSIDE the vocabulary is refused with
    // its own code and recorded NOWHERE. `granted_capabilities` is asserted EMPTY (an exact value),
    // not merely "does not hold the token".
    PackageGrantStore store;
    std::vector<GrantDiagnostic> diagnostics;
    store.record("hello-panel", {"root", "ui_events "}, diagnostics);
    CHECK(store.granted_capabilities("hello-panel").empty());
    CHECK(has_code(diagnostics, shell::kErrGrantsUnknownCapability));
    // DECIDED nonetheless: the operator answered, and every token they named was refused. That is a
    // recorded refusal, not an absent decision - the distinction section 2 pins.
    CHECK(store.decided("hello-panel"));
}

// ------------------------------------------------------- 2. persistence across a restart (DoD line)

void test_grants_survive_a_restart()
{
    const std::filesystem::path root = shelltest::make_temp_project("ce-grants", "restart");
    const std::filesystem::path file = root / ".context" / shell::kPackageGrantsFileName;

    {
        PackageGrantStore writing;
        std::vector<GrantDiagnostic> diagnostics;
        writing.record("hello-panel", {"ui_events", "read_query"}, diagnostics);
        writing.record("quiet-panel", {}, diagnostics); // a recorded REFUSAL
        CHECK(diagnostics.empty());
        std::string error;
        CHECK(writing.save(file, error));
        CHECK(error.empty());
    }

    // THE RESTART. A brand-new store object reading the document off disk - nothing survives in
    // memory between the two scopes, which is the whole claim.
    std::vector<GrantDiagnostic> diagnostics;
    const PackageGrantStore reloaded = PackageGrantStore::load(file, diagnostics);
    CHECK(diagnostics.empty());
    CHECK(reloaded.size() == 2);
    // EXACT VALUES, in the vocabulary's canonical order (read_query before ui_events) - so a bug that
    // dropped, added or reordered a token is visible. A `holds(...)` pair would pass on a superset.
    CHECK(reloaded.granted_capabilities("hello-panel") ==
          (std::vector<std::string>{"read_query", "ui_events"}));
    CHECK(reloaded.granted("hello-panel", gc::kCapabilityUiEvents));
    CHECK(!reloaded.granted("hello-panel", gc::kCapabilityFileWrite));

    // THE THREE-WAY DISTINCTION the surface depends on, all after the restart: granted something,
    // recorded a refusal, never asked.
    CHECK(reloaded.decided("hello-panel"));
    CHECK(reloaded.decided("quiet-panel"));
    CHECK(reloaded.granted_capabilities("quiet-panel").empty());
    CHECK(!reloaded.decided("never-seen"));
    CHECK(reloaded.granted_capabilities("never-seen").empty());

    shelltest::cleanup(root);
}

void test_a_re_decision_replaces_and_can_revoke()
{
    const std::filesystem::path root = shelltest::make_temp_project("ce-grants", "revoke");
    const std::filesystem::path file = root / ".context" / shell::kPackageGrantsFileName;

    PackageGrantStore store;
    std::vector<GrantDiagnostic> diagnostics;
    store.record("hello-panel", {"file_write", "ui_events"}, diagnostics);
    std::string error;
    CHECK(store.save(file, error));

    // REVOCATION, through the same one mutator. A store that could only ADD would make this
    // unexpressible - and a capability system with no revocation is not one.
    store.record("hello-panel", {"ui_events"}, diagnostics);
    CHECK(store.save(file, error));

    const PackageGrantStore reloaded = PackageGrantStore::load(file, diagnostics);
    CHECK(reloaded.granted_capabilities("hello-panel") == std::vector<std::string>{"ui_events"});
    CHECK(!reloaded.granted("hello-panel", gc::kCapabilityFileWrite));
    // The package still counts ONCE: a replace, not an append.
    CHECK(reloaded.size() == 1);

    shelltest::cleanup(root);
}

// ------------------------------------------------------------------- 3. deny-by-default at load
//
// Every fault yields NO grant for the affected package. Each case pins the grep-stable CODE, because
// the faults overlap (a malformed document is also a document with no usable `grants` object) and a
// bare "the store is empty" cannot tell a held rule from a differently-held one.

void test_load_faults_all_deny()
{
    const std::filesystem::path root = shelltest::make_temp_project("ce-grants", "faults");

    // (a) ABSENT - the first-run state. Reported (so "my package is denied" has an answer) but not a
    //     failure, and it grants nothing.
    {
        std::vector<GrantDiagnostic> diagnostics;
        const PackageGrantStore store = PackageGrantStore::load(root / "nope.json", diagnostics);
        CHECK(store.size() == 0);
        CHECK(has_code(diagnostics, shell::kErrGrantsAbsent));
    }

    // (b) An EMPTY path - no home directory. Silent, deliberately: it is not a fault an operator can
    //     act on. This is the case that distinguishes "reported" from "denied": both deny.
    {
        std::vector<GrantDiagnostic> diagnostics;
        const PackageGrantStore store = PackageGrantStore::load({}, diagnostics);
        CHECK(store.size() == 0);
        CHECK(diagnostics.empty());
    }

    // (c) Unparseable bytes.
    {
        const std::filesystem::path file = root / "garbage.json";
        shelltest::write_file(file, "{{{ not json");
        std::vector<GrantDiagnostic> diagnostics;
        const PackageGrantStore store = PackageGrantStore::load(file, diagnostics);
        CHECK(store.size() == 0);
        CHECK(has_code(diagnostics, shell::kErrGrantsMalformed));
    }

    // (d) A version this build does not know. A best-effort read of an unknown SHAPE is how a future
    //     format's "denied" member gets read as a grant.
    {
        const std::filesystem::path file = root / "future.json";
        shelltest::write_file(file, R"({"version": 99, "grants": {"hello-panel": ["file_write"]}})");
        std::vector<GrantDiagnostic> diagnostics;
        const PackageGrantStore store = PackageGrantStore::load(file, diagnostics);
        CHECK(!store.granted("hello-panel", gc::kCapabilityFileWrite));
        CHECK(store.size() == 0);
        CHECK(has_code(diagnostics, shell::kErrGrantsMalformed));
    }

    // (e) `grants` is not an object.
    {
        const std::filesystem::path file = root / "listy.json";
        shelltest::write_file(file, R"({"version": 1, "grants": ["hello-panel"]})");
        std::vector<GrantDiagnostic> diagnostics;
        const PackageGrantStore store = PackageGrantStore::load(file, diagnostics);
        CHECK(store.size() == 0);
        CHECK(has_code(diagnostics, shell::kErrGrantsMalformed));
    }

    shelltest::cleanup(root);
}

void test_a_bad_entry_drops_only_its_own_package()
{
    const std::filesystem::path root = shelltest::make_temp_project("ce-grants", "entries");
    const std::filesystem::path file = root / "mixed.json";
    shelltest::write_file(file, R"({
  "version": 1,
  "grants": {
    "healthy": ["read_query", "ui_events"],
    "not-an-array": "ui_events",
    "not-strings": [7],
    "part-unknown": ["ui_events", "root"],
    "Upper": ["ui_events"]
  }
})");
    std::vector<GrantDiagnostic> diagnostics;
    const PackageGrantStore store = PackageGrantStore::load(file, diagnostics);

    // THE POSITIVE COUNTERPART, and it is what makes the four refusals below mean anything: the
    // healthy neighbour survives untouched. Without it a load that returned an EMPTY store for ANY
    // reason - including "the reader is broken" - would satisfy every other assertion here.
    CHECK(store.granted_capabilities("healthy") ==
          (std::vector<std::string>{"read_query", "ui_events"}));

    // A malformed ENTRY drops that PACKAGE whole: nothing partial is honoured.
    CHECK(!store.decided("not-an-array"));
    CHECK(!store.decided("not-strings"));
    CHECK(has_code(diagnostics, shell::kErrGrantsEntryInvalid));

    // An unknown TOKEN drops only the token - the sibling the operator did consent to survives. The
    // two granularities are deliberately different (package_grants.cpp, `read_entry`), so they are
    // pinned as exact values rather than as "something survived".
    CHECK(store.granted_capabilities("part-unknown") == std::vector<std::string>{"ui_events"});
    // ⚠ THE LINE ABOVE IS DEFENCE-IN-DEPTH, NOT THE DISCRIMINATOR, AND IT IS RECORDED RATHER THAN
    // OVERSTATED. Two independent mechanisms drop an unknown token - `read_entry`'s
    // `capability_supported` guard and `canonical_capabilities`' vocabulary re-order - so removing
    // either alone leaves the VALUE unchanged and that assertion cannot fail. (MEASURED: this task's
    // plant P02 removes the guard and the value assertion stayed green; the line below is the one that
    // reddened.) The guard's own contribution is the NAMED DIAGNOSTIC, so that is what pins it.
    CHECK(has_code(diagnostics, shell::kErrGrantsUnknownCapability));

    // A key no package could ever be named is ignored: `is_valid_package_id` is the SAME predicate the
    // scheme mounts against and the session table validates, so a grant recorded under an unreachable
    // id would only make the document read as granting more than it does.
    CHECK(!store.decided("Upper"));

    shelltest::cleanup(root);
}

// ------------------------------------------------------------------- 4. capabilities -> scopes

void test_scope_derivation()
{
    // The three grantable capabilities map one-to-one onto their scopes - and each grants ONLY its
    // own. Asserted in both directions per case, so a mapping that granted everything would fail.
    const bridge::ScopeSet writer = shell::granted_scope_set({gc::kCapabilityFileWrite});
    CHECK(writer.has(bridge::Scope::file_write));
    CHECK(!writer.has(bridge::Scope::session_control));
    CHECK(!writer.has(bridge::Scope::build_install));

    const bridge::ScopeSet builder = shell::granted_scope_set({gc::kCapabilityBuildInstall});
    CHECK(builder.has(bridge::Scope::build_install));
    CHECK(!builder.has(bridge::Scope::file_write));

    const bridge::ScopeSet driver = shell::granted_scope_set({gc::kCapabilitySessionControl});
    CHECK(driver.has(bridge::Scope::session_control));
    CHECK(!driver.has(bridge::Scope::file_write));

    // THE LOAD-BEARING NEGATIVE: `ui_events` is an editor-core-LOCAL grant over the `editor.ui` bus and
    // confers NO daemon scope at all. If it ever mapped to one, granting a package the right to watch
    // the editor's own UI facts would silently widen its DAEMON session - the threat wearing the
    // control's clothes. Paired with `read_query` (the baseline, which is held regardless) so the case
    // cannot pass by everything mapping to nothing.
    const bridge::ScopeSet watcher =
        shell::granted_scope_set({gc::kCapabilityUiEvents, gc::kCapabilityReadQuery});
    CHECK(watcher.has(bridge::Scope::read_query)); // baseline, always
    CHECK(!watcher.has(bridge::Scope::file_write));
    CHECK(!watcher.has(bridge::Scope::session_control));
    CHECK(!watcher.has(bridge::Scope::build_install));

    // THE SPEC ROUND-TRIPS THROUGH THE REAL PARSER for every combination. Asserted as a round trip
    // rather than as a string compare because the daemon reads the SPEC, not this function's bytes: a
    // spelling this parser does not accept is a silently-denied package, and a spelling it accepts
    // that we did not intend is a silently-granted one.
    for (unsigned mask = 0; mask < 8u; ++mask)
    {
        std::vector<std::string> capabilities;
        if ((mask & 1u) != 0u)
        {
            capabilities.emplace_back(gc::kCapabilityFileWrite);
        }
        if ((mask & 2u) != 0u)
        {
            capabilities.emplace_back(gc::kCapabilitySessionControl);
        }
        if ((mask & 4u) != 0u)
        {
            capabilities.emplace_back(gc::kCapabilityBuildInstall);
        }
        const bridge::ScopeSet intended = shell::granted_scope_set(capabilities);
        const bridge::ScopeSet parsed = bridge::ScopeSet::parse(shell::attach_scope_spec(intended));
        CHECK(parsed.has(bridge::Scope::file_write) == intended.has(bridge::Scope::file_write));
        CHECK(parsed.has(bridge::Scope::session_control) ==
              intended.has(bridge::Scope::session_control));
        CHECK(parsed.has(bridge::Scope::build_install) == intended.has(bridge::Scope::build_install));
    }

    // The bare baseline is spelled, not empty - `kPackageSessionScope`'s own rationale (a default is
    // not a decision anyone can find), preserved through the derivation that replaced it.
    CHECK(shell::attach_scope_spec(bridge::ScopeSet::read_query()) ==
          std::string(shell::kPackageSessionScope));
}

// --------------------------------------------------- 5. the clamp, and the registry's own refusal

void test_the_clamp_and_the_registry_refusal()
{
    // The clamp is an INTERSECTION, asserted as an exact list in both directions.
    CHECK(shell::clamp_to_declared({"file_write", "ui_events"}, {"ui_events"}) ==
          std::vector<std::string>{"ui_events"});
    CHECK(shell::clamp_to_declared({"ui_events"}, {"file_write"}).empty());
    CHECK(shell::clamp_to_declared({"ui_events", "read_query"}, {"read_query", "ui_events"}) ==
          (std::vector<std::string>{"read_query", "ui_events"}));

    const std::filesystem::path root = shelltest::make_temp_project("ce-grants", "clamp");
    const std::filesystem::path store_root = root / "packages";
    stage_package(store_root, "hello-panel", two_contribution_manifest("hello-panel"));
    shell::PackageStoreScan scan = shell::scan_package_store(store_root);
    CHECK(scan.packages.size() == 1);

    // BEFORE: least privilege on every contribution - `read_package_manifest` grants nothing, which is
    // the property that makes the AFTER below attributable to this task rather than to the parser.
    const gc::Contribution* panel = contribution_for(scan, "hello-panel.panel");
    const gc::Contribution* viewer = contribution_for(scan, "hello-panel.viewer");
    CHECK(panel != nullptr && viewer != nullptr);
    if (panel == nullptr || viewer == nullptr)
    {
        shelltest::cleanup(root);
        return;
    }
    CHECK(!panel->sandbox.granted_scopes.has(bridge::Scope::file_write));

    PackageGrantStore grants;
    std::vector<GrantDiagnostic> diagnostics;
    // A STRICT SUBSET of what `.panel` declares, and MORE than `.viewer` declares - the discriminating
    // shape (discipline 3).
    grants.record("hello-panel", {"file_write", "ui_events"}, diagnostics);
    shell::apply_package_grants(scan, grants);

    const gc::Contribution* granted_panel = contribution_for(scan, "hello-panel.panel");
    const gc::Contribution* granted_viewer = contribution_for(scan, "hello-panel.viewer");
    CHECK(granted_panel != nullptr && granted_viewer != nullptr);
    if (granted_panel == nullptr || granted_viewer == nullptr)
    {
        shelltest::cleanup(root);
        return;
    }
    // THE POSITIVE: the declaring contribution HOLDS the granted scope...
    CHECK(granted_panel->sandbox.granted_scopes.has(bridge::Scope::file_write));
    // ...and holds nothing that was not granted, so the clamp is not "grant everything".
    CHECK(!granted_panel->sandbox.granted_scopes.has(bridge::Scope::build_install));
    CHECK(!granted_panel->sandbox.granted_scopes.has(bridge::Scope::session_control));
    // THE PER-CONTRIBUTION CLAMP: its sibling declared only `read_query`, so the SAME package-level
    // grant leaves it at the baseline. A union-clamp bug is visible exactly here.
    CHECK(!granted_viewer->sandbox.granted_scopes.has(bridge::Scope::file_write));
    CHECK(granted_viewer->sandbox.granted_scopes.has(bridge::Scope::read_query));

    // THE REGISTRY'S INDEPENDENT SECOND CONTROL, exercised by ACTUALLY VIOLATING it - the DoD line.
    // `manifest_defect` refuses a contribution whose grant exceeds its declaration, and it stays where
    // it is precisely so the clamp above is not the only thing standing between a stale grant document
    // and an ambient privilege.
    gc::ExtensionRegistry registry;
    gc::Contribution violating = *granted_viewer;         // declares read_query only...
    violating.sandbox.granted_scopes.grant(bridge::Scope::file_write); // ...and is handed file_write
    const gc::RegistrationResult refused = registry.register_contribution(violating);
    CHECK(!refused.ok);
    CHECK(refused.error_code == gc::kErrInvalidManifest);
    // The MESSAGE is pinned too: `kErrInvalidManifest` covers every structural defect, so the code
    // alone cannot tell a grant violation from an empty id.
    CHECK(shelltest::mentions(refused.message, "may never exceed the declared manifest"));

    // THE POSITIVE COUNTERPART, same registry, same fixture family: the CLAMPED contribution - the one
    // this module actually produces - registers cleanly. Without it the refusal above would pass just
    // as happily against a registry that refused everything.
    const gc::RegistrationResult accepted = registry.register_contribution(*granted_viewer);
    CHECK(accepted.ok);
    CHECK(accepted.error_code.empty());
    const gc::RegistrationResult accepted_panel = registry.register_contribution(*granted_panel);
    CHECK(accepted_panel.ok);

    shelltest::cleanup(root);
}

// ----------------------------------------------------------------- 6. the install-consent surface

void test_the_consent_surface_lists_the_requested_scopes()
{
    const std::filesystem::path root = shelltest::make_temp_project("ce-grants", "consent");
    const std::filesystem::path store_root = root / "packages";
    stage_package(store_root, "hello-panel", two_contribution_manifest("hello-panel"));
    stage_package(store_root, "quiet-panel", bare_manifest("quiet-panel"));
    shell::PackageStoreScan scan = shell::scan_package_store(store_root);
    CHECK(scan.packages.size() == 2);

    PackageGrantStore grants;
    std::vector<PackageConsentRequest> requests = shell::package_consent_requests(scan, grants);
    CHECK(requests.size() == 2);
    if (requests.size() != 2)
    {
        shelltest::cleanup(root);
        return;
    }

    // THE DoD LINE: the surface lists the scopes the MANIFEST requested - the UNION across the
    // package's contributions, deduplicated, in the vocabulary's canonical order. An EXACT list, so a
    // surface that silently dropped or invented one is visible.
    CHECK(requests[0].id == "hello-panel");
    CHECK(requests[0].version == "2.0.1");
    CHECK(requests[0].requested ==
          (std::vector<std::string>{"read_query", "file_write", "ui_events"}));
    CHECK(requests[0].granted.empty());
    CHECK(!requests[0].decided);

    // The bare package requests NOTHING beyond the baseline - listed, but with an empty request, which
    // is what `pending_consent_requests` filters on below.
    CHECK(requests[1].id == "quiet-panel");
    CHECK(requests[1].requested.empty());

    // The operator-facing line NAMES each requested scope. Asserted per token rather than as one
    // string compare so a reordering does not red it while a DROPPED scope does.
    const std::string line = shell::consent_prompt_line(requests[0]);
    CHECK(shelltest::mentions(line, "hello-panel"));
    CHECK(shelltest::mentions(line, "2.0.1"));
    CHECK(shelltest::mentions(line, "read_query"));
    CHECK(shelltest::mentions(line, "file_write"));
    CHECK(shelltest::mentions(line, "ui_events"));
    CHECK(shelltest::mentions(line, "NOT YET CONSENTED"));

    // PENDING = undecided AND actually asking for something. Both filters, asserted together: a
    // package that asks for nothing must not produce a prompt (it would train an operator to dismiss
    // them), and neither must one already answered.
    std::vector<PackageConsentRequest> pending = shell::pending_consent_requests(scan, grants);
    CHECK(pending.size() == 1);
    if (pending.size() == 1)
    {
        CHECK(pending[0].id == "hello-panel");
    }

    std::vector<GrantDiagnostic> diagnostics;
    grants.record("hello-panel", {"ui_events"}, diagnostics);
    requests = shell::package_consent_requests(scan, grants);
    CHECK(requests.size() == 2);
    if (requests.size() == 2)
    {
        // ANSWERED: `granted` now reports exactly what was consented to, and `decided` flips. The
        // three fields move independently, which is why they are three fields.
        CHECK(requests[0].decided);
        CHECK(requests[0].granted == std::vector<std::string>{"ui_events"});
        // ...and `requested` is UNCHANGED by the answer: the surface must keep showing what the
        // package asked for, or a later reviewer cannot see what was declined.
        CHECK(requests[0].requested ==
              (std::vector<std::string>{"read_query", "file_write", "ui_events"}));
    }
    CHECK(shell::pending_consent_requests(scan, grants).empty());
    CHECK(shelltest::mentions(shell::consent_prompt_line(requests[0]), "granted: ui_events"));

    shelltest::cleanup(root);
}

// ------------------------------------------------------------------------------- 7. the host

void test_the_grant_host_records_clamps_and_persists()
{
    const std::filesystem::path root = shelltest::make_temp_project("ce-grants", "host");
    const std::filesystem::path store_root = root / "packages";
    const std::filesystem::path file = root / ".context" / shell::kPackageGrantsFileName;
    stage_package(store_root, "hello-panel", two_contribution_manifest("hello-panel"));
    shell::PackageStoreScan scan = shell::scan_package_store(store_root);

    PackageGrantHost host(scan, file);
    // ⚠ THE CONSTRUCTOR'S DIAGNOSTIC SURVIVES CONSTRUCTION. A first-run boot has no grant document,
    // so the load reports `kErrGrantsAbsent` - and this assertion is what pins that the report is
    // still THERE once the object is built. It is not bookkeeping: `grants_` was originally built in
    // the member-initializer list while `diagnostics_` (declared after it) had not been constructed
    // yet, so the load pushed into a vector whose own constructor then ran and RESET it. The
    // diagnostic vanished in the `dev` build - silently, since nothing read it - while ASan aborted
    // the whole suite on the same line (MEASURED on the local `sanitize` gate; it would have redded
    // both CI sanitizer legs). This CHECK reproduces the loss deterministically WITHOUT a sanitizer,
    // which is the only reason it can guard the fix on every leg.
    CHECK(!host.diagnostics().empty());
    CHECK(host.diagnostics().size() == 1 &&
          host.diagnostics()[0].error_code == shell::kErrGrantsAbsent);
    // CONSTRUCTION APPLIES: with no document there is nothing to apply, so every contribution is still
    // at least privilege - the baseline this case measures the AFTER against.
    const gc::Contribution* panel = contribution_for(scan, "hello-panel.panel");
    CHECK(panel != nullptr && !panel->sandbox.granted_scopes.has(bridge::Scope::file_write));

    // A decision naming MORE than the manifest declares. `build_install` is nowhere in the manifest,
    // so it must not be recorded - the consent surface cannot hand the registry a contribution
    // `manifest_defect` would refuse.
    const BridgeResult decided = host.decide("hello-panel", {"file_write", "build_install"});
    CHECK(decided.error_code.empty());
    CHECK(host.decisions_recorded() == 1);
    CHECK(host.grants().granted_capabilities("hello-panel") ==
          std::vector<std::string>{"file_write"});
    CHECK(!host.grants().granted("hello-panel", gc::kCapabilityBuildInstall));

    // THE SCAN WAS RE-APPLIED, so the live contribution now holds the scope - the observable that
    // makes `decide` a change to the running editor rather than only to a file.
    const gc::Contribution* after = contribution_for(scan, "hello-panel.panel");
    CHECK(after != nullptr && after->sandbox.granted_scopes.has(bridge::Scope::file_write));
    CHECK(after != nullptr && !after->sandbox.granted_scopes.has(bridge::Scope::build_install));

    // ...AND IT PERSISTED. Read back off disk by a store that shares nothing with the host.
    std::vector<GrantDiagnostic> diagnostics;
    const PackageGrantStore reloaded = PackageGrantStore::load(file, diagnostics);
    CHECK(reloaded.granted_capabilities("hello-panel") == std::vector<std::string>{"file_write"});

    // The reply is the refreshed LIST, so a renderer needs no second round trip to redraw.
    CHECK(decided.value.contains("packages"));
    CHECK(decided.value.at("packages").is_array() && decided.value.at("packages").size() == 1);

    // A package the SCAN did not accept gets no grant recorded: a consent record for a directory that
    // does not exist yet is a pre-authorization waiting for one to appear.
    const BridgeResult unknown = host.decide("ghost-panel", {"file_write"});
    CHECK(unknown.error_code == shell::kErrGrantsUnknownPackage);
    CHECK(!host.grants().decided("ghost-panel"));
    CHECK(host.decisions_recorded() == 1); // unchanged - it was refused, not recorded

    // ...and an id that is not even a legal package id is refused earlier still, with its own code.
    CHECK(host.decide("Upper", {}).error_code == shell::kErrGrantsBadParams);

    // `list()` reports the surface over the router. The shape is pinned by NAME, since editor-core
    // parses these members and a rename here is a silently-denied editor.
    const BridgeResult listed = host.list();
    CHECK(listed.error_code.empty());
    CHECK(listed.value.at("packages").size() == 1);
    const Json& entry = listed.value.at("packages").at(std::size_t{0});
    CHECK(entry.at("id").as_string() == "hello-panel");
    CHECK(entry.at("version").as_string() == "2.0.1");
    CHECK(entry.at("requested").is_array() && entry.at("requested").size() == 3);
    CHECK(entry.at("granted").is_array() && entry.at("granted").size() == 1);
    CHECK(entry.at("granted").at(std::size_t{0}).as_string() == "file_write");
    CHECK(entry.at("decided").as_bool());

    // BOTH ROUTES BIND. `register_method` answers false for an empty name, an already-bound one, AND
    // one on `forbidden_bridge_methods()` (the credential denylist), so a true here is the whole
    // wiring claim in one assertion - there is no way for a forbidden or colliding name to reach it.
    shell::BridgeRouter router;
    CHECK(host.install(router));
    // ...and the SECOND install is refused, which is what proves the first one actually bound rather
    // than silently no-op'd: an `install` that registered nothing would answer true twice.
    CHECK(!host.install(router));

    shelltest::cleanup(root);
}

// ------------------------------------------- 8. the DERIVED scope, against the REAL dispatcher
//
// THE DoD's TWO HALVES, in one fixture family. The un-granted session's refusals are meaningless
// without the granted session's acceptance two blocks down: a build that refused EVERY method would
// satisfy the negatives alone.

void test_the_derived_scope_through_the_real_dispatcher()
{
    const bridge::Dispatcher dispatcher; // ceiling = all scopes, so nothing but our spec is clamping
    contract::ClientHandshake handshake;
    handshake.protocol_major = contract::kProtocolMajor;

    const auto session_for = [&](const std::vector<std::string>& capabilities)
    {
        return dispatcher.attach(
            handshake,
            bridge::ScopeSet::parse(shell::attach_scope_spec(shell::granted_scope_set(capabilities))));
    };

    // (a) NOTHING CONSENTED. The DoD's blocking negative, on the session a package panel really rides:
    //     `file_write` and `build_install` are refused IN THE DISPATCHER, with the catalog's own code.
    const bridge::Dispatcher::AttachResult denied_attach = session_for({});
    const bridge::Session* denied = std::get_if<bridge::Session>(&denied_attach);
    CHECK(denied != nullptr);
    if (denied == nullptr)
    {
        return;
    }
    const contract::Envelope wrote = dispatcher.dispatch("set", Json::object(), *denied);
    CHECK(wrote.error().has_value() && wrote.error()->code == bridge::kScopeDeniedCode);
    const contract::Envelope built = dispatcher.dispatch("build", Json::object(), *denied);
    CHECK(built.error().has_value() && built.error()->code == bridge::kScopeDeniedCode);
    const contract::Envelope installed =
        dispatcher.dispatch("package.add", Json::object(), *denied);
    CHECK(installed.error().has_value() && installed.error()->code == bridge::kScopeDeniedCode);
    // THE NON-VACUITY CONTROL: the baseline read is NOT refused, so the three above are a scope
    // decision rather than a broken daemon.
    const contract::Envelope read = dispatcher.dispatch("query", Json::object(), *denied);
    CHECK(!(read.error().has_value() && read.error()->code == bridge::kScopeDeniedCode));

    // (b) `ui_events` GRANTED, and nothing else. It confers no daemon scope, so the write is STILL
    //     refused - the assertion that stops a `ui_events` grant from quietly becoming a general one.
    const bridge::Dispatcher::AttachResult watcher_attach =
        session_for({gc::kCapabilityUiEvents, gc::kCapabilityReadQuery});
    const bridge::Session* watcher = std::get_if<bridge::Session>(&watcher_attach);
    CHECK(watcher != nullptr);
    if (watcher != nullptr)
    {
        const contract::Envelope still = dispatcher.dispatch("set", Json::object(), *watcher);
        CHECK(still.error().has_value() && still.error()->code == bridge::kScopeDeniedCode);
    }

    // (c) `file_write` CONSENTED. THE POSITIVE COUNTERPART: the same verb, the same dispatcher, the
    //     same code path - and it is NOT refused. This is the half that proves the grant is what
    //     decides, rather than the daemon refusing everything.
    const bridge::Dispatcher::AttachResult writer_attach = session_for({gc::kCapabilityFileWrite});
    const bridge::Session* writer = std::get_if<bridge::Session>(&writer_attach);
    CHECK(writer != nullptr);
    if (writer != nullptr)
    {
        const contract::Envelope allowed = dispatcher.dispatch("set", Json::object(), *writer);
        CHECK(!(allowed.error().has_value() && allowed.error()->code == bridge::kScopeDeniedCode));
        // ...and the grant is NOT a blanket: `build` needs its own consent and is still refused.
        const contract::Envelope refused = dispatcher.dispatch("build", Json::object(), *writer);
        CHECK(refused.error().has_value() && refused.error()->code == bridge::kScopeDeniedCode);
    }
}

// The scope this class REQUESTS is read off the WIRE, not off its own resolver - the half that proves
// the derived value actually reaches `AttachOptions`, which the dispatcher block above assumes.
void test_the_derived_scope_reaches_the_wire()
{
    std::vector<clientmock::MockChannel*> channels;
    const auto factory = [&channels](std::string& /*error*/) -> std::unique_ptr<client::Client>
    {
        auto channel = std::make_unique<clientmock::MockChannel>();
        clientmock::MockChannel* raw = channel.get();
        raw->on("attach",
                [](const clientmock::Request&)
                {
                    // FLAT in `result`, the shape `Dispatcher::handle` really emits (mock_channel.h's
                    // standing warning: a mock more capable than the daemon hides real reader bugs).
                    Json result = Json::object();
                    result.set("protocolMajor",
                               Json(static_cast<std::uint64_t>(contract::kProtocolMajor)));
                    result.set("clientId", Json(static_cast<std::uint64_t>(7)));
                    Json caps = Json::array();
                    caps.push_back(Json(std::string("describe")));
                    result.set("capabilities", std::move(caps));
                    Json scopes = Json::array();
                    scopes.push_back(Json(std::string("read-query")));
                    result.set("scopes", std::move(scopes));
                    return result;
                });
        raw->on("query",
                [](const clientmock::Request&)
                {
                    Json data = Json::object();
                    data.set("rows", Json::array());
                    return clientmock::MockChannel::ok_envelope(std::move(data));
                });
        channels.push_back(raw);
        return std::make_unique<client::Client>(std::move(channel));
    };

    PackageGrantStore grants;
    std::vector<GrantDiagnostic> diagnostics;
    grants.record("writer-panel", {gc::kCapabilityFileWrite}, diagnostics);
    // `quiet-panel` is deliberately NOT recorded - the discriminating pair.

    shell::PackageSessionHost host(factory,
                                   [&grants](const std::string& package_id) -> std::string
                                   {
                                       return shell::attach_scope_spec(shell::granted_scope_set(
                                           grants.granted_capabilities(package_id)));
                                   });

    // The decision path, before a wire is spent on it.
    CHECK(bridge::ScopeSet::parse(host.attach_scope_for("writer-panel"))
              .has(bridge::Scope::file_write));
    CHECK(!bridge::ScopeSet::parse(host.attach_scope_for("quiet-panel"))
               .has(bridge::Scope::file_write));

    CHECK(host.forward("writer-panel", "query", Json::object()).error_code.empty());
    CHECK(host.forward("quiet-panel", "query", Json::object()).error_code.empty());
    CHECK(channels.size() == 2);
    if (channels.size() != 2)
    {
        return;
    }
    const std::vector<clientmock::Request> granted_attach = channels[0]->requests_for("attach");
    const std::vector<clientmock::Request> denied_attach = channels[1]->requests_for("attach");
    CHECK(granted_attach.size() == 1 && denied_attach.size() == 1);
    if (granted_attach.empty() || denied_attach.empty())
    {
        return;
    }
    // THE POSITIVE ARTIFACT is the parsed SCOPE SET the daemon would build from what went out, not the
    // string: the daemon reads the spec through `ScopeSet::parse`, so that is the fact worth pinning.
    CHECK(bridge::ScopeSet::parse(granted_attach[0].params.at("scope").as_string())
              .has(bridge::Scope::file_write));
    CHECK(!bridge::ScopeSet::parse(denied_attach[0].params.at("scope").as_string())
               .has(bridge::Scope::file_write));
    // The un-granted package's request is BYTE-IDENTICAL to the constant e13c-1 hardcoded, so the
    // deny-all build is the floor of this one rather than a nearby cousin.
    CHECK(denied_attach[0].params.at("scope").as_string() == std::string(shell::kPackageSessionScope));
    // NO TOKEN passes through this class, unchanged by the derivation.
    CHECK(!granted_attach[0].params.contains("token") ||
          granted_attach[0].params.at("token").as_string().empty());

    // AND THE FLOOR ITSELF: a host constructed with NO resolver is the e13c-1 build exactly.
    shell::PackageSessionHost unwired(factory);
    CHECK(unwired.attach_scope_for("writer-panel") == std::string(shell::kPackageSessionScope));
}

} // namespace

int main()
{
    test_every_supported_capability_is_recordable();
    test_grants_survive_a_restart();
    test_a_re_decision_replaces_and_can_revoke();
    test_load_faults_all_deny();
    test_a_bad_entry_drops_only_its_own_package();
    test_scope_derivation();
    test_the_clamp_and_the_registry_refusal();
    test_the_consent_surface_lists_the_requested_scopes();
    test_the_grant_host_records_clamps_and_persists();
    test_the_derived_scope_through_the_real_dispatcher();
    test_the_derived_scope_reaches_the_wire();
    SHELL_TEST_MAIN_END();
}
