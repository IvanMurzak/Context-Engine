// InputRouter — the deterministic device-events -> mapped-actions front-end (M6 P7, R-SYS-007 / L-45).
//
// THE SEAM (read first): the headless session already owns the ONE sim-facing input sink — a
// world-singleton InputState the `input` system folds injected session::ActionActivation /
// session::InputEvent into each tick (src/runtime/session/). The input PACKAGE is the authoring/
// mapping/routing FRONT-END that turns raw device events into that mapped ACTION layer and FEEDS that
// same sink. It emits session::ActionActivation / session::TickInputs — the EXISTING types the sink
// consumes — so there is no parallel sim-path input representation (replay/determinism keep working).
//
// The router holds a set of installed INPUT CONTEXTS and an active STACK of them (top = highest
// priority). route() walks the stack top-down per raw event and applies the R-SYS-007 UI-vs-gameplay
// FOCUS ARBITRATION:
//   * the first context (highest priority) with a binding for the event's (device, code) OWNS it and
//     emits the mapped action;
//   * a capturing UI context that does NOT bind the event still SWALLOWS it — gameplay contexts below
//     never see it (the L-45 modal-capture rule: while a UI layer captures, gameplay gets no input);
//   * a non-capturing (overlay) context that does not bind the event lets it fall through to the next
//     lower context.
// Routing is a PURE, deterministic function of (stack, raw events) — no float, no hidden state — so a
// device->action->InputState pipeline hashes byte-identically on the determinism matrix.
//
// Runtime rebinding repoints a context's binding for an action to a new (device, code) live.

#pragma once

#include "context/packages/input/action_map.h"
#include "context/runtime/session/input.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::packages::input
{

// The router feeds the EXISTING sim input sink, so it speaks the session tier's input vocabulary
// (InputEvent / ActionActivation / TickInputs) rather than a forked one.
namespace session = ::context::runtime::session;

// The action-lifecycle phase the router stamps on a mapped activation, derived deterministically from
// the raw event value: a non-zero value is an active ("performed") activation, a zero value is a
// release ("canceled"). Kept phase-stateless so route() is a pure function (a started/performed split
// would need cross-tick memory and break that purity); the session sink reads value for gameplay
// actions and folds action+phase+value for ui_* actions, so this deterministic mapping is sufficient.
inline constexpr const char* kPhasePerformed = "performed";
inline constexpr const char* kPhaseCanceled = "canceled";

class InputRouter
{
public:
    // Install a context under its unique id. Refuses with:
    //   kInvalidContextCode   — empty id, or a binding with an empty device/code/action, or a binding
    //                           whose device is not a recognised source (is_known_device);
    //   kDuplicateContextCode — the id is already installed (nothing overwritten).
    // A newly installed context is NOT active until push_context() puts it on the stack.
    const char* install_context(InputContext context);

    // Push an installed context id onto the active stack (top = highest priority). The same context id
    // may appear once; a re-push of an already-active id is a no-op success. kUnknownContextCode if the
    // id is not installed.
    const char* push_context(const std::string& id);

    // Pop the top (highest-priority) context off the active stack. kUnknownContextCode if the stack is
    // empty.
    const char* pop_context();

    // Runtime rebinding: repoint `context_id`'s binding for `action` to a new (device, code). The new
    // device must be a recognised source. kUnknownContextCode if the context is not installed;
    // kUnknownActionCode if the context has no binding for `action`; kInvalidContextCode if the new
    // device/code is empty or the device is unknown.
    const char* rebind(const std::string& context_id, const std::string& action,
                       const std::string& device, const std::string& code);

    // Route this tick's raw device events into mapped action activations, honoring the stack priority
    // + the UI-capture focus arbitration above. Pure + deterministic. Returns a session::TickInputs
    // whose `.tick == tick` and whose `.actions` are the mapped activations that FEED the existing
    // InputState sink (its `.events` are left empty — the package's output is the mapped action layer).
    [[nodiscard]] session::TickInputs route(std::uint64_t tick,
                                            const std::vector<session::InputEvent>& raw) const;

    // --- introspection (tests + tooling) ------------------------------------------------------
    [[nodiscard]] std::size_t installed_count() const noexcept { return installed_.size(); }
    [[nodiscard]] std::size_t stack_depth() const noexcept { return stack_.size(); }
    [[nodiscard]] bool is_active(const std::string& id) const noexcept;
    [[nodiscard]] const InputContext* installed(const std::string& id) const noexcept;

private:
    [[nodiscard]] InputContext* find_installed(const std::string& id) noexcept;

    std::vector<InputContext> installed_; // installed contexts, keyed by id
    std::vector<std::string> stack_;      // active context ids; back() is the top (highest priority)
};

} // namespace context::packages::input
