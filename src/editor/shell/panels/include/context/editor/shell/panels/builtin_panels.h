// The panel COMPOSITION ROOT (M9 e05d1/e05d3, design 04 §3-§4): the one place that binds the
// Shell's panel-agnostic `PanelHost` to the concrete C++ panel libraries this build can actually
// link.
//
// WHY THIS IS A SEPARATE LIBRARY FROM `context_editor_shell`. The D10 shell-boundary gate
// (`context_assert_shell_boundary`, src/CMakeLists.txt) audits the transitive link closure of
// `context_editor_shell` and `context_editor` and FATAL_ERRORs at configure time on any EditorKernel
// internal module. Panel libraries are exactly where that bites — until e05d3, TWO of them
// (`context_gui_panel_scenetree` / `context_gui_panel_inspector`) linked `context_compose`, which IS
// forbidden. Keeping the provider bindings HERE — in a library only the executable links — means
// `context_editor_shell` itself never touches a panel library, so the runtime half stays
// boundary-clean by construction and the gate's FORBIDDEN list needs no edit. e05d3 resolved the two
// violations by splitting the kernel-typed builders out (gui/panels/builders/, linked daemon-side);
// this file is the seam that now hosts both.
//
// WHAT "HOSTABLE TODAY" MEANS. Six of the rostered panels are hostable in this build
// (`hostable_panel_ids()` is the ONE enumeration — this list is prose about it, and
// `editor-shell-test_builtin_panels` asserts the two agree):
//
//   * `placeholder`         — `gui/uitree/builtin.h`, in `context_gui_uitree`. Clean.
//   * `builtin.problems`    — `context_gui_panel_problems` -> uitree + bridge. Clean.
//   * `builtin.scene-tree`  — `context_gui_panel_scenetree` -> uitree + bridge. Clean since e05d3
//                             split its kernel-typed builder out (gui/panels/builders/).
//   * `builtin.inspector`   — `context_gui_panel_inspector` -> uitree + serializer. Clean since
//                             e05d3 (builders split out + the boundary-clean gateway seam).
//   * `builtin.files`       — `context_gui_panel_files` -> uitree + bridge (M9 e1, D10 read half).
//                             Clean from day one: its kernel-typed builder (assetdb-backed) was
//                             born in gui/panels/builders/, the e05d3 split's precedent applied
//                             up front rather than retrofitted.
//   * `builtin.session.undo`— `context_gui_undo` -> the inspector panel + uitree + serializer. Clean
//                             since e09c: every one of those was ALREADY on this target's closure.
//   * `builtin.viewport`    — `context_gui_viewport` -> uitree + gui_compositor + render (editor-UX
//                             e3, D7). Clean: none of the three is on the D10 FORBIDDEN list, and
//                             the sibling `context_gui_viewport_edit` — which DOES link
//                             `context_compose` — is deliberately NOT linked here. THE ONE PANEL
//                             BOUND THROUGH `provide_factory` (c3's `instances.mode: "unlimited"`):
//                             several scene views at once is the point, and two copies must hold
//                             different cameras (viewport_feed.h states why `provide()` is wrong).
//
// `builtin.playbar` (hostable e08b..e09) was RETIRED by editor-window-chrome e1 (D2): the d1
// titlebar strip is the Play Bar's only home. The `SessionFeed` below survives as PURE TRANSPORT —
// the `session` subscriber and the play/selection write seams the strip drives over
// `session.control` — it simply no longer hosts a panel.
//
// The rest have no provider yet and are LISTED-BUT-UNHOSTED (panel_host.h explains why that is an
// honest state rather than a hidden one). The e05d3 pair proves the D10 split end-to-end: both
// panels' kernel-typed model builders run DAEMON-SIDE (served over `editor.scene-tree` /
// `editor.inspect`), the models arrive here as data, and the gate's FORBIDDEN list never moved.

#pragma once

#include "context/editor/shell/panel_host.h"
// The session.control outcome + verb vocabulary (editor-window-chrome d1). Deliberately INCLUDED
// rather than forward-declared: session_bridge.h is CEF-free and `-fno-rtti`-safe by design (its
// header says why), so it costs the smokes nothing — unlike the panel headers this file forwards.
#include "context/editor/shell/session_bridge.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace context::editor::client
{
// Forward-declared, NOT included: the pump below takes the Shell's live daemon client by reference,
// and only builtin_panels.cpp needs the complete type.
class Client;
} // namespace context::editor::client

namespace context::editor::shell
{
// Forward-declared for the SAME reason (M9 e09c): the two undo-persistence seams below take the
// Shell's editor-state store and its document by reference, and only builtin_panels.cpp needs the
// complete types. Including `editor_state.h` here would put `render/rhi.h` + the client arbitration
// header on the include path of every TU that hosts a panel, for two function signatures.
class EditorStateStore;
struct EditorState;
// Forward-declared for the SAME reason (M9 e09b-3): `bind_write_notice_relay` below takes the
// Shell's loud-write-notice relay by reference, and only builtin_panels.cpp needs the complete type.
// Including `write_notice.h` here would put the mirror store + the window registry on the include
// path of every TU that hosts a panel — including the `-fno-rtti` CEF smokes — for one signature.
class WriteNoticeRelay;
// Forward-declared for the SAME reason (M9 editor-UX e3): `install_builtin_panels` takes the
// window's viewport producer by pointer, and only builtin_panels.cpp needs the complete type.
// Including `viewport_binding.h` here would put render/rhi.h + the compositor header on the include
// path of every TU that hosts a panel — the `-fno-rtti` CEF smokes included — for one parameter.
class ViewportBinding;
} // namespace context::editor::shell

namespace context::render
{
// Forward-declared for the SAME reason as `ViewportBinding` above (M9 editor-UX e4): `viewport_pick`
// below takes a region point/size by VALUE and a `RenderSnapshot` by const reference, and only
// builtin_panels.cpp needs the complete types (a function DECLARATION — as opposed to a call or a
// definition — needs no more than the name). Including `view.h`/`render_world.h` here would put
// render/rhi.h on the include path of every TU that hosts a panel, the `-fno-rtti` CEF smokes
// included, for one signature.
struct RegionPoint;
struct Extent2D;
struct RenderSnapshot;
} // namespace context::render

namespace context::editor::shell::panels
{

// Forward-declared, NOT included: `problems_feed.h` drags in the daemon-facing bridge/kernel headers
// (`event_stream.h` -> `event_bus.h`), whose templated `EventBus::subscribe`/`publish` use `typeid`.
// Those headers compile fine ANYWHERE this library links (context_editor_panels has no CEF/RTTI
// constraint), but this header is also transitively pulled into the Shell's live CEF boot smoke
// (`cef_shell_smoke.cpp`), which CEF mandates be compiled `-fno-rtti` — and Clang/GCC diagnose an
// in-header `typeid` use at PARSE time (it does not depend on the template parameter), so merely
// INCLUDING the full chain from an `-fno-rtti` TU fails to compile even though nothing here ever
// instantiates `subscribe`/`publish`. `BuiltinPanels` only needs `ProblemsFeed` behind a
// `unique_ptr`, so a forward declaration is enough here; the complete type is pulled in only by
// `builtin_panels.cpp` (which links normally, no RTTI constraint) and by whichever TU actually calls
// a `ProblemsFeed` method (a runtime `#include "context/editor/shell/panels/problems_feed.h"` at the
// call site — see `test_builtin_panels.cpp`).
class ProblemsFeed;
// The e05d3 live feeds, forward-declared for the SAME reason: scenetree_feed.h reaches
// scene_tree_panel.h -> bridge/event_stream.h (the typeid chain), and inspector_feed.h reaches this
// library's own panel headers. Callers holding only the bag drive them through the free seams below.
class SceneTreeFeed;
class InspectorFeed;
// The M9 e1 live feed, forward-declared for the SAME reason: files_feed.h reaches files_panel.h ->
// bridge/event_stream.h (the typeid chain). Callers holding only the bag drive it through the free
// seams below.
class FilesFeed;
// The e08b daemon-session feed, forward-declared for the SAME reason (it includes both panel headers
// and reaches the typeid chain through scene_tree_panel.h). Callers holding only the bag drive it
// through the free seams below.
class SessionFeed;
// The M9 editor-UX e3 Scene-viewport feed, forward-declared for the SAME reason (viewport_feed.h
// names `shell::ViewportBinding`, which reaches render/rhi.h + the compositor header — a chain the
// `-fno-rtti` CEF smokes should not pay for two signatures). Callers holding only the bag drive it
// through the free seams below.
class ViewportFeed;
// The M9 e09b-2 wire write gateway, forward-declared for the SAME reason (its header names
// `inspector::OverrideWriteGateway`, so it reaches the panel headers' include chain). Callers holding
// only the bag re-point its daemon connection through `bind_write_client` below.
class WireOverrideWriteGateway;
// M9 e2's FILE write gateway, forward-declared for the same reason and re-pointed by the same seam.
class WireFileWriteGateway;
// The M9 e09c session undo host, forward-declared for the SAME reason (undo_feed.h names
// `inspector::OverrideWriteGateway`, so it reaches the panel headers' include chain). Callers
// holding only the bag drive it through the two persistence seams below.
class UndoFeed;
// The owner bag, defined below; forward-declared so `bind_write_client` can sit beside the sibling
// `bind_session_client` seam it mirrors rather than being separated from it by the struct.
struct BuiltinPanels;

// The event topics the Problems live feed consumes. Declared HERE rather than in `problems_feed.h`
// (which fully defines `ProblemsFeed` and is NOT safe to include from an `-fno-rtti` CEF TU — see
// above): `editor_main.cpp` subscribes to these same two strings to drive the feed, and it — like
// `cef_shell_smoke.cpp` — only ever sees the forward declaration above, never `problems_feed.h`
// itself. Keeping both topic strings in the ONE header every caller already includes is what stops
// the subscription (editor_main.cpp) and the dispatch (problems_feed.cpp) from silently disagreeing;
// `problems_feed.cpp` includes this header for them.
inline constexpr const char* kDiagnosticsTopic = "diagnostics";
inline constexpr const char* kDerivationTopic = "derivation";
// The M9 e08b DAEMON SESSION topic (selection / cameras / play — e08a, D7 tier 1). Re-declared here
// for exactly the reason the two above are: `editor_main.cpp` subscribes to this string and only ever
// sees the forward-declared `SessionFeed`, never `session_feed.h` (which is not `-fno-rtti`-safe to
// include). `session_feed.h` defines the same value as `kSessionTopicName` and this header's own
// static_assert in builtin_panels.cpp keeps the two spellings from drifting.
inline constexpr const char* kSessionTopic = "session";

// Thin free-function wrappers over `ProblemsFeed::apply_snapshot` / `apply_event`, for exactly the
// same reason: a caller holding only the forward-declared `ProblemsFeed` above cannot call a member
// function on it (that needs the complete type), so these non-member seams do it on the caller's
// behalf. Defined in builtin_panels.cpp, where `problems_feed.h` IS included.
void apply_problems_snapshot(ProblemsFeed& feed, const contract::Json& snapshot,
                             std::uint64_t generation);
bool apply_problems_event(ProblemsFeed& feed, const std::string& topic, const contract::Json& payload,
                          std::uint64_t generation);

// Publish a SHELL-LOCAL diagnostic into the Problems panel (M9 e09d). Everything else in that panel
// arrives from the daemon's `diagnostics` topic; this is the seam for a problem the Shell is the only
// process that can KNOW — concretely, `.editor/editor-state.json` having been corrupt and reset
// (07 §6 demands that recovery be LOUD, and a stderr line nobody is watching is not loud in a GUI).
//
// It builds a payload in the shape the daemon publishes (`code` / `message` / `file`) and pushes it
// through the SAME `apply_problems_event` dispatch, deliberately: a second rendering path for
// "a problem" is how the two drift, and the panel's own tolerant parser already accepts this shape.
//
// `file` is the document the diagnostic is ABOUT, and it goes in the `file` member rather than
// `pointer` because that is the one `ProblemsFeed`'s parser renders and offers navigation for —
// `pointer` is read into the nav record but never displayed, so a path put there is invisible in the
// panel. (The daemon's own `editor.session_state_invalid` puts its path in `pointer`; it gets away
// with it only because the path is ALSO spelled inside its message text.)
//
// Returns true when the panel's model actually changed. A no-op — no Problems feed in this build —
// returns false rather than failing: the caller has ALREADY reported to stderr, and an editor that
// refused to boot because it could not render its own diagnostic would be the joke version of loud.
bool report_local_problem(BuiltinPanels& panels, const std::string& code, const std::string& message,
                          const std::string& file);

// The same seam for the e05d3 Scene tree feed: forward one subscription event (the caller already
// subscribes to `derivation` for Problems — the Scene tree rides the SAME stream). Returns true when
// the panel's rendered surface changed. Defined in builtin_panels.cpp.
bool apply_scenetree_event(SceneTreeFeed& feed, const std::string& topic,
                           const contract::Json& payload, std::uint64_t generation);

// The same seam for the M9 e1 Files feed — rides the SAME `derivation` stream. Returns true when the
// panel's rendered surface changed. Defined in builtin_panels.cpp.
bool apply_files_event(FilesFeed& feed, const std::string& topic, const contract::Json& payload,
                       std::uint64_t generation);

// The same seam for the e09e-2 INSPECTOR FAN-OUT — design 05 §8's tail, where a landed write reaches
// every subscribed client. Forward one event off the SAME `derivation` stream the Scene tree and
// Problems already ride; the feed re-reads the inspected entity so a landed write appears without
// touching the selection. Returns true when a re-read was ARMED.
//
// ⚠ SINCE M9 x9 (CE #449) THE PUBLISHER HALF IS WIRED, so the cross-window case IS live: a plain
// `edit` — the Shell's only write — publishes `files.changed` and then `derivation.settled{gen}`, and
// this seam is how that reaches the Inspector in every attached client INCLUDING the one that wrote.
// inspector_feed.h § THE PUBLISHER HALF carries the producer list; what remains for e09e-3 is the
// two-window CEF smoke, not the plumbing.
//
// It takes NO generation, unlike the scenetree seam: the Inspector has no generation/stability status
// line to advance, so the parameter would be received and dropped — and a seam that accepts a value it
// ignores invites a caller to believe it matters.
//
// ⚠ The feed WITHHOLDS the re-read while a gesture is staged (inspector_feed.h § apply_event — a
// mid-gesture refresh silently re-bases the L-30 collision guard), so a `false` return is an ordinary,
// correct outcome here and NOT a signal that the event was unrecognized.
bool apply_inspector_event(InspectorFeed& feed, const std::string& topic,
                           const contract::Json& payload);

// --- the e09e-3 INSPECTOR DRIVE + OBSERVE seams (a `-fno-rtti` caller's whole view of the feed) ---
//
// WHY THESE EXIST AT ALL, and why they are not a test backdoor. `inspector_feed.h` reaches the
// `typeid` chain (see the forward-declaration note above), so a CEF-linking TU — every live smoke
// under `src/editor/shell/cef/src/**`, which CEF mandates be compiled `-fno-rtti` — physically cannot
// include it and therefore cannot call ONE member function on the feed. The existing
// `apply_inspector_event` / `session_play_state` / `session_facts_applied` seams are the same
// construct for the same reason; these three complete the set the live two-window fan-out smoke
// (e09e-3, ctest `editor-cef-smoke-shell-inspector-fanout`) needs to say anything at all about what
// the Inspector did.
//
// They are READ-ONLY apart from `request_inspector`, and none of them can manufacture the outcome a
// caller asserts: every counter below is incremented ONLY by the feed's own production paths.

// Arm a fetch for `identity` (L-35 id-path) — the seam behind `InspectorFeed::request`.
//
// The PRODUCTION driver is the Scene tree's selection listener (install_builtin_panels wires it), and
// this is deliberately NOT a second policy: it forwards to the same one function, so the L-30
// mid-gesture deferral documented on `InspectorFeed::request` binds a caller here exactly as it binds
// the listener. Returns what `request` returns — true when the fetch was ARMED, false when the guard
// DEFERRED it (an ordinary outcome, not a failure). False also when this bag has no Inspector feed.
bool request_inspector(BuiltinPanels& panels, const std::string& identity);

// One read-only snapshot of a bag's Inspector — every observable the feed and its panel expose, as
// plain data, so a `-fno-rtti` caller can assert on them.
//
// `present` is false when the bag has no Inspector feed at all (a partial `install_builtin_panels`),
// which is an ordinary state and NOT the same as "an Inspector that has done nothing" — a caller
// asserting on counters must check it first, or a missing feed reads as a feed that stayed idle.
struct InspectorObservation
{
    bool present = false;
    bool has_selection = false;
    bool has_staged_edit = false;
    // A settle-driven re-read is OWED but withheld behind an in-flight gesture (e09e-2).
    bool refresh_deferred = false;
    // The SELECTION-driven half of that deferral (M9 x10, CE #452): a move to another entity — or a
    // clear — is owed but withheld behind an in-flight gesture. `deferred_selection` is the identity
    // that will be adopted when the gesture ends, and "" means either nothing is owed OR the withheld
    // fact was a CLEAR: read `selection_deferred` first to tell those apart (that is the same
    // present-vs-empty discipline `present` itself documents above).
    bool selection_deferred = false;
    std::string deferred_selection;
    // Selection-driven reads RECOGNIZED and withheld — the counter that keeps the x10 negatives
    // ("the staged edit survived", "nothing was armed") from passing on a feed that never saw the
    // selection change at all.
    std::size_t selections_deferred = 0;
    // Staged gestures ABANDONED at the one undeferrable door (x10 — `InspectorFeed::apply_result`).
    // Must stay 0 on every path the deferral covers; non-zero is a reported data-loss event.
    std::size_t abandons_observed = 0;
    bool fetch_pending = false;
    // The L-30 collision base the NEXT commit will CAS against. 0 = no CAS guard.
    std::uint64_t base_raw_hash = 0;
    std::size_t results_applied = 0;
    // `derivation.settled` facts RECOGNIZED — counted before the staged-gesture guard, so it climbs
    // whether the settle was served or deferred (inspector_feed.h § events_applied).
    std::size_t events_applied = 0;
    std::size_t rereads_armed = 0;
    std::size_t commits_observed = 0;
    std::size_t drops_observed = 0;
    // Override writes this bag's gateway actually applied over the wire (0 when it has none).
    std::size_t writes_applied = 0;
    // The last resolved commit, as tokens: `applied` / `rebased` / `dropped` / `error` / `none`.
    std::string last_commit_status;
    std::string last_commit_code;
    // The in-flight gesture, canonically serialized (R-FILE-001) — "" when nothing is staged.
    std::string staged_pointer;
    std::string staged_value;
};
[[nodiscard]] InspectorObservation observe_inspector(const BuiltinPanels& panels);

// The CURRENT composed value of one field as the panel model holds it, canonically serialized;
// `overridden` reports the model's own flag. "" when the field does not resolve (or there is no feed)
// — indistinguishable from an empty value only for a field that cannot exist, since `render_value`
// never yields "" for a present field.
[[nodiscard]] std::string inspector_field_value(const BuiltinPanels& panels,
                                                const std::string& pointer, bool& overridden);

// The Inspector's contribution id, its edit command, and the node-id prefix `build_panel` gives every
// editable widget — RE-DECLARED here for exactly the reason `kSessionTopic` above is: a `-fno-rtti`
// caller cannot include the header that owns them, and a live smoke needs all three to address the
// rendered DOM element (`<prefix><pointer>`) and to drive `panel.command` / `panel.gesture`.
// builtin_panels.cpp static_asserts each against its owning spelling, so the two cannot drift.
inline constexpr const char* kInspectorPanelId = "builtin.inspector";
inline constexpr const char* kInspectorEditCommand = "inspector.edit";
inline constexpr const char* kInspectorWidgetNodePrefix = "inspector.widget.";

// The same seam for the e08b Session feed. `apply_session_event` forwards one `session` fact (the
// feed itself drops our own echo — see session_feed.h). Both defined in builtin_panels.cpp.
bool apply_session_event(SessionFeed& feed, const std::string& topic, const contract::Json& payload);

// The READ half of that seam (M9 e08d): the feed's CURRENT L-51 play-state token
// (`edit`/`playing`/`paused`, byte-identical to `gui::playbar::state_token()`), and how many facts
// have actually moved it. `editor_main.cpp` relays both to editor-core over `session.state`
// (session_bridge.h), so the browser side's `when`-contexts see daemon truth instead of a frozen
// boot baseline. Free functions for the SAME forward-declaration reason as `apply_session_event`
// above: the caller never sees `SessionFeed`'s complete type.
[[nodiscard]] std::string session_play_state(const SessionFeed& feed);
[[nodiscard]] std::size_t session_facts_applied(const SessionFeed& feed);

// The d1 additions to that READ half (editor-window-chrome d1, target design 02 §7), for the same
// forward-declaration reason:
//
//   * `session_sim_tick` — the running session's simTick as the feed's playbar model last adopted
//     it (a fact from another client, or this Shell's own transport reply). Relayed in
//     `session.state` so the strip's `t+` timer is daemon truth.
//   * `session_control_generation` — the model's control generation, which moves on EVERY rendered
//     play transition INCLUDING this Shell's own writes. `facts_applied` alone cannot see those
//     (the daemon's echo of our own write is dropped — session_feed.h), so a `session.state`
//     generation built only from it would freeze every poller in this process across a locally
//     driven transition.
//   * `session_state_generation` — the composed generation `session.state` relays: the SUM of
//     `facts_applied` and the control generation (each monotone, so the sum is too). The one value
//     a composition root should report; the parts stay exposed for the tests that pin each
//     counter's own movement.
[[nodiscard]] std::uint64_t session_sim_tick(const SessionFeed& feed);
[[nodiscard]] std::uint64_t session_control_generation(const SessionFeed& feed);
[[nodiscard]] std::uint64_t session_state_generation(const SessionFeed& feed);

// The d1 WRITE seam: drive one `session.control` verb through the feed's `PlaybarModel`
// (`SessionFeed::control`), reported in the shape the bridge relays. Since e1 retired the docked
// playbar this is the ONLY press path — the strip's — but the write itself is unchanged from the
// one the dock panel's invoke funnelled through. An unknown verb answers
// `error_code = kSessionControlBadVerbCode` with nothing
// dispatched (belt-and-braces — the bridge validates first); a feed with no daemon client reports
// the model's own honest "nothing to drive" (`ok:false`, no code, state unmoved).
[[nodiscard]] SessionControlOutcome session_control(SessionFeed& feed, const std::string& verb);

// The d3 SELECTION write seam (editor-window-chrome d3): drive one `session.select` write through
// the feed's selection writer (`SessionFeed::request_selection` — the proven e08b `editor.select`
// chain, `origin` echo suppression included), reported in the shape the bridge relays. Today's only
// production caller is the `selection.clear` menu/palette command (`ids: []`); a feed with no daemon
// client reports the writer's own honest nullopt as `applied:false`.
[[nodiscard]] SessionSelectOutcome session_select(SessionFeed& feed,
                                                  const std::vector<std::string>& ids);

// Point the Session feed at the daemon link's CURRENT client — `nullptr` to clear it. The feed's
// pointer is a NON-OWNING view of a client the daemon lifecycle owns and can destroy (a lost daemon
// tears the link down; exit resets it), so this is the seam the owner calls at EVERY point the
// lifecycle can change that client, and with `nullptr` before it frees one. A cleared feed refuses
// every write honestly (session_feed.h § LIFETIME).
//
// The echo-suppression identity is DERIVED from the client here rather than passed alongside it: ids
// are minted per wire connection, so a caller pairing a client with a separately-carried id is a
// stale-id bug waiting to happen — and a stale id silently suppresses another client's facts while
// applying our own. There is no id to get wrong if the client is the only argument.
void bind_session_client(SessionFeed& feed, client::Client* client);

// Wire the c1/D3 `selection-focus` CONSUMER: the Inspector renders the FOCUSED subject.
//
// The Inspector's whole vocabulary is composed ENTITIES, so "render the focused subject" has exactly
// two readings here, and both are done through the feed's existing, L-30-guarded seams rather than a
// second policy:
//
//   * focus moved to `entity`   -> re-point the Inspector at the entity selection the Scene tree
//                                  currently renders (`InspectorFeed::request`), or clear it when
//                                  there is none.
//   * focus moved anywhere else -> the human is working on files/assets, which this panel cannot
//                                  inspect, so it shows its no-selection placeholder
//                                  (`InspectorFeed::request_clear`) instead of an entity nobody is
//                                  looking at.
//
// Both calls DEFER under a staged gesture exactly as the Scene tree's selection listener does — a
// focus move must not destroy an in-flight edit any more than a foreign selection may.
//
// `scenetree` may be null (its provider can be refused); the entity arm then finds no selection and
// clears, which is the honest rendering of "no entity is selected here".
//
// This is the PRODUCTION wiring `install_builtin_panels` performs, exposed as a named seam so a test
// can drive the real thing over its own feeds instead of re-implementing the policy beside it.
void bind_selection_focus(SessionFeed& session, InspectorFeed& inspector, SceneTreeFeed* scenetree);

// Wire the c1/D3 `selection-focus` CONSUMER for the Files panel (M9 e1) — simpler than the
// Inspector's, because the Files panel already holds its OWN selection (D1 coexistence) and only
// needs to know WHETHER it currently has focus, not WHAT to show. Every focus move calls
// `files_feed.panel().set_focused(subject == "file")` — the panel renders the boolean in its status
// line ("the panel renders focus state", the DoD line this seam exists to satisfy). Takes the FEED
// (forward-declared, like `SceneTreeFeed*` above) rather than the panel directly, so this header
// still never includes files_panel.h's typeid chain; `files_feed` must outlive the binding.
void bind_files_focus(SessionFeed& session, FilesFeed& files_feed);

// The SAME seam for the M9 e09b-2 WRITE path: point the Inspector's wire override-write gateway at
// the daemon link's CURRENT client — `nullptr` to clear it. A no-op when no gateway is in the bag.
//
// It exists for exactly the reason `bind_session_client` does, and must be called at exactly the same
// points: the gateway's `client::Client*` is a NON-OWNING view of a client the daemon lifecycle owns
// and destroys (a lost daemon tears the link down; exit resets it), while a gesture commit is
// renderer-driven and can land in that window. Clearing it BEFORE the lifecycle frees the client is
// what turns a would-be use-after-free into an honest refusal (wire_override_gateway.h § LIFETIME).
//
// It takes the BAG, not the gateway, because the gateway type is only forward-declared here — and
// because the two writes seams then read identically at the call site.
//
// Since e2 it re-points BOTH write gateways — the override one and the file one. They are re-pointed
// together on purpose: they share one daemon link, so a seam that cleared only one would leave the
// other holding a freed client, which is exactly the use-after-free this function exists to prevent.
void bind_write_client(BuiltinPanels& panels, client::Client* client);

// Point every REFUSED write in this bag at the Shell's loud-notice relay (M9 e09b-3, design 05 §8) —
// and, since M9 x10 (CE #452), every ABANDONED Inspector gesture as well.
//
// ONE seam for ALL THREE loss sites — the Inspector's gesture commit, the undo/redo replay, and the
// Inspector's undeferrable gesture abandonment — because the human does not care which of them lost
// their work; they care that the editor said so. Keeping the
// message COMPOSITION here (in builtin_panels.cpp, the one TU that sees both feeds AND the relay) is
// what stops two hand-rolled `WriteNotice` builders drifting into two different vocabularies for the
// same event, and is why the feeds themselves hold only an erased `std::function` and never name the
// relay (inspector_feed.h / undo_feed.h § the notice sink).
//
// `relay` must OUTLIVE the bag: the sinks capture a raw pointer to it, exactly as the checkpoint sink
// captures the UndoFeed. The composition root declares it before the bag for that reason.
//
// Calling it twice REPLACES the sinks rather than adding a second one — a notice must reach the human
// once. Safe on any subset of the feeds.
void bind_write_notice_relay(BuiltinPanels& panels, WriteNoticeRelay& relay);

// --- the M9 e09c undo-journal persistence seams (design 03 §1 / C-F3) ---------------------------
//
// The journal lives in this library and owns no disk; `.editor/editor-state.json` is the Shell's and
// the Shell is its SINGLE WRITER. These two functions are the whole bridge between them, and there
// is deliberately no third: every write of the journal to that file goes through `restore` at boot
// and `publish` from the owner loop, both landing in `EditorStateStore::set_undo`. Keeping it to one
// seam is what lets e09d assert the ownership split structurally rather than by inspection.
//
// Both take the BAG (not the feed) for the same two reasons `bind_write_client` does: `UndoFeed` is
// only forward-declared here, and the call sites then read identically.

// Adopt the persisted journal into the live one (boot / restart). Returns true when a STEP actually
// came back — false for a fresh project, an absent blob, a well-formed but empty journal document
// (which restores nothing, so it is a fresh project as far as any caller can tell), or a corrupt one
// (which leaves the journal EMPTY and reports why; a corrupt session file costs the history, never
// the boot).
bool restore_undo_state(BuiltinPanels& panels, const EditorState& state);

// Offer the journal to the store IF it has moved since the last publish. Returns true when it was
// offered. Cheap enough to call every frame: an unmoved journal is not re-serialized at all, and the
// store's own identical-value rule is the second backstop. The store's debounce + atomic write turn
// a burst of edits into one crash-safe file per quiet period.
bool publish_undo_state(BuiltinPanels& panels, EditorStateStore& store, std::uint64_t now_us);

// The roster ids this build can render. Exposed so a caller (and the T1 suite) can assert what was
// bound WITHOUT restating the list — the one enumeration lives in `install_builtin_panels`.
[[nodiscard]] const std::vector<std::string>& hostable_panel_ids();

// Everything `install_builtin_panels` created and the caller must keep alive.
//
// THE LIFETIME IS THE POINT. Providers are std::functions capturing the models below, and the
// PanelHost holds them for as long as it routes. Returning the owners in one bag makes "these must
// outlive the host" a thing the type system participates in, rather than a comment someone violates
// by letting a local ProblemsFeed go out of scope and leaving the host with dangling captures.
//
// Special members are declared here and DEFINED (as `= default`) in builtin_panels.cpp, not inlined:
// `std::unique_ptr<ProblemsFeed>` needs the complete type at the point its destructor/move ops are
// generated, and `ProblemsFeed` is only forward-declared above (the RTTI/CEF seam this struct exists
// to keep clean of).
struct BuiltinPanels
{
    BuiltinPanels();
    ~BuiltinPanels();
    BuiltinPanels(BuiltinPanels&&) noexcept;
    BuiltinPanels& operator=(BuiltinPanels&&) noexcept;
    BuiltinPanels(const BuiltinPanels&) = delete;
    BuiltinPanels& operator=(const BuiltinPanels&) = delete;

    // The e09b-2 WIRE override-write gateway. DECLARED FIRST so it is DESTROYED LAST (members
    // destroy in reverse order), for the SAME reason the session feed is declared before the scene
    // tree: it IS the OverrideWriteGateway the Inspector's panel holds a raw pointer to, and a
    // gateway is contractually required to outlive its panel (inspector_panel.h).
    std::unique_ptr<WireOverrideWriteGateway> writes;
    // The M9 e2 WIRE file-write gateway. DECLARED BESIDE `writes` and before every consumer, for the
    // identical lifetime reason: BOTH the Files panel and the undo journal hold a raw pointer to it,
    // and a gateway must outlive its panel.
    std::unique_ptr<WireFileWriteGateway> file_writes;
    // The e09c session undo host. DECLARED SECOND — after `writes`, before every panel feed — because
    // it sits between them in the lifetime order: its journal holds a raw pointer to the gateway
    // above (so the gateway must outlive it), and the Inspector's checkpoint sink holds a raw
    // pointer to IT (so it must outlive the Inspector). Members destroy in reverse declaration
    // order, which makes exactly that sandwich.
    std::unique_ptr<UndoFeed> undo;
    std::unique_ptr<ProblemsFeed> problems;
    // The e08b daemon-session feed. DECLARED BEFORE THE SCENE TREE so it is destroyed AFTER it
    // (members destroy in reverse order): it IS the SelectionGateway the Scene tree's panel writes
    // through, and a gateway is contractually required to outlive its panel (scene_tree_panel.h). The
    // reverse pointer — the feed's raw `SceneTreePanel*` — is only dereferenced from `apply_event`,
    // which the owner loop stops calling before teardown, so no fact can arrive at a half-destroyed
    // bag.
    std::unique_ptr<SessionFeed> session;
    // The e05d3 live feeds. DECLARATION ORDER IS LOAD-BEARING: `install_builtin_panels` wires the
    // Scene tree's selection listener at the INSPECTOR feed, so the inspector must be destroyed
    // FIRST (members destroy in reverse order) — which member order scenetree-then-inspector gives.
    // (No notification fires during destruction today; the ordering keeps that a non-event forever.)
    std::unique_ptr<SceneTreeFeed> scenetree;
    std::unique_ptr<InspectorFeed> inspector;

    // The M9 e1 Files feed. Its selection is written through `session`'s
    // `files::SelectionGateway` adapter (session_feed.h), so — the SAME reason `session` precedes
    // `scenetree`/`inspector` above — it must be destroyed BEFORE `session` is, which member
    // declaration order (destruction is reverse order) gives by placing it AFTER `session` here.
    std::unique_ptr<FilesFeed> files;

    // The M9 editor-UX e3 Scene-viewport feed. Declaration position is NOT load-bearing here (it
    // holds no gateway anybody else points at, and points at nothing declared in this bag): it holds
    // its own per-instance `ViewportPanel` models and a NON-owning pointer to the window's
    // `ViewportBinding`, which lives in `EditorWindow` and outlives every bag by construction.
    std::unique_ptr<ViewportFeed> viewport;

    // How many providers actually bound. Checked by the caller: a silently dropped binding presents
    // later as a panel that mysteriously reports `hosted: false`.
    std::size_t bound = 0;
};

// Bind every hostable provider on `host`. `host` must outlive the returned bag.
//
// Returns the owners; `bound` reports how many bindings succeeded, which the caller compares against
// `hostable_panel_ids().size()`. A partial result is REPORTED rather than fatal: an editor that
// refused to start because one panel could not bind would be less useful than one that opens with
// the rest and says so.
// `viewports` (M9 editor-UX e3) is the WINDOW's viewport producer — `EditorWindow::viewports()` in
// the live editor, a stack instance in the T1 suite, and `nullptr` for a caller with no window (the
// headless harnesses, and every existing call site, which is why it is trailing and defaulted). With
// it the Scene viewport's copies drive real cameras and read a real adapter verdict; without it they
// still render, keep their cameras locally and push nothing.
[[nodiscard]] BuiltinPanels install_builtin_panels(PanelHost& host,
                                                   ViewportBinding* viewports = nullptr);

// Drain the live-hydration work the feeds have marked due — called once per owner-loop frame, AFTER
// the subscription pump (M9 e05d3, design 05 §7 "hydrate from reads + subscriptions"):
//
//   * Scene tree: when a fetch is due (first hydration, or a `derivation.settled` arrived) and
//     `scene_path` names a root scene, issue `editor.scene-tree {path}` and hand the reply to the
//     feed. An EMPTY `scene_path` is the honest no-scene state: nothing is fetched and the panel
//     stays empty (exactly the Problems-without-a-daemon posture).
//   * Inspector: when the Scene tree's selection left a fetch pending, issue
//     `editor.inspect {path, idPath}` and hand the reply over. Since e09c a LANDED undo/redo also
//     arms that fetch (READ-YOUR-REPLAYS): a replay writes through the gateway directly, so the
//     Inspector's own commit listener never fires and the panel would otherwise keep both the
//     pre-undo value and the pre-undo CAS token — making the human's next edit to that field drop
//     as a "concurrent writer" that is really their own Ctrl+Z (undo_feed.h § take_replay_landed).
//     Since e09e-2 a `derivation.settled` arms it too (`apply_inspector_event`) — the cross-client
//     FAN-OUT path, with the deliberate exception of a settle arriving mid-gesture, which the feed
//     DEFERS rather than serving (inspector_feed.h § apply_event). Since M9 x9 a plain `edit`
//     PRODUCES that settle, so every client write drives this path (inspector_feed.h § THE PUBLISHER
//     HALF) — and the tree refetch the same settle triggers can re-arm the Inspector's fetch through
//     the selection listener, which the feed defers under the same L-30 rule (§ request).
//   * Files (M9 e1): when a fetch is due (first hydration, or a `derivation.settled` arrived — the
//     same signal the Scene tree rides), issue `editor.files` (no params: it lists the WHOLE
//     project, not one scene, so there is no `scene_path` gate here) and hand the reply to the
//     feed.
//
// FAILURE POSTURE: a fetch is CLAIMED before its RPC, so a failed call (daemon gone, refused) is
// reported to stderr and NOT retried until the next settle / selection change — an editor hammering
// a dead daemon once per frame would be strictly worse than one that waits for the next reason to
// ask. The calls are SYNCHRONOUS on the owner loop (bounded by the client's own timeouts) — the
// same single-threaded trade editor_main.cpp documents for the diagnostics pump, made in the same
// direction for the same reason (the feeds mutate the very models the bridge handlers render).
// Defined in builtin_panels.cpp; safe to call with any subset of the feeds bound.
void pump_panel_feeds(BuiltinPanels& panels, client::Client& client, const std::string& scene_path);

// Re-point the Scene viewport feed at a window's viewport producer (M9 editor-UX e3).
//
// The `bind_write_client` seam's twin, and it exists for the same ordering reason: the bag is built
// at boot, before any window (and therefore before any `render::IDevice`) exists, so the producer
// cannot be a constructor argument. Called per frame from the owner loop; safe with no viewport feed
// bound, and `nullptr` detaches.
void bind_viewport_producer(BuiltinPanels& panels, ViewportBinding* viewports);

// --- the M9 editor-UX e3 viewport camera round trip (D7) ----------------------------------------
//
// The viewport's daemon traffic, kept OUT of `pump_panel_feeds` on purpose: that pump is a pure READ
// loop (every branch issues a read and adopts the reply), while this one both READS
// (`editor.cameras-get`, once, then whenever a new copy opens) and WRITES (`editor.camera-set`, per
// locally-moved camera). Folding a write path into a function documented as "the live-hydration
// work the feeds have marked due" would make its failure posture — a failed fetch waits for the next
// settle — silently cover a lost WRITE, which is a different and much worse outcome.
//
// FAILURE POSTURE. The read claims its fetch before issuing (the sibling rule). A failed WRITE is
// reported and the camera is RE-ARMED, because a camera the daemon never received is a camera the
// human loses on restart — the opposite trade from a read, which can simply be retried later.
//
// Returns how many `editor.camera-set` calls were attempted. Safe with no viewport feed bound.
std::size_t pump_viewport_cameras(BuiltinPanels& panels, client::Client& client);

// --- the M9 editor-UX e4 picking write (D8) -------------------------------------------------------
//
// A thin wrapper over `ViewportFeed::pick`, for the SAME forward-declaration reason as every other
// seam here: `editor_main.cpp` holds only the forward-declared `ViewportFeed` above, so a caller
// that queued a `shell::ViewportPressEvent` (shell.h) off `EditorWindow::take_viewport_presses()`
// drives the pick through this free function instead of a member call. See `ViewportFeed::pick`
// (viewport_feed.h) for the raycast + write it performs; `false` here is the SAME honest no-op —
// no viewport feed bound, or nothing bound for it to write through.
bool viewport_pick(BuiltinPanels& panels, const std::string& instance_id, render::RegionPoint point,
                   render::Extent2D region_size, const render::RenderSnapshot& snapshot);

} // namespace context::editor::shell::panels
