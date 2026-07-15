// The ui.* fail-closed error-code strings (M7 T5 / a5, R-UI-006 / R-CLI-008). SOURCE OF TRUTH for the
// codes the contract error catalog registers in its ui.* domain block — the same
// promote-a-local-string pattern as bridge's scope.denied / runtime/ts's kTs*Code / the M6 sim-package
// k*Code blocks — so this package never links the contract layer (the dependency direction stays
// package -> kernel/runtime, per the L-60 microkernel model). They back the headless `context ui …`
// drive/assert verbs (src/cli/ui_command.cpp): loading a UI-scene file, addressing a node, sending a
// synthetic event, and asserting a tree fact. All deterministic refusals (a bare retry cannot conjure
// a missing file/node or make a false assertion true).

#pragma once

namespace context::packages::ui
{

// The named UI-scene file does not exist (not-found class).
inline constexpr const char* kSceneNotFoundCode = "ui.scene_not_found";

// The UI-scene document is malformed, an unsupported version, or names an unknown role/event
// (validation class): nothing was built.
inline constexpr const char* kSceneInvalidCode = "ui.scene_invalid";

// A drive/assert verb named a node (by author `name`) that is not in the tree (not-found class).
inline constexpr const char* kNodeNotFoundCode = "ui.node_not_found";

// `ui send` was malformed: an unknown event kind, or a required field for it was missing (usage class).
inline constexpr const char* kInvalidEventCode = "ui.invalid_event";

// `ui assert` — a stated expectation did not hold over the loaded tree (validation class). This is the
// fail-closed verdict of the "asserted headless via CLI" exit leg.
inline constexpr const char* kAssertionFailedCode = "ui.assertion_failed";

} // namespace context::packages::ui
