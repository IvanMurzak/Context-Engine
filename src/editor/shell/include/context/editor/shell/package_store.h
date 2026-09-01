// THE PACKAGE STORE — the canonical on-disk home of installed editor packages, the
// manifest -> `Contribution` mapping, and the FIRST REAL PRODUCER of `CefShellOptions::ext_packages`
// (M9 e13c-3, design 04 §3/§5 / 08 §1-§2).
//
// WHAT THIS IS. Until now nothing in the repo could answer "which packages are installed?". The
// `context-ext://` scheme (e13a-1) could serve a package's bytes, the iframe host (e13a-2) could
// frame one, editor-core could bind a port to one (e13b-1) and the Shell could open a baseline
// daemon session for one (e13c-1) — but every one of those paths began with an id and a directory
// handed in from outside, and the only thing that ever handed them in was a TEST. Three separate
// headers say so in as many words: `ext_scheme.h`'s two E13B obligations ("WHERE THE ROOT CAME FROM
// IS NOT CHECKED HERE, AND MUST BE CHECKED THERE"), `cef_shell.h`'s "⚠ NO PRODUCER EXISTS YET", and
// `package_sessions.h`'s "Real provenance is e13c-3's package store". This file is that store.
//
// ================================ THE THREE DECISIONS ================================
//
//  1. ONE CANONICAL ROOT, DERIVED, NEVER CONFIGURED BY A PACKAGE. `<home>/.context/packages` — the
//     fourth member of a family the Shell already owns (`config.json`, `keybindings.json`,
//     `themes/`), resolved through the SAME `home_directory()` every one of those uses — i.e. through
//     the same `HOME` / `USERPROFILE` lookup, which IS an environment variable; what cannot redirect
//     it is a PACKAGE. A package cannot influence it, and a manifest cannot name an
//     asset root anywhere else: a package's root IS `<store>/<package-id>`, computed by
//     ENUMERATION, and the manifest is never consulted for a path. That is the difference between
//     this and the design the E13B obligation warned about, where "a package whose manifest points
//     its asset root at `~/.ssh` gets exactly what it asked for".
//
//  2. THE PACKAGE ID IS THE DIRECTORY NAME, AND BOTH SIDES MUST AGREE. The directory name must be a
//     valid `context-ext://` host (`is_valid_package_id`), and the manifest's own `id` must match it
//     EXACTLY. Neither is redundant: the directory name is what the origin will be, and a manifest
//     that disagrees with it is either a mispackaged bundle or an attempt to have one package's
//     bytes served under another's origin. Refused, not reconciled.
//
//  3. EVERY REFUSAL IS NAMED AND COLLECTED, NEVER A SILENT DROP. `scan_package_store` returns
//     accepted packages AND refusals, each with a grep-stable code and a human message. A store
//     scan that quietly skipped a malformed package would present to a user as "my panel did not
//     appear" with nothing anywhere naming why — and to a reviewer as a security check that cannot
//     be distinguished from an absent one. The Shell prints the refusals; the suite asserts them.
//
// ⚠ WHAT THIS DELIBERATELY DOES NOT DO — the e13c-3/-4 boundary, stated so neither half is looked
// for here and neither is assumed done.
//   * IT DOES NOT REGISTER CONTRIBUTIONS INTO THE PANEL ROSTER. `builtin_roster.cpp` is the
//     BUILT-IN roster, and the standing `gui-a11y-coverage` gate asserts it equals
//     `coverage.manifest.jsonl` in BOTH directions — so appending a third-party contribution to it
//     would red a blocking gate on all three legs, correctly: a package panel is not covered by the
//     first-party a11y scan and must not claim to be. Where a parsed contribution enters the live
//     registry, the palette and the layout targets is e13c-4/e13f's, behind the install consent this
//     store has no opinion about.
//   * IT GRANTS NOTHING. `Contribution::capabilities` is what a manifest ASKS for; `sandbox`
//     (`SandboxPolicy`) is what it is GIVEN, and this parser leaves it at its default — least
//     privilege — for every package, ignoring any grant a manifest tries to state. The consent
//     surface and the persisted grant store are e13c-4's, which is also where
//     `granted_scopes` stops being empty and `kPackageSessionScope` stops being a constant.
//   * IT INSTALLS NOTHING. There is no download, no unpack, no `context package add`. The store is
//     READ here; whatever writes it (a CLI verb, an AI agent, a human copying a directory) is out of
//     scope, which is why the scan treats the whole store as untrusted input rather than as
//     something it produced itself.
//
// ⚠ THE RESIDUALS — what a provenance-checked ROOT does NOT imply about its CONTENTS. Recorded here
// because the next task inherits these, and "provenance established" is exactly the phrase that would
// let it assume otherwise:
//   * A PROVENANCE-CHECKED ROOT IS NOT A PROVENANCE-CHECKED SUBTREE. The mount walk refuses an OS link
//     anywhere from the store root down to the package root INCLUSIVE, and stops there. A link planted
//     INSIDE an accepted root is caught only by the resolver's per-request canonical containment pass
//     (`ext_scheme.h` § the resolver) — which is `weakly_canonical`-dependent, i.e. the layer whose
//     MinGW behaviour motivated `path_is_os_link` in the first place. The ONE file this module itself
//     opens, `context-package.json`, IS link-refused by name (`read_package_manifest`); nothing else
//     under the root is. A recursive subtree walk is deliberately NOT the answer — it is unbounded work
//     over untrusted input on a path the scan takes at every editor start.
//   * HARD LINKS ARE INVISIBLE TO EVERY LAYER HERE, and unavoidably so: a hard link has no name-level
//     target, so `lstat` / `GetFileAttributesW` / canonicalization all correctly report an ordinary
//     file. Only whatever WRITES the store can close that, which is the install path's job.
//   * A DIRECTORY NAMED FOR A FIRST-PARTY NAMESPACE IS NOT REFUSED. `builtin` is a legal package id, so
//     `<store>/builtin/` satisfies rule (a)'s namespacing check for a contribution id like
//     `builtin.inspector` — byte-identical to a first-party panel's. Inert today (nothing is
//     registered), but it means rule (a) does not by itself deliver the shadowing refusal its rationale
//     describes; reserving the first-party namespaces belongs with registration, in e13c-4.

#pragma once

#include "context/editor/gui/contract/extension.h"
#include "context/editor/shell/ext_scheme.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace context::editor::shell
{

// ------------------------------------------------------------------------- the canonical store root

// The directory under `~/.context/` that holds installed packages.
inline constexpr const char* kPackageStoreDirName = "packages";

// The manifest file every installed package must carry at its root.
//
// NAMED `context-package.json`, NOT `package.json`: the repo already reads a `package.json` — the
// npm manifest of the TS scripting tier (`src/editor/pkg/`'s `lockfile.cpp` / `npm_install.cpp`, and
// `src/cli/src/install_command.cpp`) — and an editor
// package will frequently BE an npm package too, so a package that carried both under one name could
// not have both. The `context-` prefix also makes a store directory self-describing to a human who
// finds one, and keeps `check_licenses.py`'s tree-wide `package.json` scan away from it.
inline constexpr const char* kPackageManifestFileName = "context-package.json";

// The read cap for one manifest. A manifest is a small declarative document; anything larger is
// treated as unreadable rather than parsed, exactly as `kMaxUserConfigBytes` does for the config
// document. Untrusted input with no bound is an allocation an attacker chooses.
inline constexpr std::uintmax_t kMaxPackageManifestBytes = 256u * 1024u;

// How many store ENTRIES one scan will consider — every directory entry, loose files included, not
// only package directories. A bound rather than a `directory_iterator` run to exhaustion, for the same
// reason: the store is untrusted input, and 512 installed editor packages is already far past any
// plausible use. Entries past the cap are reported as a refusal (`kErrPackageStoreTooManyEntries`)
// rather than silently ignored.
//
// ⚠ WHICH entries survive the cap is ARBITRARY, and deliberately so. The cap is applied DURING
// enumeration and the sort runs after it, so past 512 entries the surviving SET is
// `directory_iterator` order — which is what bounds the work on untrusted input, and is why it is not
// deferred until after a full read. The sort's determinism guarantee therefore covers the ORDER of
// what was considered, not the CHOICE of it; a store that large is already a reported fault.
inline constexpr std::size_t kMaxPackageStoreEntries = 512;

// `<home>/.context/packages`, or an EMPTY path when the home directory cannot be resolved.
//
// EMPTY IS FULLY SUPPORTED AND MEANS "NO PACKAGES", not "packages from anywhere": an empty store root
// refuses every mount at `ExtAssetResolver::mount` (`kErrMountStoreRootUnset`), and
// `scan_package_store` on an empty path returns no packages and no refusals. So a machine with no
// `HOME`/`USERPROFILE` runs the editor with third-party panels absent rather than unbounded.
[[nodiscard]] std::filesystem::path package_store_root();

// ------------------------------------------------------------------------------------- the refusals
//
// Grep-stable module-local codes, NOT R-CLI-008 catalog rows — the discipline `registry.h` states and
// `ext_scheme.h`'s mount codes follow: a store refusal is reported to the operator and asserted in
// this module's suite, it never crosses the CLI/protocol surface, so protocolMajor and the
// contract-freeze gate are untouched. Distinct codes because they are distinct FAULTS: a package
// author must be able to tell "your directory name is not a legal package id" from "your manifest
// says a different id than your directory" from "your manifest is not JSON".

/** The store root does not exist / is not a directory. NOT an error — a first-run state. */
inline constexpr const char* kErrPackageStoreAbsent = "package.store_absent";
/** The store directory could not be enumerated (permissions, an IO fault). */
inline constexpr const char* kErrPackageStoreUnreadable = "package.store_unreadable";
/** More than `kMaxPackageStoreEntries` entries — the remainder was not considered. */
inline constexpr const char* kErrPackageStoreTooManyEntries = "package.store_too_many_entries";
/** A store entry's name is not a valid `context-ext://` host (`is_valid_package_id`). */
inline constexpr const char* kErrPackageIdInvalid = "package.id_invalid";
/** No `context-package.json` at the package root, or it could not be read / is oversized. */
inline constexpr const char* kErrManifestMissing = "package.manifest_missing";
/** The manifest is not parseable JSON, or its top level is not an object. */
inline constexpr const char* kErrManifestMalformed = "package.manifest_malformed";
/** The manifest's `id` is absent, or differs from the directory name. */
inline constexpr const char* kErrManifestIdMismatch = "package.manifest_id_mismatch";
/** A structural defect in the manifest's contributions (see `read_package_manifest`). */
inline constexpr const char* kErrManifestInvalid = "package.manifest_invalid";

// ------------------------------------------------------------------------------ the manifest -> C++

// One package the store scan ACCEPTED: its id, its provenance-checked canonical root, and the
// contributions its manifest declares.
struct InstalledPackage
{
    // The directory name, which is also the `context-ext://` host and the manifest's own `id`.
    std::string id;
    // CANONICAL and PROVENANCE-CHECKED — the value `package_root_provenance_ok` produced, so it is
    // inside the store root, reached through no OS link, and ready to hand to
    // `ExtAssetResolver::mount` without a second decision.
    std::filesystem::path root;
    // The manifest's `version`, verbatim. DIAGNOSTIC ONLY — nothing compares or orders it today; a
    // package-version contract (ranges, upgrades) belongs with the install flow that has to honour
    // it, not with a reader that would only be guessing at the semantics.
    std::string version;
    // The manifest's `contributions`, mapped to the R-EDIT-001 descriptor. `sandbox` is at its
    // default on every one — see the header: this file grants nothing.
    std::vector<gui::contract::Contribution> contributions;
};

// One package (or one store-level fault) the scan REFUSED, with the reason.
struct PackageRefusal
{
    // The store entry's name as found, which may not be a valid package id — that is often the fault
    // being reported. EMPTY for a store-level refusal (absent / unreadable / too many entries).
    std::string id;
    std::filesystem::path path;
    std::string error_code;
    std::string message;
};

struct PackageStoreScan
{
    std::vector<InstalledPackage> packages;
    std::vector<PackageRefusal> refusals;
};

// Read and validate ONE package manifest at `manifest_file`, for a package whose directory name is
// `expected_package_id` and whose provenance-checked canonical root is `package_root`.
//
// THE MANIFEST SHAPE IS THE ONE ALREADY ON THE WIRE, deliberately. `PanelHost::list` (panel_host.cpp)
// projects a `Contribution` to JSON for editor-core, and `parsePanelManifest` (panels.ts) parses that
// projection back; this function is the C++ INVERSE of the same projection, so the format a package
// author writes is the format the editor already speaks, and one shape has one meaning across three
// languages. Per contribution:
//
//     { "id": "<pkg>.<name>", "kind": "panel", "title": "...", "icon": "...",
//       "target": "",            // parsed, NOT projected — see the note under rule (e)
//       "contractVersion": 3,
//       "dock":    { "zone": "left|right|top|bottom|center",
//                    "minWidth": int, "minHeight": int },
//       "instances": { "mode": "singleton|limited|unlimited", "max": int },
//       "path":    "Scene/Debug",
//       "selection": { "subjects": [ "<pkg>.<kind>", ... ] },
//       "events":  { "publishes": [ "<pkg>.<topic>", ... ],
//                    "subscribes": [ "<other-pkg>.<topic>", ... ] },
//       "content": { "type": "iframe", "entry": "context-ext://<pkg>/panel.html" },
//       "state":   { "schemaVersion": int },
//       "capabilities": [ "read_query", ... ],
//       "commands": [ { "id": "...", "title": "...", "when": "..." } ] }
//
// ⚠ `dock.singleton` was REMOVED by manifest v3 (kContractMajor 2 -> 3): `instances.mode:"singleton"`
// replaces it exactly. Since (e) refuses any stated `contractVersion` that is not the current major,
// a v2 manifest is refused outright rather than half-read — so there is no "old shape still accepted"
// path to document, and none to test for.
//
// SEVEN VALIDATION RULES, and each one is a refusal a package could otherwise turn into an escalation:
//
//   (a) `contributions` must be a non-empty array of objects, each with a non-empty `id`, and every
//       id must be UNIQUE within the manifest and NAMESPACED to the package (`<pkg-id>.` prefix, or
//       exactly the package id). Without the namespace rule a package could contribute
//       `builtin.inspector` and shadow — or, once e13c-4 registers these, collide with — a
//       first-party panel; the registry's own duplicate-id refusal would fire only if the built-in
//       were registered FIRST, which is an ordering, not a control.
//   (b) `content.type` FAILS CLOSED. Only `iframe` is accepted for a package contribution: `uitree`
//       and `local` both mean "the editor renders this from its own code", which a third-party
//       package by definition does not have, and an unrecognised token is refused rather than
//       defaulted. `readContentType` in panels.ts fails closed for the same reason and says so.
//   (c) `content.entry` must be a `context-ext://<this-package-id>/…` URL. A package may not name
//       ANOTHER package's origin as its panel entry (the cross-package read this whole subsystem
//       exists to refuse), nor an `http(s)://`, `file://` or `data:` entry.
//   (d) Every `capabilities` token must be on the CLOSED vocabulary (`capability_supported`).
//       Deny-by-default: an unknown capability is refused, never dropped — silently dropping one
//       would present a package to a future consent surface as asking for LESS than it wrote down.
//   (e) `contractVersion`, when present, must equal `gui::contract::kContractMajor`. The
//       compatibility window is a single major (extension.h), so accepting another value here would
//       only defer the registry's own refusal to a point where the diagnostic is worse.
//   (f) `instances.mode`, when present, must be on the CLOSED v3 vocabulary. This FAILS CLOSED where
//       `dock.zone` does not, and the difference is the cost of being wrong: an unrecognised zone
//       costs a panel its first position, while an unrecognised instance mode would decide how many
//       live copies of it may exist. Absent is legal and means `singleton` — the restrictive answer.
//       The same rule covers the v3 NAME LISTS: `selection.subjects`, `events.publishes` and
//       `events.subscribes` are each read STRICTLY — present-but-not-an-array, or an array holding a
//       non-string, is a refusal, never an entry quietly dropped. That is the reasoning (d) states
//       for capabilities, one member over: a dropped name presents the package to a consent surface
//       as declaring LESS than it wrote down. (Absent stays legal and means "declared nothing".)
//   (g) THE REGISTRY'S OWN STRUCTURAL VERDICT, asked rather than restated. After (a)-(f) the parsed
//       contribution is handed to `gui::contract::manifest_defect` — the SAME function
//       `ExtensionRegistry::register_contribution` refuses on — and any diagnostic it returns is a
//       refusal here. That is what makes THE SCAN'S PROMISE BELOW structurally true rather than
//       hand-maintained: v3 alone added `instances` coherence, the `path` display-text form, and the
//       D2/D4 namespacing of `selection.subjects[]` / `events.{publishes,subscribes}[]`, and a
//       hand-copied second opinion on any of them could drift into accepting a package the registry
//       then rejects. (The namespacing rules are stated against the DECLARING package id, which this
//       reader supplies as `Contribution::package_id` from the package DIRECTORY — never from the
//       manifest text, so a package cannot name its own namespace.)
//
// Everything else — i.e. everything OUTSIDE rules (a)-(g) — is read permissively with a default
// (title defaults to the id, icon/`when`/`path` to empty, dock to `center`), matching
// `parsePanelManifest`: the cost of a wrong default there is
// cosmetic, and a parser that refuses a manifest over a missing `icon` is a parser package authors
// route around. `kind` is matched against `gc::contribution_kind_token` itself rather than a second
// hand-written table, so the accepted tokens are exactly the ones the projection emits
// (`panel` / `inspector` / `gizmo` / `asset-kind-editor` — note the HYPHENS).
//
// ⚠ `target` IS THE ONE MANIFEST FIELD READ HERE THAT THE PROJECTION DOES NOT WRITE, so this parser
// is the inverse of `PanelHost::list` for every field EXCEPT that one. (`Contribution::package_id`
// is the other asymmetry and is NOT a counter-example: it is provenance this reader DERIVES, not a
// manifest member it reads, which is exactly why the projection has nothing to emit for it.)
// Saying so is the point: `target`
// is only meaningful for the `inspector` / `gizmo` / `asset-kind-editor` kinds, `ExtensionRegistry`
// resolves those by matching the FIRST contribution with a given `(kind, target)`, and there is no
// duplicate-target refusal anywhere. So a package declaring `{"kind":"inspector","target":"Transform"}`
// would be resolved by REGISTRATION ORDER — the same ordering-is-not-a-control hazard rule (a) rejects
// for ids. Nothing is live today (this file registers nothing), and validating it belongs with the
// consent + registration surface that will: e13c-4 must not inherit this as settled.
[[nodiscard]] bool read_package_manifest(const std::filesystem::path& manifest_file,
                                         const std::string& expected_package_id,
                                         const std::filesystem::path& package_root,
                                         InstalledPackage& out, std::string& error_code,
                                         std::string& message);

// -------------------------------------------------------------------------------------- the scan

// Enumerate `store_root` and return every package it holds, plus every refusal.
//
// AN EMPTY `store_root` YIELDS AN EMPTY SCAN — no packages, no refusals, no diagnostics. That is the
// no-home-directory case, and it is not a fault.
//
// A store root that is absent or is not a directory yields ONE `kErrPackageStoreAbsent` refusal and
// no packages: a first-run machine has no store, and the editor must boot with no panels rather than
// report an error the user cannot act on. It is still REPORTED rather than silent, so "no third-party
// panels" always has a stated reason.
//
// ⚠ THE ENUMERATION IS THE SECOND E13B OBLIGATION'S DISCHARGE, AND IT NEEDS NO DEDUPE STEP — which is
// worth spelling out, because "add a case-insensitive collision refusal" is the obvious answer and it
// would be DEAD CODE. The obligation was that an install path able to produce two spellings of one
// root must dedupe them before mounting, since the overlap refusal in `ExtAssetResolver::mount` uses a
// case-SENSITIVE `path::compare` while NTFS is case-INSENSITIVE. Two properties together mean this
// path cannot produce two spellings at all:
//   * ROOTS ARE BUILT, NOT READ. Each root is `store / <enumerated name>`, so it arrives in exactly
//     the one spelling the filesystem itself reports — never in a spelling a manifest chose.
//   * A VALID PACKAGE ID IS LOWER-CASE ONLY (`is_valid_package_id`, and for its own reason: Chromium
//     lower-cases a standard URL's host, so an upper-case id could never be matched by a request).
//     So two ACCEPTED ids cannot differ only by case — there is only one spelling of any name that is
//     a legal id.
// A directory named `Pkg` on NTFS is therefore refused as `kErrPackageIdInvalid` — reported, not
// silently skipped — which is exactly the "loud mount-time failure rather than a silently unreachable
// package" `is_valid_package_id` says it exists to produce. A collision refusal downstream of that
// could never fire, and an assertion that cannot fail is worse than an absent one.
//
// Each accepted package's root has ALREADY passed `package_root_provenance_ok` against `store_root`,
// so the scan cannot produce a root the mount would refuse — and it is checked HERE as well as at the
// mount rather than only at the mount, because a scan that reported a package the Shell then refused
// would be reporting something untrue.
[[nodiscard]] PackageStoreScan scan_package_store(const std::filesystem::path& store_root);

// The mounts a scan yields, in scan order — THE FIRST REAL PRODUCER of
// `CefShellOptions::ext_packages`.
//
// A pure projection with no decision of its own: every root in `scan.packages` is already canonical
// and provenance-checked, so this cannot admit anything the scan refused. It exists so the producer
// in `editor_main.cpp` is one call rather than a loop that a later edit could get subtly wrong.
[[nodiscard]] std::vector<ExtPackageMount> package_mounts(const PackageStoreScan& scan);

} // namespace context::editor::shell
