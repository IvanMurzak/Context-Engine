// THE PACKAGE FACT BUS, the SHELL's half (editor-UX d2, D4 + D5; design 02 §C / 05 §3 / 08 §2).
//
// WHAT THIS IS. The answer to "how do independent packages work together without depending on each
// other". Package A declares a topic in its own manifest and publishes a FACT onto it; package B
// declares an interest in that topic and, once the operator has consented, receives every value.
// Neither names the other's code, neither loads the other, and either can be uninstalled without
// the other noticing more than a topic that stopped moving.
//
// ⚠ THE MECHANISM IS SPLIT ACROSS TWO FILES ON PURPOSE, AND THE SPLIT IS "WHO HAS READ A MANIFEST".
//
//   * `bridge/event_stream.h` (the DAEMON) owns what a daemon can know: the topic grammar, the
//     registry of declared topics, the D5 last-value retention + dedup, snapshot-on-subscribe, the
//     reentrancy refusal, and the bounds. It has never read a manifest and never will, so it cannot
//     answer "was that package entitled to that topic".
//   * THIS FILE (the SHELL) owns exactly that question, because the store scan and the operator's
//     consent document are here and nowhere else. It answers it twice — once for PUBLISHING (did
//     the package declare the topic in `events.publishes[]`?) and once for SUBSCRIBING (did it
//     declare the topic in `events.subscribes[]` AND was it consented to?).
//
// Both halves are real controls and neither is redundant: strip the Shell and any client could
// publish on any registered topic; strip the daemon and a package could publish a `session` fact by
// spelling one. That is the "both controls, independently" discipline `package_sessions.h`
// control 2 states, applied to a second surface.
//
// ================================ THE FOUR DECISIONS ================================
//
//  1. THE PUBLISH VERB IS A ROUTE OF ITS OWN, NOT AN ALLOWLIST ENTRY. `panel.facts.publish` is a
//     first-class Shell router method rather than `events.publish` added to
//     `panel_callable_daemon_methods()`. The difference is the whole security content of D4:
//     `bridge.call` forwards its `method` AND its `params` VERBATIM (panelverbs.ts says so in as
//     many words), so an allowlist entry would let a panel publish on ANY topic the daemon had
//     registered — including another package's — and the declaration check below would be an
//     adapter a caller simply routes around. R-SEC-007's "adapters are bypassable" applies to our
//     own adapters too. So the allowlist stays closed, this is the one door, and the check stands
//     on it.
//
//  2. THE GRANT RIDES THE EXISTING CONSENT MACHINERY, WITH ONE NEW TOKEN AND NO NEW DOCUMENT.
//     Subscribing to another package's topic requires `package_events` (extension.h) in the
//     `~/.context/package-grants.json` document `package_grants.h` already owns — the file no
//     package can write, already clamped to what the manifest declared, already fail-closed in
//     every direction, already snapshot-at-boot. A per-topic grant vocabulary was rejected: it
//     would put package-CHOSEN strings into the closed capability set that `capability_supported`
//     exists to keep closed. The per-topic answer comes from the MANIFEST instead
//     (`events.subscribes[]`), which the registry already forces to be namespaced under somebody.
//     TWO CLAMPS, and both are load-bearing — the token alone would let a consented package
//     subscribe to a topic it never declared an interest in, and the manifest alone would make
//     consent decorative.
//
//  3. A PACKAGE ALWAYS RECEIVES ITS OWN TOPICS, WITH NO GRANT AND NO PROMPT. Its own fact is not
//     somebody else's data, and prompting for it would train the operator to dismiss the prompt —
//     the reasoning `pending_consent_requests` already applies to a package that declares no
//     capabilities. It also makes the common single-package case (a panel that publishes and reads
//     back its own state through the bus) work at the deny-all baseline.
//
//  4. DECLARATION IS CHECKED AGAINST THE **SCAN**, NEVER AGAINST THE REQUEST. The `packageId` this
//     host is handed comes from editor-core's per-panel binding (the S2 residual
//     `package_sessions.h` records, unchanged here), and the topic list comes from the manifest the
//     store scan validated. Nothing a panel sends can widen either. The consequence worth stating:
//     an editor-core that mis-attributed a panel could publish under the wrong package — bounded,
//     because it could still only publish topics THAT package declared, so a mis-attribution
//     remains a mis-attribution and never becomes an escalation.
//
// ⚠ WHAT THIS FILE DELIBERATELY DOES NOT DO.
//   * IT DOES NOT ADD AN `editor.ui` TOPIC. Package facts are daemon facts precisely so the CLI, an
//     agent and a second window can see them; the built-in `editor.ui` set stays closed at nine and
//     `packageui.ts`'s refusal of cross-package `editor.ui` subscription STAYS. D4 answers that
//     refusal's security reason with a grant instead of removing the check.
//   * IT DOES NOT ADD THE BOUNDARY CHECKER'S DENY-LIST ENTRY for `panel.facts.publish`. That was
//     `f1`, deliberately sequenced after this task — and it is why `check_ui_bus_boundary.py` had
//     TWO daemon-reaching methods to name rather than one. LANDED: rule 3 of that checker now denies
//     this method (and `panel.daemon.call`) to any module that implements `UiMirrorSink` or calls
//     `attachMirror(`, so a cross-window mirror sink written where the mirror lives can no longer be
//     pointed at this verb. A compliant panel-surface caller bears no sink and is untouched. The
//     scope is a MODULE, so a sink object built one module out — naming neither the sink type nor
//     `attachMirror(` — is a named, pinned RESIDUAL, not coverage; see docs/editor-ui-bus.md § the
//     D7 boundary.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/package_grants.h"
#include "context/editor/shell/package_sessions.h"
#include "context/editor/shell/package_store.h"

#include <string>
#include <vector>

namespace context::editor::shell
{

// The router method editor-core publishes a package fact over.
//
// Params `{packageId, topic, payload}`; result the daemon's own `{topic, changed, seq}`. MIRRORED in
// `packagefacts.ts` (`PANEL_FACTS_PUBLISH_METHOD`) exactly as `kPanelDaemonCallMethod` and
// `kPanelEventsPollMethod` are, and byte-compared against the BUILT bundle by
// `tools/check_webui_assets.py --panel-contract`. A rename on either side leaves editor-core calling
// a method the Shell no longer routes: `PanelPortBridge` maps the `unknown_method` onto
// `verb_not_granted`, which is INDISTINGUISHABLE from "this build does not implement the fact bus",
// so every publishing package silently stops broadcasting with nothing reporting it.
inline constexpr const char* kPanelFactsPublishMethod = "panel.facts.publish";

/** `packageId` / `topic` missing, not a string, or not syntactically valid. */
inline constexpr const char* kErrFactsBadParams = "panel.facts.bad_params";
/**
 * The package did not declare `topic` in its manifest's `events.publishes[]` — D4's publish
 * authorization.
 *
 * DISTINCT FROM the daemon's `package.topic_undeclared`, and the pair is not redundant: this one
 * says "YOUR manifest does not claim that topic" (a file the author edits), the daemon's says "no
 * package registered it on this daemon" (a load/installation state). A package author who cannot
 * tell them apart edits the wrong file.
 */
inline constexpr const char* kErrFactsTopicNotDeclared = "panel.facts.topic_not_declared";
/**
 * The topic is not namespaced under the publishing package.
 *
 * ⚠ UNREACHABLE THROUGH AN INSTALLED PACKAGE TODAY, AND KEPT ANYWAY — stated plainly because a
 * reader will otherwise wonder. `manifest_defect` already refuses a `events.publishes` entry that is
 * not namespaced under its declaring package (registry.cpp § names_defect, `owned = true`), so a
 * topic that reaches decision 4's declaration check is namespaced by construction. This is the
 * second, independent statement of D4's namespacing rule at the point of USE, which is what keeps
 * it true if the scan ever gains a path that does not run `manifest_defect` — and it is drivable
 * today with a hand-built scan, which is how the suite proves it is a check and not a comment.
 */
inline constexpr const char* kErrFactsTopicNotNamespaced = "panel.facts.topic_not_namespaced";

// Serves `panel.facts.publish` and supplies `PackageSessionHost`'s control-6 policy.
//
// ⚠ THE SCAN AND THE GRANT HOST ARE HELD BY REFERENCE AND ARE THE COMPOSITION ROOT'S, exactly as
// `PackageGrantHost` holds its scan: `editor_main.cpp` scans the store once at boot and owns the
// result for the process lifetime, and copying either here would let the fact bus answer about
// packages the mount table no longer reflects — or about grants the running Shell has since
// recorded.
class PackageFactHost
{
public:
    PackageFactHost(PackageStoreScan& scan, PackageGrantHost& grants, PackageSessionHost& sessions);

    PackageFactHost(const PackageFactHost&) = delete;
    PackageFactHost& operator=(const PackageFactHost&) = delete;

    // Bind `panel.facts.publish` AND install control 6's policy onto the session host. False when
    // the binding was refused (a name collision, or the name landing on `forbidden_bridge_methods()`)
    // — a wiring bug the caller must treat as fatal.
    //
    // ⚠ THE POLICY IS INSTALLED HERE RATHER THAN IN THE CONSTRUCTOR so that a build which never
    // installs the route also never widens what packages receive: an editor with no fact route is
    // the deny-all editor, whole, rather than one whose delivery filter quietly opened.
    [[nodiscard]] bool install(BridgeRouter& router);

    // THE WHOLE DECISION PATH for `panel.facts.publish`, exposed so the suite drives it without a
    // router in the way (the shape `PackageSessionHost::forward` establishes).
    [[nodiscard]] BridgeResult publish(const std::string& package_id, const std::string& topic,
                                       const contract::Json& payload);

    // The topics `package_id` DECLARED it publishes — the union across its contributions,
    // deduplicated, in declaration order. EMPTY for an uninstalled package, which is what makes an
    // uninstalled package unable to publish anything.
    [[nodiscard]] std::vector<std::string> declared_publishes(const std::string& package_id) const;

    // The topics `package_id` declared an interest in (`events.subscribes[]`), same shape.
    [[nodiscard]] std::vector<std::string> declared_subscribes(const std::string& package_id) const;

    // Decision 2 + 3 — MAY `package_id` RECEIVE a fact on `topic`?
    //
    // TRUE for its own declared topic with no grant at all (decision 3); TRUE for another package's
    // only when `events.subscribes[]` names it AND the operator granted `package_events`. FALSE for
    // an uninstalled package, an unknown topic and every failure path — deny-by-default, in the
    // direction `ShellPackageGrants` and `PackageGrantStore` already fail in.
    [[nodiscard]] bool may_subscribe(const std::string& package_id, const std::string& topic) const;

    /** Publishes REFUSED by decision 4 (undeclared or mis-namespaced). Non-zero is a package bug. */
    [[nodiscard]] std::size_t refused_publishes() const { return refused_publishes_; }
    /** Publishes this host handed to a session (a D5 dedup counts — the daemon ACCEPTED it). */
    [[nodiscard]] std::size_t accepted_publishes() const { return accepted_publishes_; }

private:
    [[nodiscard]] const InstalledPackage* find_package(const std::string& package_id) const;

    PackageStoreScan& scan_;
    PackageGrantHost& grants_;
    PackageSessionHost& sessions_;
    std::size_t refused_publishes_ = 0;
    std::size_t accepted_publishes_ = 0;
};

} // namespace context::editor::shell
