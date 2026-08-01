// ctest `editor-shell-ext-package-scaffold` — the M9 e13e SCAFFOLD-THEN-LOAD proof: what
// `context new --template extension-panel` writes is a package the editor actually accepts.
//
// WHY THIS TEST EXISTS, AND WHY IT LIVES HERE. A scaffolder is trivially "testable" by asserting on
// the text it emits — that the manifest mentions `iframe`, that a `panel.html` appears in the file
// list. Every such assertion passes against a template the loader REJECTS, because it verifies the
// scaffolder's string handling and nothing about the contract. So this test never reads the
// template: it scaffolds into a temp PACKAGE STORE and then takes the result through the real load
// path the editor takes at boot —
//
//     cli::run({"new", "--template", "extension-panel", <store>/<id>})   the developer's step 1
//         -> shell::scan_package_store(<store>)                          the editor's boot scan
//         -> shell::package_mounts(scan) -> ExtAssetResolver::mount(...)  the mount table
//         -> resolver.resolve("context-ext://<id>/panel.html")            the served panel document
//
// — which spans `src/cli/` and `src/editor/shell/`, two tiers no single module's suite can link
// together. That is what puts it in the integration tier rather than beside either half.
//
// THE THREE ANTI-DRIFT PINS. The CLI sits BELOW the shell in the link order, so it cannot call
// `is_valid_package_id`, `kPackageManifestFileName` or `kContractMajor`; it mirrors all three. A
// mirror that drifts produces a scaffold whose output the store silently refuses, and NOTHING else
// in the repo can see it — the CLI's own suite cannot link the shell, and the shell's own suite has
// no scaffolder. This test links both, so each pin is an assertion here:
//   (1) the manifest FILE NAME the CLI writes == the name the store reads;
//   (2) the `contractVersion` VALUE READ BACK OUT OF THE WRITTEN FILE == `gc::kContractMajor`;
//   (3) the CLI's package-id grammar agrees with the store's, id for id, over a table that includes
//       every clause either one refuses on.
// Each is read from the artifact or computed from the two live implementations — never from a
// constant this file also spells.

#include "context/cli/app.h"
#include "context/cli/scaffold.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/registry.h"
#include "context/editor/gui/contract/sandbox.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/shell/ext_scheme.h"
#include "context/editor/shell/package_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace cli = context::cli;
namespace shell = context::editor::shell;
namespace gc = context::editor::gui::contract;
namespace fs = std::filesystem;

using context::editor::contract::Envelope;
using context::editor::contract::Json;

namespace
{

int g_failures = 0;
void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

// A unique temp root per run. Unique PER PROCESS, not merely per tag: the Windows CI legs ride
// self-hosted runners sharing one TEMP, so a PR rollup and a `main` push overlap routinely, and this
// function begins by removing the path it is about to use.
fs::path make_temp_root(const char* tag)
{
    static int counter = 0;
    // The tick count is materialised into a CONCRETE integer BEFORE std::to_string: a chrono rep is
    // implementation-defined and Apple libc++ finds the overload ambiguous on one where GCC and MSVC
    // do not.
    static const long long run_ticks = static_cast<long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) /
                    ("ctx-e13e-" + std::string(tag) + "-" + std::to_string(run_ticks) + "-" +
                     std::to_string(++counter));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void cleanup(const fs::path& path)
{
    std::error_code ec;
    fs::remove_all(path, ec);
}

[[nodiscard]] std::string read_text(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

[[nodiscard]] bool mentions(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Both sides canonicalized. `temp_directory_path()` is NON-canonical on macOS (`/var/folders/...`
// reaches through a `/var -> private/var` symlink), so a raw compare of a resolved file against a
// fixture path refuses every fixture on that one host while passing on Windows and Linux.
[[nodiscard]] bool same_file(const fs::path& a, const fs::path& b)
{
    std::error_code ec_a;
    std::error_code ec_b;
    const fs::path ca = fs::weakly_canonical(a, ec_a);
    const fs::path cb = fs::weakly_canonical(b, ec_b);
    return !ec_a && !ec_b && ca == cb;
}

// ===================================================================================================
// 1. THE THREE STEPS — nothing to a rendering panel in three, and the third is the editor's own path.
// ===================================================================================================

void test_three_steps_from_nothing_to_a_served_panel()
{
    const fs::path root = make_temp_root("threesteps");
    // The package STORE. In production this is `<home>/.context/packages` (package_store.h); here it
    // is a temp one, which is the whole reason the budget is three: a package's id IS its directory
    // name, so scaffolding INTO the store is the install — there is no separate install step to
    // spend a fourth on.
    const fs::path store = root / "packages";
    std::error_code ec;
    fs::create_directories(store, ec);
    CHECK(!ec);

    const std::string package_id = "hello-panel";
    const fs::path package_root = store / package_id;

    // --- STEP 1 of 3: scaffold it into the store, through the REAL verb ---------------------------
    const Envelope created =
        cli::run({"new", "--template", "extension-panel", package_root.string()});
    CHECK(created.ok());
    if (!created.ok())
    {
        std::fprintf(stderr, "[e13e] `context new --template extension-panel` FAILED: %s\n",
                     created.error().has_value() ? created.error()->message.c_str() : "(no error)");
        cleanup(root);
        return;
    }
    CHECK(created.data().at("packageId").as_string() == package_id);
    CHECK(created.data().at("loadable").as_bool());

    // --- STEP 2 of 3: start the editor. This is what its boot does with the store ------------------
    const shell::PackageStoreScan scan = shell::scan_package_store(store);
    // ZERO refusals, not merely "one package": the scan reports every refusal by name, so a template
    // that produced something subtly wrong would show up HERE rather than as a missing panel later.
    CHECK(scan.refusals.empty());
    for (const shell::PackageRefusal& refusal : scan.refusals)
    {
        std::fprintf(stderr, "[e13e] the store REFUSED the scaffolded package: %s: %s — %s\n",
                     refusal.id.c_str(), refusal.error_code.c_str(), refusal.message.c_str());
    }
    CHECK(scan.packages.size() == 1);
    if (scan.packages.size() != 1)
    {
        cleanup(root);
        return;
    }
    const shell::InstalledPackage& installed = scan.packages.front();
    CHECK(installed.id == package_id);
    CHECK(installed.version == "0.1.0");
    CHECK(installed.contributions.size() == 1);

    // The mount table the Shell hands CEF, built by the real producer and driven through the real
    // consumer — so "the scan accepted it" is proven against the thing that will receive it.
    const std::vector<shell::ExtPackageMount> mounts = shell::package_mounts(scan);
    CHECK(mounts.size() == 1);
    shell::ExtAssetResolver resolver;
    for (const shell::ExtPackageMount& mount : mounts)
    {
        std::string reason;
        CHECK(resolver.mount(mount.id, mount.root, store, reason));
        if (!reason.empty())
        {
            std::fprintf(stderr, "[e13e] the scaffolded package did NOT mount: %s: %s\n",
                         mount.id.c_str(), reason.c_str());
        }
    }
    CHECK(resolver.is_mounted(package_id));

    // --- STEP 3 of 3: open the panel. The document the manifest names is SERVED --------------------
    //
    // The entry URL is read OUT OF THE PARSED CONTRIBUTION, never spelled here: a hardcoded URL would
    // pass even if the template wrote a different one, which is precisely the drift this test exists
    // to catch.
    const gc::Contribution& contribution = installed.contributions.front();
    const shell::ExtResolution entry = resolver.resolve(contribution.content.entry);
    CHECK(entry.ok());
    CHECK(entry.package_id == package_id);
    // The resolved file IS the scaffolded panel document — not merely "some file resolved".
    CHECK(same_file(entry.file, package_root / cli::kExtensionPanelEntryFileName));
    // Its media type is one a panel document may be NAVIGATED to. Asked of the shell's own predicate
    // rather than compared against a hardcoded mime string, so this cannot drift from the gate that
    // actually decides it.
    CHECK(shell::ext_document_media_type_permitted(entry.mime_type));

    // The served BODY takes the panel-port bootstrap splice — i.e. what the iframe host will hand the
    // frame really does carry the editor channel ahead of the package's own script. A POSITIVE
    // artifact (the tag is present), not "the body changed".
    const std::string body = read_text(entry.file);
    CHECK(!body.empty());
    const std::string served = shell::ext_inject_port_bootstrap(entry.mime_type, body);
    CHECK(mentions(served, shell::kExtPortBootstrapAsset));
    CHECK(served.size() > body.size());
    // The package's own script is what CONSUMES that channel, and it is a separate same-origin file
    // because the panel CSP is `script-src 'self'` with no `'unsafe-inline'`.
    CHECK(mentions(body, "panel.js"));
    CHECK(!mentions(body, "<script>"));
    CHECK(mentions(read_text(package_root / "panel.js"), shell::kExtPortGlobalName));

    // ...and the panel's own subresources resolve over the same origin, which is the rest of what a
    // rendering panel needs: a document that loads but whose stylesheet and script 404 is a blank
    // frame with nothing naming why.
    const std::string origin = std::string(shell::kExtUrlPrefix) + package_id + "/";
    const shell::ExtResolution css = resolver.resolve(origin + "panel.css");
    const shell::ExtResolution js = resolver.resolve(origin + "panel.js");
    CHECK(css.ok());
    CHECK(js.ok());
    CHECK(same_file(css.file, package_root / "panel.css"));
    CHECK(same_file(js.file, package_root / "panel.js"));

    // The NEGATIVE half of the mount claim: a sibling id that was never scaffolded is NOT served, so
    // the positives above come from the mount table rather than from a resolver that serves anything.
    CHECK(!resolver.resolve("context-ext://not-installed/panel.html").ok());

    cleanup(root);
}

// ===================================================================================================
// 2. THE PARSED CONTRIBUTION — what the generated manifest MEANS once the store has read it.
// ===================================================================================================

void test_generated_contribution_is_what_the_editor_accepts()
{
    const fs::path root = make_temp_root("contribution");
    const fs::path store = root / "packages";
    std::error_code ec;
    fs::create_directories(store, ec);
    const std::string package_id = "acme.hello-panel";
    const fs::path package_root = store / package_id;

    CHECK(cli::run({"new", "--template", "extension-panel", package_root.string()}).ok());

    const shell::PackageStoreScan scan = shell::scan_package_store(store);
    CHECK(scan.refusals.empty());
    CHECK(scan.packages.size() == 1);
    if (scan.packages.size() != 1)
    {
        cleanup(root);
        return;
    }
    const shell::InstalledPackage& installed = scan.packages.front();
    CHECK(installed.contributions.size() == 1);
    if (installed.contributions.size() != 1)
    {
        cleanup(root);
        return;
    }
    const gc::Contribution& c = installed.contributions.front();

    // Namespaced to the package — the store refuses a manifest whose contribution id is not.
    CHECK(c.id == package_id + ".panel");
    CHECK(c.kind == gc::ContributionKind::panel);
    // `iframe` is the only content type a package contribution may declare.
    CHECK(c.content.type == gc::ContentType::iframe);
    CHECK(c.content.entry == std::string(shell::kExtUrlPrefix) + package_id + "/" +
                                 cli::kExtensionPanelEntryFileName);
    // The task's own manifest-v2 requirements, asserted on the PARSED value rather than the text.
    CHECK(c.dock.singleton);
    CHECK(c.dock.default_zone == gc::DockZone::right);
    CHECK(c.dock.min_width > 0);
    CHECK(c.dock.min_height > 0);
    CHECK(c.state.schema_version == 1);
    CHECK(c.contract_version == gc::kContractMajor);
    // A DECLARED capability, on the closed vocabulary. The store refuses an unknown token outright,
    // so this also pins that the template does not invent one.
    CHECK(c.capabilities.size() == 1);
    CHECK(std::find(c.capabilities.begin(), c.capabilities.end(), gc::kCapabilityReadQuery) !=
          c.capabilities.end());

    // ⭐ THE PARSED CONTRIBUTION IS ACCEPTED BY THE REAL REGISTRY — the assertion that makes this a
    // PROPERTY rather than a field-by-field opinion: the registry independently re-validates the
    // contract version, the sandbox conformance, the manifest structure and the capability
    // vocabulary, so a template producing a structurally impossible Contribution is refused here even
    // if every CHECK above passed.
    gc::ExtensionRegistry registry;
    const gc::RegistrationResult result = registry.register_contribution(c);
    CHECK(result.ok);
    if (!result.ok)
    {
        std::fprintf(stderr, "[e13e] the registry REFUSED the scaffolded contribution: %s — %s\n",
                     result.error_code.c_str(), result.message.c_str());
    }

    // The scaffold GRANTS nothing: `capabilities` is what the manifest ASKS for, `sandbox` is what it
    // is GIVEN, and the store leaves it at least privilege. A template that could move any of these
    // would be the capability model bypassed by one JSON member.
    const gc::Contribution default_policy;
    CHECK(c.sandbox.granted_scopes.names() == default_policy.sandbox.granted_scopes.names());
    CHECK(!c.sandbox.node_integration);
    CHECK(c.sandbox.isolated_renderer);
    CHECK(c.sandbox.sandboxed_iframe);
    CHECK(!c.sandbox.daemon_socket_access);
    CHECK(gc::sandbox_conformant(c.sandbox));

    // The manifest is a canonicalization FIXPOINT — `context new` is a tool save (R-FILE-001), so the
    // bytes it wrote are already THE canonical form.
    const std::string manifest_text = read_text(package_root / cli::kExtensionPanelManifestFileName);
    CHECK(!manifest_text.empty());
    CHECK(context::editor::serializer::canonicalize(manifest_text).bytes == manifest_text);

    cleanup(root);
}

// ===================================================================================================
// 3. THE THREE ANTI-DRIFT PINS — the CLI's three mirrors of shell-tier vocabulary, checked against
//    the live shell-tier values. Nothing else in the repo links both sides.
// ===================================================================================================

void test_cross_tier_pins()
{
    // (1) The manifest FILE NAME. A different name yields `package.manifest_missing` for every
    //     scaffolded package, with the CLI reporting success.
    CHECK(std::string(cli::kExtensionPanelManifestFileName) ==
          std::string(shell::kPackageManifestFileName));

    // (2) The CONTRACT MAJOR — read out of a manifest the scaffolder actually wrote, not out of the
    //     CLI's constant. `read_package_manifest` refuses any stated value that is not
    //     `kContractMajor`, so a major bump would otherwise turn this template into one that emits
    //     packages the editor rejects, silently.
    const fs::path root = make_temp_root("pins");
    const fs::path package_root = root / "pinned-panel";
    CHECK(cli::run({"new", "--template", "extension-panel", package_root.string()}).ok());
    const std::string manifest_text = read_text(package_root / cli::kExtensionPanelManifestFileName);
    // Bail on a CHECK rather than letting `Json::parse` throw out of main. When the scaffold did not
    // run — or wrote its manifest under another name, which is exactly what pin (1) exists to catch —
    // this text is EMPTY, and an uncaught parse exception reports `Subprocess aborted` with no line
    // number, burying the assertion that actually failed. (Measured: the P13/P14 plants of this
    // task's own planting round aborted here before this guard.)
    CHECK(!manifest_text.empty());
    if (manifest_text.empty())
    {
        std::fprintf(stderr,
                     "[e13e] no manifest at %s — the scaffold did not run, or wrote it elsewhere\n",
                     (package_root / cli::kExtensionPanelManifestFileName).string().c_str());
        cleanup(root);
        return;
    }
    const Json manifest = Json::parse(manifest_text);
    const Json& contribution = manifest.at("contributions").at(0);
    CHECK(contribution.at("contractVersion").as_int() ==
          static_cast<std::int64_t>(gc::kContractMajor));
    // Stated in the file rather than omitted, so the template TEACHES the version it targets — and
    // the assertion above is what keeps that statement true.
    CHECK(contribution.at("contractVersion").as_int() == cli::kExtensionPanelContractVersion);

    // (3) The `context-ext://` prefix the entry carries, against the shell's own spelling.
    const std::string entry = contribution.at("content").at("entry").as_string();
    CHECK(entry.rfind(shell::kExtUrlPrefix, 0) == 0);

    cleanup(root);
}

// The package-id GRAMMAR, id for id. The table covers every clause either implementation refuses on,
// because agreement on a handful of obviously-good names would be agreement about nothing.
void test_package_id_grammar_agreement()
{
    static const char* const kIds[] = {
        // accepted shapes
        "hello-panel", "acme.hello-panel", "a", "pkg2", "context.hello-panel_2", "x_y-z.w",
        // refused: empty / case / leading or trailing punctuation
        "", "Upper", "hello-Panel", ".hidden", "trailing-", "-leading", "trailing.", "_lead",
        // refused: dot-traversal shapes
        ".", "..", "a..b", "../etc",
        // refused: a last label that is entirely digits (the URL Standard's "ends in a number")
        "12345", "pkg.2", "pkg.007",
        // refused: bytes outside the grammar
        "has space", "has/slash", "has\\backslash", "has:colon", "has%pct", "has@at", "UPPER.CASE",
        // the length bound, from both sides (64 accepted, 65 refused)
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};

    for (const char* id : kIds)
    {
        const bool cli_says = cli::is_scaffold_package_id(id);
        const bool shell_says = shell::is_valid_package_id(id);
        CHECK(cli_says == shell_says);
        if (cli_says != shell_says)
        {
            std::fprintf(stderr,
                         "[e13e] package-id grammar DRIFT on '%s': cli=%d shell=%d — a scaffold "
                         "using the CLI's answer would %s\n",
                         id, static_cast<int>(cli_says), static_cast<int>(shell_says),
                         cli_says ? "emit a package the store refuses"
                                  : "refuse a directory the store would accept");
        }
    }

    // NON-VACUITY: the table must contain both answers, or an implementation that returned a constant
    // would agree with itself on every row. Asserted on the SHELL's answers, the authority here.
    std::size_t accepted = 0;
    std::size_t refused = 0;
    for (const char* id : kIds)
    {
        if (shell::is_valid_package_id(id))
        {
            ++accepted;
        }
        else
        {
            ++refused;
        }
    }
    CHECK(accepted >= 6);
    CHECK(refused >= 15);
}

// ===================================================================================================
// 4. THE REFUSAL IS PROTECTING A REAL ONE — the CLI refuses exactly the directory names the store
//    would refuse, and refuses them BEFORE writing anything.
// ===================================================================================================

void test_cli_refuses_what_the_store_would_refuse()
{
    const fs::path root = make_temp_root("refusal");
    const fs::path store = root / "packages";
    std::error_code ec;
    fs::create_directories(store, ec);

    // A directory name that is not a legal package id. The CLI refuses it, and — the half that
    // matters — writes NOTHING, so there is no half-package left behind for a later scan to report.
    const fs::path bad = store / "Upper";
    const Envelope refused = cli::run({"new", "--template", "extension-panel", bad.string()});
    CHECK(!refused.ok());
    CHECK(refused.error().has_value());
    if (refused.error().has_value())
    {
        CHECK(refused.error()->code == "usage.invalid");
    }
    CHECK(!fs::exists(bad));

    // And that refusal is not the CLI being fussy: a directory of that name, staged by hand with a
    // manifest, IS refused by the store — with the code the CLI's message exists to prevent a user
    // ever seeing. This is the positive proof that the two refusals are the same refusal.
    std::error_code stage_ec;
    fs::create_directories(bad, stage_ec);
    {
        std::ofstream manifest(bad / shell::kPackageManifestFileName, std::ios::binary);
        manifest << "{\"id\":\"Upper\",\"contributions\":[]}";
    }
    const shell::PackageStoreScan scan = shell::scan_package_store(store);
    CHECK(scan.packages.empty());
    CHECK(scan.refusals.size() == 1);
    if (scan.refusals.size() == 1)
    {
        CHECK(scan.refusals.front().error_code == shell::kErrPackageIdInvalid);
    }

    cleanup(root);
}

} // namespace

int main()
{
    test_three_steps_from_nothing_to_a_served_panel();
    test_generated_contribution_is_what_the_editor_accepts();
    test_cross_tier_pins();
    test_package_id_grammar_agreement();
    test_cli_refuses_what_the_store_would_refuse();
    return g_failures == 0 ? 0 : 1;
}
