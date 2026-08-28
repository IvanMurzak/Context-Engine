// The Win32 DPI entry-point resolver under the SHIPPING link condition: delay-loaded user32.
//
// `context_editor` links with CEF's standard `/DELAYLOAD:user32.dll`, so at the moment the window
// backend resolves the per-monitor DPI entry points — deliberately BEFORE the process has created a
// window or called any user32 function, because the awareness context must be set before the first
// window exists — user32.dll is NOT resident. A `GetModuleHandleW`-based resolver returns null
// there, every entry point resolves to nullptr, and the shell silently pins itself to 96 DPI
// (measured 2026-08-27: a 150% desktop rendered the whole editor at 1.0x, with the uncovered band
// of the window left black — no diagnostic anywhere, because every layer behaved as designed).
//
// This executable is itself linked with `/DELAYLOAD:user32.dll` (see the target in CMakeLists.txt),
// so it rebuilds that exact condition: with the old resolver the suite goes RED, with the
// `LoadLibraryW` one it stays GREEN. Registered only on MSVC — the delay-load flag is an MSVC
// linker feature, and the local Strawberry-GCC dev gate cannot express it (the same split every
// CEF-linked target already lives with).

#include "context/editor/shell/dpi.h"

#include "shell_test.h"

#if !defined(_WIN32)
#error "test_win32_dpi is registered only on Windows (see CMakeLists.txt)"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using context::editor::shell::win32_apply_per_monitor_dpi_awareness;
using context::editor::shell::Win32DpiApiStatus;
using context::editor::shell::win32_dpi_api_status;

namespace
{

// The delay-load PRECONDITION, asserted rather than assumed: user32 must not be resident before the
// resolver runs, or this suite stops discriminating — with user32 already in the process even the
// broken GetModuleHandleW resolver passes, and every assertion below goes vacuously green. An
// environment that force-injects user32 into every process (an AppInit hook, a compatibility shim)
// therefore turns this test loudly red instead of silently meaningless, which is the honest failure.
// GetModuleHandleW itself lives in kernel32 and never loads anything.
void test_user32_is_not_resident_before_the_resolver_runs()
{
    CHECK(::GetModuleHandleW(L"user32.dll") == nullptr);
}

// The fix under test: resolution must succeed even though user32 was not resident when it ran.
// This call is also what loads user32 — everything after it runs in a user32-resident process,
// which is why the precondition above must come first in main().
void test_resolver_finds_all_three_entry_points_under_delay_load()
{
    const Win32DpiApiStatus status = win32_dpi_api_status();
    CHECK(status.set_process_dpi_awareness_context);
    CHECK(status.get_dpi_for_window);
    CHECK(status.adjust_window_rect_ex_for_dpi);
}

// The reason the resolver exists at all: applying per-monitor-v2 must succeed as the FIRST setter
// in a fresh process (there is no CEF here to have set it earlier) — but ONLY in an interactive
// window session. A SERVICE session refuses the transition outright: the self-hosted CI runner
// (LocalSystem, Session 0) returned FALSE from the very same call on run 33133287819 while both
// resolver checks above passed, disproving this file's first claim that the apply half was
// "Session-0 safe". So the hard assertion holds exactly where the property is provable — every
// interactive run, which includes the local MSVC gate and any desktop box — and Session 0 degrades
// to a LOGGED partial skip, the same shape the x11/cocoa smokes use for a missing display. The
// delay-load regression this executable exists for is fully discriminated by the two checks above,
// which run everywhere.
void test_per_monitor_v2_is_applied()
{
    DWORD session = 0;
    if (::ProcessIdToSessionId(::GetCurrentProcessId(), &session) == 0 || session == 0)
    {
        std::fprintf(stderr, "[test_win32_dpi] SKIP apply-half: service session (session %lu) "
                             "refuses the awareness transition; the resolver checks above still "
                             "ran\n",
                     session);
        return;
    }
    CHECK(win32_apply_per_monitor_dpi_awareness());
}

} // namespace

int main()
{
    // ORDER MATTERS: the precondition is observable only before the resolver's own LoadLibraryW
    // brings user32 into the process.
    test_user32_is_not_resident_before_the_resolver_runs();
    test_resolver_finds_all_three_entry_points_under_delay_load();
    test_per_monitor_v2_is_applied();
    SHELL_TEST_MAIN_END();
}
