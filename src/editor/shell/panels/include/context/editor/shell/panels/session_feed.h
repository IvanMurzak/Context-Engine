// The LIVE DAEMON SESSION feed (M9 e08b, design 05 §4 / D7 tier 1) — the one place the Shell's
// panels meet e08a's daemon session state, in BOTH directions:
//
//   * WRITER — it implements the panels' two boundary-clean seams (scenetree::SelectionGateway and
//     playbar::PlayControlGateway) over the REAL wire (`editor.select`, `editor.play|pause|stop|step`)
//     through the Shell's ordinary client connection (D10: the editor is a client like any other).
//   * SUBSCRIBER — it consumes the `session` topic's facts (`selection-changed`, `play-state`) and
//     applies them to the panels, so a selection made by the CLI, a scripted agent, or a second
//     window is visible here with NO panel-local write at all.
//
// ECHO SUPPRESSION LIVES HERE, AND ONLY HERE (docs/editor-session-state.md). The daemon fans every
// fact out to every subscriber with no per-client filtering; a consumer APPLIES a fact whose `origin`
// differs from its own client id and DROPS one that matches. Doing it at this single seam is what
// makes "no flicker, no double-apply" a structural property rather than a per-panel discipline: the
// panels never see their own echo, so they cannot double-apply it, and they cannot tell a second
// client's change from their own — which is exactly the point.
//
// LIFETIME — THE CLIENT POINTER IS A NON-OWNING VIEW WITH A DEFINED CLEAR POINT. The feed does not
// own the `client::Client` and cannot extend its life: the daemon lifecycle owns it and DESTROYS it
// on a lost daemon (tear_down_link) and at exit (shutdown_at_exit). A cached pointer that outlives
// either is a use-after-free reachable from ordinary UI — a panel write is renderer-driven, so it can
// land in the window between "the daemon died" and "we reattached", or during the exit pump. The rule
// that makes that impossible is structural, not a comment: the OWNER re-derives this binding from the
// lifecycle at every point the lifecycle can change the client, and clears it with `nullptr` BEFORE
// tearing the lifecycle down — one seam, `panels::bind_session_client` (builtin_panels.h), never a
// pointer stashed once and trusted afterwards. A cleared feed is a plain subscriber whose every write
// honestly reports "not delivered", which is the same state it starts in.
//
// WHY THE IDENTITY TRAVELS WITH THE POINTER. Ids are minted per WIRE CONNECTION and never reused
// within a daemon lifetime; a reconnect (a daemon restart, the lifecycle's ladder) mints a NEW one.
// A stale id would suppress a DIFFERENT client's facts and apply our own — both failure modes silent.
// So the id is derived FROM the client at the seam, and clearing the pointer clears the id with it.
//
// § THE SUBJECT FILTER (c1/D1) — LOAD-BEARING, AND ITS ABSENCE FAILS SILENTLY. Since selection is
// typed per SUBJECT KIND and selections of different subjects coexist, this feed is no longer the
// consumer of "the selection" — it is the consumer of the ENTITY selection. It is also the ONLY
// consumer of `selection-changed` in the Shell (the Inspector is driven transitively, through
// `SceneTreePanel::add_selection_listener`), so this one comparison is what keeps a `file` selection
// out of `SceneTreePanel::apply_selection`, which would read its ids as L-35 entity id-paths.
//
// The failure mode without it is the reason the filter is tested BOTH WAYS: a file selection applied
// to the scene tree resolves no rows, so the tree simply renders nothing selected — indistinguishable
// from a correct empty result. A test asserting only "a file fact does not move the tree" would pass
// with the whole feed disconnected, so a sibling asserts an ENTITY fact DOES move it.
//
// A fact with NO `subject` member reads as `entity`: absence is the documented default of the wire
// parameter, so an older daemon (or a hand-written client) means exactly what it always meant.
//
// NOT ROUTED THROUGH THE IN-PROCESS SHIM. `gui/contract`'s shim calls `Dispatcher::attach` directly,
// which has no connection and is therefore permanently `origin 0` — indistinguishable from the daemon
// itself and from any other in-process consumer. e08a's own docs call this out as the constraint
// e08b must design around; the Shell already IS a real wire client (`context_client`), so it simply
// uses that.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"
#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/shell/panel_host.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::client
{
// Forward-declared, NOT included: only session_feed.cpp needs the complete type (the same discipline
// builtin_panels.h applies to the pump's client).
class Client;
} // namespace context::editor::client

namespace context::editor::shell::panels
{

namespace scenetree = gui::panels::scenetree;
namespace playbar = gui::playbar;

// The e08a topic every session fact rides. Declared here (and re-exported by builtin_panels.h for
// the subscriber, which only ever sees the forward declaration) so the subscription and the dispatch
// cannot silently disagree — the kDiagnosticsTopic/kDerivationTopic pattern.
inline constexpr const char* kSessionTopicName = "session";

// The fact kinds e08a publishes on it. `camera-changed` is recognised and deliberately IGNORED:
// the viewport/camera UI is e11, and silently dropping an unknown fact would make a future one
// indistinguishable from a bug.
inline constexpr const char* kSelectionChangedEvent = "selection-changed";
inline constexpr const char* kPlayStateEvent = "play-state";
inline constexpr const char* kCameraChangedEvent = "camera-changed";
// c1/D3: WHICH live selection the human is actually working on. A tier-1 daemon fact, not tier-2
// panel focus, so the CLI and agents can see the same answer this Shell renders.
inline constexpr const char* kSelectionFocusEvent = "selection-focus";

// The DEFAULT selection subject (c1/D1) — and the only one this Shell's Scene tree can render, since
// its ids are L-35 entity id-paths.
//
// ⚠ SPELLED HERE RATHER THAN INCLUDED. The daemon owns the vocabulary in
// `editorkernel/editor_session_state.h`, which this library must NOT reach (D10: the Shell links no
// EditorKernel module — the configure-time boundary gate refuses it). So this is a deliberate
// MIRROR of a wire token, the same discipline `kSessionTopicName` above and the playbar's L-51
// tokens already follow: the cross-process `editor-session-panels-t2` drill against a real daemon is
// what proves the two spellings agree.
inline constexpr const char* kSelectionSubjectEntity = "entity";

class SessionFeed final : public scenetree::SelectionGateway, public playbar::PlayControlGateway
{
public:
    // Non-owning: `host` must outlive the feed. `playbar_panel_id` is passed rather than hardcoded —
    // the feed is a MECHANISM the composition root points at a roster id (the ProblemsFeed pattern).
    SessionFeed(PanelHost& host, std::string playbar_panel_id);

    SessionFeed(const SessionFeed&) = delete;
    SessionFeed& operator=(const SessionFeed&) = delete;
    SessionFeed(SessionFeed&&) = delete;
    SessionFeed& operator=(SessionFeed&&) = delete;

    // --- wiring ---------------------------------------------------------------------------------

    // Bind the live connection + THIS connection's echo-suppression identity; `nullptr` + 0 detaches
    // (every write then honestly reports "not delivered"). The LOW-LEVEL setter: the Shell always
    // goes through `panels::bind_session_client`, which derives the id from the client so the two
    // cannot disagree. Taking them separately is for tests that need an attached IDENTITY with no
    // live connection (echo suppression in isolation) — see § LIFETIME above.
    void bind_client(client::Client* client, std::uint64_t client_id) noexcept;

    // The scene-tree panel whose RENDERED selection this feed drives, and the roster id to touch when
    // it changes. `nullptr` when the Scene tree did not bind (its provider was refused) — the feed
    // then still drives the play transport, because one unavailable surface must not take the other
    // down.
    void bind_scene_tree(scenetree::SceneTreePanel* panel, std::string panel_id);

    // --- the subscriber half ----------------------------------------------------------------------

    // Consume one subscription event. Returns true when a panel's rendered surface actually changed.
    // A non-`session` topic, an echo of our own write, and an unrecognised fact all return false —
    // and so does a `selection-changed` fact for a subject this Shell's Scene tree cannot render
    // (see § THE SUBJECT FILTER below).
    bool apply_event(const std::string& topic, const contract::Json& payload);

    // --- the c1/D3 focus half ---------------------------------------------------------------------

    // WHICH selection subject the daemon says the human is working on. `entity` until a
    // `selection-focus` fact says otherwise — the same boot default the daemon holds, so an
    // unattached Shell and a fresh daemon agree without a round trip.
    [[nodiscard]] const std::string& selection_focus() const noexcept { return selection_focus_; }

    // React to a focus MOVE. Called with the new subject, only when it actually changed, so a
    // listener never has to dedup. The production listener re-points the Inspector (wired by
    // `panels::bind_selection_focus`); a second one would be an ordinary additional consumer.
    using FocusListener = std::function<void(const std::string& subject)>;
    void add_focus_listener(FocusListener listener);

    // How many `selection-changed` facts were dropped because they addressed a subject this Shell
    // does not render. A counter rather than a silent skip: the failure this filter exists to prevent
    // is INVISIBLE (a file selection fed to the scene tree as entity id-paths simply shows nothing
    // selected, which is indistinguishable from a correct empty result), so the drop needs an
    // observable of its own for a test to key on.
    [[nodiscard]] std::size_t foreign_subject_facts() const noexcept
    {
        return foreign_subject_facts_;
    }

    // --- the writer half (the two panel seams) ----------------------------------------------------

    [[nodiscard]] std::optional<std::vector<std::string>>
    request_selection(const std::vector<std::string>& ids) override;

    [[nodiscard]] playbar::PlayCommandResult play() override;
    [[nodiscard]] playbar::PlayCommandResult pause() override;
    [[nodiscard]] playbar::PlayCommandResult stop() override;
    [[nodiscard]] playbar::PlayCommandResult step(std::uint64_t ticks) override;

    // The `session.control` write path (editor-window-chrome d1): one verb token
    // (session_bridge.h's `play|pause|stop|step` vocabulary) driven through the SAME
    // `PlaybarModel` transport write the dock panel's invoke path funnelled through until
    // editor-window-chrome e1 retired it — one implementation for the strip and the `play.*`
    // palette commands, the ONLY press paths since e1. `step` advances 1 tick, the button
    // gesture. Returns `std::nullopt` for a verb this build does not name: nothing is dispatched,
    // and the bridge answers a handler error rather than a silent drop.
    [[nodiscard]] std::optional<playbar::PlayAction> control(const std::string& verb);

    // --- the playbar transport (its docked panel retired by editor-window-chrome e1) --------------

    [[nodiscard]] playbar::PlaybarModel& playbar_model() noexcept { return playbar_; }
    [[nodiscard]] const playbar::PlaybarModel& playbar_model() const noexcept { return playbar_; }

    // The provider a PanelHost can bind. Captures `this` — the feed must OUTLIVE the binding.
    // Since e1 retired the docked playbar from the built-in roster, PRODUCTION binds no provider
    // (`install_builtin_panels` creates the feed as pure transport); the command->verb funnel is
    // kept as real transport surface and exercised over a synthetic roster in test_session_feed.cpp.
    // The playbar is a transport: four commands, no gestures, no persisted state (both REPORTED
    // absent rather than stubbed).
    [[nodiscard]] PanelProvider make_provider();

    // --- observability (what the T1 suite asserts on) ---------------------------------------------

    [[nodiscard]] std::uint64_t client_id() const noexcept { return client_id_; }
    [[nodiscard]] std::size_t facts_applied() const noexcept { return facts_applied_; }
    // How many facts were dropped as OUR OWN echo. A count that never moves while the panel writes is
    // itself a signal — it means `origin` is not round-tripping (an unattached client is origin 0).
    [[nodiscard]] std::size_t echoes_dropped() const noexcept { return echoes_dropped_; }
    [[nodiscard]] std::size_t writes_issued() const noexcept { return writes_issued_; }

private:
    // The ONE place a play command becomes a PlayCommandResult (all four transports share it).
    [[nodiscard]] playbar::PlayCommandResult drive_play(const char* method, contract::Json params);

    PanelHost& host_;
    std::string playbar_panel_id_;
    playbar::PlaybarModel playbar_;

    client::Client* client_ = nullptr;
    std::uint64_t client_id_ = 0;

    scenetree::SceneTreePanel* scene_tree_ = nullptr;
    std::string scene_tree_panel_id_;

    std::string selection_focus_ = kSelectionSubjectEntity;
    std::vector<FocusListener> focus_listeners_;

    std::size_t facts_applied_ = 0;
    std::size_t echoes_dropped_ = 0;
    std::size_t writes_issued_ = 0;
    std::size_t foreign_subject_facts_ = 0;
};

} // namespace context::editor::shell::panels
