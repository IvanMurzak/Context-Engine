// The LIVE CEF windowed-OSR boot smoke (M9 e04) — ctest `editor-cef-smoke-shell`.
//
// The Session-0-safe smoke (src/editor/shell/smoke/) proves the Shell's own machinery against
// scripted software-OSR frames on every OS leg. This one proves the other half: that a REAL CEF
// browser, driven by the REAL integrated pump, produces frames the REAL compositor composites and
// presents. It is the only place the two meet, and it can only run where CEF links — the per-OS
// `editor-cef-smoke` CI job.
//
// It is deliberately HEADLESS (a windowless browser, no native window) and presents through e03's
// MemoryBlitter, so it is safe on the Session-0 self-hosted Windows runner: no visible window, no
// GPU device, no native-render teardown. The Windows hard exit after success mirrors
// editor_host.cpp / cef_boot_smoke.cpp, skipping CEF's flaky Session-0 teardown.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "context/editor/shell/cef/cef_shell.h"
#include "context/editor/shell/shell.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

namespace shell = context::editor::shell;
namespace render = context::render;
namespace present = context::render::present;

namespace
{

int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (condition)
    {
        return;
    }
    std::fprintf(stderr, "[editor-cef-smoke-shell] FAIL (line %d): %s\n", line, what);
    ++g_failures;
}

#define SMOKE_CHECK(cond, what) check((cond), (what), __LINE__)

std::uint64_t now_us()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// A self-contained placeholder document over a data: URL — no temp file, no scheme handler, and no
// network. It paints a solid, KNOWN colour so the composited output can be asserted rather than
// merely counted.
const char* placeholder_url()
{
    return "data:text/html,<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<title>Context Editor</title></head>"
           "<body style=\"margin:0;background:%23102040\">"
           "<main role=\"main\" aria-label=\"Context Editor placeholder\"></main>"
           "</body></html>";
}

int finish(int code)
{
#if defined(_WIN32)
    // Session-0 carve-out (mirrors cef_boot_smoke.cpp / editor_host.cpp): CEF's teardown is flaky on
    // the self-hosted Windows runner, so exit hard once the verdict is decided.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
#else
    return code;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    // Subprocess re-entry FIRST: CEF's renderer/GPU/utility processes re-exec this binary.
    const int subprocess_exit = shell::cef::execute_subprocess(argc, argv);
    if (subprocess_exit >= 0)
    {
        return subprocess_exit;
    }

    std::printf("[editor-cef-smoke-shell] live windowed-OSR CEF -> compositor -> present\n");

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-shell-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const render::Extent2D size{640, 480};

    shell::WindowDesc desc;
    desc.title = "Context Editor (cef smoke)";
    desc.logical_size = size;
    desc.visible = false;
    // Headless on purpose — see the file header on why this is Session-0-safe.
    auto backend = std::make_unique<shell::HeadlessWindowBackend>(desc);
    shell::HeadlessWindowBackend* backend_raw = backend.get();

    shell::cef::CefShellOptions cef_options;
    cef_options.native_window = nullptr; // windowless: no native window on a Session-0 runner
    cef_options.logical_size = size;
    cef_options.dpi = shell::DpiScale{};
    cef_options.url = placeholder_url();
    // Keep the paint rate low: this smoke wants a FRAME, not a frame rate.
    cef_options.windowless_frame_rate = 10;

    std::string error;
    std::unique_ptr<shell::IBrowserHost> browser =
        shell::cef::make_cef_browser_host(cef_options, error);
    if (browser == nullptr)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell] FAIL: the browser did not start: %s\n",
                     error.c_str());
        return finish(1);
    }

    shell::EditorWindowConfig config;
    // Software OSR — the shipping Windows path per the owner ruling of 2026-07-19.
    config.compositor.import_options.force_software = true;
    config.placement_poll_us = 0;
    auto window = std::make_unique<shell::EditorWindow>(std::move(backend), std::move(browser),
                                                        config);

    // The C-F2 CPU present path with e03's portable blitter: no adapter, no swapchain.
    auto blitter = std::make_unique<present::MemoryBlitter>();
    present::MemoryBlitter* blitter_raw = blitter.get();
    window->compositor().attach_cpu(std::move(blitter), size);

    shell::WindowManager manager(project);
    manager.add(std::move(window));
    shell::EditorWindow* editor = manager.window(0);
    SMOKE_CHECK(editor != nullptr, "the manager adopted the window");
    if (editor == nullptr)
    {
        return finish(1);
    }

    // Drive the integrated pump until the browser has painted and the compositor has presented at
    // least one composited frame. 30s is the same budget editor_host.cpp allows for a headless load.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool presented = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (editor->compositor().stats().view_frames > 0 && blitter_raw->blit_count() > 0)
        {
            presented = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    SMOKE_CHECK(presented, "a real CEF OSR frame was composited and presented within 30s");
    SMOKE_CHECK(editor->compositor().stats().view_frames > 0,
                "the compositor adopted at least one OnPaint frame");
    SMOKE_CHECK(blitter_raw->blit_count() > 0, "the composited frame reached the present blitter");
    SMOKE_CHECK(!editor->compositor().cpu_surface().empty(), "the composed surface is non-empty");

    // Input round-trip into the LIVE browser: this is the assertion that the event translation
    // (DIP positions, modifier flags, CEF event types) is accepted by CEF rather than merely
    // well-formed on our side — a malformed event trips CEF's own checks.
    shell::ShellEvent move;
    move.kind = shell::ShellEventKind::pointer;
    move.pointer.action = shell::PointerAction::move;
    move.pointer.position = shell::PointI{100, 100};
    backend_raw->post(move);

    shell::ShellEvent click = move;
    click.pointer.action = shell::PointerAction::down;
    click.pointer.button = shell::MouseButton::left;
    backend_raw->post(click);

    shell::ShellEvent release = click;
    release.pointer.action = shell::PointerAction::up;
    backend_raw->post(release);

    shell::ShellEvent key;
    key.kind = shell::ShellEventKind::key;
    key.key.action = shell::KeyAction::raw_key_down;
    key.key.windows_key_code = 0x09; // VK_TAB — moves DOM focus, so it is not a no-op
    backend_raw->post(key);

    SMOKE_CHECK(manager.pump_once(now_us()), "the loop ran with live input");
    SMOKE_CHECK(editor->input().pointer_dispatches() == 3, "the pointer samples were arbitrated");
    SMOKE_CHECK(editor->input().key_dispatches() == 1, "the key was arbitrated");

    // A live resize: the browser must accept WasResized and repaint at the new size.
    shell::ShellEvent resize;
    resize.kind = shell::ShellEventKind::resize;
    resize.size = render::Extent2D{800, 500};
    backend_raw->post(resize);
    const int frames_before_resize = editor->compositor().stats().view_frames;
    const auto resize_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool repainted = false;
    while (std::chrono::steady_clock::now() < resize_deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (editor->compositor().stats().view_frames > frames_before_resize)
        {
            repainted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SMOKE_CHECK(repainted, "the browser repainted after a live resize (WasResized)");
    SMOKE_CHECK(editor->compositor().size().width == 800u, "the compositor took the new size");

    manager.shutdown();
    shell::cef::shutdown();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell] PASS: live CEF windowed-OSR composited + presented "
                "(%d frames), input round-tripped, live resize repainted\n",
                blitter_raw->blit_count());
    return finish(0);
}
