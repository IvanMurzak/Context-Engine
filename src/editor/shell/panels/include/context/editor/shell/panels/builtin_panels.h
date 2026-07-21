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
// WHAT "HOSTABLE TODAY" MEANS. Four of the rostered panels are hostable in this build:
//
//   * `placeholder`         — `gui/uitree/builtin.h`, in `context_gui_uitree`. Clean.
//   * `builtin.problems`    — `context_gui_panel_problems` -> uitree + bridge. Clean.
//   * `builtin.scene-tree`  — `context_gui_panel_scenetree` -> uitree + bridge. Clean since e05d3
//                             split its kernel-typed builder out (gui/panels/builders/).
//   * `builtin.inspector`   — `context_gui_panel_inspector` -> uitree + serializer. Clean since
//                             e05d3 (builders split out + the boundary-clean gateway seam).
//
// The rest have no provider yet and are LISTED-BUT-UNHOSTED (panel_host.h explains why that is an
// honest state rather than a hidden one). The e05d3 pair proves the D10 split end-to-end: both
// panels' kernel-typed model builders run DAEMON-SIDE (served over `editor.scene-tree` /
// `editor.inspect`), the models arrive here as data, and the gate's FORBIDDEN list never moved.

#pragma once

#include "context/editor/shell/panel_host.h"

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

// The event topics the Problems live feed consumes. Declared HERE rather than in `problems_feed.h`
// (which fully defines `ProblemsFeed` and is NOT safe to include from an `-fno-rtti` CEF TU — see
// above): `editor_main.cpp` subscribes to these same two strings to drive the feed, and it — like
// `cef_shell_smoke.cpp` — only ever sees the forward declaration above, never `problems_feed.h`
// itself. Keeping both topic strings in the ONE header every caller already includes is what stops
// the subscription (editor_main.cpp) and the dispatch (problems_feed.cpp) from silently disagreeing;
// `problems_feed.cpp` includes this header for them.
inline constexpr const char* kDiagnosticsTopic = "diagnostics";
inline constexpr const char* kDerivationTopic = "derivation";

// Thin free-function wrappers over `ProblemsFeed::apply_snapshot` / `apply_event`, for exactly the
// same reason: a caller holding only the forward-declared `ProblemsFeed` above cannot call a member
// function on it (that needs the complete type), so these non-member seams do it on the caller's
// behalf. Defined in builtin_panels.cpp, where `problems_feed.h` IS included.
void apply_problems_snapshot(ProblemsFeed& feed, const contract::Json& snapshot,
                             std::uint64_t generation);
bool apply_problems_event(ProblemsFeed& feed, const std::string& topic, const contract::Json& payload,
                          std::uint64_t generation);

// The same seam for the e05d3 Scene tree feed: forward one subscription event (the caller already
// subscribes to `derivation` for Problems — the Scene tree rides the SAME stream). Returns true when
// the panel's rendered surface changed. Defined in builtin_panels.cpp.
bool apply_scenetree_event(SceneTreeFeed& feed, const std::string& topic,
                           const contract::Json& payload, std::uint64_t generation);

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

    std::unique_ptr<ProblemsFeed> problems;
    // The e05d3 live feeds. DECLARATION ORDER IS LOAD-BEARING: `install_builtin_panels` wires the
    // Scene tree's selection listener at the INSPECTOR feed, so the inspector must be destroyed
    // FIRST (members destroy in reverse order) — which member order scenetree-then-inspector gives.
    // (No notification fires during destruction today; the ordering keeps that a non-event forever.)
    std::unique_ptr<SceneTreeFeed> scenetree;
    std::unique_ptr<InspectorFeed> inspector;

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
[[nodiscard]] BuiltinPanels install_builtin_panels(PanelHost& host);

// Drain the live-hydration work the feeds have marked due — called once per owner-loop frame, AFTER
// the subscription pump (M9 e05d3, design 05 §7 "hydrate from reads + subscriptions"):
//
//   * Scene tree: when a fetch is due (first hydration, or a `derivation.settled` arrived) and
//     `scene_path` names a root scene, issue `editor.scene-tree {path}` and hand the reply to the
//     feed. An EMPTY `scene_path` is the honest no-scene state: nothing is fetched and the panel
//     stays empty (exactly the Problems-without-a-daemon posture).
//   * Inspector: when the Scene tree's selection left a fetch pending, issue
//     `editor.inspect {path, idPath}` and hand the reply over.
//
// FAILURE POSTURE: a fetch is CLAIMED before its RPC, so a failed call (daemon gone, refused) is
// reported to stderr and NOT retried until the next settle / selection change — an editor hammering
// a dead daemon once per frame would be strictly worse than one that waits for the next reason to
// ask. The calls are SYNCHRONOUS on the owner loop (bounded by the client's own timeouts) — the
// same single-threaded trade editor_main.cpp documents for the diagnostics pump, made in the same
// direction for the same reason (the feeds mutate the very models the bridge handlers render).
// Defined in builtin_panels.cpp; safe to call with any subset of the feeds bound.
void pump_panel_feeds(BuiltinPanels& panels, client::Client& client, const std::string& scene_path);

} // namespace context::editor::shell::panels
