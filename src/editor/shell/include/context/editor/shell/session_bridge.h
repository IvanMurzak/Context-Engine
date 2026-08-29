// The DAEMON SESSION read surface for editor-core (M9 e08d, design 05 §4 / §6, D7 tier 1).
//
// WHAT THIS IS, AND WHY IT HAS TO EXIST. e08a put the semantic human state — selection, cameras,
// PLAY STATE — in the daemon, and e08b made the Shell a real subscriber of it (`SessionFeed`, the
// `session` topic). editor-core's `when`-contexts (05 §6) need the play state too: a command guarded
// by `playState == playing` is only correct if the browser side can SEE the daemon's play state.
// editor-core is a PURE WIRE-CLIENT of the Shell (04 §1 / 08 §1) — it has no daemon socket and no
// attach token — so the only way that fact reaches it is over this privileged bridge, exactly like
// `keybindings.get` / `themes.get` / `config.get` before it.
//
// WHY A READ AND NOT A PUSH. The e05c bridge accepts NO persistent queries by construction
// (`cef_shell.cpp`: every query completes inside `OnQuery`), so there is no subscription channel to
// the renderer at all. editor-core therefore READS this snapshot — once at boot and then on a cheap
// GENERATION-compare poll, the same shape `themes.get` / `keybindings.get` already use.
//
// THE REPLY IS THE DAEMON'S OWN FACT SHAPE, deliberately. `session.state` answers with the same
// `play-state` payload the daemon publishes on the `session` topic
// (`{event, state, origin}` — docs/editor-session-state.md), so editor-core feeds the reply
// VERBATIM to `DaemonSessionState.applyFact` (when.ts, e08b) with no translation layer that could
// drift. `origin` is `0` — the daemon's own origin — because this is a RELAY of the daemon's state,
// not a change caused by any client; editor-core holds no client id of its own (it is not a wire
// client) and so applies every fact, which is correct: echo suppression already happened Shell-side,
// in `SessionFeed`, where the Shell's own writes are dropped.
//
// CEF-FREE and D10 BOUNDARY-CLEAN, like ipc_bridge.h / keybindings_bridge.h and for the same
// reasons: the handler runs nowhere the local dev gate can reach, so the logic lives here where the
// T1 suite (tests/test_session_bridge.cpp) drives the SAME code the renderer reaches on all three
// default `build` legs, and nothing here touches a kernel-internal module.
//
// THE PROVIDER IS A CALLBACK, not a `SessionFeed&`. `session_feed.h` reaches `scene_tree_panel.h` ->
// the typeid chain, which is not safe to include from the `-fno-rtti` CEF executable; the
// composition root (`editor_main.cpp`) already drives that feed through the non-member seams in
// `builtin_panels.h`, so it hands this bridge a small lambda instead. UNBOUND is a supported,
// HONEST state — it serves the `edit` boot baseline with `attached:false` — which is what lets the
// CEF smokes install the surface without a daemon (see § THE SMOKES below).
//
// ⚠ THE SMOKES MUST INSTALL IT. editor-core calls `session.state` during boot, and the router denies
// unknown methods by DEFAULT, so an uninstalled surface is an `unknown_method` REFUSAL that trips
// every live smoke's strict `bridge.refused() == 0` invariant even though boot.ts degrades
// gracefully — the exact regression e06d shipped with its config surface.
//
// KNOWN STALENESS (CE #356, out of scope here). The daemon publishes play state as a FACT and
// exposes no `play-state` GET verb, so a daemon RESTART leaves the Shell's last-known state with no
// honest repair path. Resetting to `edit` on re-attach was rejected: a dropped wire to a SURVIVING
// daemon would then falsely assert "no live session". `attached` is reported so a consumer can at
// least tell "no link" from "edit", and the real fix is a daemon-side read verb.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ipc_bridge.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace context::editor::shell
{

// The bridge method editor-core calls to read the session snapshot. Grep-stable and MIRRORED by the
// TS side (src/editor/webui/core/src/session.ts `SESSION_STATE_METHOD`); the `webui-panel-contract`
// gate re-reads this value out of the BUILT bundle and compares it to this constant, exactly as it
// does for `keybindings.get` / `themes.get` / `config.*`. A rename on either side would leave
// editor-core calling a method the Shell no longer routes — the browser-side `playState` would
// silently freeze at `edit` again, with NOTHING reporting it. That is the precise regression this
// task exists to remove, so it is mechanised rather than trusted.
inline constexpr const char* kSessionStateMethod = "session.state";

// The daemon's own `play-state` fact discriminator (docs/editor-session-state.md). Mirrored by the
// TS side (when.ts `PLAY_STATE_EVENT`, which `DaemonSessionState.applyFact` compares against) and
// cross-checked by the same gate: a drift here makes every reply silently unrecognised, which reads
// EXACTLY like the frozen-stub bug. Its C++ twin is `panels::kPlayStateEvent` (session_feed.h) —
// two spellings of one wire token that must stay identical; they cannot share a header because
// session_feed.h is not `-fno-rtti`-safe for this library's CEF-side consumers.
inline constexpr const char* kSessionPlayStateEvent = "play-state";

// The L-51 play state with no live session — the boot baseline, and what an UNBOUND bridge serves.
// Byte-identical to `gui::playbar::state_token(PlayState::edit)`; see docs/editor-session-state.md
// § Play state for why `edit` (not "stopped") is the authored-truth token.
inline constexpr const char* kSessionPlayStateEdit = "edit";

// The WRITE half of the session surface (editor-window-chrome d1, target design 02 §7): the ONE
// bridge method the play-bar strip's transport (and the `play.*` palette commands behind it)
// dispatches. Grep-stable and MIRRORED by the TS side (session.ts `SESSION_CONTROL_METHOD`); the
// `webui-panel-contract` gate cross-checks it exactly as it does `session.state`. The handler relays
// to the surviving `SessionFeed` writer — the proven e08b chain (`editor.play|pause|stop|step` with
// its `origin` echo suppression) — so the strip, the palette and the dock panel drive ONE
// implementation. NOT the D19 contract fan-in: that is a later, separate seam (boot.ts keeps its
// honest-refusal stub), and this method carries exactly the four verbs that already have a tested
// writer.
//
// ⚠ THE SMOKES: registered by `install()` alongside `session.state`, so every live CEF smoke that
// installs this bridge serves it with no per-smoke wiring — but each smoke still ASSERTS the method
// routes (`has_method`), the window_bridge.h ten-smoke rule.
inline constexpr const char* kSessionControlMethod = "session.control";

// The `verb` vocabulary `session.control` accepts — the daemon's own transport verbs, minus the
// `editor.` prefix (each maps 1:1 onto `editor.<verb>`). Mirrored by the TS side (session.ts
// `SESSION_CONTROL_VERB_*`) and cross-checked by the same gate: a drifted verb would be refused as
// `session.bad_verb` on every press of a button that looks perfectly wired.
inline constexpr const char* kSessionControlVerbPlay = "play";
inline constexpr const char* kSessionControlVerbPause = "pause";
inline constexpr const char* kSessionControlVerbStop = "stop";
inline constexpr const char* kSessionControlVerbStep = "step";

// The bridge-local error code for a `session.control` request whose `verb` is missing or not in the
// vocabulary above (the `window.bad_params` pattern — a bridge-envelope code, not an R-CLI-008
// catalog code). editor-core only ever sends the four constants, so answering this is a wiring bug
// surfacing loudly, never a user-reachable state.
inline constexpr const char* kSessionControlBadVerbCode = "session.bad_verb";

// What the Shell knows about the live session right now.
struct SessionStateSnapshot
{
    // The L-51 token, byte-identical to `gui::playbar::state_token()`.
    std::string play_state = kSessionPlayStateEdit;
    // Is there a live daemon link behind `play_state` at all? Reported so a consumer can tell
    // "no daemon" from "a daemon that is in edit" — see § KNOWN STALENESS above.
    bool attached = false;
    // Bumped by the provider whenever the relayed state actually moved — a fact applied from ANOTHER
    // client, and (since d1) a transport write THIS Shell adopted, which is invisible to
    // `facts_applied` alone because the daemon's echo of our own write is dropped (session_feed.h).
    // editor-core re-applies only when it moves, so an idle poll is one integer compare (the
    // `keybindings.get` discipline).
    std::uint64_t generation = 0;
    // The running session's simTick, as the daemon last reported it (0 in edit). ADDITIVE
    // (editor-window-chrome d1): the daemon already mints it on every `play-state` fact and every
    // transport reply (kernel_server.cpp), and `SessionFeed` already holds it — relaying it is what
    // makes the strip's `t+` timer daemon truth rather than a browser-local clock. It advances only
    // when a fact/reply lands (CE #356's restart-staleness caveat is inherited, not solved here).
    std::uint64_t sim_tick = 0;
};

// One `session.control` outcome, as the composition root's handler reports it — the model's own
// `PlayAction` vocabulary (playbar_model.h), relayed rather than re-derived:
//
//   * `changed`      — something actually moved (`PlayAction::ok`, which is fed from the daemon's
//                      `changed`). The state/tick below are then the daemon's answer.
//   * `error_code`   — the reserved `play.*` catalog code on a REFUSAL; empty otherwise.
//   * `changed:false` with an EMPTY code deliberately covers all three of "benign no-op", "no
//     daemon link" and "no handler bound" — the model itself does not distinguish them (playbar_
//     model.h: a gateway-less or daemon-less transport reports exactly this), and inventing a
//     distinction here would be a second truth the dock panel does not render.
//   * `play_state`/`sim_tick` — the state AFTER, as the DAEMON last reported it (never a locally
//     computed transition).
struct SessionControlOutcome
{
    bool changed = false;
    std::string error_code;
    std::string play_state = kSessionPlayStateEdit;
    std::uint64_t sim_tick = 0;
};

class SessionBridge
{
public:
    using Provider = std::function<SessionStateSnapshot()>;
    // The `session.control` relay. The composition root binds a lambda over the Shell's
    // `SessionFeed` (through the `panels::session_control` seam — the same forward-declaration
    // discipline as the Provider); the T1 suite binds a scripted one. The handler receives ONLY a
    // verb from the vocabulary above — the bridge validates before dispatching.
    using ControlHandler = std::function<SessionControlOutcome(const std::string& verb)>;

    SessionBridge() = default;

    // Non-copyable and non-movable, like every sibling bridge: `install` binds a handler capturing
    // `this`, and a router outlives nothing that could be relocated out from under it.
    SessionBridge(const SessionBridge&) = delete;
    SessionBridge& operator=(const SessionBridge&) = delete;
    SessionBridge(SessionBridge&&) = delete;
    SessionBridge& operator=(SessionBridge&&) = delete;

    // Point the bridge at the live session. The composition root binds a lambda over the Shell's
    // `SessionFeed` + daemon lifecycle; the T1 suite binds a scripted one. An EMPTY provider (the
    // default, and what the CEF smokes install) is not an error — it serves the `edit` boot baseline
    // with `attached:false`, which is exactly what a Shell with no daemon knows.
    void bind_provider(Provider provider);

    // Point the WRITE half at the live session. An EMPTY handler (the default, and what the CEF
    // smokes install) is not an error: `session.control` then answers the same honest "nothing to
    // drive" a gateway-less PlaybarModel reports — `changed:false` with NO code, the current
    // snapshot's state — rather than refusing, so the smokes' strict `refused() == 0` invariant
    // holds with no daemon anywhere in sight.
    void bind_control(ControlHandler handler);

    // The current snapshot. A provider that THROWS is contained here and degrades to the boot
    // baseline: a session read must never be able to take the renderer's boot down with it.
    [[nodiscard]] SessionStateSnapshot snapshot() const;

    // The snapshot as the `session.state` reply — the daemon's own `play-state` fact shape plus the
    // relay facts (`attached`, `generation`, `simTick`) editor-core needs to poll cheaply.
    [[nodiscard]] contract::Json snapshot_json() const;

    // One `session.control` verb, resolved to the reply object `{changed, state, simTick,
    // errorCode}`. The verb MUST already be validated (install() does); an unbound or THROWING
    // handler degrades to the honest "nothing to drive" answer — this runs on the renderer's query
    // path, exactly like `snapshot()`.
    [[nodiscard]] contract::Json control_json(const std::string& verb);

    // Bind `session.state` AND `session.control` on `router`. False when either binding was refused
    // (a name collision), which the caller must treat as a wiring bug rather than ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // How many times `session.state` was served over the router — non-zero after a live renderer
    // boots is the end-to-end proof the channel is wired (the `keybindings.get` `reads()` pattern).
    [[nodiscard]] std::size_t reads() const { return reads_; }

    // How many `session.control` verbs were dispatched to the handler (valid-verb requests only) —
    // the same wiring evidence for the write half.
    [[nodiscard]] std::size_t controls() const { return controls_; }

private:
    Provider provider_;
    ControlHandler control_;
    std::size_t reads_ = 0;
    std::size_t controls_ = 0;
};

} // namespace context::editor::shell
