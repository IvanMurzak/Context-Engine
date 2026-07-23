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

// What the Shell knows about the live session right now.
struct SessionStateSnapshot
{
    // The L-51 token, byte-identical to `gui::playbar::state_token()`.
    std::string play_state = kSessionPlayStateEdit;
    // Is there a live daemon link behind `play_state` at all? Reported so a consumer can tell
    // "no daemon" from "a daemon that is in edit" — see § KNOWN STALENESS above.
    bool attached = false;
    // Bumped by the provider whenever a fact actually moved the state. editor-core re-applies only
    // when it moves, so an idle poll is one integer compare (the `keybindings.get` discipline).
    std::uint64_t generation = 0;
};

class SessionBridge
{
public:
    using Provider = std::function<SessionStateSnapshot()>;

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

    // The current snapshot. A provider that THROWS is contained here and degrades to the boot
    // baseline: a session read must never be able to take the renderer's boot down with it.
    [[nodiscard]] SessionStateSnapshot snapshot() const;

    // The snapshot as the `session.state` reply — the daemon's own `play-state` fact shape plus the
    // two relay facts (`attached`, `generation`) editor-core needs to poll cheaply.
    [[nodiscard]] contract::Json snapshot_json() const;

    // Bind `session.state` on `router`. False when the binding was refused (a name collision), which
    // the caller must treat as a wiring bug rather than ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // How many times `session.state` was served over the router — non-zero after a live renderer
    // boots is the end-to-end proof the channel is wired (the `keybindings.get` `reads()` pattern).
    [[nodiscard]] std::size_t reads() const { return reads_; }

private:
    Provider provider_;
    std::size_t reads_ = 0;
};

} // namespace context::editor::shell
