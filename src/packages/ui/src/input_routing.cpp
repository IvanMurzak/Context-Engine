// UI-side input routing glue implementation (M7 T3 / a3) — see input_routing.h for the seam.

#include "context/packages/ui/input_routing.h"

#include "context/packages/ui/layout.h"
#include "context/packages/ui/ui_tree.h"

#include <utility>

namespace context::packages::ui
{

UiInputRouter::UiInputRouter(UiTree& tree, input::InputRouter& router, std::string ui_context_id)
    : tree_(tree), router_(router), ui_context_id_(std::move(ui_context_id))
{
}

const char* UiInputRouter::focus()
{
    // L-45: push the managed ui context onto the router's active stack (a capturing context becomes
    // modal). push_context is idempotent, so a re-focus is a no-op success.
    const char* code = router_.push_context(ui_context_id_);
    if (code == nullptr)
        focused_ = true;
    return code;
}

const char* UiInputRouter::blur()
{
    // Pop the managed ui context. While focused it is the top layer (the modal/overlay discipline), so
    // pop the top; if it is not active (already popped elsewhere), blur is a no-op success.
    if (!focused_)
        return nullptr;
    focused_ = false;
    if (router_.is_active(ui_context_id_))
        return router_.pop_context();
    return nullptr;
}

bool UiInputRouter::capturing() const noexcept
{
    const input::InputContext* ctx = router_.installed(ui_context_id_);
    return ctx != nullptr && ctx->capture;
}

void UiInputRouter::emit_action(session::ActionActivation action)
{
    // Collected into the current route's output; a no-op outside a route_pointer call (nothing to feed).
    if (routing_)
        pending_.push_back(std::move(action));
}

session::TickInputs UiInputRouter::route_pointer(std::uint64_t tick, const PointerSample& sample)
{
    session::TickInputs out;
    out.tick = tick;

    routing_ = true;
    pending_.clear();

    // Hit-test only when the UI is focused; a blurred UI is inert (the pointer belongs to gameplay).
    const NodeId hit = focused_ ? hit_test(tree_, sample.x, sample.y) : kInvalidNode;

    if (hit != kInvalidNode)
    {
        // The UI owns the pointer: dispatch a target-then-bubble pointer event; the pointer is consumed
        // (never routed to gameplay). Handlers may emit_action gameplay intents into `pending_`.
        Event ev;
        ev.type = sample.event.value != 0 ? EventType::PointerDown : EventType::PointerUp;
        ev.target = hit;
        ev.pointer_x = sample.x;
        ev.pointer_y = sample.y;
        tree_.dispatch(ev);
        out.actions = std::move(pending_);
    }
    else if (focused_ && capturing())
    {
        // Missed every widget while modal: the capturing ui context swallows the pointer (the modal
        // backdrop) — gameplay sees nothing. `out` stays empty.
    }
    else
    {
        // Missed while non-capturing (overlay) or blurred: fall through to gameplay — route the raw
        // device event through the InputRouter exactly as a key-bound source (route() emits only the
        // mapped action layer; its .events are empty).
        session::TickInputs routed = router_.route(tick, {sample.event});
        out.actions = std::move(routed.actions);
    }

    routing_ = false;
    return out;
}

session::TickInputs UiInputRouter::route_events(std::uint64_t tick,
                                                const std::vector<session::InputEvent>& raw)
{
    // Keyboard/gamepad/button arbitration (including the focused-capturing modal swallow) is the
    // router's existing job — forward to it so the caller has one single-sink surface per tick.
    return router_.route(tick, raw);
}

} // namespace context::packages::ui
