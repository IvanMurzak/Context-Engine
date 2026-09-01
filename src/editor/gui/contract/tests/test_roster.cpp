// The built-in roster + panel-manifest tests (M9 e05b + editor-UX c2, R-EDIT-001 / design 04 §3 +
// 04 §2): the single global roster registers cleanly under deny-by-default, every built-in declares a
// well-formed v3 manifest, and the v2 invariants (content/state/dock/commands/capabilities) plus the
// v3 ones (instances/path/selection/events) are each REFUSED when violated — happy path, edge cases,
// and failure paths (R-QA-013).

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/registry.h"
#include "context/editor/gui/contract/sandbox.h"

#include "context/editor/bridge/scope.h"

#include "contract_test.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace context::editor::gui::contract;

namespace
{

// A minimal, valid v3 panel manifest — the base every failure-path case below mutates ONE field of.
// `package_id` is left EMPTY, i.e. a BUILT-IN: the failure paths that turn on being a package set it
// explicitly, so the difference between the two is always visible at the case that depends on it.
Contribution valid_panel(std::string id)
{
    Contribution c;
    c.id = std::move(id);
    c.kind = ContributionKind::panel;
    c.title = "Panel";
    // contract_version, sandbox, content (uitree), state (v1), dock, instances (singleton) and path
    // (empty = top level) all default to valid values.
    return c;
}

// The same, declared BY A PACKAGE — the form every namespacing rule is stated against.
Contribution valid_package_panel(const std::string& package_id)
{
    Contribution c = valid_panel(package_id + ".panel");
    c.package_id = package_id;
    return c;
}

} // namespace

int main()
{
    // --- the contract major is v3 (the editor-UX c2 manifest break) ------------------------------
    {
        CHECK(kContractMajor == 3);
        // A default-constructed Contribution declares the CURRENT major, so every in-repo caller that
        // does not set contract_version explicitly moved with the bump.
        CHECK(Contribution{}.contract_version == kContractMajor);
    }

    // --- deny: every PAST major — the BREAKING half of the 1 -> 2 and 2 -> 3 bumps ----------------
    // The compatibility window is exactly {kContractMajor}, so an OLDER contribution is REFUSED, never
    // leniently adopted. Written with LITERALS on purpose: every other version case below mutates
    // `kContractMajor + 1`, so relaxing the registry's `!=` into a `> kContractMajor` ("be lenient with
    // older majors" — exactly the refactor a major bump invites) would keep all of them green while
    // silently re-admitting both.
    //
    // ⚠ `2` IS THE ONE THAT EARNS ITS KEEP HERE, and it is the DoD's own case: it was legal yesterday,
    // so it is the value a leniency relaxation would re-admit FIRST and the only one a
    // "just the previous major" carve-out would target. `1` is kept beside it because the two together
    // say "no past major", which is the actual rule, rather than "not the last one".
    for (const std::uint32_t past : {1u, 2u})
    {
        ExtensionRegistry reg;
        Contribution old_panel = valid_panel("legacy.past-panel");
        old_panel.contract_version = past;
        const RegistrationResult r = reg.register_contribution(old_panel);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrUnsupportedContractVersion);
        CHECK(reg.size() == 0);
    }

    // --- the roster is non-empty, duplicate-free, and REGISTERS under deny-by-default -------------
    {
        bool all_ok = false;
        const ExtensionRegistry registry = make_builtin_registry(&all_ok);
        CHECK(all_ok); // every built-in satisfies every contract invariant it is itself subject to
        CHECK(!builtin_contributions().empty());
        CHECK(registry.size() == builtin_contributions().size());

        std::set<std::string> ids;
        for (const Contribution& c : builtin_contributions())
        {
            CHECK(ids.insert(c.id).second); // a duplicate roster id is itself a defect
            CHECK(!c.id.empty());
            CHECK(!c.title.empty());
            CHECK(c.contract_version == kContractMajor);
            // Every built-in is either a headless uitree panel (its C++ model IS the content) or —
            // since M9 e06d — a `local` panel editor-core renders itself. NEVER an iframe: a built-in
            // loading third-party content into the trusted zone would be a contradiction in terms,
            // and that is the assertion this line is really making.
            CHECK(c.content.type == ContentType::uitree || c.content.type == ContentType::local);
            // Either way there is nothing to LOAD: the model is the content, wherever it lives.
            CHECK(c.content.entry.empty());
            // D6: every panel declares a state schema version (>= 1) — "state contract on EVERY panel".
            CHECK(c.state.schema_version >= 1);
            CHECK(c.dock.min_width >= 0 && c.dock.min_height >= 0);
            // Manifest v3: every built-in declares a coherent `instances` block and a well-formed
            // `path`. Asserted through `manifest_defect` — the SAME verdict registration uses — rather
            // than by re-listing the rules here, so a roster entry can never satisfy a paraphrase of a
            // rule while failing the rule.
            CHECK(manifest_defect(c).empty());
            // A built-in is the EDITOR'S OWN contribution: `package_id` empty is what buys it the
            // right to name an unnamespaced contract-owned selection subject, and a roster entry that
            // claimed a package would silently acquire a namespacing rule nobody wrote it against.
            CHECK(c.package_id.empty());
            CHECK(sandbox_conformant(c.sandbox));
            // Every declared capability is on the closed vocabulary.
            for (const std::string& cap : c.capabilities)
            {
                CHECK(capability_supported(cap));
            }
            // Each is findable in the roster registry by id.
            CHECK(registry.find(c.id) != nullptr);
        }

        // A-F2: the session-undo surface is ON the roster (it was absent from BOTH anchors pre-e05b).
        CHECK(ids.count("builtin.session.undo") == 1);
        // The panels the M5 exit gate names are still there after the promotion — MINUS the docked
        // playbar, AMENDED OUT by editor-window-chrome e1 (D2): the d1 titlebar strip is the Play
        // Bar's only home, so `builtin.playbar` left the roster (with its a11y + help anchors).
        for (const char* id : {"placeholder", "builtin.scene-tree", "builtin.inspector",
                               "builtin.viewport", "builtin.problems"})
        {
            CHECK(ids.count(id) == 1);
        }
        // The retirement itself, pinned: re-adding the dock panel is a reviewed roster change, not a
        // drift this gate would wave through.
        CHECK(ids.count("builtin.playbar") == 0);

        // --- the v3 roster migration, pinned at both ends ----------------------------------------
        // `builtin.viewport` was the ONE entry declared non-singleton under v2 (`singleton: false`),
        // and `unlimited` is that same statement in the v3 vocabulary. Asserting it here — rather than
        // only that every entry is well-formed — is what makes the migration falsifiable: a sweep that
        // mapped every entry to `singleton` would satisfy every other assertion in this block.
        const Contribution* viewport = registry.find("builtin.viewport");
        CHECK(viewport != nullptr);
        if (viewport != nullptr)
        {
            CHECK(viewport->instances.mode == InstanceMode::unlimited);
            CHECK(viewport->instances.max == 0); // `max` is meaningful only for `limited`
        }
        const Contribution* inspector = registry.find("builtin.inspector");
        CHECK(inspector != nullptr);
        if (inspector != nullptr)
        {
            CHECK(inspector->instances.mode == InstanceMode::singleton);
        }
        // EVERY entry gained a `path` (d1's Window-menu grouping), and at least one is NESTED — the
        // multi-segment form is the only one that exercises the separator, so a roster of single
        // segments would leave `path_defect`'s segment walk unproven by the roster itself.
        bool any_nested = false;
        for (const Contribution& c : builtin_contributions())
        {
            CHECK(!c.path.empty());
            any_nested = any_nested || c.path.find('/') != std::string::npos;
        }
        CHECK(any_nested);
    }

    // --- the roster is a stable, shared instance (built once, same order every call) --------------
    {
        // Consumers walk this by reference (a11y registered_panels(), the CEF host) and the coverage
        // ctest pins its ORDER, so assert real endpoints rather than that a reference equals itself.
        const std::vector<Contribution>& roster = builtin_contributions();
        CHECK(!roster.empty());
        CHECK(roster.front().id == "placeholder");
        CHECK(roster.back().id == "builtin.settings"); // M9 e06d appended the one `local` panel
        // The A-F2 promotion is still on the roster, just no longer at its tail.
        CHECK(std::any_of(roster.begin(), roster.end(), [](const Contribution& c)
                          { return c.id == "builtin.session.undo"; }));
    }

    // --- manifest v2 accepts a fully-populated iframe contribution --------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("ext.hello-panel");
        c.icon = "sparkle";
        c.dock.default_zone = DockZone::right;
        c.dock.min_width = 280;
        c.dock.min_height = 200;
        c.package_id = "hello";
        c.instances.mode = InstanceMode::limited;
        c.instances.max = 4;
        c.path = "Scene/Debug";
        c.selection.subjects = {"hello.tile"};
        c.events.publishes = {"hello.brush"};
        c.events.subscribes = {"other.thing"};
        c.content.type = ContentType::iframe;
        c.content.entry = "context-ext://hello/panel.html";
        c.state.schema_version = 3;
        c.capabilities = {kCapabilityReadQuery, kCapabilityUiEvents};
        c.commands = {CommandContribution{"hello.greet", "Greet", "panelFocus == hello"},
                      CommandContribution{"hello.wave", "Wave", ""}};
        c.themes = {"themes/hello.theme.json"};
        CHECK(reg.register_contribution(c).ok);

        const Contribution* got = reg.find("ext.hello-panel");
        CHECK(got != nullptr);
        CHECK(got->content.type == ContentType::iframe);
        CHECK(got->content.entry == "context-ext://hello/panel.html");
        CHECK(got->state.schema_version == 3);
        CHECK(got->dock.default_zone == DockZone::right);
        CHECK(got->instances.mode == InstanceMode::limited);
        CHECK(got->instances.max == 4);
        CHECK(got->path == "Scene/Debug");
        CHECK(got->selection.subjects.size() == 1);
        CHECK(got->events.publishes.size() == 1);
        CHECK(got->events.subscribes.size() == 1);
        CHECK(got->capabilities.size() == 2);
        CHECK(got->commands.size() == 2);
        CHECK(got->commands[0].when == "panelFocus == hello");
        CHECK(got->themes.size() == 1);
        CHECK(got->icon == "sparkle");
    }

    // --- deny: an iframe contribution with no entry URL -------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.iframe-no-entry");
        c.content.type = ContentType::iframe; // entry left empty
        const RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrInvalidManifest);
        CHECK(reg.size() == 0); // registry unchanged on refusal
    }

    // --- deny: a uitree contribution that ALSO names an entry URL ---------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.uitree-with-entry");
        c.content.entry = "context-ext://sneaky/panel.html";
        const RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrInvalidManifest);
    }

    // --- deny: a zero state schema version (a persisted D6 blob could never match) -----------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.state-v0");
        c.state.schema_version = 0;
        CHECK(reg.register_contribution(c).error_code == kErrInvalidManifest);
    }

    // --- deny: a negative dock minimum size --------------------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.dock");
        c.dock.min_height = -1;
        CHECK(reg.register_contribution(c).error_code == kErrInvalidManifest);
    }

    // ============================================================================================
    // MANIFEST v3 (editor-UX c2, 04 §2) — instances, path, selection.subjects, events
    // ============================================================================================
    // Every case below asserts the DIAGNOSTIC as well as the refusal, per the DoD: a refusal a package
    // author cannot read is a refusal they will work around by guessing.

    // --- deny: `limited` without a positive ceiling ------------------------------------------------
    // `max` is what `limited` MEANS, so a `limited` with none has not said what it asked for. Zero and
    // negative are the same fault as absent — the field's own "0 = unstated" contract.
    for (const int bad_max : {0, -1, -2147483647})
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.limited");
        c.instances.mode = InstanceMode::limited;
        c.instances.max = bad_max;
        const RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrInvalidManifest);
        CHECK(r.message.find("positive maximum") != std::string::npos);
        CHECK(reg.size() == 0);
    }

    // --- deny: `max` on a mode that cannot use it --------------------------------------------------
    // REFUSED rather than ignored, which is the whole difference: a silently dropped ceiling leaves a
    // manifest claiming a limit the editor never applies, and nothing anywhere would say so.
    for (const InstanceMode mode : {InstanceMode::singleton, InstanceMode::unlimited})
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.stray-max");
        c.instances.mode = mode;
        c.instances.max = 4;
        const RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrInvalidManifest);
        CHECK(r.message.find("only for") != std::string::npos);
    }

    // --- allow: each of the three modes, correctly stated ------------------------------------------
    // THE NON-VACUITY SIBLING for the two blocks above: without it they would pass just as well if the
    // registry refused every `instances` block it was shown.
    {
        ExtensionRegistry reg;
        Contribution single = valid_panel("ok.single");
        single.instances.mode = InstanceMode::singleton;
        CHECK(reg.register_contribution(single).ok);

        Contribution limited = valid_panel("ok.limited");
        limited.instances.mode = InstanceMode::limited;
        limited.instances.max = 1; // the smallest legal ceiling
        CHECK(reg.register_contribution(limited).ok);

        Contribution unlimited = valid_panel("ok.unlimited");
        unlimited.instances.mode = InstanceMode::unlimited;
        CHECK(reg.register_contribution(unlimited).ok);
        CHECK(reg.size() == 3);
    }

    // --- deny: a malformed `path` ------------------------------------------------------------------
    {
        struct PathCase
        {
            const char* path;
            const char* mention;
        };
        const PathCase cases[] = {
            {"/Scene", "leading or trailing"},
            {"Scene/", "leading or trailing"},
            {"/", "leading or trailing"},
            {"Scene//Debug", "empty segment"},
            {"Scene/ /Debug", "empty segment"},
        };
        for (const PathCase& tc : cases)
        {
            ExtensionRegistry reg;
            Contribution c = valid_panel("bad.path");
            c.path = tc.path;
            const RegistrationResult r = reg.register_contribution(c);
            CHECK(!r.ok);
            CHECK(r.error_code == kErrInvalidManifest);
            CHECK(r.message.find(tc.mention) != std::string::npos);
        }
    }

    // --- allow: the `path` shapes a menu actually needs --------------------------------------------
    // Including the EMPTY one, which means top level and must not be confused with "unset and
    // therefore suspect" — and a nested one, since a single-segment-only allowance would leave the
    // separator walk unproven.
    {
        ExtensionRegistry reg;
        int n = 0;
        for (const char* good : {"", "Scene", "Scene/Debug", "Scene/Debug/Tiles", "Scene 2D"})
        {
            Contribution c = valid_panel("ok.path-" + std::to_string(n++));
            c.path = good;
            CHECK(reg.register_contribution(c).ok);
        }
        CHECK(reg.size() == 5);
    }

    // --- deny: a PACKAGE naming an unnamespaced selection subject ----------------------------------
    // THE D2 CASE, and the sharpest in this file: `entity` is a real contract-owned subject kind
    // (editorkernel's kSelectionSubjectEntity), so admitting it would let a third-party package speak
    // for the editor's own selection vocabulary — the same shadowing hazard the package store's id
    // rule (a) exists to refuse, one member over.
    {
        struct SubjectCase
        {
            const char* subject;
            const char* mention;
        };
        const SubjectCase cases[] = {
            {"entity", "namespaced"},     // a contract-owned kind
            {"tile", "namespaced"},       // any bare name
            {"acme", "namespaced"},       // the bare package id is a namespace, not a member of it
            {"other.tile", "namespaced"}, // another package's namespace
            {"acmex.tile", "namespaced"}, // a PREFIX that is not a segment boundary
            {"editor.thing", "reserved"}, // the editor's own namespace
            {"Acme.Tile", "lowercase"},   // the grammar, not just the prefix
            {"acme..tile", "lowercase"},  // an empty segment
        };
        for (const SubjectCase& tc : cases)
        {
            ExtensionRegistry reg;
            Contribution c = valid_package_panel("acme");
            c.selection.subjects = {tc.subject};
            const RegistrationResult r = reg.register_contribution(c);
            CHECK(!r.ok);
            CHECK(r.error_code == kErrInvalidManifest);
            CHECK(r.message.find("selection.subjects") != std::string::npos);
            CHECK(r.message.find(tc.mention) != std::string::npos);
        }
    }

    // --- allow: a BUILT-IN's unnamespaced, contract-owned subject ----------------------------------
    // THE OTHER HALF OF THE SAME RULE, and it is what makes the block above a NAMESPACING test rather
    // than a "reject the word `entity`" test: the identical string is accepted from the editor itself.
    // `package_id` empty is the whole difference.
    {
        ExtensionRegistry reg;
        Contribution builtin = valid_panel("builtin.scene-tree-ish");
        CHECK(builtin.package_id.empty());
        builtin.selection.subjects = {"entity", "file", "asset"};
        CHECK(reg.register_contribution(builtin).ok);

        // ...and a package naming ITS OWN subject is accepted too, so the package path is not simply
        // closed.
        Contribution package_panel = valid_package_panel("acme");
        package_panel.selection.subjects = {"acme.tile", "acme.brush-stroke"};
        CHECK(reg.register_contribution(package_panel).ok);
    }

    // --- deny: a duplicated declared name ----------------------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_package_panel("acme");
        c.selection.subjects = {"acme.tile", "acme.tile"};
        const RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.message.find("twice") != std::string::npos);
    }

    // --- deny: a PACKAGE publishing outside its own namespace --------------------------------------
    // D4: `publishes` is a claim about the package's OWN vocabulary, so it is held to the same rule as
    // a selection subject.
    {
        for (const char* topic : {"brush", "other.brush", "editor.ui.focus"})
        {
            ExtensionRegistry reg;
            Contribution c = valid_package_panel("acme");
            c.events.publishes = {topic};
            const RegistrationResult r = reg.register_contribution(c);
            CHECK(!r.ok);
            CHECK(r.error_code == kErrInvalidManifest);
            CHECK(r.message.find("events.publishes") != std::string::npos);
        }
    }

    // --- subscribes is the ASYMMETRIC one, and both directions are pinned --------------------------
    // D4's cross-package subscription is the entire point of the member, so `subscribes` may name
    // ANOTHER package's topic. That makes "is it namespaced under the declarer" the WRONG rule here —
    // and the rule that is left is still a rule: namespaced under SOMEBODY, and never inside the
    // reserved editor namespace (D4 routes package facts through the daemon precisely so the closed
    // nine-member `editor.ui` set stays closed).
    {
        ExtensionRegistry reg;
        Contribution consumer = valid_package_panel("acme");
        consumer.events.subscribes = {"other.brush", "acme.own-echo"};
        CHECK(reg.register_contribution(consumer).ok);

        for (const char* bad : {"brush", "editor.ui.focus", "editor"})
        {
            ExtensionRegistry reg2;
            Contribution c = valid_package_panel("acme");
            c.events.subscribes = {bad};
            const RegistrationResult r = reg2.register_contribution(c);
            CHECK(!r.ok);
            CHECK(r.error_code == kErrInvalidManifest);
            CHECK(r.message.find("events.subscribes") != std::string::npos);
        }
    }

    // --- deny: a declaring package id that is not a usable NAMESPACE -------------------------------
    // Scoped to contributions that actually declare something: `shell::is_valid_package_id` admits `_`
    // while a topic segment may not, so a blanket check would newly refuse to INSTALL an
    // underscore-named package that declares no v3 names at all.
    {
        ExtensionRegistry reg;
        Contribution silent = valid_panel("my_pkg.panel");
        silent.package_id = "my_pkg";
        CHECK(reg.register_contribution(silent).ok); // declares nothing: unaffected

        Contribution declaring = valid_panel("my_pkg.other");
        declaring.package_id = "my_pkg";
        declaring.selection.subjects = {"my_pkg.tile"};
        const RegistrationResult r = reg.register_contribution(declaring);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrInvalidManifest);
        CHECK(r.message.find("not a valid namespace") != std::string::npos);
    }

    // --- deny: an empty id -------------------------------------------------------------------------
    {
        ExtensionRegistry reg;
        CHECK(reg.register_contribution(valid_panel("")).error_code == kErrInvalidManifest);
    }

    // --- deny: an empty or duplicated manifest command id ------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution empty_cmd = valid_panel("bad.command-empty");
        empty_cmd.commands = {CommandContribution{"", "No id", ""}};
        CHECK(reg.register_contribution(empty_cmd).error_code == kErrInvalidManifest);

        Contribution dup_cmd = valid_panel("bad.command-dup");
        dup_cmd.commands = {CommandContribution{"a.b", "First", ""},
                            CommandContribution{"a.b", "Second", ""}};
        CHECK(reg.register_contribution(dup_cmd).error_code == kErrInvalidManifest);
        CHECK(reg.size() == 0);
    }

    // --- deny: a capability outside the closed vocabulary ------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.capability");
        c.capabilities = {kCapabilityReadQuery, "root_access"};
        const RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrUnknownCapability);
        CHECK(reg.size() == 0);
        // An unknown capability is REFUSED, never silently dropped to a weaker grant.
        CHECK(!capability_supported("root_access"));
        CHECK(!capability_supported(""));
        CHECK(!capability_supported("READ_QUERY")); // the vocabulary is case-SENSITIVE
        CHECK(capability_supported(kCapabilityReadQuery));
        CHECK(capability_supported(kCapabilityFileWrite));
        CHECK(capability_supported(kCapabilitySessionControl));
        CHECK(capability_supported(kCapabilityBuildInstall));
        CHECK(capability_supported(kCapabilityUiEvents));
    }

    // --- deny: a bridge grant the manifest never declared (ambient privilege, R-SEC-007) -----------
    // `capabilities` is what a contribution ASKS for; `sandbox.granted_scopes` is what it HOLDS on the
    // bridge. A grant wider than the declaration fails OPEN — a reviewer reading the manifest would
    // never see it — so the registry refuses it for every non-baseline scope.
    {
        struct GrantCase
        {
            context::editor::bridge::Scope scope;
            const char* token;
        };
        const GrantCase cases[] = {
            {context::editor::bridge::Scope::file_write, kCapabilityFileWrite},
            {context::editor::bridge::Scope::session_control, kCapabilitySessionControl},
            {context::editor::bridge::Scope::build_install, kCapabilityBuildInstall},
        };

        for (const GrantCase& tc : cases)
        {
            ExtensionRegistry reg;
            Contribution c = valid_panel("bad.undeclared-grant");
            c.sandbox.granted_scopes.grant(tc.scope); // granted on the bridge, absent from the manifest
            const RegistrationResult r = reg.register_contribution(c);
            CHECK(!r.ok);
            CHECK(r.error_code == kErrInvalidManifest);
            CHECK(reg.size() == 0); // registry unchanged on refusal

            // Declaring the capability it is granted makes the SAME contribution legal.
            c.capabilities = {kCapabilityReadQuery, tc.token};
            CHECK(reg.register_contribution(c).ok);
        }
    }

    // --- allow: a declaration WIDER than the grant (asking is not holding) -------------------------
    {
        ExtensionRegistry reg;
        // Declares file_write + build_install but is granted neither: legal, and the shape every
        // built-in in the roster actually ships (the operator extends the grant, the manifest asks).
        Contribution asks_more = valid_panel("ext.asks-more");
        asks_more.capabilities = {kCapabilityReadQuery, kCapabilityFileWrite,
                                  kCapabilityBuildInstall};
        CHECK(reg.register_contribution(asks_more).ok);

        // read_query is the always-held R-SEC-007 baseline, so declaring NOTHING is still legal —
        // the rule must not turn an empty capability list into a refusal.
        Contribution silent = valid_panel("ext.declares-nothing");
        CHECK(silent.capabilities.empty());
        CHECK(silent.sandbox.granted_scopes.has(context::editor::bridge::Scope::read_query));
        CHECK(reg.register_contribution(silent).ok);
    }

    // --- refusal precedence: the two ORIGINAL invariants still report first ------------------------
    // (an existing caller's diagnostics are unchanged by the new manifest checks)
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.everything");
        c.contract_version = kContractMajor + 1;
        c.sandbox.node_integration = true;
        c.state.schema_version = 0;
        c.capabilities = {"root_access"};
        CHECK(reg.register_contribution(c).error_code == kErrUnsupportedContractVersion);

        c.contract_version = kContractMajor;
        CHECK(reg.register_contribution(c).error_code == kErrSandboxNonconformant);

        c.sandbox.node_integration = false;
        CHECK(reg.register_contribution(c).error_code == kErrInvalidManifest);

        c.state.schema_version = 1;
        CHECK(reg.register_contribution(c).error_code == kErrUnknownCapability);

        c.capabilities.clear();
        CHECK(reg.register_contribution(c).ok);
    }

    // --- the v2 token tables -----------------------------------------------------------------------
    {
        CHECK(std::string(dock_zone_token(DockZone::left)) == "left");
        CHECK(std::string(dock_zone_token(DockZone::right)) == "right");
        CHECK(std::string(dock_zone_token(DockZone::top)) == "top");
        CHECK(std::string(dock_zone_token(DockZone::bottom)) == "bottom");
        CHECK(std::string(dock_zone_token(DockZone::center)) == "center");
        CHECK(std::string(content_type_token(ContentType::uitree)) == "uitree");
        CHECK(std::string(content_type_token(ContentType::iframe)) == "iframe");
        CHECK(std::string(content_type_token(ContentType::local)) == "local");
        // The v3 instance-mode table. These three tokens are the manifest's spelling of the mode AND
        // the wire's (panel_host.cpp projects through this function, package_store.cpp inverts by
        // searching it), so a rename here moves both C++ ends at once — which is the reason no
        // second hand-written table exists on THIS side. Since editor-UX c3 the TS mirror
        // (`panels.ts`'s `PANEL_INSTANCE_MODES`) is compared against this very switch by
        // `webui-panel-contract`, so this assertion pins the C++ spelling and that gate pins the
        // agreement — the two together are what make a rename here a RED rather than a silent
        // collapse of every panel to `singleton`.
        CHECK(std::string(instance_mode_token(InstanceMode::singleton)) == "singleton");
        CHECK(std::string(instance_mode_token(InstanceMode::limited)) == "limited");
        CHECK(std::string(instance_mode_token(InstanceMode::unlimited)) == "unlimited");
        // kInstanceModes IS the vocabulary every inverse searches: if it and the token table ever
        // disagree in size, an inverse silently stops accepting a mode the projection still emits.
        CHECK(std::size(kInstanceModes) == 3u);
    }

    GUI_CONTRACT_TEST_MAIN_END();
}
