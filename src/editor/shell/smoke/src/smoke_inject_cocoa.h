// The Cocoa arm of the smoke-tier real-mode injection seam (M9 e12c-3) — INTERNAL to
// context_editor_shell_smoke_support. See smoke_window.h for the contract and for why macOS injects
// in-process with -[NSApplication postEvent:atStart:] rather than CGEventPost.
//
// WHY A SEPARATE TU PAIR RATHER THAN AN #if INSIDE smoke_window.cpp — the same reason
// cocoa_window.mm / cocoa_window.cpp exist as a pair: CMake picks a compiler by file EXTENSION and
// OBJCXX is enabled only under `if(APPLE)`, so Objective-C++ cannot hide behind a preprocessor guard
// in a `.cpp` every leg compiles. The `.mm` carries the real implementation on Apple; the always-
// compiled `.cpp` carries the honest refusal everywhere else, which is what keeps
// `inject_cocoa_event` a linkable symbol on all three legs (the landed shape of
// `make_cocoa_window_backend` / `make_cocoa_layer_blitter`).
//
// WHY IT IS NOT IN smoke_window.h. The public seam is `inject_event`; this is the platform half it
// dispatches to, and no smoke should ever call it directly — a caller that did would bypass the
// anti-degrade guards `inject_event` applies FIRST (a headless backend, an empty resize).

#pragma once

#include "context/editor/shell/shell.h"
#include "context/editor/shell/window.h"

namespace context::editor::shell::smoke
{

// Deliver `event` to the Cocoa window behind `backend` through the REAL AppKit event queue.
//
// Returns false — always a smoke failure, never a quiet degrade — when this is not an Apple build,
// when the backend carries no CAMetalLayer this process owns a window for (which is itself the
// anti-degrade check: a headless backend reports NativeWindowKind::None), or when the event shape
// has no honest NSEvent encoding (see the .mm for which, and why refusing beats inventing one).
//
// PRECONDITION, enforced by the only caller: `inject_event` has already refused a headless backend
// and already handled the resize arm, so this sees only pointer + key events on a native window.
[[nodiscard]] bool inject_cocoa_event(const IWindowBackend& backend, const ShellEvent& event);

} // namespace context::editor::shell::smoke
