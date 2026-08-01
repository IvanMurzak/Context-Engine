// THE PERSISTED PER-PACKAGE CAPABILITY GRANT STORE + the install-consent surface over it
// (M9 e13c-4, design 08 §2-§3 (C-F18, the L-49 consent gate) / 04 §3/§5).
//
// WHAT THIS IS. e13c-1/-2/-3 built every mechanism this file supplies the VALUES for, and each of
// them says so in as many words:
//
//   * `package_sessions.h` control 1 — "`AttachOptions::scope` is client-CHOSEN ... Deriving that
//     string from a manifest, a grant or a consent answer is e13c-4's job and is deliberately
//     absent". This file is that derivation.
//   * `package_store.h` — "IT GRANTS NOTHING ... The consent surface and the persisted grant store
//     are e13c-4's, which is also where `granted_scopes` stops being empty and
//     `kPackageSessionScope` stops being a constant." This file is that store.
//   * `panelverbs.ts` § the capability gate — "e13c replaces `DENY_ALL_CAPABILITY_GRANTS` at the one
//     seam that consumes it". This file is what the replacement reads from.
//
// So nothing here re-decides WHERE enforcement lives. Enforcement is, unchanged, `authorize()` in
// `Dispatcher::dispatch` for everything that reaches the daemon (R-SEC-007: adapters are bypassable,
// the dispatcher is not) and `requireCapability` at the ONE `ui_events` seam in editor-core. This
// file only answers "what was this package granted", and it answers DENY for everything nobody
// consented to.
//
// ================================ THE FOUR DECISIONS ================================
//
//  1. THE GRANT FILE IS A SHELL DOCUMENT, NOT A STORE ENTRY. `<home>/.context/package-grants.json`
//     — the fifth member of the family the Shell already owns (`config.json`, `keybindings.json`,
//     `themes/`, `packages/`), resolved through the SAME `home_directory()`. Deliberately NOT inside
//     `<home>/.context/packages/`: `scan_package_store` enumerates every entry there and refuses any
//     name that is not a valid `context-ext://` host, so a `grants.json` sitting in the store would
//     be reported to the operator as a malformed package on every boot. It is also the wrong trust
//     shape — the store is untrusted input a package author writes; this file records what the
//     OPERATOR answered, and no package may sit next to it.
//
//  2. DENY-BY-DEFAULT, AT EVERY FAILURE. An absent file, an unreadable one, an oversized one, a
//     malformed one, a non-object `grants` member, a package entry that is not an array, a
//     capability token outside the CLOSED vocabulary (`capability_supported`) — every one of them
//     yields NO grant for the affected package rather than a partial or a defaulted one. A grant
//     store that failed OPEN would hand third-party code scopes nobody consented to, which is the
//     one outcome this subsystem exists to make impossible. Refusals are COLLECTED and reported, in
//     the discipline `package_store.h` decision 3 states: a silent drop presents as "my panel cannot
//     do X" with nothing naming why.
//
//  3. A GRANT MAY NEVER EXCEED THE MANIFEST DECLARATION, AND IT IS CLAMPED HERE RATHER THAN CAUGHT
//     LATER. `registry.cpp`'s `manifest_defect` already REFUSES a `Contribution` whose
//     `sandbox.granted_scopes` exceeds its declared `capabilities` — at registration, which is the
//     right choke point and stays exactly where it is. But a store that recorded such a grant would
//     turn a stale grants file (the package shipped an update that dropped a capability) into a
//     REGISTRATION FAILURE for an otherwise-fine package. `clamp_to_declared` therefore intersects
//     the recorded grant with what the contribution actually declares, so the store path CANNOT
//     produce the violation and the registry check remains the independent second control it was
//     designed to be. Both are asserted: the clamp, and the registry's refusal of a hand-built
//     violation.
//
//  4. THE CONSENT SURFACE LISTS WHAT WAS ASKED FOR, SEPARATELY FROM WHAT WAS GIVEN. A
//     `PackageConsentRequest` carries `requested` (the union of the package's contributions'
//     manifest `capabilities` — what the package ASKED for) NEXT TO `granted` (what the operator
//     recorded) and `decided` (whether anyone ever answered). Three fields rather than one, because
//     "granted nothing" and "never asked" are different states for a human: the first is a decision
//     to honour, the second is a prompt still owed. `decided` is what stops the editor re-asking
//     about a package the operator already refused.
//
// ⚠ WHAT THIS DELIBERATELY DOES NOT DO.
//   * IT DOES NOT REGISTER CONTRIBUTIONS. Appending a third-party contribution to the built-in
//     roster would red the blocking `gui-a11y-coverage` gate, which asserts roster ==
//     `coverage.manifest.jsonl` in BOTH directions (`package_store.h` § the boundary note) — and
//     correctly so, since a package panel is not covered by the first-party a11y scan. Populating
//     `Contribution::sandbox.granted_scopes` is a value-level change on the SCANNED contributions;
//     where they enter the live registry stays e13f's.
//   * IT DOES NOT INSTALL, DOWNLOAD OR UNPACK ANYTHING. The store is READ (`package_store.h`), and
//     an install verb that would ASK the consent question at the moment of installation is that
//     verb's to add. What exists today is the editor's own boot, so that is where the pending
//     request is surfaced — the SAME request object an install path would render.
//   * IT MINTS NO ERROR CODE. `consent_required` is already reserved in `error_catalog.cpp`
//     (permission exit class 6) and `codes.h` already names it; a second code for the same fact is
//     exactly the cross-task-visible spend `PANEL_BRIDGE_REFUSALS` warns against.

#pragma once

#include "context/editor/bridge/scope.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/package_store.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace context::editor::shell
{

// ---------------------------------------------------------------------------- the document on disk

// The grant document's file name, under `<home>/.context/` — see decision 1 for why it is NOT under
// `<home>/.context/packages/`.
inline constexpr const char* kPackageGrantsFileName = "package-grants.json";

// The document's own version member. Bumped only by a shape change; an UNRECOGNISED version yields
// NO grants (decision 2) rather than a best-effort read of a shape this build does not know.
inline constexpr std::int64_t kPackageGrantsVersion = 1;

// `<home>/.context/package-grants.json`, or an EMPTY path when the home directory cannot be
// resolved. EMPTY IS FULLY SUPPORTED AND MEANS "NO GRANTS": a machine with no `HOME`/`USERPROFILE`
// runs every package at the deny-all baseline rather than unbounded, mirroring
// `package_store_root()`'s own empty-path contract.
[[nodiscard]] std::filesystem::path package_grants_path();

// ------------------------------------------------------------------------------------ the refusals
//
// Grep-stable module-local codes, in the discipline `package_store.h` § the refusals states: they are
// reported to the operator and asserted in this module's suite, and never cross the CLI/protocol
// surface, so protocolMajor and the contract-freeze gate are untouched.

/** The document is absent. NOT an error — the first-run state, and it means "nothing consented". */
inline constexpr const char* kErrGrantsAbsent = "package.grants_absent";
/** The document is not parseable JSON / not an object / carries an unrecognised `version`. */
inline constexpr const char* kErrGrantsMalformed = "package.grants_malformed";
/** One package's entry is not an array of strings — that PACKAGE is dropped, the rest survive. */
inline constexpr const char* kErrGrantsEntryInvalid = "package.grants_entry_invalid";
/** A recorded token is outside the closed manifest capability vocabulary — the TOKEN is dropped. */
inline constexpr const char* kErrGrantsUnknownCapability = "package.grants_unknown_capability";
/** The document could not be written (permissions, an IO fault). */
inline constexpr const char* kErrGrantsWriteFailed = "package.grants_write_failed";

/** One thing a load or a record refused, with the reason. */
struct GrantDiagnostic
{
    // The package the refusal is about; EMPTY for a document-level fault.
    std::string package_id;
    std::string error_code;
    std::string message;
};

// ------------------------------------------------------------------------- capabilities -> scopes

// The bridge scope set a granted capability list confers.
//
// ⚠ THE TRANSLATION IS `registry.cpp`'s, NOT A SECOND COPY OF IT: the manifest spells its tokens
// underscored (`file_write`) while the bridge's wire names are hyphenated (`file-write`), and
// `ScopeSet::parse` already accepts BOTH spellings of all three grantable scopes. So this walks the
// closed capability vocabulary and grants the matching `Scope`, leaving `read_query` implicit (it is
// the baseline `ScopeSet` holds by construction) and `ui_events` unmapped — `ui_events` is an
// editor-core-LOCAL grant over the `editor.ui` bus and deliberately corresponds to NO daemon scope,
// which is why granting it can never widen a package's daemon session.
[[nodiscard]] bridge::ScopeSet granted_scope_set(const std::vector<std::string>& capabilities);

// The `AttachOptions::scope` spec for `scopes` — the value that replaces the hardcoded
// `kPackageSessionScope` at `PackageSessionHost`'s ONE attach site.
//
// Emitted in the UNDERSCORED manifest spelling, comma-separated, baseline first (`"read"` for the
// bare baseline). Round-trips through `ScopeSet::parse` by construction, which is what the suite
// asserts rather than the string's exact bytes.
[[nodiscard]] std::string attach_scope_spec(const bridge::ScopeSet& scopes);

// `granted` intersected with `declared` — decision 3's clamp. The result is emitted in the CLOSED
// VOCABULARY's order (the intersection is re-canonicalised on the way out), NOT in `granted`'s order,
// and duplicates are collapsed. A capability outside the closed vocabulary is dropped by BOTH sides,
// so it can never survive the intersection.
[[nodiscard]] std::vector<std::string> clamp_to_declared(const std::vector<std::string>& granted,
                                                         const std::vector<std::string>& declared);

// ---------------------------------------------------------------------------------- the store

// The persisted per-package grants: what the operator consented to, and nothing else.
//
// A VALUE TYPE, loaded and saved explicitly rather than a self-watching store: the document changes
// only when a human answers a consent prompt, which is orders of magnitude rarer than the
// `keybindings.json` / `themes/` edits their bridges watch for — and a watcher on a security
// document is a way for a grant to change under a live session with nothing having asked.
class PackageGrantStore
{
public:
    PackageGrantStore() = default;

    // Read the document at `file`. ALWAYS returns a store — an absent, unreadable, oversized,
    // malformed or wrong-version document yields an EMPTY one (decision 2), with the reason appended
    // to `diagnostics`. An EMPTY `file` path yields an empty store and NO diagnostic: "this machine
    // has no home directory" is not a fault the operator can act on.
    [[nodiscard]] static PackageGrantStore load(const std::filesystem::path& file,
                                                std::vector<GrantDiagnostic>& diagnostics);

    // Has `package_id` been granted `capability`? FALSE for every unknown package, every unknown
    // capability, and every package whose entry was refused at load.
    [[nodiscard]] bool granted(const std::string& package_id, const std::string& capability) const;

    // What `package_id` holds, in the closed vocabulary's own order. EMPTY for an undecided package
    // AND for one the operator refused outright — see `decided` for the difference.
    [[nodiscard]] std::vector<std::string> granted_capabilities(const std::string& package_id) const;

    // Did anyone ever ANSWER for `package_id`? A package granted nothing and a package never asked
    // about are both `granted_capabilities().empty()`; only this tells them apart, and it is what
    // stops the editor re-prompting for a package the operator already refused.
    [[nodiscard]] bool decided(const std::string& package_id) const;

    // Record the operator's answer for `package_id` — THE CONSENT DECISION, and the only mutator.
    //
    // An EMPTY `capabilities` records a REFUSAL (decided, granted nothing), which is why this cannot
    // be expressed as "call record only when something was granted". Tokens outside the closed
    // vocabulary are DROPPED with a `kErrGrantsUnknownCapability` diagnostic — never recorded, never
    // silently normalised into something adjacent. Re-recording REPLACES the previous answer whole:
    // a consent surface that could only add would make a revocation unexpressible.
    void record(const std::string& package_id, const std::vector<std::string>& capabilities,
                std::vector<GrantDiagnostic>& diagnostics);

    // Persist to `file` through the Shell's ONE atomic write primitive (`write_user_config` —
    // create-parent, stage to a unique temp, rename over), so a crash mid-write never leaves a
    // half-written grant document behind. False + `error` on any failure, including an EMPTY `file`.
    //
    // Package ids are emitted SORTED and capability tokens in the closed vocabulary's fixed order
    // (they are already canonical by the time they are stored, so a second sort would be a competing
    // opinion). Both are deterministic, which is the property that matters: two stores holding the
    // same grants produce byte-identical documents and a diff of this file is reviewable.
    [[nodiscard]] bool save(const std::filesystem::path& file, std::string& error) const;

    /** How many packages have a recorded decision (granted or refused). */
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    struct Entry
    {
        std::string package_id;
        std::vector<std::string> capabilities; // may be EMPTY — a recorded refusal
    };

    [[nodiscard]] const Entry* find(const std::string& package_id) const;

    // A vector rather than a map: it is bounded by the number of INSTALLED packages, the lookups are
    // per-panel-mount rather than per-frame, and the order is what makes `save`'s sort the only
    // ordering decision in the file.
    std::vector<Entry> entries_;
};

// -------------------------------------------------------------------------- the consent surface

// ONE package's install-consent state — what it ASKED for, what it HOLDS, and whether anyone
// answered. See decision 4 for why the three are separate fields.
struct PackageConsentRequest
{
    std::string id;
    std::string version;
    // The union of every contribution's manifest `capabilities`, deduplicated, in the closed
    // vocabulary's own order — THE REQUESTED SCOPES the surface lists. `read_query` appears here when
    // (and only when) a manifest declared it: the baseline is held regardless, and listing it
    // unasked would present every package as requesting something.
    std::vector<std::string> requested;
    // What the store currently holds for this package, clamped to `requested` — so the surface can
    // never display a grant the manifest does not back.
    std::vector<std::string> granted;
    // Has the operator answered at all? FALSE ⇒ a prompt is still owed for this package.
    bool decided = false;
};

// THE INSTALL-CONSENT SURFACE: one request per ACCEPTED package in `scan`, in scan order.
//
// Refused packages produce nothing — a package the store scan would not mount is not a package to ask
// about, and asking would be a way for a malformed bundle to put its id in front of a human.
[[nodiscard]] std::vector<PackageConsentRequest>
package_consent_requests(const PackageStoreScan& scan, const PackageGrantStore& grants);

// The consent requests still AWAITING an answer — `package_consent_requests` filtered to
// `!decided && !requested.empty()`. A package that declares no capabilities has nothing to consent
// to (it holds the baseline like every other client), so prompting for it would train the operator to
// dismiss the prompt.
[[nodiscard]] std::vector<PackageConsentRequest>
pending_consent_requests(const PackageStoreScan& scan, const PackageGrantStore& grants);

// One operator-facing line for `request` — the LISTING the L-49 surface owes a human: the package,
// its version, the scopes it asks for, and what it holds today.
[[nodiscard]] std::string consent_prompt_line(const PackageConsentRequest& request);

// Populate `scan`'s contributions' `sandbox.granted_scopes` from `grants`, CLAMPED to each
// contribution's own declared `capabilities` (decision 3).
//
// IN PLACE and idempotent. This is the ONE producer of a non-default `SandboxPolicy` in the tree —
// `read_package_manifest` leaves it at least privilege for every package and says so.
void apply_package_grants(PackageStoreScan& scan, const PackageGrantStore& grants);

// ------------------------------------------------------------------------------------- the host

// The router methods editor-core reads the grant surface over.
//
// MIRRORED IN `packagegrants.ts`, exactly as `kPanelDaemonCallMethod` / `kPanelEventsPollMethod` are
// mirrored in their editor-core halves. A rename on either side leaves editor-core polling a method
// the Shell no longer routes — and, HERE, the failure mode is a build in which every package is
// silently denied rather than one that is silently granted, which is the direction to fail in.
inline constexpr const char* kPackageGrantsListMethod = "package.grants.list";
inline constexpr const char* kPackageGrantsDecideMethod = "package.grants.decide";

/** `packageId` missing / not a string / not a syntactically valid package id, or bad `capabilities`. */
inline constexpr const char* kErrGrantsBadParams = "package.grants.bad_params";
/** `decide` named a package this store scan did not accept — no grant is recorded for a non-package. */
inline constexpr const char* kErrGrantsUnknownPackage = "package.grants.unknown_package";

// Owns the loaded grant document and serves the consent surface over the privileged router.
//
// ⚠ THE SCAN IS HELD BY REFERENCE AND IS THE COMPOSITION ROOT'S. `editor_main.cpp` scans the store
// once at boot and owns the result for the process lifetime; copying it here would let the grant
// surface answer about packages the mount table no longer reflects.
class PackageGrantHost
{
public:
    PackageGrantHost(PackageStoreScan& scan, std::filesystem::path grants_file);

    PackageGrantHost(const PackageGrantHost&) = delete;
    PackageGrantHost& operator=(const PackageGrantHost&) = delete;

    // Bind `package.grants.list` + `package.grants.decide`. False when either binding was refused (a
    // name collision, or the name landing on `forbidden_bridge_methods()`) — a wiring bug.
    [[nodiscard]] bool install(BridgeRouter& router);

    // The whole decision path for each route, exposed so the suite drives it without a router in the
    // way (the shape `PackageSessionHost::forward` establishes).
    [[nodiscard]] BridgeResult list() const;
    [[nodiscard]] BridgeResult decide(const std::string& package_id,
                                      const std::vector<std::string>& capabilities);

    // THE `AttachOptions::scope` SPEC FOR `package_id` — the ONE value that confers daemon authority,
    // and the reason it lives HERE rather than in the composition root. The store answers with the
    // RAW recorded grant: `PackageGrantStore::load` has no scan, so it can validate the closed
    // vocabulary and the id syntax but CANNOT clamp to a manifest. Clamping is therefore the caller's
    // duty, and a caller that forgets it hands the daemon a scope the package never declared — with
    // no surface reporting the difference, since the consent listing and `sandbox.granted_scopes` are
    // both clamped and would still read as granting nothing. Only this class holds both halves (the
    // scan and the store), so this is where R-SEC-007 can actually be enforced for the attach path.
    //
    // An UNINSTALLED package yields an EMPTY spec, which `PackageSessionHost::attach_scope_for` maps
    // onto `kPackageSessionScope` — a grant left in the document for a package that is gone stays
    // unusable WHILE IT IS GONE.
    //
    // ⚠ THAT IS NOT THE SAME AS "never a pre-authorization", and the difference is the id-reuse case.
    // `Entry` is keyed by package id ALONE — no version, no manifest hash, no root identity — so if a
    // directory of that name appears again, `load` restores the entry, `decided` is already true, the
    // L-49 prompt is therefore SKIPPED (`pending_consent_requests` filters on `decided`), and the new
    // bundle inherits the previous occupant's answer up to its own declaration. Nothing here prunes or
    // re-validates on reinstall. Closing it means binding the record to package IDENTITY (version
    // and/or a manifest hash) and reporting `decided = false` when the identity changed — a shape
    // change to the document, so it is tracked as a follow-up on the PR rather than done here.
    [[nodiscard]] std::string attach_scope_spec_for(const std::string& package_id) const;

    [[nodiscard]] const PackageGrantStore& grants() const { return grants_; }
    /** Everything load/record/save refused, oldest first — the diagnosability channel. */
    [[nodiscard]] const std::vector<GrantDiagnostic>& diagnostics() const { return diagnostics_; }
    /** Decisions recorded through `decide` (a refusal counts — it is an answer). */
    [[nodiscard]] std::size_t decisions_recorded() const { return decisions_recorded_; }

private:
    PackageStoreScan& scan_;
    std::filesystem::path grants_file_;
    // ⚠ DO NOT MOVE THE DOCUMENT LOAD BACK INTO THE MEMBER-INITIALIZER LIST. The constructor loads
    // the document, which APPENDS to `diagnostics_`. When that load ran from the initializer LIST,
    // members were initialized in DECLARATION order — and with `grants_` declared first it pushed
    // into a `std::vector` whose constructor had not run yet: undefined behaviour that the plain
    // `dev` build survived by luck and ASan aborted deterministically
    // (`allocation-size-too-big` out of `push_back`, MEASURED under the `sanitize` preset — it would
    // have redded both CI sanitizer legs). No warning catches this class: `-Wreorder` only
    // fires when the initializer LIST disagrees with the declaration order, which it did not.
    //
    // THE FIX IS THE BODY LOAD (see the .cpp), not the field order. Running in the body means every
    // member is already constructed, so declaration order is IRRELEVANT to the hazard as the code now
    // stands. `diagnostics_` is nonetheless kept ahead of `grants_` as a second line of defence for
    // whoever reintroduces a list initializer — but preserving the order is NOT by itself sufficient,
    // and reading it as the fix is how this UB comes back.
    std::vector<GrantDiagnostic> diagnostics_;
    PackageGrantStore grants_;
    std::size_t decisions_recorded_ = 0;
};

} // namespace context::editor::shell
