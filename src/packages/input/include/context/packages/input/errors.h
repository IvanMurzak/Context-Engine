// The input.* fail-closed error-code strings (M6 P7, R-SYS-007 / L-45). SOURCE OF TRUTH for the codes
// the contract error catalog registers in its F0a-reserved input.* block — the same
// promote-a-local-string pattern as the physics3d/physics2d/particle/anim/spline/audio blocks — so
// this package never links the contract layer (the dependency direction stays package -> kernel/
// runtime, per the L-60 microkernel model).
//
// The input package is the authoring/mapping/ROUTING front-end: it maps raw device events into the
// mapped ACTION layer (session::ActionActivation) and feeds the EXISTING sim InputState sink — it does
// NOT own sim state itself. All its refusals are configuration-time (installing / stacking / rebinding
// contexts), deterministic (a bare retry cannot repair a duplicate id or an unknown action).

#pragma once

namespace context::packages::input
{

// An input context was rejected: an empty context id, or a binding with an empty device / code /
// action (validation class). No context is installed on refusal (fail-closed).
inline constexpr const char* kInvalidContextCode = "input.invalid_context";

// A context could not be installed: its id is already installed (validation class). Nothing is
// overwritten on refusal.
inline constexpr const char* kDuplicateContextCode = "input.duplicate_context";

// An operation named a context id that is not installed (push) or an empty active stack (pop) —
// usage class. The active stack is unchanged on refusal.
inline constexpr const char* kUnknownContextCode = "input.unknown_context";

// A rebind named an action that has no binding in the target context (usage class). No binding is
// repointed on refusal.
inline constexpr const char* kUnknownActionCode = "input.unknown_action";

} // namespace context::packages::input
