// The UI-side input routing glue (M7 T3 / a3, R-SYS-007 / L-45, D5/D6). Binds a headless UiTree to the
// EXISTING input-package InputRouter and the ONE sim InputState sink — it CONSUMES the L-45 UI-capture
// stack, it does not invent input arbitration.
//
// THE SEAM (read first): the input package already owns the deterministic device->action front-end
// (input_router.h) and the headless session already owns the ONE sim-facing input sink
// (Session::inject_action_at -> the world-singleton InputState). InputRouter::route() is a PURE function
// returning session::TickInputs; the CALLER injects the result into the session. This glue adds the
// UI-SIDE half the samples wire by hand today:
//   * FOCUS LIFECYCLE (L-45): a UI gaining focus PUSHES a capturing `ui` InputContext onto the router
//     (modal); losing focus POPS it. The context is the caller's (installed on the router like any
//     other) — the glue drives the existing stack, it does not fork arbitration.
//   * POINTER HIT-TESTING (D5): a pointer sample is hit-tested against a2's computed rects. A hit
//     dispatches a UI pointer event (target-then-bubble) and CONSUMES the pointer — its handlers may
//     emit UI-originated gameplay intents. A miss while the ui context is CAPTURING is swallowed (the
//     modal backdrop — gameplay sees nothing); a miss while NON-capturing (overlay) FALLS THROUGH to
//     gameplay, routed through the InputRouter exactly as a raw device event.
//   * SINGLE SINK (D5): every gameplay activation this glue produces — a UI button's intent AND a
//     fell-through device action — is a session::ActionActivation the caller injects through the SAME
//     Session::inject_action_at path a key-bound action uses. There is no parallel sim-path input.
//
// DETERMINISM (D6): this glue registers NO sim component and the UiTree lives OUTSIDE the sim World.
// sim -> UI is read-only; UI -> sim is ONLY the action path above. So hash_world is bit-identical with
// UI present vs absent — UI is presentation, never sim state (asserted by tests/test_input_routing.cpp).

#pragma once

#include "context/packages/input/input_router.h"
#include "context/packages/ui/events.h"
#include "context/packages/ui/ui_node.h"
#include "context/runtime/session/input.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::packages::ui
{

// Local aliases: the glue speaks the input package's router vocabulary + the session tier's input sink
// vocabulary (the existing types, not a forked representation).
namespace input = ::context::packages::input;
namespace session = ::context::runtime::session;

class UiTree; // operated on by-reference; the .cpp uses the full definition

// One raw pointer sample the UI-side router arbitrates: the underlying device event (device/code/value
// — the fall-through payload) plus its surface-space position (hit-tested against the computed rects).
// `value` follows the router's phase convention: non-zero == a press (PointerDown / "performed"), zero
// == a release (PointerUp / "canceled").
struct PointerSample
{
    session::InputEvent event; // the raw device event fed to the router on a fall-through
    float x = 0.0f;            // surface-space X, hit-tested against a2's computed rects
    float y = 0.0f;            // surface-space Y
};

// The UI-side router->session glue (L-45 CONSUMPTION). Holds a headless UiTree + an existing InputRouter
// and drives the ONE sim InputState sink through them. Not thread-safe (the deterministic single-threaded
// session model); a route call is a synchronous dispatch, so a handler emitting an intent (emit_action)
// runs inside route_pointer.
class UiInputRouter
{
public:
    // Bind `tree` to `router`. `ui_context_id` names a `Layer::Ui` InputContext the caller has ALREADY
    // installed on `router` (the glue does not install/own it — it drives the existing stack). Whether
    // that context CAPTURES (modal) is read live from the router, so a rebind/re-install is honored.
    UiInputRouter(UiTree& tree, input::InputRouter& router, std::string ui_context_id);

    // --- focus lifecycle (L-45: push/pop the managed ui context) ----------------------------------
    // Focus the UI: push the managed ui context onto the router (a capturing context becomes modal).
    // Idempotent. Returns the router's push_context code (nullptr == ok; kUnknownContextCode if the id
    // was never installed).
    const char* focus();
    // Blur the UI: pop the managed ui context if it is the active top. Returns nullptr on success (or
    // when already blurred — a no-op success), or the router's pop_context code.
    const char* blur();
    [[nodiscard]] bool focused() const noexcept { return focused_; }
    // The managed context's live capture flag (true == modal: unbound input is swallowed from gameplay).
    // False if the context is not installed.
    [[nodiscard]] bool capturing() const noexcept;

    // --- UI-originated gameplay intents (single sink, D5) -----------------------------------------
    // Emit a gameplay ActionActivation from a UI handler into the current route's output. Call it from
    // a handler registered on the tree (the handler closes over this glue): a UI button press becomes an
    // action fed through the SAME inject path as a key-bound action. A no-op outside a route_pointer call.
    void emit_action(session::ActionActivation action);

    // --- routing (returns the gameplay TickInputs the caller injects at `tick`) --------------------
    // Route one pointer sample. When focused: hit-test the tree; a HIT dispatches a UI PointerDown/Up
    // event to the top-most node and consumes the pointer (handlers may emit_action gameplay intents);
    // a MISS is swallowed when the ui context is capturing (modal backdrop) or falls through to the
    // router (gameplay) when not. When blurred: the UI is inert — the pointer falls through to gameplay.
    // Returns the gameplay activations to inject at `tick` (empty when swallowed).
    [[nodiscard]] session::TickInputs route_pointer(std::uint64_t tick, const PointerSample& sample);

    // Route non-pointer raw device events (keyboard / gamepad / touch buttons) through the router's
    // EXISTING capture arbitration — a focused capturing ui context swallows unbound events (the
    // "HUD has focus => gameplay sees nothing" rule R-SYS-007). A thin single-sink wrapper over
    // InputRouter::route(): keyboard/button arbitration is the router's job, pointer hit-testing is this
    // glue's. Returns the mapped gameplay activations to inject at `tick`.
    [[nodiscard]] session::TickInputs route_events(std::uint64_t tick,
                                                   const std::vector<session::InputEvent>& raw);

private:
    UiTree& tree_;
    input::InputRouter& router_;
    std::string ui_context_id_;
    std::vector<session::ActionActivation> pending_; // intents emit_action collects during a route call
    bool routing_ = false;                           // true only inside route_pointer (guards emit_action)
    bool focused_ = false;                            // whether the managed ui context is pushed
};

} // namespace context::packages::ui
