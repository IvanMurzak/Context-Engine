// The OFF-macOS half of the smoke-tier Cocoa injection arm (M9 e12c-3) — see smoke_inject_cocoa.h
// for why this file exists as the twin of the `.mm` rather than as an `#if` inside one TU.
//
// This is the file the Windows and Linux legs compile, and its honest refusal is what real-mode
// injection resolves to there. On Linux that arm is unreachable (`inject_event` routes to the X11
// encoder when the X11 headers are present); on Windows it IS the answer, and it preserves exactly
// the behaviour that shipped before this task: real-mode injection has no implementation on the
// Session-0 runner, and saying so is the whole contract (every caller treats false as a failure).

#include "smoke_inject_cocoa.h"

#if !defined(__APPLE__)

namespace context::editor::shell::smoke
{

bool inject_cocoa_event(const IWindowBackend& backend, const ShellEvent& event)
{
    (void)backend;
    (void)event;
    return false;
}

} // namespace context::editor::shell::smoke

#endif // !__APPLE__
