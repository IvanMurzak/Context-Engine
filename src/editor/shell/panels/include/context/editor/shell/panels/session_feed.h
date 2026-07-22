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
// WHY THE CLIENT ID IS RE-BOUND ON EVERY ATTACH. Ids are minted per WIRE CONNECTION and never reused
// within a daemon lifetime; a reconnect (a daemon restart, the lifecycle's ladder) mints a NEW one.
// A stale id would suppress a DIFFERENT client's facts and apply our own — both failure modes silent.
// `bind_client` is therefore called from the lifecycle's on_attached hook, not once at construction.
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

// The three fact kinds e08a publishes on it. `camera-changed` is recognised and deliberately IGNORED:
// the viewport/camera UI is e11, and silently dropping an unknown fact would make a future one
// indistinguishable from a bug.
inline constexpr const char* kSelectionChangedEvent = "selection-changed";
inline constexpr const char* kPlayStateEvent = "play-state";
inline constexpr const char* kCameraChangedEvent = "camera-changed";

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

    // Bind the live connection + THIS connection's echo-suppression identity. Called on every
    // (re)attach; `nullptr` + 0 detaches (every write then honestly reports "not delivered").
    void bind_client(client::Client* client, std::uint64_t client_id) noexcept;

    // The scene-tree panel whose RENDERED selection this feed drives, and the roster id to touch when
    // it changes. `nullptr` when the Scene tree did not bind (its provider was refused) — the feed
    // then still drives the playbar, because one unavailable panel must not take the other down.
    void bind_scene_tree(scenetree::SceneTreePanel* panel, std::string panel_id);

    // --- the subscriber half ----------------------------------------------------------------------

    // Consume one subscription event. Returns true when a panel's rendered surface actually changed.
    // A non-`session` topic, an echo of our own write, and an unrecognised fact all return false.
    bool apply_event(const std::string& topic, const contract::Json& payload);

    // --- the writer half (the two panel seams) ----------------------------------------------------

    [[nodiscard]] std::optional<std::vector<std::string>>
    request_selection(const std::vector<std::string>& ids) override;

    [[nodiscard]] playbar::PlayCommandResult play() override;
    [[nodiscard]] playbar::PlayCommandResult pause() override;
    [[nodiscard]] playbar::PlayCommandResult stop() override;
    [[nodiscard]] playbar::PlayCommandResult step(std::uint64_t ticks) override;

    // --- the hosted playbar -----------------------------------------------------------------------

    [[nodiscard]] playbar::PlaybarModel& playbar_model() noexcept { return playbar_; }
    [[nodiscard]] const playbar::PlaybarModel& playbar_model() const noexcept { return playbar_; }

    // The provider to bind on the PanelHost. Captures `this` — the feed must OUTLIVE the binding.
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

    std::size_t facts_applied_ = 0;
    std::size_t echoes_dropped_ = 0;
    std::size_t writes_issued_ = 0;
};

} // namespace context::editor::shell::panels
