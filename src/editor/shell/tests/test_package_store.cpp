// ctest `editor-shell-test_package_store` — the package store: `~/.context/packages` enumeration, the
// manifest -> `Contribution` mapping, and the mount projection (M9 e13c-3, design 04 §3/§5 / 08 §1-§2).
//
// THE STORE IS UNTRUSTED INPUT. Whatever writes it — a CLI verb, an AI agent, a human copying a
// directory in — is out of this module's scope, so every directory it finds and every byte of every
// manifest is adversarial by construction. This suite is therefore refusal-first: each of the five
// manifest validation rules and each store-level fault has its own case, and each pins the grep-stable
// CODE rather than a bare `!read(...)`, because the rules overlap (an entry naming another package's
// origin is also an entry with a suspicious id) and a bare boolean cannot tell a held rule from a
// differently-held one.
//
// NON-VACUITY IS ASSERTED, NOT ASSUMED, in the two disciplines `test_ext_scheme.cpp` established:
//   (1) EVERY REFUSAL IS PAIRED WITH A POSITIVE. A manifest identical to the refused one except for
//       the single member under test is ACCEPTED — so each block proves the rule and not that the
//       parser refuses everything. `test_manifest_happy_path` is the shared baseline every negative is
//       a one-member mutation of.
//   (2) EVERY REFUSAL PINS ITS CODE. `kErrManifestInvalid` vs `kErrManifestIdMismatch` vs
//       `kErrManifestMalformed` is the difference between "your JSON is wrong", "your directory and
//       your manifest disagree" and "your contribution asked for something it may not have".
//
// The PROVENANCE half of this task (`package_root_provenance_ok`, `path_is_os_link`, and the
// discriminating symlink case that separates our link refusal from the STL's canonicalization) is
// asserted next door in `test_ext_scheme.cpp`, where the function it tests is declared and where the
// mount that calls it lives. What is asserted HERE is that the SCAN inherits it — a store entry that
// is a symlink is refused by the scan too, with the same code, so a package the Shell would refuse is
// never reported as installed.

#include "context/editor/shell/package_store.h"

#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/registry.h"
#include "context/editor/gui/contract/sandbox.h"
#include "context/editor/shell/ext_scheme.h"
#include "context/editor/shell/user_config.h" // user_config_path() — the sibling ~/.context/ member

#include "shell_test.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace shell = context::editor::shell;
namespace gc = context::editor::gui::contract;

namespace
{

using shelltest::write_file;

// The baseline manifest every negative below is a ONE-MEMBER mutation of. Written as a function of the
// package id so an id-mismatch case cannot accidentally also change the entry URL.
[[nodiscard]] std::string baseline_manifest(const std::string& id)
{
    return R"({
  "id": ")" + id + R"(",
  "version": "1.2.3",
  "contributions": [
    {
      "id": ")" + id + R"(.panel",
      "kind": "panel",
      "title": "Hello Panel",
      "icon": "puzzle",
      "contractVersion": 2,
      "dock": { "zone": "right", "singleton": true, "minWidth": 240, "minHeight": 120 },
      "content": { "type": "iframe", "entry": "context-ext://)" + id + R"(/panel.html" },
      "state": { "schemaVersion": 3 },
      "capabilities": [ "read_query", "ui_events" ],
      "commands": [ { "id": ")" + id + R"(.refresh", "title": "Refresh", "when": "panelFocus" } ]
    }
  ]
})";
}

// Stage one package directory inside `store` carrying `manifest` verbatim. Returns its root.
//
// NOT `[[nodiscard]]`: staging IS the effect, and most callers only need the fixture on disk. (The
// existence of the manifest file it writes is asserted by `shelltest::write_file` itself — the
// harness's own reason for existing: on an adversarial suite a fixture that silently failed to write
// makes every "refused" assertion trivially true.)
std::filesystem::path stage_package(const std::filesystem::path& store, const std::string& dir_name,
                                    const std::string& manifest)
{
    const std::filesystem::path root = store / dir_name;
    write_file(root / shell::kPackageManifestFileName, manifest);
    write_file(root / "panel.html", "<!DOCTYPE html><title>hi</title>");
    return root;
}

// Read one manifest through the real entry point. `store`/`dir_name` mirror what the scan would do.
[[nodiscard]] bool read_staged(const std::filesystem::path& store, const std::string& dir_name,
                               shell::InstalledPackage& out, std::string& code, std::string& message)
{
    const std::filesystem::path root = store / dir_name;
    return shell::read_package_manifest(root / shell::kPackageManifestFileName, dir_name, root, out,
                                        code, message);
}

// --------------------------------------------------------------------------- the canonical store root

void test_store_root_shape()
{
    // The store root is DERIVED, never configured by a package. Its shape is pinned rather than its
    // value, because the value is the developer's own home directory.
    const std::filesystem::path store = shell::package_store_root();
    if (store.empty())
    {
        // No HOME / USERPROFILE. A supported state (package_store.h) and NOT a failure — but never
        // silent, or a leg on which this whole assertion was skipped would read as a leg on which it
        // passed.
        std::fprintf(stderr, "[test_package_store] SKIPPED the store-root shape case — no home "
                             "directory resolved on this host\n");
    }
    else
    {
        CHECK(store.filename() == shell::kPackageStoreDirName);
        CHECK(store.parent_path().filename() == ".context");
        // The FOURTH member of a family the Shell already owns, resolved through the SAME
        // home_directory() — so `~/.context/packages` cannot disagree with `~/.context/config.json`
        // about where the user's home is.
        CHECK(store.parent_path() == shell::user_config_path().parent_path());
    }

    // The manifest file name is deliberately NOT `package.json` — the repo already reads that one (the
    // npm manifest of the TS scripting tier), and a package that is BOTH could not carry both.
    CHECK(std::string(shell::kPackageManifestFileName) == "context-package.json");
    CHECK(std::string(shell::kPackageManifestFileName) != "package.json");
}

// -------------------------------------------------------------------- the manifest -> Contribution

void test_manifest_happy_path()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-pkg-store", "manifest-ok");
    const std::filesystem::path store = root / "packages";
    stage_package(store, "hello-panel", baseline_manifest("hello-panel"));

    shell::InstalledPackage package;
    std::string code;
    std::string message;
    CHECK(read_staged(store, "hello-panel", package, code, message));
    CHECK(code.empty() && message.empty());

    CHECK(package.id == "hello-panel");
    CHECK(package.version == "1.2.3");
    CHECK(package.contributions.size() == 1);
    if (package.contributions.size() == 1)
    {
        const gc::Contribution& c = package.contributions.front();
        CHECK(c.id == "hello-panel.panel");
        CHECK(c.kind == gc::ContributionKind::panel);
        CHECK(c.title == "Hello Panel");
        CHECK(c.icon == "puzzle");
        CHECK(c.contract_version == gc::kContractMajor);
        CHECK(c.dock.default_zone == gc::DockZone::right);
        CHECK(c.dock.singleton);
        CHECK(c.dock.min_width == 240);
        CHECK(c.dock.min_height == 120);
        CHECK(c.content.type == gc::ContentType::iframe);
        CHECK(c.content.entry == "context-ext://hello-panel/panel.html");
        CHECK(c.state.schema_version == 3);
        CHECK(c.capabilities.size() == 2);
        CHECK(std::find(c.capabilities.begin(), c.capabilities.end(), gc::kCapabilityReadQuery) !=
              c.capabilities.end());
        CHECK(std::find(c.capabilities.begin(), c.capabilities.end(), gc::kCapabilityUiEvents) !=
              c.capabilities.end());
        CHECK(c.commands.size() == 1);
        if (c.commands.size() == 1)
        {
            CHECK(c.commands.front().id == "hello-panel.refresh");
            CHECK(c.commands.front().title == "Refresh");
            CHECK(c.commands.front().when == "panelFocus");
        }

        // ⭐ THE PARSED CONTRIBUTION IS ACCEPTED BY THE REAL REGISTRY. This is the assertion that
        // makes the mapping's correctness a PROPERTY rather than a field-by-field opinion: the
        // registry independently re-validates the contract version, the sandbox conformance, the
        // manifest structure and the capability vocabulary (registry.h), so a mapping that produced a
        // structurally impossible Contribution would be refused here even if every CHECK above
        // passed. It also documents the e13c-3/-4 boundary from the other side: registering into a
        // LOCAL registry is what a test may do; doing it into the editor's live roster is e13c-4's.
        gc::ExtensionRegistry registry;
        const gc::RegistrationResult result = registry.register_contribution(c);
        CHECK(result.ok);
        if (!result.ok)
        {
            std::fprintf(stderr, "[test_package_store] the registry REFUSED a parsed contribution: "
                                 "%s — %s\n",
                         result.error_code.c_str(), result.message.c_str());
        }
    }

    // GRANTS NOTHING: `capabilities` is what the manifest ASKS for; `sandbox` is what it is GIVEN, and
    // this reader leaves it at the least-privilege default for every package. e13c-4 owns the consent
    // surface that changes that, and a reader honouring a self-declared grant would be the whole
    // capability model bypassed by one JSON member.
    if (!package.contributions.empty())
    {
        const gc::Contribution default_policy;
        const gc::SandboxPolicy& sandbox = package.contributions.front().sandbox;
        CHECK(sandbox.csp == default_policy.sandbox.csp);
        CHECK(sandbox.granted_scopes.names() == default_policy.sandbox.granted_scopes.names());
        // The non-negotiable half, spelled out rather than left to the default: no Node, an isolated
        // renderer, a sandboxed iframe, no socket — the R-EDIT-001 trust boundary. A manifest cannot
        // move any of these, because nothing in the parser reads them.
        CHECK(!sandbox.node_integration);
        CHECK(sandbox.isolated_renderer);
        CHECK(sandbox.sandboxed_iframe);
        CHECK(!sandbox.daemon_socket_access);
        CHECK(gc::sandbox_conformant(sandbox));
    }

    shelltest::cleanup(root);
}

void test_manifest_defaults_are_permissive()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-pkg-store", "defaults");
    const std::filesystem::path store = root / "packages";
    // The MINIMAL legal manifest: an id, one contribution with an id, a legal content type and an
    // own-origin entry. Everything else omitted. Permissive-with-a-default is deliberate for the
    // members whose wrong value costs something COSMETIC (title, icon, dock, `when`) — a parser that
    // refuses a manifest over a missing `icon` is a parser package authors route around.
    stage_package(store, "min", R"({
  "id": "min",
  "contributions": [
    { "id": "min", "content": { "type": "iframe", "entry": "context-ext://min/index.html" } }
  ]
})");

    shell::InstalledPackage package;
    std::string code;
    std::string message;
    CHECK(read_staged(store, "min", package, code, message));
    CHECK(package.version.empty());
    CHECK(package.contributions.size() == 1);
    if (package.contributions.size() == 1)
    {
        const gc::Contribution& c = package.contributions.front();
        // An id EXACTLY equal to the package id is namespaced (rule (a) accepts both forms).
        CHECK(c.id == "min");
        CHECK(c.title == "min"); // title defaults to the id, as parsePanelManifest does
        CHECK(c.icon.empty());
        CHECK(c.kind == gc::ContributionKind::panel);
        CHECK(c.dock.default_zone == gc::DockZone::center);
        CHECK(!c.dock.singleton);
        CHECK(c.dock.min_width == 0 && c.dock.min_height == 0);
        CHECK(c.state.schema_version == 1);
        CHECK(c.capabilities.empty());
        CHECK(c.commands.empty());
        // An ABSENT contractVersion means "the current one" — a manifest that states nothing is not
        // claiming an incompatible contract.
        CHECK(c.contract_version == gc::kContractMajor);
    }

    // An UNRECOGNISED dock zone falls back to `center` rather than refusing: the vocabulary is closed,
    // so anything else is drift, and the cost of the fallback is where a panel first appears. Contrast
    // `content.type` below, which fails CLOSED because its cost is not cosmetic.
    stage_package(store, "zoned", R"({
  "id": "zoned",
  "contributions": [
    { "id": "zoned", "dock": { "zone": "diagonal", "minWidth": -8 },
      "content": { "type": "iframe", "entry": "context-ext://zoned/index.html" } }
  ]
})");
    shell::InstalledPackage zoned;
    CHECK(read_staged(store, "zoned", zoned, code, message));
    CHECK(zoned.contributions.size() == 1);
    if (zoned.contributions.size() == 1)
    {
        CHECK(zoned.contributions.front().dock.default_zone == gc::DockZone::center);
        // A NEGATIVE minimum becomes "unstated" (0), which DockDefaults documents; passing it through
        // would hand the docking layer a value it documents as refused.
        CHECK(zoned.contributions.front().dock.min_width == 0);
    }

    shelltest::cleanup(root);
}

void test_manifest_refusals()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-pkg-store", "refuse");
    const std::filesystem::path store = root / "packages";
    shell::InstalledPackage package;
    std::string code;
    std::string message;

    // --- absent / unreadable -------------------------------------------------------------------
    // A directory with no manifest at all. `stage_package` is not used: that is the point.
    std::error_code ec;
    std::filesystem::create_directories(store / "bare", ec);
    // THE FIXTURE ASSERTS ITSELF — the one fixture in this suite that `write_file`/`stage_package` do
    // not cover. `kErrManifestMissing` is ALSO what an absent DIRECTORY yields, so a silently-failed
    // `create_directories` would leave this case passing while testing nothing.
    CHECK(std::filesystem::is_directory(store / "bare"));
    CHECK(!read_staged(store, "bare", package, code, message));
    CHECK(code == shell::kErrManifestMissing);

    // OVERSIZED. The cap exists because untrusted input with no bound is an allocation an attacker
    // chooses; a manifest one byte past it is treated as unreadable rather than parsed.
    {
        std::string huge = "{\"id\":\"huge\",\"pad\":\"";
        huge.append(static_cast<std::size_t>(shell::kMaxPackageManifestBytes) + 64u, 'x');
        huge += "\"}";
        write_file(store / "huge" / shell::kPackageManifestFileName, huge);
        CHECK(!read_staged(store, "huge", package, code, message));
        CHECK(code == shell::kErrManifestMissing);
    }

    // --- malformed ------------------------------------------------------------------------------
    write_file(store / "broken" / shell::kPackageManifestFileName, "{ not json ,,, ");
    CHECK(!read_staged(store, "broken", package, code, message));
    CHECK(code == shell::kErrManifestMalformed);

    write_file(store / "arr" / shell::kPackageManifestFileName, "[1, 2, 3]");
    CHECK(!read_staged(store, "arr", package, code, message));
    CHECK(code == shell::kErrManifestMalformed);

    // --- DECISION 2: the directory name and the manifest id must AGREE -------------------------
    // The security half: a manifest declaring another package's id is either mispackaged or trying to
    // have its bytes served under that package's origin.
    stage_package(store, "claimant", baseline_manifest("victim"));
    CHECK(!read_staged(store, "claimant", package, code, message));
    CHECK(code == shell::kErrManifestIdMismatch);
    CHECK(shelltest::mentions(message, "victim"));
    CHECK(shelltest::mentions(message, "claimant"));
    // An ABSENT id is the same fault, not a separate one — "no id" and "the wrong id" are both "this
    // manifest does not say it belongs to this directory".
    write_file(store / "anon" / shell::kPackageManifestFileName, R"({"contributions":[]})");
    CHECK(!read_staged(store, "anon", package, code, message));
    CHECK(code == shell::kErrManifestIdMismatch);

    // --- contributions must exist ---------------------------------------------------------------
    write_file(store / "empty" / shell::kPackageManifestFileName, R"({"id":"empty"})");
    CHECK(!read_staged(store, "empty", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);
    write_file(store / "none" / shell::kPackageManifestFileName,
               R"({"id":"none","contributions":[]})");
    CHECK(!read_staged(store, "none", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);

    // --- rule (a): a namespaced, unique, non-empty id -------------------------------------------
    // ⭐ THE SHADOWING CASE, and the reason the rule exists: without it a package could contribute
    // `builtin.inspector`. The registry's own duplicate-id refusal would only fire if the built-in
    // were registered FIRST, which is an ORDERING and not a control.
    write_file(store / "shadow" / shell::kPackageManifestFileName, R"({
  "id": "shadow",
  "contributions": [
    { "id": "builtin.inspector",
      "content": { "type": "iframe", "entry": "context-ext://shadow/index.html" } }
  ]
})");
    CHECK(!read_staged(store, "shadow", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);
    CHECK(shelltest::mentions(message, "namespaced"));

    // A PREFIX that is not a SEGMENT boundary must not pass: `shadowy.panel` is not inside `shadow`.
    write_file(store / "shadow2" / shell::kPackageManifestFileName, R"({
  "id": "shadow2",
  "contributions": [
    { "id": "shadow2x.panel",
      "content": { "type": "iframe", "entry": "context-ext://shadow2/index.html" } }
  ]
})");
    CHECK(!read_staged(store, "shadow2", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);

    write_file(store / "noid" / shell::kPackageManifestFileName, R"({
  "id": "noid",
  "contributions": [ { "content": { "type": "iframe", "entry": "context-ext://noid/i.html" } } ]
})");
    CHECK(!read_staged(store, "noid", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);

    write_file(store / "dup" / shell::kPackageManifestFileName, R"({
  "id": "dup",
  "contributions": [
    { "id": "dup.a", "content": { "type": "iframe", "entry": "context-ext://dup/a.html" } },
    { "id": "dup.a", "content": { "type": "iframe", "entry": "context-ext://dup/b.html" } }
  ]
})");
    CHECK(!read_staged(store, "dup", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);
    CHECK(shelltest::mentions(message, "more than once"));

    // --- rule (b): content.type FAILS CLOSED ----------------------------------------------------
    // `uitree` and `local` both mean "the editor renders this from its own code", which a third-party
    // package does not have — accepting either would be accepting a claim it cannot back. An absent or
    // unrecognised token is refused, never defaulted.
    for (const char* type : {"uitree", "local", "webview", ""})
    {
        const std::string dir = std::string("ct-") + (*type == '\0' ? "absent" : type);
        write_file(store / dir / shell::kPackageManifestFileName,
                   R"({"id":")" + dir + R"(","contributions":[{"id":")" + dir +
                       R"(","content":{"type":")" + type + R"(","entry":"context-ext://)" + dir +
                       R"(/i.html"}}]})");
        CHECK(!read_staged(store, dir, package, code, message));
        CHECK(code == shell::kErrManifestInvalid);
        CHECK(shelltest::mentions(message, "iframe"));
    }

    // --- rule (c): the entry must name THIS package's origin -----------------------------------
    // ⭐ THE CROSS-PACKAGE CASE this whole subsystem exists to refuse, plus the off-scheme shapes.
    // `context-ext://minex/` is the sharpest: it is a strict PREFIX-match of the package id `mine`, so
    // a naive `starts_with` check would admit it as this package's own origin.
    for (const char* entry : {"context-ext://other/i.html", "context-ext://minex/i.html",
                              "https://evil.example/i.html", "file:///etc/passwd",
                              "data:text/html,<b>hi", "context-ext://mine", "/i.html", ""})
    {
        write_file(store / "mine" / shell::kPackageManifestFileName,
                   R"({"id":"mine","contributions":[{"id":"mine","content":{"type":"iframe",)"
                   R"("entry":")" + std::string(entry) + R"("}}]})");
        CHECK(!read_staged(store, "mine", package, code, message));
        CHECK(code == shell::kErrManifestInvalid);
    }
    // NON-VACUITY for rule (c): the SAME manifest with its own origin is accepted.
    write_file(store / "mine" / shell::kPackageManifestFileName,
               R"({"id":"mine","contributions":[{"id":"mine","content":{"type":"iframe",)"
               R"("entry":"context-ext://mine/i.html"}}]})");
    CHECK(read_staged(store, "mine", package, code, message));

    // --- rule (d): capabilities are a CLOSED vocabulary, deny-by-default -----------------------
    // An unknown token is REFUSED, never dropped: dropping one would present this package to e13c-4's
    // consent surface as asking for LESS than its manifest states.
    write_file(store / "cap" / shell::kPackageManifestFileName, R"({
  "id": "cap",
  "contributions": [
    { "id": "cap", "capabilities": [ "read_query", "root_access" ],
      "content": { "type": "iframe", "entry": "context-ext://cap/i.html" } }
  ]
})");
    CHECK(!read_staged(store, "cap", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);
    CHECK(shelltest::mentions(message, "root_access"));
    // A HYPHENATED spelling is the near-miss worth pinning: the manifest vocabulary is underscored
    // (`file_write`) while the bridge's WIRE names are hyphenated (`file-write`), and extension.h says
    // registry.cpp owns the one translation. So a manifest using the wire spelling is refused rather
    // than quietly granted nothing.
    write_file(store / "cap2" / shell::kPackageManifestFileName, R"({
  "id": "cap2",
  "contributions": [
    { "id": "cap2", "capabilities": [ "file-write" ],
      "content": { "type": "iframe", "entry": "context-ext://cap2/i.html" } }
  ]
})");
    CHECK(!read_staged(store, "cap2", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);
    // A NON-STRING token, and a non-array `capabilities`.
    write_file(store / "cap3" / shell::kPackageManifestFileName, R"({
  "id": "cap3",
  "contributions": [
    { "id": "cap3", "capabilities": [ 7 ],
      "content": { "type": "iframe", "entry": "context-ext://cap3/i.html" } }
  ]
})");
    CHECK(!read_staged(store, "cap3", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);
    write_file(store / "cap4" / shell::kPackageManifestFileName, R"({
  "id": "cap4",
  "contributions": [
    { "id": "cap4", "capabilities": "read_query",
      "content": { "type": "iframe", "entry": "context-ext://cap4/i.html" } }
  ]
})");
    CHECK(!read_staged(store, "cap4", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);

    // --- rule (e): a STATED contract version must match ----------------------------------------
    // The compatibility window is a single major (extension.h), so accepting another value would only
    // defer the registry's own refusal to a point where the diagnostic is worse.
    write_file(store / "ver" / shell::kPackageManifestFileName, R"({
  "id": "ver",
  "contributions": [
    { "id": "ver", "contractVersion": 1,
      "content": { "type": "iframe", "entry": "context-ext://ver/i.html" } }
  ]
})");
    CHECK(!read_staged(store, "ver", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);

    // --- a nonsensical state schema version -----------------------------------------------------
    write_file(store / "st" / shell::kPackageManifestFileName, R"({
  "id": "st",
  "contributions": [
    { "id": "st", "state": { "schemaVersion": 0 },
      "content": { "type": "iframe", "entry": "context-ext://st/i.html" } }
  ]
})");
    CHECK(!read_staged(store, "st", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);

    // --- commands are TOTAL, matching readManifestCommands --------------------------------------
    // A malformed command entry is DROPPED, not fatal: a command the editor cannot name is simply not
    // offered, which costs the package a menu entry and costs the editor nothing.
    write_file(store / "cmd" / shell::kPackageManifestFileName, R"({
  "id": "cmd",
  "contributions": [
    { "id": "cmd",
      "commands": [ 5, { "title": "no id" }, { "id": "cmd.ok" } ],
      "content": { "type": "iframe", "entry": "context-ext://cmd/i.html" } }
  ]
})");
    CHECK(read_staged(store, "cmd", package, code, message));
    CHECK(package.contributions.size() == 1);
    if (package.contributions.size() == 1)
    {
        CHECK(package.contributions.front().commands.size() == 1);
        if (package.contributions.front().commands.size() == 1)
        {
            CHECK(package.contributions.front().commands.front().id == "cmd.ok");
            // `title` defaults to the id; `when` to empty (= always), mirroring the C++ field's
            // "empty = always" contract and panels.ts's reader.
            CHECK(package.contributions.front().commands.front().title == "cmd.ok");
            CHECK(package.contributions.front().commands.front().when.empty());
        }
    }

    shelltest::cleanup(root);
}

// ------------------------------------------------------------------------------------------ the scan

// ----------------------------------------- HOSTILE NUMBERS + the manifest's OWN link provenance (03)
//
// Every numeric case in this suite previously stopped at `minWidth: -8`, so the whole class below was
// unreachable — and three of these five were real defects. Their failure mode is NOT a wrong default
// (that is `test_manifest_defaults_are_permissive`'s subject): it is a value that passes a guard and
// then MEANS SOMETHING ELSE after the cast, which is invisible to any test that only feeds sane numbers.
void test_manifest_hostile_numbers_and_links()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-pkg-store", "hostile");
    const std::filesystem::path store = root / "packages";

    shell::InstalledPackage package;
    std::string code;
    std::string message;

    // (a) OUTSIDE int64 — UNDEFINED BEHAVIOUR before the range guard. `Json::parse` accepts `1e300`
    // happily, and `Json::as_int()` is a bare `static_cast<std::int64_t>` of the stored double, which
    // the blocking `sanitize (ASan+UBSan, ubuntu)` leg reports as `float-cast-overflow`. Reads as
    // "unstated" now, via the Shell's one range-guarded numeric read.
    write_file(store / "big" / shell::kPackageManifestFileName,
               R"({ "id": "big", "contributions": [ { "id": "big.p", "kind": "panel",
      "dock": { "minWidth": 1e300, "minHeight": -1e300 },
      "content": { "type": "iframe", "entry": "context-ext://big/p.html" } } ] })");
    CHECK(read_staged(store, "big", package, code, message));
    CHECK(package.contributions.size() == 1);
    if (package.contributions.size() == 1)
    {
        CHECK(package.contributions.front().dock.min_width == 0);
        CHECK(package.contributions.front().dock.min_height == 0);
    }

    // (b) INSIDE int64 BUT OUTSIDE int — an EXACT integer, so no UB is involved at all and no range
    // guard on the double would have caught it. `4294967295` survived `std::max<std::int64_t>(0, …)`
    // and then NARROWED TO -1 on the cast; `registry.cpp` refuses a negative `dock.minSize`, so the
    // scan would have reported as ACCEPTED a package the registry then rejects — exactly what
    // package_store.h § the scan promises never happens.
    write_file(store / "narrow" / shell::kPackageManifestFileName,
               R"({ "id": "narrow", "contributions": [ { "id": "narrow.p", "kind": "panel",
      "dock": { "minWidth": 4294967295, "minHeight": 2147483648 },
      "content": { "type": "iframe", "entry": "context-ext://narrow/p.html" } } ] })");
    CHECK(read_staged(store, "narrow", package, code, message));
    CHECK(package.contributions.size() == 1);
    if (package.contributions.size() == 1)
    {
        // NEVER NEGATIVE, and specifically "unstated" — the property registry.cpp depends on.
        CHECK(package.contributions.front().dock.min_width == 0);
        CHECK(package.contributions.front().dock.min_height == 0);
    }

    // (c) `state.schemaVersion` PAST u32 — likewise exact, likewise no UB. It passed the `>= 1` test
    // and then narrowed to 0, the one value `registry.cpp` names in so many words. REFUSED now rather
    // than silently defaulted, because a package whose state version means something other than what it
    // wrote down is not a package this scan may report as installed.
    write_file(store / "sv" / shell::kPackageManifestFileName,
               R"({ "id": "sv", "contributions": [ { "id": "sv.p", "kind": "panel",
      "state": { "schemaVersion": 4294967296 },
      "content": { "type": "iframe", "entry": "context-ext://sv/p.html" } } ] })");
    CHECK(!read_staged(store, "sv", package, code, message));
    CHECK(code == shell::kErrManifestInvalid);
    // NON-VACUITY: the same manifest one below the boundary is ACCEPTED, so this refuses a RANGE and
    // not the member.
    write_file(store / "sv-ok" / shell::kPackageManifestFileName,
               R"({ "id": "sv-ok", "contributions": [ { "id": "sv-ok.p", "kind": "panel",
      "state": { "schemaVersion": 4294967295 },
      "content": { "type": "iframe", "entry": "context-ext://sv-ok/p.html" } } ] })");
    CHECK(read_staged(store, "sv-ok", package, code, message));

    // (d) THE KIND TOKEN IS THE ONE THE PROJECTION EMITS. `contribution_kind_token` spells this
    // `asset-kind-editor`, with HYPHENS; the first draft of this parser's inverse matched the C++
    // ENUMERATOR spelling `asset_kind_editor`, so a manifest written against the editor's own
    // `panel.list` output silently read as `panel`. Asserted against the forward table itself rather
    // than a literal, so reader and writer cannot drift apart again.
    const std::string kind_token =
        gc::contribution_kind_token(gc::ContributionKind::asset_kind_editor);
    write_file(store / "kinds" / shell::kPackageManifestFileName,
               R"({ "id": "kinds", "contributions": [ { "id": "kinds.a", "kind": ")" + kind_token +
                   R"(",
      "content": { "type": "iframe", "entry": "context-ext://kinds/a.html" } } ] })");
    CHECK(read_staged(store, "kinds", package, code, message));
    CHECK(package.contributions.size() == 1);
    if (package.contributions.size() == 1)
    {
        CHECK(package.contributions.front().kind == gc::ContributionKind::asset_kind_editor);
    }

    // (e) THE MANIFEST ITSELF MUST NOT BE AN OS LINK. The mount provenance walk stops AT the package
    // root, so nothing above it covers a link INSIDE the root — and both `is_regular_file` and
    // `std::ifstream` follow links. Without this refusal a package shipping
    // `context-package.json -> <somewhere outside>` had up to the manifest cap of an arbitrary readable
    // file pulled into the process, with derived content handed to the operator channel.
    //
    // The pinned CODE is what makes this non-vacuous: the linked target below carries an EMPTY
    // `contributions` array, so if the link refusal were removed the file would be READ and refused as
    // `kErrManifestInvalid`. Only the link check produces `kErrManifestMissing` here.
    const std::filesystem::path outside = root / "outside.json";
    write_file(outside, R"({ "id": "linked", "contributions": [] })");
    std::error_code dir_ec;
    std::filesystem::create_directories(store / "linked", dir_ec);
    CHECK(std::filesystem::is_directory(store / "linked"));
    std::error_code link_ec;
    std::filesystem::create_symlink(outside, store / "linked" / shell::kPackageManifestFileName,
                                    link_ec);
#ifndef _WIN32
    CHECK(!link_ec);
#endif
    // Gated on an INDEPENDENT authority, never on the predicate under test (the discipline
    // test_ext_scheme.cpp's OS-link suite states).
    std::error_code probe_ec;
    if (!link_ec &&
        std::filesystem::is_symlink(store / "linked" / shell::kPackageManifestFileName, probe_ec))
    {
        CHECK(!read_staged(store, "linked", package, code, message));
        CHECK(code == shell::kErrManifestMissing);
        CHECK(shelltest::mentions(message, "OS link"));
    }
    else
    {
        // NEVER SILENT — the ctest log must say the gate did not run on this leg.
        std::fprintf(stderr,
                     "[test_package_store] SKIPPED the linked-manifest case (%s) — the manifest OS-link "
                     "refusal is UNCOVERED on this leg\n",
                     link_ec.message().c_str());
    }

    shelltest::cleanup(root);
}

void test_scan_empty_and_absent()
{
    // An EMPTY store root is the no-home-directory case: no packages, no refusals, no diagnostics.
    // Not a fault, and not reported as one.
    const shell::PackageStoreScan unbound = shell::scan_package_store({});
    CHECK(unbound.packages.empty());
    CHECK(unbound.refusals.empty());
    CHECK(shell::package_mounts(unbound).empty());

    // A store that does not exist yet is a FIRST-RUN state: reported (so "no third-party panels"
    // always has a stated reason) but not an error a user can act on.
    const std::filesystem::path root = shelltest::make_temp_project("ctx-pkg-store", "absent");
    const shell::PackageStoreScan absent = shell::scan_package_store(root / "packages");
    CHECK(absent.packages.empty());
    CHECK(absent.refusals.size() == 1);
    if (absent.refusals.size() == 1)
    {
        CHECK(absent.refusals.front().error_code == shell::kErrPackageStoreAbsent);
        CHECK(absent.refusals.front().id.empty());
    }

    // A store root that is a FILE is the same state, not a crash.
    write_file(root / "file-store", "x");
    const shell::PackageStoreScan as_file = shell::scan_package_store(root / "file-store");
    CHECK(as_file.packages.empty());
    CHECK(as_file.refusals.size() == 1);
    if (as_file.refusals.size() == 1)
    {
        // PIN THE CODE, like every other refusal in this suite: "not a directory" must report the
        // first-run state, NOT the unreadable-store fault the scan now distinguishes from it.
        CHECK(as_file.refusals.front().error_code == shell::kErrPackageStoreAbsent);
    }

    // An EXISTING but empty store yields nothing at all — no packages AND no refusals. An installed
    // editor with no packages is the common case and must be quiet.
    std::error_code ec;
    std::filesystem::create_directories(root / "packages", ec);
    const shell::PackageStoreScan quiet = shell::scan_package_store(root / "packages");
    CHECK(quiet.packages.empty());
    CHECK(quiet.refusals.empty());

    shelltest::cleanup(root);
}

void test_scan_accepts_and_refuses()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-pkg-store", "scan");
    const std::filesystem::path store = root / "packages";

    stage_package(store, "alpha", baseline_manifest("alpha"));
    stage_package(store, "beta", baseline_manifest("beta"));
    // An id no `context-ext://` request could ever name again — upper case (Chromium lower-cases a
    // standard URL's host). ⭐ THIS IS ALSO THE SECOND E13B OBLIGATION'S DISCHARGE: because a valid id
    // is lower-case ONLY, two accepted ids can never differ by case, so no downstream dedupe is
    // needed and none is written (a refusal that cannot fire is worse than an absent one).
    stage_package(store, "Upper", baseline_manifest("Upper"));
    // A leading dot — refused by the same grammar rule that refuses `.` and `..` as ids. (A directory
    // literally NAMED `..` cannot be created, so that spelling is refused by the OS before this code
    // is reached; the id grammar covers it for the paths where an id is a STRING rather than a
    // directory entry, which `test_ext_scheme.cpp`'s grammar suite is what pins.)
    stage_package(store, ".hidden", baseline_manifest(".hidden"));
    // A package whose manifest is broken: reported, and does NOT stop the healthy ones.
    write_file(store / "gamma" / shell::kPackageManifestFileName, "{{{");
    // A loose FILE in the store — skipped SILENTLY, the one silence in the scan: a store legitimately
    // accumulates siblings (a leftover archive, a .DS_Store) and reporting each would bury the
    // refusals that mean something.
    write_file(store / "leftover.tgz", "not a package");

    const shell::PackageStoreScan scan = shell::scan_package_store(store);

    // The two healthy packages are accepted, in SORTED order (determinism: directory_iterator order is
    // filesystem-defined, and the mount table it feeds refuses the SECOND of any overlapping pair, so
    // an unstable order would make WHICH package is refused vary run to run).
    CHECK(scan.packages.size() == 2);
    if (scan.packages.size() == 2)
    {
        CHECK(scan.packages[0].id == "alpha");
        CHECK(scan.packages[1].id == "beta");
        CHECK(scan.packages[0].contributions.size() == 1);
    }

    // Three refusals, each NAMED. The loose file is not among them.
    const auto refusal_for = [&scan](const char* id) -> const shell::PackageRefusal* {
        for (const shell::PackageRefusal& refusal : scan.refusals)
        {
            if (refusal.id == id)
            {
                return &refusal;
            }
        }
        return nullptr;
    };
    CHECK(scan.refusals.size() == 3);
    CHECK(refusal_for("Upper") != nullptr);
    if (refusal_for("Upper") != nullptr)
    {
        CHECK(refusal_for("Upper")->error_code == shell::kErrPackageIdInvalid);
    }
    CHECK(refusal_for(".hidden") != nullptr);
    if (refusal_for(".hidden") != nullptr)
    {
        CHECK(refusal_for(".hidden")->error_code == shell::kErrPackageIdInvalid);
    }
    CHECK(refusal_for("gamma") != nullptr);
    if (refusal_for("gamma") != nullptr)
    {
        CHECK(refusal_for("gamma")->error_code == shell::kErrManifestMalformed);
    }
    CHECK(refusal_for("leftover.tgz") == nullptr);

    // THE MOUNT PROJECTION — the first real producer of `CefShellOptions::ext_packages`. Every root it
    // yields is canonical, provenance-checked, and MOUNTS: asserted by driving the real resolver, so
    // the producer's output is proven against the consumer that will receive it rather than inspected.
    const std::vector<shell::ExtPackageMount> mounts = shell::package_mounts(scan);
    CHECK(mounts.size() == 2);
    shell::ExtAssetResolver resolver;
    for (const shell::ExtPackageMount& mount : mounts)
    {
        std::string reason;
        CHECK(resolver.mount(mount.id, mount.root, store, reason));
        if (!reason.empty())
        {
            std::fprintf(stderr, "[test_package_store] a scanned package did NOT mount: %s: %s\n",
                         mount.id.c_str(), reason.c_str());
        }
    }
    CHECK(resolver.mounts().size() == 2);
    CHECK(resolver.is_mounted("alpha"));
    CHECK(resolver.is_mounted("beta"));
    // ...and the panel entry each manifest declared actually RESOLVES over the scheme, which is the
    // end-to-end claim: store -> manifest -> mount -> served bytes.
    CHECK(resolver.resolve("context-ext://alpha/panel.html").ok());
    CHECK(resolver.resolve("context-ext://beta/panel.html").ok());
    // The refused ids are NOT mounted, so a broken package cannot serve anything.
    CHECK(!resolver.is_mounted("gamma"));

    shelltest::cleanup(root);
}

// The scan INHERITS the provenance check — a store entry that is a symlink is refused with the same
// code the mount would use, so a package the Shell would refuse is never reported as installed. The
// discriminating analysis of that refusal (and the case that separates it from a `weakly_canonical`
// compare) lives in test_ext_scheme.cpp; what is asserted here is that the scan does not bypass it.
void test_scan_refuses_linked_entries()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-pkg-store", "scanlink");
    const std::filesystem::path store = root / "packages";
    stage_package(store, "real", baseline_manifest("real"));
    // A fully-valid package OUTSIDE the store, so the refusal cannot be about a broken manifest.
    const std::filesystem::path outside_pkg =
        stage_package(root / "elsewhere", "linked", baseline_manifest("linked"));

    std::error_code ec;
    std::filesystem::create_directory_symlink(outside_pkg, store / "linked", ec);
#ifndef _WIN32
    CHECK(!ec);
    // The TRUE direction, pinned — see test_ext_scheme.cpp's OS-link suite: without it a
    // `path_is_os_link` that answered FALSE to everything would skip the block below, and the `else`
    // would report an EMPTY reason (`ec` is clear) rather than the real one.
    CHECK(shell::path_is_os_link(store / "linked"));
#endif
    // Gated on an INDEPENDENT authority, never on the function under test (same reason as
    // test_ext_scheme.cpp's OS-link suite).
    std::error_code ec_probe;
    if (!ec && std::filesystem::is_symlink(store / "linked", ec_probe))
    {
        const shell::PackageStoreScan scan = shell::scan_package_store(store);
        // The real package is still accepted — one hostile entry does not disable the store.
        CHECK(scan.packages.size() == 1);
        if (scan.packages.size() == 1)
        {
            CHECK(scan.packages.front().id == "real");
        }
        CHECK(scan.refusals.size() == 1);
        if (scan.refusals.size() == 1)
        {
            CHECK(scan.refusals.front().id == "linked");
            CHECK(scan.refusals.front().error_code == shell::kErrMountRootLink);
        }
        // NON-VACUITY: that very package, staged as a REAL directory in the store, is accepted. So the
        // refusal is about the LINK and not about the manifest or the bytes.
        stage_package(store, "linked2", baseline_manifest("linked2"));
        const shell::PackageStoreScan again = shell::scan_package_store(store);
        CHECK(again.packages.size() == 2);
    }
    else
    {
        // NEVER SILENT — the ctest log must say the gate did not run on this leg.
        std::fprintf(stderr,
                     "[test_package_store] SKIPPED the linked-store-entry case (%s) — the scan's "
                     "provenance inheritance is UNCOVERED on this leg\n",
                     ec.message().c_str());
    }

    shelltest::cleanup(root);
}

void test_scan_entry_cap()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-pkg-store", "cap");
    const std::filesystem::path store = root / "packages";
    std::error_code ec;
    std::filesystem::create_directories(store, ec);
    // One PAST the cap. Bare directories (no manifest) so the fixture stays cheap — what is under test
    // is the BOUND, and the bound is on entries considered, not on packages accepted.
    for (std::size_t i = 0; i <= shell::kMaxPackageStoreEntries; ++i)
    {
        std::filesystem::create_directory(store / ("p" + std::to_string(i)), ec);
    }

    const shell::PackageStoreScan scan = shell::scan_package_store(store);
    CHECK(scan.packages.empty());
    // The cap refusal is REPORTED, not a silent truncation: a store the editor only half-read must say
    // so, or a user whose package sits past the cap has no way to learn why it never appeared.
    const bool capped =
        std::any_of(scan.refusals.begin(), scan.refusals.end(),
                    [](const shell::PackageRefusal& refusal) {
                        return refusal.error_code == shell::kErrPackageStoreTooManyEntries;
                    });
    CHECK(capped);
    // Exactly the cap's worth of entries was considered (each yields its own manifest-missing
    // refusal), plus the one cap refusal.
    CHECK(scan.refusals.size() == shell::kMaxPackageStoreEntries + 1u);

    shelltest::cleanup(root);
}

} // namespace

int main()
{
    test_store_root_shape();
    test_manifest_happy_path();
    test_manifest_defaults_are_permissive();
    test_manifest_refusals();
    test_manifest_hostile_numbers_and_links();
    test_scan_empty_and_absent();
    test_scan_accepts_and_refuses();
    test_scan_refuses_linked_entries();
    test_scan_entry_cap();
    SHELL_TEST_MAIN_END();
}
