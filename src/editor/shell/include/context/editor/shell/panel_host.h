// The Shell-side PANEL HOST (M9 e05d1, design 04 §3-§4): the panel-agnostic seam that turns the
// e05b roster plus a set of headless C++ panel models into the `panel.*` bridge surface the
// hydration runtime calls.
//
// WHAT THIS IS NOT. It is not "the Problems panel's bridge". Nothing in this file, in panel_host.cpp,
// or in the TS hydration runtime knows a single panel id. The panel SET comes from the e05b roster
// (gui/contract/builtin_roster.h); the ABILITY to render one comes from a `PanelProvider` — a bundle
// of std::functions — bound at the app's composition root. Adding a panel is therefore a roster entry
// plus one provider binding, with ZERO change here or in the runtime. That property is not a nicety:
// e05d3 landed the Scene tree and Inspector by binding exactly two more providers, with zero edits
// here or in the runtime — the claim, cashed. `test_panel_host.cpp` asserts it directly, over
// synthetic panels this file has never heard of.
//
// WHY A std::function BUNDLE RATHER THAN AN INTERFACE PANELS IMPLEMENT. The D10 shell-boundary gate
// (src/CMakeLists.txt, `context_assert_shell_boundary`) forbids the EditorKernel's internal modules on
// the Shell's transitive link closure — and until e05d3, TWO panels violated it
// (`context_gui_panel_scenetree` / `context_gui_panel_inspector` PUBLIC-linked `context_compose`).
// If a panel had to implement an interface declared here, hosting one would mean linking it HERE,
// and this library would drag its closure across the boundary. A std::function is erased:
// `context_editor_shell` links only `context_gui_uitree` + `context_gui_contract` (both
// boundary-clean), the PANEL libraries are linked by the executable that binds their providers, and
// the gate's FORBIDDEN list stays byte-identical. e05d3 resolved the two violations by splitting the
// kernel-typed builders out (gui/panels/builders/, daemon-side) — the erased seam is what let it
// host both panels with zero edits here.
//
// THE ROSTER IS AUTHORITATIVE, THE PROVIDER TABLE IS CAPABILITY. Every rostered panel is LISTED;
// a rostered panel with no provider is listed as `hosted: false` — an honest "this build cannot
// render it yet", not a hidden entry. That is what lets the editor show its whole panel set while
// e05d3 is still in flight, and it is why `provide()` refuses an id that is not on the roster: a
// provider for an unrostered panel is a wiring bug, not a way to smuggle a panel past the roster.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/uitree/panel.h"
#include "context/editor/shell/ipc_bridge.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::shell
{

// --------------------------------------------------------------------------- the wire vocabulary
//
// Grep-stable, and MIRRORED by the TS side (src/editor/webui/core/src/panels.ts). The
// `webui-panel-contract` gate re-reads these values out of the BUILT bundle and compares them to the
// C++ constants here, the same cross-language discipline e05c's `webui-scheme-contract` applies to
// the scheme vocabulary — so a rename on either side reds a ctest instead of silently unbinding the
// panel surface at runtime.

inline constexpr const char* kPanelListMethod = "panel.list";
inline constexpr const char* kPanelRenderMethod = "panel.render";
inline constexpr const char* kPanelCommandMethod = "panel.command";
inline constexpr const char* kPanelGestureMethod = "panel.gesture";
inline constexpr const char* kPanelStateGetMethod = "panel.state.get";
inline constexpr const char* kPanelStateSetMethod = "panel.state.set";

// Refusal codes a panel method answers with. LOCAL codes (not R-CLI-008 catalog codes) — the same
// rationale gui/contract/registry.h states: these classify a HOST-side wiring or caller error, not a
// daemon-contract failure, and minting catalog codes for them would pollute the published surface.
inline constexpr const char* kErrPanelBadParams = "panel.bad_params";
inline constexpr const char* kErrPanelUnknown = "panel.unknown";
inline constexpr const char* kErrPanelNotHosted = "panel.not_hosted";
inline constexpr const char* kErrPanelUnknownCommand = "panel.unknown_command";
inline constexpr const char* kErrPanelBadGesture = "panel.bad_gesture";
inline constexpr const char* kErrPanelNoState = "panel.no_state";

// The continuous-gesture verbs (04 §4). A CLOSED vocabulary, deliberately: the C++ panel models were
// designed against exactly these four (`begin/extend/commit/cancel`), so a fifth verb invented in the
// renderer must be REFUSED rather than forwarded to a model that cannot mean anything by it.
enum class GestureVerb
{
    begin,
    extend,
    commit,
    cancel,
};

[[nodiscard]] const char* gesture_verb_token(GestureVerb verb);

// Parse a wire token. nullopt for anything outside the closed set above (including the empty string).
[[nodiscard]] std::optional<GestureVerb> parse_gesture_verb(std::string_view token);

// ------------------------------------------------------------------------------- the provider seam

// What a hosted panel supplies. NO panel type appears in this struct — see the file header on why
// that erasure is what keeps the D10 boundary intact.
//
// Only `build` is required. Everything else is genuinely OPTIONAL and its absence is a REPORTED
// capability rather than a runtime failure: a read-only observer like Problems has no gestures and no
// persisted state, and it must be able to say so instead of being forced to supply do-nothing stubs
// that make `panel.gesture` look supported.
struct PanelProvider
{
    // Build the panel's CURRENT uitree. Called on every `panel.render`, so it must be cheap and
    // deterministic: identical model state must produce an identical tree, or the hydration
    // runtime's id-keyed incremental patch degrades into a full replace on every poll.
    std::function<gui::uitree::Panel()> build;

    // Dispatch a command the uitree bound to a node. Returns false when the panel declined it (an
    // unknown row, a non-navigable target); the caller reports that as `dispatched: false`, NOT as an
    // error — a click on a dead row is an ordinary outcome, not a protocol fault.
    std::function<bool(const std::string& command_id, const contract::Json& params)> invoke;

    // Continuous gestures (04 §4). Empty => the panel reports `gestures: false` and `panel.gesture`
    // refuses with kErrPanelBadGesture rather than silently succeeding.
    std::function<bool(GestureVerb verb, const contract::Json& params)> gesture;

    // The D6 state contract (04 §3). Both or neither in practice; each is checked independently so a
    // half-wired provider degrades honestly instead of crashing. The blob is OPAQUE to the host — it
    // is persisted and handed back verbatim, never interpreted (panel_state.h states the rule).
    std::function<contract::Json()> get_state;
    std::function<bool(const contract::Json& data)> restore_state;
};

// One rendered panel, as the hydration runtime receives it.
struct PanelRender
{
    std::string panel_id;
    // Bumped on every model-visible change. The runtime re-renders only when this moves, which is
    // what makes an idle editor cost nothing.
    std::uint64_t revision = 0;
    // The semantic HTML from uitree::render_html — every interpolation already through the C-F6
    // escaping contract (node.h). The strict no-inline-script CSP is the backstop, not the control.
    //
    // DELIBERATELY THE ONLY TREE ON THE WIRE. An obvious alternative is to also send a structured
    // node array (id/role/label/text per node) for the runtime to diff against. That would be a
    // SECOND serialization of the same tree, free to drift from render_html's — and it is
    // unnecessary: render_html already emits `id`, `role`, `aria-label`, `tabindex` and
    // `data-command` on every node, so the runtime parses the incoming document and diffs it against
    // the mounted one by id. One tree, one format, no drift.
    std::string html;
    // The keyboard focus order (uitree::focus_order): node ids in depth-first document order. Sent
    // even though it is derivable from the HTML, because the MODEL is its authority — the runtime
    // must follow the panel's declared order rather than re-deriving one and disagreeing.
    std::vector<std::string> focus_order;
    // The commands the panel exposes. The runtime checks a node's `data-command` against this set
    // before dispatching, so a stale mounted DOM cannot dispatch a command the model has since
    // dropped — the same reachability invariant uitree::audit_a11y asserts, enforced at the seam.
    std::vector<gui::uitree::Command> commands;
};

// ---------------------------------------------------------------------------------- the host

class PanelHost
{
public:
    // Built over the e05b roster. The default constructor takes `gui::contract::builtin_contributions()`;
    // the explicit overload exists for tests, which supply synthetic rosters this file has never seen —
    // which is precisely how the panel-agnosticism claim is asserted rather than asserted-by-comment.
    PanelHost();
    explicit PanelHost(std::vector<gui::contract::Contribution> roster);

    // Non-copyable and non-movable, for the same reason BridgeRouter is: `install` binds handlers
    // that capture `this`, and a router outlives nothing that could be relocated out from under them.
    PanelHost(const PanelHost&) = delete;
    PanelHost& operator=(const PanelHost&) = delete;
    PanelHost(PanelHost&&) = delete;
    PanelHost& operator=(PanelHost&&) = delete;

    // Bind a provider. Returns false — and binds NOTHING — when `panel_id` is not on the roster, is
    // already bound, or when `provider.build` is null (a provider that cannot render is not a
    // provider). CHECKED at every call site: a silently dropped binding presents later as a panel
    // that mysteriously reports `hosted: false`.
    [[nodiscard]] bool provide(const std::string& panel_id, PanelProvider provider);

    [[nodiscard]] bool knows(const std::string& panel_id) const;
    [[nodiscard]] bool hosts(const std::string& panel_id) const;
    [[nodiscard]] std::size_t roster_size() const { return roster_.size(); }
    [[nodiscard]] std::size_t hosted_count() const;

    // Mark a panel's model as changed from OUTSIDE a bridge call — the live-feed path (a daemon
    // event advanced the diagnostics set). Bumps the revision so the next `panel.render` is seen as
    // fresh. A no-op for an unknown id, so a feed does not have to guard.
    void touch(const std::string& panel_id);

    [[nodiscard]] std::uint64_t revision(const std::string& panel_id) const;

    // --- the method bodies, exposed for direct testing ------------------------------------------
    // Each is total over arbitrary input (the bridge hands them renderer-controlled params) and each
    // is what the corresponding `panel.*` handler calls, so the T1 suite exercises the SAME code the
    // renderer reaches rather than a lookalike.

    // The manifest-v2 projection of the WHOLE roster (hosted or not).
    [[nodiscard]] contract::Json list() const;

    // Render one panel. nullopt when it is unknown or not hosted; `error_code` says which.
    [[nodiscard]] std::optional<PanelRender> render(const std::string& panel_id,
                                                    std::string& error_code) const;

    // Dispatch a bound command. `dispatched` is the panel's own verdict; the return value is whether
    // the CALL was well-formed (false => `error_code` is set and nothing was dispatched).
    [[nodiscard]] bool invoke(const std::string& panel_id, const std::string& command_id,
                              const contract::Json& params, bool& dispatched,
                              std::string& error_code);

    // Dispatch a gesture verb. Same contract as `invoke`.
    [[nodiscard]] bool gesture(const std::string& panel_id, GestureVerb verb,
                               const contract::Json& params, bool& dispatched,
                               std::string& error_code);

    // D6 persist. nullopt when the panel is unknown / not hosted / declares no state.
    [[nodiscard]] std::optional<contract::Json> get_state(const std::string& panel_id,
                                                          std::string& error_code) const;

    // D6 restore. NEVER an error for a version mismatch: that is the documented degrade path — the
    // panel receives NO state and the caller surfaces `diagnostic`. `restored` reports which
    // happened; the return value is again only about the call being well-formed.
    [[nodiscard]] bool restore_state(const std::string& panel_id, const contract::Json& persisted,
                                     bool& restored, std::string& code, std::string& diagnostic,
                                     std::string& error_code);

    // Bind every `panel.*` method on `router`. False when ANY binding was refused (a name collision
    // with something already registered), which is a wiring bug the caller must not ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // --- what it saw (the live-smoke assertion surface) -------------------------------------------
    // `editor-cef-smoke-shell` asserts these are non-zero after the real renderer boots, which is the
    // end-to-end proof that the LIVE hydration runtime actually called this host — a claim no local
    // test can make and no counter-free design can support.
    //
    // WHAT EACH ONE COUNTS, precisely, so a reader does not over-read them. `lists_served` and
    // `renders_served` are incremented by their bridge HANDLERS, so they count wire calls only —
    // and those two are the ones `editor-cef-smoke-shell` actually asserts.
    // `commands_dispatched` is incremented in `invoke`, so it counts every command that REACHED a
    // provider — over the wire or by a direct call — including one the provider then DECLINED. It
    // is a reachability signal, NOT a count of commands the model acted on; `Entry::revision`,
    // which moves only on an accepted command, is the signal for that. Its reader is the unit test
    // (`test_panel_host.cpp`), not the smoke.
    [[nodiscard]] std::size_t lists_served() const { return lists_served_; }
    [[nodiscard]] std::size_t renders_served() const { return renders_served_; }
    [[nodiscard]] std::size_t commands_dispatched() const { return commands_dispatched_; }

private:
    struct Entry
    {
        gui::contract::Contribution manifest;
        PanelProvider provider;
        bool hosted = false;
        std::uint64_t revision = 1; // 1, not 0: "never rendered" is distinguishable from "revision 0"
    };

    [[nodiscard]] Entry* find(const std::string& panel_id);
    [[nodiscard]] const Entry* find(const std::string& panel_id) const;
    // Resolve to a HOSTED entry, setting `error_code` to unknown/not-hosted on failure.
    [[nodiscard]] Entry* resolve_hosted(const std::string& panel_id, std::string& error_code);
    [[nodiscard]] const Entry* resolve_hosted(const std::string& panel_id,
                                              std::string& error_code) const;

    std::vector<Entry> roster_;
    std::size_t lists_served_ = 0;
    std::size_t renders_served_ = 0;
    std::size_t commands_dispatched_ = 0;
};

} // namespace context::editor::shell
