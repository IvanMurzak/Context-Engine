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
// M9 editor-UX c3 — the ONE instance-lifecycle verb on the wire (D6 / design 04 section 3).
//
// WHY EXACTLY ONE, AND WHY IT IS `close` AND NOT `open`. Instance CREATION is implicit: the renderer
// owns panel lifecycle (panelhost.ts states it), it mints the instance id from the same rule this
// file publishes (`make_panel_instance_id`), and the first `panel.render` carrying that id
// materialises the model here. So an `open` verb would buy nothing but a round trip the renderer
// would have to await before it could add a Dockview panel — turning a synchronous `open()` into an
// async one across every caller.
//
// RELEASE cannot be implicit the same way, and that asymmetry is the whole reason this verb exists:
// nothing else on the wire says "this instance is gone". Without it the instance table would only
// ever grow, and a `limited` kind would exhaust its ceiling after `max` opens over the WHOLE session
// rather than holding `max` LIVE copies — a panel the user closed would keep occupying its slot
// forever, with nothing reporting why the next open is refused.
inline constexpr const char* kPanelInstanceCloseMethod = "panel.instance.close";

// The separator between a panel KIND and its instance ordinal in an instance id
// (`builtin.problems#1`). MIRRORED by the TS side (`PANEL_INSTANCE_SEPARATOR`, panels.ts) and
// byte-compared by `webui-panel-contract`, because both sides COMPOSE and DECOMPOSE ids with it: a
// drift makes the renderer mint ids the Shell parses to a different kind, so every instance resolves
// to the wrong panel (or to none) with no build error and no refusal that names the cause.
inline constexpr const char* kPanelInstanceSeparator = "#";

// The RESOURCE ceiling on live instances of ONE kind, distinct from the manifest's DECLARATIVE one.
//
// `instances.mode: "unlimited"` is a statement about the panel ("several scene views is the point"),
// not a licence for a renderer-controlled allocation with no bound. Instance ids arrive off the
// privileged bridge like every other parameter here, so "as many as are asked for" would let a
// compromised or merely buggy renderer mint models without limit. This is the backstop; it is far
// above any arrangement a human docks, so it never binds in practice.
inline constexpr std::size_t kMaxPanelInstances = 64;

// The longest instance id this host will accept. Same rationale as the file header's "an id that is
// 4 MiB of nul bytes": the id is a map key AND is echoed into diagnostics, so its length is bounded
// at the seam rather than trusted.
inline constexpr std::size_t kMaxPanelInstanceIdLength = 128;

// Refusal codes a panel method answers with. LOCAL codes (not R-CLI-008 catalog codes) — the same
// rationale gui/contract/registry.h states: these classify a HOST-side wiring or caller error, not a
// daemon-contract failure, and minting catalog codes for them would pollute the published surface.
inline constexpr const char* kErrPanelBadParams = "panel.bad_params";
inline constexpr const char* kErrPanelUnknown = "panel.unknown";
inline constexpr const char* kErrPanelNotHosted = "panel.not_hosted";
inline constexpr const char* kErrPanelUnknownCommand = "panel.unknown_command";
inline constexpr const char* kErrPanelBadGesture = "panel.bad_gesture";
inline constexpr const char* kErrPanelNoState = "panel.no_state";
// c3: the open (or lazy materialisation) would exceed what the manifest declares — or the resource
// ceiling above. Its diagnostic NAMES the limit, per design 04 section 3's open semantics.
inline constexpr const char* kErrPanelInstanceLimit = "panel.instance_limit";

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

// ------------------------------------------------------------------------- instance identity (c3)
//
// A panel's identity is the PAIR `(panel_id, instance_id)`: `panel_id` names the KIND, `instance_id`
// the live copy. The id is composed rather than opaque — `builtin.problems#1` — and that is
// load-bearing on the renderer side: Dockview restores a persisted arrangement by PANEL ID and calls
// `createComponent` for each one BEFORE editor-core has any chance to register it, so the kind has to
// be recoverable from the id alone or a restore cannot know which renderer to build. Both halves of
// the rule (compose / decompose) live here and are mirrored in `panels.ts` under the gated
// `PANEL_INSTANCE_SEPARATOR`.

// `panel_id` + the separator + `ordinal` (1-based). The ordinal is per KIND, never global, so the
// FIRST instance of every panel is `<id>#1` on every boot — which is what makes a persisted
// single-instance arrangement restore unchanged.
[[nodiscard]] std::string make_panel_instance_id(const std::string& panel_id, std::uint64_t ordinal);

// The KIND an instance id names, or the whole string when it carries no separator.
//
// Splits on the LAST separator, not the first: a panel id is free to contain one (nothing in the
// registry forbids it), and splitting on the first would resolve `a#b#1` to the kind `a` — a panel
// that does not exist — rather than to `a#b`, which does.
[[nodiscard]] std::string panel_id_of_instance(std::string_view instance_id);

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

// Build a provider for ONE instance of a kind (c3) — the multi-instance binding.
//
// THE DIFFERENCE FROM `provide()` IS WHERE THE MODEL LIVES, and it is the whole of D6's imperative
// half. `provide()` binds ONE provider that every instance of the kind shares, so two copies read and
// write the SAME model: correct for a `singleton`, and correct for a kind whose model is genuinely
// global (the Problems feed is one diagnostics set however many views of it are open). A kind whose
// copies must hold DIFFERENT state — a viewport with its own camera, an inspector pinned to another
// entity — binds this instead, and the host calls it once per instance so each copy gets a model of
// its own.
//
// The instance id is passed so a factory can label or key its model by it; it is NOT required to.
using PanelProviderFactory = std::function<PanelProvider(const std::string& instance_id)>;

// What an `open_instance` call did (design 04 section 3's open semantics).
enum class InstanceOutcome
{
    // A new live copy was minted.
    opened,
    // An existing copy answers this open. NOT a failure — a second open of a `singleton` is the
    // designed path, and reporting it as a refusal is what made `dock.singleton` feel broken.
    focused,
    // The manifest (or the resource ceiling) forbids another copy. `code` + `diagnostic` say which.
    refused,
};

// The result of one `open_instance`. `instance_id` is the live copy on `opened`/`focused`, empty on
// `refused`; `code`/`diagnostic` are empty unless refused.
struct InstanceOpen
{
    InstanceOutcome outcome = InstanceOutcome::refused;
    std::string instance_id;
    std::string code;
    std::string diagnostic;
};

// One rendered panel, as the hydration runtime receives it.
struct PanelRender
{
    std::string panel_id;
    // WHICH COPY this render is of (c3). Echoed back on the wire so a renderer holding several
    // instances of one kind can route the payload to the right DOM slot rather than to the first
    // one that matches the kind.
    std::string instance_id;
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

    // Bind ONE provider, SHARED by every instance of the kind. Returns false — and binds NOTHING —
    // when `panel_id` is not on the roster, is already bound, or when `provider.build` is null (a
    // provider that cannot render is not a provider). CHECKED at every call site: a silently dropped
    // binding presents later as a panel that mysteriously reports `hosted: false`.
    //
    // ⚠ SHARED IS THE WORD (c3). Every instance of this kind renders, invokes and persists through
    // the SAME model, so two copies are two VIEWS of one thing. That is exactly right for a
    // `singleton` (there is never a second copy) and for a kind whose model is genuinely global — the
    // Problems feed holds ONE diagnostics set no matter how many views of it exist. It is NOT right
    // for a kind whose copies must diverge; that binds `provide_factory` instead. The host does not
    // refuse a non-singleton binding here, deliberately: "one model, several views" is a legitimate
    // panel design, and refusing it would be the host deciding a question the panel owns.
    [[nodiscard]] bool provide(const std::string& panel_id, PanelProvider provider);

    // Bind a PER-INSTANCE provider factory (c3) -- one model per live copy. Same refusal rules as
    // `provide` (unrostered / already bound / cannot render), applied to a PROBE.
    //
    // ⚠ THE FACTORY IS CALLED ONCE AT BIND TIME, with an empty instance id, and the provider it
    // returns is DISCARDED. That probe is what lets `panel.list` answer `gestures` / `persists`
    // HONESTLY for a kind with no live copy yet -- and the roster is read at boot, BEFORE any panel
    // is opened, so a host that deferred the question would tell the renderer "no gestures" for
    // every multi-instance panel and the renderer would then never send one. It is also what makes
    // `build == nullptr` a bind-time refusal here exactly as it is for `provide`, rather than a
    // surprise at the first render.
    //
    // The contract this places on a factory: it must be safe to call for a throwaway model --
    // build state, register nothing global, take no lock nobody will release. Every factory in the
    // tree satisfies that by construction (they close over a feed and return std::functions).
    [[nodiscard]] bool provide_factory(const std::string& panel_id, PanelProviderFactory factory);

    [[nodiscard]] bool knows(const std::string& panel_id) const;
    [[nodiscard]] bool hosts(const std::string& panel_id) const;
    [[nodiscard]] std::size_t roster_size() const { return roster_.size(); }
    [[nodiscard]] std::size_t hosted_count() const;

    // Mark a panel's model as changed from OUTSIDE a bridge call — the live-feed path (a daemon
    // event advanced the diagnostics set). Bumps the revision so the next `panel.render` is seen as
    // fresh. A no-op for an unknown id, so a feed does not have to guard.
    void touch(const std::string& panel_id);

    [[nodiscard]] std::uint64_t revision(const std::string& panel_id) const;

    // --- the instance lifecycle (c3, design 04 section 3) ----------------------------------------

    // Open a live copy of `panel_id`, honouring its declared `instances.mode`:
    //   * `singleton`  — the second open FOCUSES the first (`focused`), never fails;
    //   * `limited`    — opens up to `max`; the `max + 1`th is `refused`, with the limit NAMED;
    //   * `unlimited`  — mints a new copy every time (up to `kMaxPanelInstances`).
    //
    // `requested_instance_id` is the RESTORE / REHOME channel: a persisted arrangement names the
    // exact ids it wants back, and a caller that passes one gets that id or an honest refusal —
    // never a silently different one, which would leave the restored state attached to a copy the
    // layout does not mention. Empty means "mint one".
    [[nodiscard]] InstanceOpen open_instance(const std::string& panel_id,
                                             const std::string& requested_instance_id = std::string());

    // Release one live copy and its model. False when the kind or the instance is unknown — an
    // ordinary outcome for a double close, never an error.
    [[nodiscard]] bool close_instance(const std::string& panel_id, const std::string& instance_id);

    // The live copies of `panel_id`, in open order. Empty for an unknown / unhosted / unopened kind.
    [[nodiscard]] std::vector<std::string> instances(const std::string& panel_id) const;


    // Which copy a call naming `instance_id` ADDRESSED — the id itself, or the kind's default
    // instance when it named none. What the wire echoes back, so a caller that sent no id still
    // learns which copy answered instead of receiving its own empty string.
    [[nodiscard]] std::string addressed_instance(const std::string& panel_id,
                                                 const std::string& instance_id) const;

    // --- the method bodies, exposed for direct testing ------------------------------------------
    // Each is total over arbitrary input (the bridge hands them renderer-controlled params) and each
    // is what the corresponding `panel.*` handler calls, so the T1 suite exercises the SAME code the
    // renderer reaches rather than a lookalike.

    // The manifest-v2 projection of the WHOLE roster (hosted or not).
    [[nodiscard]] contract::Json list() const;

    // --- THE INSTANCE PARAMETER, common to the five methods below (c3) ---------------------------
    //
    // `instance_id` is TRAILING and DEFAULTED, which is what makes this change additive rather than a
    // 55-call-site rewrite: every existing caller keeps meaning what it meant. Empty resolves to the
    // kind's DEFAULT instance -- the first live copy, or a freshly materialised one when none exists
    // yet -- so a single-instance panel behaves exactly as it did before the pair existed.
    //
    // A NON-EMPTY id that is not live is MATERIALISED, not refused, and that is deliberate: the
    // renderer owns panel lifecycle and has already decided this copy exists (it minted the id and
    // docked a slot for it), so the first call naming it is the moment its model is needed. The
    // manifest's ceiling is still enforced HERE -- a renderer cannot mint past what the panel
    // declares -- which is what keeps the wire's untrusted half honest.

    // Render one panel instance. nullopt when the kind is unknown / not hosted, or the instance
    // cannot exist; `error_code` says which.
    //
    // NOT `const` since c3, and the loss is real rather than cosmetic: rendering an instance the
    // renderer has just docked MATERIALISES its model, which is a mutation of the host. Marking it
    // const and hiding the instance table behind `mutable` would be the same mutation with the
    // signature lying about it.
    [[nodiscard]] std::optional<PanelRender>
    render(const std::string& panel_id, std::string& error_code,
           const std::string& instance_id = std::string());

    // Dispatch a bound command. `dispatched` is the panel's own verdict; the return value is whether
    // the CALL was well-formed (false => `error_code` is set and nothing was dispatched).
    [[nodiscard]] bool invoke(const std::string& panel_id, const std::string& command_id,
                              const contract::Json& params, bool& dispatched,
                              std::string& error_code,
                              const std::string& instance_id = std::string());

    // Dispatch a gesture verb. Same contract as `invoke`.
    [[nodiscard]] bool gesture(const std::string& panel_id, GestureVerb verb,
                               const contract::Json& params, bool& dispatched,
                               std::string& error_code,
                               const std::string& instance_id = std::string());

    // D6 persist, PER INSTANCE. nullopt when the panel is unknown / not hosted / declares no state.
    // Not `const` for the same reason `render` is not -- see there.
    [[nodiscard]] std::optional<contract::Json>
    get_state(const std::string& panel_id, std::string& error_code,
              const std::string& instance_id = std::string());

    // D6 restore. NEVER an error for a version mismatch: that is the documented degrade path — the
    // panel receives NO state and the caller surfaces `diagnostic`. `restored` reports which
    // happened; the return value is again only about the call being well-formed.
    [[nodiscard]] bool restore_state(const std::string& panel_id, const contract::Json& persisted,
                                     bool& restored, std::string& code, std::string& diagnostic,
                                     std::string& error_code,
                                     const std::string& instance_id = std::string());

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
    // One LIVE COPY of a kind: its id plus the model binding it renders through (c3). For a
    // `provide()`-bound kind every instance carries a COPY of the one shared provider -- the same
    // std::functions, so the same model; for a `provide_factory()`-bound kind each carries the
    // provider its own factory call produced.
    struct Instance
    {
        std::string id;
        PanelProvider provider;
    };

    struct Entry
    {
        gui::contract::Contribution manifest;
        // The shared-model binding (`provide`) -- or, for a factory-bound kind, the PROBE provider
        // `provide_factory` built (see there). Never a null `build` on a hosted entry either way, so
        // it is NOT the discriminator between the two bindings: `factory != nullptr` is, and that is
        // what `create_instance` branches on.
        PanelProvider provider;
        // The per-instance binding (`provide_factory`). Null when the kind is shared-model bound.
        PanelProviderFactory factory;
        bool hosted = false;
        // The binding's CAPABILITY shape, decided once at bind time and reported by `panel.list`.
        // Stored rather than re-derived so the roster's answer cannot depend on a LIVE instance --
        // asking one would make the answer flip under the renderer according to whether a copy
        // happens to be open. Deriving from `provider` above would agree TODAY (`bind` reads these
        // from the very provider it stores), but only for as long as the probe keeps being retained,
        // which is an implementation detail of `provide_factory` rather than a promise to `list()`.
        bool offers_gestures = false;
        bool offers_state = false;
        std::uint64_t revision = 1; // 1, not 0: "never rendered" is distinguishable from "revision 0"
        std::vector<Instance> instances;
        // The next ordinal this kind will mint. MONOTONIC -- never reused, even after a close -- so a
        // stale renderer that keeps rendering a closed instance re-materialises THAT id rather than
        // colliding with whatever was opened after it.
        std::uint64_t next_ordinal = 1;
    };

    [[nodiscard]] Entry* find(const std::string& panel_id);
    [[nodiscard]] const Entry* find(const std::string& panel_id) const;
    // Resolve to a HOSTED entry, setting `error_code` to unknown/not-hosted on failure.
    [[nodiscard]] Entry* resolve_hosted(const std::string& panel_id, std::string& error_code);
    [[nodiscard]] const Entry* resolve_hosted(const std::string& panel_id,
                                              std::string& error_code) const;

    // Bind either provider shape onto a rostered, unbound entry (the ONE binding path both public
    // `provide*` overloads run through, so their refusal rules cannot drift apart).
    [[nodiscard]] bool bind(const std::string& panel_id, PanelProvider provider,
                            PanelProviderFactory factory);

    // The live copy an instance id names, or nullptr.
    [[nodiscard]] Instance* find_instance(Entry& entry, const std::string& instance_id);

    // May this kind hold ANOTHER live copy? `diagnostic` names the limit when it may not.
    [[nodiscard]] bool may_open(const Entry& entry, std::string& diagnostic) const;

    // The MESSAGE a refused `panel.*` method carries on the wire.
    //
    // `kErrPanelInstanceLimit` is the ONE code whose cause the method name does not imply -- design
    // 04 section 3 requires the limit to be NAMED -- and `resolve_instance` has nowhere to put the
    // diagnostic `may_open` produced, so it is RECOMPUTED here from the same predicate that refused.
    // Safe because a refusal creates nothing: the instance table is exactly what `may_open` just
    // judged, so the answer cannot have moved. Every other code keeps the caller's own wording,
    // which already names the method that refused.
    [[nodiscard]] std::string refusal_message(const std::string& panel_id,
                                              const std::string& error_code,
                                              std::string fallback) const;

    // Create one live copy, calling the factory (or copying the shared provider). nullptr -- with
    // `code`/`diagnostic` set -- when the ceiling forbids it or the binding cannot render.
    [[nodiscard]] Instance* create_instance(Entry& entry, const std::string& instance_id,
                                            std::string& code, std::string& diagnostic);

    // THE ONE RESOLVER the five wire-facing methods share: the default instance for an empty id, the
    // named one when it is live, and otherwise a materialisation subject to the ceiling. `error_code`
    // is set on every nullptr return.
    [[nodiscard]] Instance* resolve_instance(Entry& entry, const std::string& instance_id,
                                             std::string& error_code);

    std::vector<Entry> roster_;
    std::size_t lists_served_ = 0;
    std::size_t renders_served_ = 0;
    std::size_t commands_dispatched_ = 0;
};

} // namespace context::editor::shell
