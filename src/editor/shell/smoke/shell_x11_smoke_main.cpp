// The M9 e12a LIVE WINDOWED LINUX SMOKE — the DoD line "the editor boots windowed with live panels
// under Linux", proven against a REAL X server.
//
// WHY THIS EXISTS ALONGSIDE THE SESSION-0 SMOKE. `editor-shell-smoke-session0` deliberately opens no
// window: the self-hosted Windows runner has no interactive desktop, so the blocking cross-OS proof
// has to drive the HEADLESS backend. That makes it structurally unable to say anything about the one
// thing e12a adds — whether an X11 window really opens, really delivers OS events, and really
// receives pixels. So this smoke is its mirror image: same real owner loop, same real compositor,
// same real software-OSR frames, same real panel models, but through
//
//   * a REAL X11 window created by the REAL make_window_backend (not a test double),
//   * the REAL X11-SHM present blitter (XShmPutImage, or XPutImage where SHM is refused),
//   * the REAL X server as the event source — the repaint and the resize below are OBSERVED coming
//     back from the server, never posted into a queue by the test.
//
// and it asserts the LIVE panel models the Shell would render: the e05d1 composition root binds
// every hostable provider on a real PanelHost and each one is rendered here, so "with live panels"
// is a checked claim rather than a screenshot someone looked at once.
//
// NOTHING HERE LINKS CEF. That is what makes it runnable on the local dev host (WSL/WSLg exposes a
// real X server) and cheap in CI, where the `editor-cef-smoke` job already carries libx11-dev + xvfb.
//
// EXIT CODES. 0 = pass. 1 = a real failure. 77 = ctest's SKIP: no X display (or a build configured
// without the X11 headers), which is the ordinary state of the default `build` legs. The skip is
// non-vacuous because the CI job that DOES have a display runs this with --require-x11
// --require-display, under which both of those become hard failures.

#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/shell.h"
#include "context/render/present/present_blit.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace render = context::render;
namespace present = context::render::present;
namespace fs = std::filesystem;

namespace
{

int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (condition)
    {
        return;
    }
    std::fprintf(stderr, "[editor-shell-x11] FAIL (line %d): %s\n", line, what);
    ++g_failures;
}

#define X11_CHECK(cond, what) check((cond), (what), __LINE__)

constexpr int kSkipExitCode = 77; // ctest SKIP_RETURN_CODE

const render::Extent2D kWindowSize{320, 200};

struct Texel
{
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 0;
};

Texel sample(const std::vector<std::uint8_t>& surface, render::Extent2D size, std::uint32_t x,
             std::uint32_t y)
{
    Texel texel;
    const std::size_t offset = (static_cast<std::size_t>(y) * size.width + x) * 4u;
    if (offset + 3u >= surface.size())
    {
        return texel;
    }
    texel.b = surface[offset + 0];
    texel.g = surface[offset + 1];
    texel.r = surface[offset + 2];
    texel.a = surface[offset + 3];
    return texel;
}

// A software-OSR producer frame with an allocation LARGER than the visible rect at a PADDED stride,
// the visible area at a non-zero origin inside it, and a contrasting margin — the same shape the
// Session-0 smoke uses, and for the same reason: a uniformly-filled padded allocation would catch
// none of the UV/stride/origin mistakes it exists to catch.
std::vector<std::uint8_t> padded_frame(render::Extent2D coded, std::uint32_t bytes_per_row,
                                       const render::Rect2D& visible, Texel content, Texel margin)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes_per_row) * coded.height, 0u);
    for (std::uint32_t y = 0; y < coded.height; ++y)
    {
        for (std::uint32_t x = 0; x < coded.width; ++x)
        {
            const bool inside = x >= visible.origin.x && y >= visible.origin.y &&
                                x < visible.origin.x + visible.size.width &&
                                y < visible.origin.y + visible.size.height;
            const Texel& source = inside ? content : margin;
            std::uint8_t* texel = pixels.data() + static_cast<std::size_t>(y) * bytes_per_row +
                                  static_cast<std::size_t>(x) * 4u;
            texel[0] = source.b;
            texel[1] = source.g;
            texel[2] = source.r;
            texel[3] = source.a;
        }
    }
    return pixels;
}

// Pump the owner loop until `predicate` holds or the budget runs out. The events this smoke waits
// for come from the X SERVER, so there is nothing to post and no deterministic frame count to
// assume — the round trip is the entire point of waiting.
template <typename Predicate>
bool pump_until(shell::WindowManager& manager, std::uint64_t& clock_us, Predicate predicate,
                int max_iterations = 400)
{
    for (int i = 0; i < max_iterations; ++i)
    {
        if (predicate())
        {
            return true;
        }
        clock_us += 1'000;
        if (!manager.pump_once(clock_us))
        {
            break;
        }
    }
    return predicate();
}

bool has_flag(int argc, char** argv, const char* flag)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], flag) == 0)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    const bool require_x11 = has_flag(argc, argv, "--require-x11");
    const bool require_display = has_flag(argc, argv, "--require-display");

    std::printf("[editor-shell-x11] live windowed Linux smoke: real X11 window + real X11 present\n");

    // ------------------------------------------------------- 1. a REAL window, or an honest skip
    shell::WindowDesc desc;
    desc.title = "Context Editor (x11 smoke)";
    desc.logical_size = kWindowSize;
    desc.visible = true;

    shell::WindowBackendSelection selection = shell::make_window_backend(desc);
    if (selection.backend == nullptr)
    {
        if (require_x11 || require_display)
        {
            std::fprintf(stderr,
                         "[editor-shell-x11] FAIL: --require-x11/--require-display was passed but no "
                         "native window could be created: %s\n",
                         selection.diagnostic.c_str());
            return 1;
        }
        std::printf("[editor-shell-x11] SKIP: %s\n", selection.diagnostic.c_str());
        return kSkipExitCode;
    }

    shell::IWindowBackend* backend = selection.backend.get();
    X11_CHECK(std::strcmp(backend->name(), "x11") == 0,
              "make_window_backend resolved to the X11 backend on Linux");
    const render::NativeWindowDesc native = backend->native_window();
    X11_CHECK(native.kind == render::NativeWindowKind::XlibWindow,
              "the backend reports an Xlib native window");
    X11_CHECK(native.handle != nullptr, "the native window carries a Window XID");
    X11_CHECK(native.display != nullptr, "the native window carries the Display connection");
    X11_CHECK(!render::is_empty(backend->client_size()),
              "the X server reported a non-empty client area");
    // The DPI came from the desktop (Xft.dpi) or the screen derivation; either way it must be a
    // usable scale rather than the zero an unclamped read would produce.
    X11_CHECK(backend->dpi().dpi >= shell::kMinDpi && backend->dpi().dpi <= shell::kMaxDpi,
              "the X11 DPI lookup produced a clamped, usable scale");

    // ------------------------------------------------------- 2. the REAL X11 present blitter
    present::BlitterSelection blit = present::make_present_blitter(native);
    if (blit.blitter == nullptr)
    {
        if (require_x11)
        {
            std::fprintf(stderr,
                         "[editor-shell-x11] FAIL: --require-x11 was passed but no X11 present "
                         "blitter exists in this build: %s\n",
                         blit.diagnostic.c_str());
            return 1;
        }
        std::printf("[editor-shell-x11] SKIP: %s\n", blit.diagnostic.c_str());
        return kSkipExitCode;
    }
    const std::string blitter_name = blit.blitter->name();
    X11_CHECK(blitter_name.rfind("x11", 0) == 0,
              "make_present_blitter resolved to an X11 blitter on Linux");

    std::error_code ec;
    const fs::path project = fs::temp_directory_path(ec) / "context-editor-shell-x11-smoke";
    fs::remove_all(project, ec);
    fs::create_directories(project, ec);

    // ------------------------------------------------------- 3. the real shell over that window
    auto browser_owned = std::make_unique<shell::ScriptedBrowserHost>();
    shell::ScriptedBrowserHost* browser = browser_owned.get();

    shell::EditorWindowConfig config;
    // The L-41 switch, set deliberately: this smoke proves the SOFTWARE-OSR + CPU-present path,
    // which is exactly what a Linux box with no usable adapter falls back to (C-F2).
    config.compositor.import_options.force_software = true;
    config.placement_poll_us = 0;

    const render::Extent2D client = backend->client_size();
    auto window = std::make_unique<shell::EditorWindow>(std::move(selection.backend),
                                                        std::move(browser_owned), config);
    window->compositor().attach_cpu(std::move(blit.blitter), client);
    X11_CHECK(window->compositor().path() == shell::PresentPath::cpu_blit,
              "the compositor took the C-F2 CPU present path");
    X11_CHECK(window->compositor().diagnostic().empty(),
              "the X11 CPU present path attached with no diagnostic");

    shell::WindowManager manager(project);
    manager.add(std::move(window));
    shell::EditorWindow* editor = manager.window(0);
    X11_CHECK(editor != nullptr, "the manager adopted the window");
    if (editor == nullptr)
    {
        return 1;
    }
    editor->input().regions().publish(
        {shell::ShellRegion{"scene",
                            render::Rect2D{render::Origin2D{0, 0},
                                           render::Extent2D{client.width / 2u, client.height}},
                            shell::RegionKind::viewport}});

    // ------------------------------------------------------- 4. LIVE PANELS (the e05d1 root)
    // The same composition root `context_editor` uses, over the same real roster. Rendering each
    // hostable panel is what makes "boots WITH LIVE PANELS" a checked claim.
    shell::PanelHost panel_host;
    panels::BuiltinPanels builtin = panels::install_builtin_panels(panel_host);
    X11_CHECK(builtin.bound == panels::hostable_panel_ids().size(),
              "every hostable panel provider bound on the real roster");
    X11_CHECK(panel_host.roster_size() > 0u, "the built-in roster is non-empty");
    X11_CHECK(panel_host.hosted_count() == panels::hostable_panel_ids().size(),
              "the host reports exactly the hostable panels as hosted");
    for (const std::string& panel_id : panels::hostable_panel_ids())
    {
        std::string error_code;
        const auto rendered = panel_host.render(panel_id, error_code);
        X11_CHECK(rendered.has_value(), "a hosted panel rendered a live model");
        if (!rendered.has_value())
        {
            std::fprintf(stderr, "[editor-shell-x11]   panel '%s' -> %s\n", panel_id.c_str(),
                         error_code.c_str());
        }
    }

    // ------------------------------------------------------- 5. a frame reaches the real window
    const render::Extent2D coded{client.width + 24, client.height + 16};
    const std::uint32_t kPaddedStride = coded.width * 4u + 64u;
    const render::Rect2D kViewVisible{render::Origin2D{8, 6}, client};
    const Texel kViewColor{20, 40, 60, 255};
    const Texel kMarginColor{7, 7, 7, 255};

    std::uint64_t clock_us = 1'000;
    browser->queue_frame(shell::BrowserLayer::view, coded, kViewVisible,
                         padded_frame(coded, kPaddedStride, kViewVisible, kViewColor, kMarginColor),
                         kPaddedStride);
    X11_CHECK(manager.pump_once(clock_us), "the owner loop ran over the real X11 window");
    X11_CHECK(editor->compositor().stats().view_frames == 1, "the view frame was adopted");
    X11_CHECK(editor->compositor().stats().frames_presented >= 1,
              "a composited frame was PRESENTED through the X11 blitter");
    {
        const std::vector<std::uint8_t>& surface = editor->compositor().cpu_surface();
        X11_CHECK(surface.size() ==
                      static_cast<std::size_t>(client.width) * client.height * 4u,
                  "the composed surface is the window's client extent");
        const Texel texel = sample(surface, client, 5, 5);
        X11_CHECK(texel.b == 20 && texel.g == 40 && texel.r == 60,
                  "the composed surface carries the browser's premultiplied BGRA pixels");
    }

    // ------------------------------------------------------- 6. the X SERVER is the event source
    // request_redraw() asks the server for an Expose; the paint_requested that comes back has made a
    // full client -> server -> client round trip through the real decoder. Nothing is posted here.
    const int paints_before = editor->compositor().stats().frames_attempted;
    backend->request_redraw();
    const bool repainted = pump_until(manager, clock_us, [&] {
        return editor->compositor().stats().frames_attempted > paints_before;
    });
    X11_CHECK(repainted, "an Expose from the real X server drove a repaint through the owner loop");

    // A REAL resize: ask the server to move+resize the window and wait for the ConfigureNotify that
    // comes back. This is the whole resize protocol (03 §4) over a real window — swapchain-free,
    // but the browser really is told, by the real code path.
    const render::Extent2D before_resize = backend->client_size();
    shell::WindowPlacement resized;
    resized.x = 40;
    resized.y = 60;
    resized.width = before_resize.width + 120u;
    resized.height = before_resize.height + 80u;
    backend->apply_placement(resized);
    const bool observed_resize = pump_until(manager, clock_us, [&] {
        const render::Extent2D now = backend->client_size();
        return now.width != before_resize.width || now.height != before_resize.height;
    });
    X11_CHECK(observed_resize, "a ConfigureNotify from the real X server resized the shell");
    if (observed_resize)
    {
        X11_CHECK(editor->compositor().size().width == backend->client_size().width,
                  "the compositor took the size the X server actually granted");
        X11_CHECK(browser->last_logical_size().width != 0u,
                  "the browser was told about the real resize (WasResized)");
    }

    // The placement the SERVER reports, not the one we asked for — a reparenting window manager is
    // entitled to adjust it, and reading it back is what proves placement() talks to the server.
    const shell::WindowPlacement observed = backend->placement();
    X11_CHECK(!render::is_empty(observed.size()), "placement() read a real geometry back");
    X11_CHECK(observed.monitor.rfind("x11:", 0) == 0,
              "placement() names the X screen it read the geometry from");

    // ------------------------------------------------------- 7. teardown persists the session
    manager.shutdown();
    X11_CHECK(manager.state_store().write_count() >= 1,
              "the shutdown flushed the pending session state");
    X11_CHECK(fs::exists(shell::editor_state_path(project)),
              ".editor/editor-state.json was written by the Shell");
    fs::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-shell-x11] FAILED with %d assertion failure(s)\n", g_failures);
        return 1;
    }
    std::printf("[editor-shell-x11] PASS: real X11 window, %s present, %zu live panels rendered, "
                "server-driven repaint + resize observed, session persisted\n",
                blitter_name.c_str(), panels::hostable_panel_ids().size());
    return 0;
}
