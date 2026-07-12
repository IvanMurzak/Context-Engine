// InputRouter — device-events -> mapped-actions routing with UI-vs-gameplay arbitration (see
// input_router.h).

#include "context/packages/input/input_router.h"

#include "context/packages/input/errors.h"

#include <algorithm>

namespace context::packages::input
{

namespace
{
// A binding is well-formed iff every field is non-empty and its device is a recognised source.
[[nodiscard]] bool binding_valid(const Binding& b)
{
    return !b.device.empty() && !b.code.empty() && !b.action.empty() && is_known_device(b.device);
}

// The action-lifecycle phase for a raw event value (deterministic, phase-stateless — see the header).
[[nodiscard]] const char* phase_for(std::int64_t value)
{
    return value != 0 ? kPhasePerformed : kPhaseCanceled;
}
} // namespace

InputContext* InputRouter::find_installed(const std::string& id) noexcept
{
    for (InputContext& c : installed_)
        if (c.id == id)
            return &c;
    return nullptr;
}

const InputContext* InputRouter::installed(const std::string& id) const noexcept
{
    for (const InputContext& c : installed_)
        if (c.id == id)
            return &c;
    return nullptr;
}

bool InputRouter::is_active(const std::string& id) const noexcept
{
    return std::find(stack_.begin(), stack_.end(), id) != stack_.end();
}

const char* InputRouter::install_context(InputContext context)
{
    if (context.id.empty())
        return kInvalidContextCode;
    for (const Binding& b : context.bindings)
        if (!binding_valid(b))
            return kInvalidContextCode;
    if (installed(context.id) != nullptr)
        return kDuplicateContextCode;
    installed_.push_back(std::move(context));
    return nullptr;
}

const char* InputRouter::push_context(const std::string& id)
{
    if (installed(id) == nullptr)
        return kUnknownContextCode;
    if (!is_active(id)) // a re-push of an already-active id is an idempotent no-op
        stack_.push_back(id);
    return nullptr;
}

const char* InputRouter::pop_context()
{
    if (stack_.empty())
        return kUnknownContextCode;
    stack_.pop_back();
    return nullptr;
}

const char* InputRouter::rebind(const std::string& context_id, const std::string& action,
                                const std::string& device, const std::string& code)
{
    if (device.empty() || code.empty() || !is_known_device(device))
        return kInvalidContextCode;
    InputContext* ctx = find_installed(context_id);
    if (ctx == nullptr)
        return kUnknownContextCode;
    for (Binding& b : ctx->bindings)
        if (b.action == action)
        {
            b.device = device;
            b.code = code;
            return nullptr;
        }
    return kUnknownActionCode;
}

session::TickInputs InputRouter::route(std::uint64_t tick,
                                       const std::vector<session::InputEvent>& raw) const
{
    session::TickInputs out;
    out.tick = tick;

    for (const session::InputEvent& e : raw)
    {
        // Walk the active stack from the top (highest priority) down. The first context that binds
        // this (device, code) owns the event; a capturing UI context that does not bind it swallows
        // it (gameplay below never sees it); a non-capturing context lets it fall through.
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it)
        {
            const InputContext* ctx = installed(*it);
            if (ctx == nullptr)
                continue; // an installed context can never be un-installed, but stay defensive

            bool bound = false;
            for (const Binding& b : ctx->bindings)
                if (b.device == e.device && b.code == e.code)
                {
                    out.actions.push_back(
                        session::ActionActivation{b.action, phase_for(e.value), e.value});
                    bound = true;
                    break;
                }
            if (bound)
                break; // consumed by this context
            if (ctx->layer == Layer::Ui && ctx->capture)
                break; // modal capture: swallowed here, gameplay below is blocked (R-SYS-007)
            // else: fall through to the next lower context
        }
        // An event no context consumed is unmapped and dropped (never forked onto the sim path).
    }

    return out;
}

} // namespace context::packages::input
