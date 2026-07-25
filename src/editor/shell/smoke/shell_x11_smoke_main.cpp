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
// real X server) and cheap in CI, where the `editor-cef-smoke` job already carries a real X display
// (xvfb) plus libx11-dev. Note that job also needs libxext-dev — libx11-dev alone satisfies the
// WINDOW backend but not the present blitter's MIT-SHM probe, so without it this smoke fails at
// --require-x11 with no blitter; ci.yml installs both.
//
// EXIT CODES. 0 = pass. 1 = a real failure. 77 = ctest's SKIP: no X display (or a build configured
// without the X11 headers), which is the ordinary state of the default `build` legs. The skip is
// non-vacuous because the CI job that DOES have a display runs this with --require-x11
// --require-display, under which both of those become hard failures.

#include "context/editor/shell/dpi.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/smoke/smoke_window.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace smoke = context::editor::shell::smoke;
namespace render = context::render;
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

// True when NO texel of the composed surface carries the producer's margin colour. The margin is
// allocation padding that must never reach the window: seeing it means the compose path presented
// the ALLOCATION instead of the visible rect. This is what makes padded_frame's contrasting margin
// worth building — sampling a single interior texel would pass against a stride, origin or
// right/bottom-edge bleed, which is exactly the class of mistake the padded shape exists to catch.
[[nodiscard]] bool free_of(const std::vector<std::uint8_t>& surface, render::Extent2D size,
                           Texel margin)
{
    for (std::uint32_t y = 0; y < size.height; ++y)
    {
        for (std::uint32_t x = 0; x < size.width; ++x)
        {
            const Texel texel = sample(surface, size, x, y);
            if (texel.b == margin.b && texel.g == margin.g && texel.r == margin.r)
            {
                return false;
            }
        }
    }
    return true;
}

// Pump the owner loop until `predicate` holds or the budget runs out. The events this smoke waits
// for come from the X SERVER, so there is nothing to post and no deterministic frame count to
// assume — the round trip is the entire point of waiting.
//
// ⚠ THE BUDGET IS WALL-CLOCK, AND MUST BE. `clock_us` is a FAKE clock the owner loop is merely
// TOLD, and nothing inside pump_once() blocks — Xlib's queue check is non-blocking — so a budget
// counted in ITERATIONS can burn all of them in microseconds and "time out" before the server has
// had any chance to deliver the Expose or ConfigureNotify being waited on. That made both waits
// racy: measured 3 failures in 8 consecutive runs against WSLg's real X server, on a step that is a
// BLOCKING CI gate. Worse, the flake is invisible behind a predicate that is already true, which is
// exactly why the repaint wait looked stable while it was still counting frames_attempted.
// Sleeping between pumps makes the budget mean what it says and gives the server real time to
// answer; the deadline (not the iteration count) is what ends the wait.
template <typename Predicate>
bool pump_until(shell::WindowManager& manager, std::uint64_t& clock_us, Predicate predicate,
                std::chrono::milliseconds budget = std::chrono::seconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;)
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
        if (std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
    //
    // Through the SHARED smoke-tier seam (e12a-x11-legs), not a private copy of the construction:
    // this smoke is the CEF-FREE, locally-runnable proving ground for the seam the nine live
    // `editor-cef-smoke-shell*` scenarios now depend on, so it must exercise the SAME code they do.
    // `WindowMode::real` never degrades — a null backend here is a creation failure or a build with
    // no X11 headers, never a headless substitution.
    shell::WindowDesc desc;
    desc.title = "Context Editor (x11 smoke)";
    desc.logical_size = kWindowSize;

    smoke::WindowSetup selection = smoke::make_smoke_window(desc, smoke::WindowMode::real);
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

    // ------------------------------------------------------- 2. the REAL X11 present blitter
    //
    // Through the seam again, which routes real mode into `EditorWindow::attach_cpu_present()` —
    // the SHIPPING call `context_editor` makes on a GPU-less boot. It selects the blitter from the
    // window's own native handle, so a break there breaks the product and not merely this test, and
    // it refuses the in-memory blitter outright (the degrade real mode exists to forbid).
    const smoke::PresentSetup present_setup =
        smoke::attach_smoke_present(*window, smoke::WindowMode::real);
    if (!present_setup.ok)
    {
        if (require_x11)
        {
            std::fprintf(stderr,
                         "[editor-shell-x11] FAIL: --require-x11 was passed but no X11 present "
                         "blitter exists in this build: %s\n",
                         present_setup.diagnostic.c_str());
            return 1;
        }
        std::printf("[editor-shell-x11] SKIP: %s\n", present_setup.diagnostic.c_str());
        return kSkipExitCode;
    }
    const std::string blitter_name = present_setup.blitter_name;
    X11_CHECK(blitter_name.rfind("x11", 0) == 0,
              "attach_cpu_present resolved to an X11 blitter on Linux");
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
        X11_CHECK(free_of(surface, client, kMarginColor),
                  "no texel of the padded allocation's margin reached the window");
    }

    // ------------------------------------------------------- 6. the X SERVER is the event source
    // request_redraw() asks the server for an Expose; the paint_requested that comes back has made a
    // full client -> server -> client round trip through the real decoder. Nothing is posted here.
    //
    // ⚠ WAIT ON frames_presented, NEVER frames_attempted. render_frame() bumps `frames_attempted`
    // UNCONDITIONALLY, before its damage gate (compositor.cpp), and the owner loop calls it once per
    // pump_once — so a `frames_attempted > before` predicate goes true on the second iteration no
    // matter what the server did, and would still pass with request_redraw(), XClearArea, or the
    // decoder's Expose arm deleted outright. `frames_presented` only advances THROUGH that damage
    // gate, and at this point in the smoke the only thing that can re-damage the compositor is the
    // paint_requested -> mark_external_damage() path: step 5's frame already presented and cleared
    // the damage, the scripted browser's queue is empty, and poll_placement sets placement_dirty_
    // WITHOUT marking damage. So this predicate is true only if the Expose really came back.
    const int presented_before = editor->compositor().stats().frames_presented;
    backend->request_redraw();
    const bool repainted = pump_until(manager, clock_us, [&] {
        return editor->compositor().stats().frames_presented > presented_before;
    });
    X11_CHECK(repainted, "an Expose from the real X server drove a repaint through the owner loop");

    // A REAL resize: ask the server to move+resize the window and wait for the ConfigureNotify that
    // comes back. This is the whole resize protocol (03 §4) over a real window — swapchain-free,
    // but the browser really is told, by the real code path.
    const render::Extent2D before_resize = backend->client_size();
    // The size the browser was last told about BEFORE the resize. It is already non-zero — the owner
    // loop syncs it on the very first pump_once (the browser_size_synced_ latch) — so asserting
    // merely that it is non-zero afterwards proves nothing and would pass with sync_browser_size()
    // deleted from the resize arm. The claim is that it CHANGED.
    const render::Extent2D browser_size_before = browser->last_logical_size();
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
        X11_CHECK(editor->compositor().size().width == backend->client_size().width &&
                      editor->compositor().size().height == backend->client_size().height,
                  "the compositor took the size the X server actually granted");
        const render::Extent2D browser_size_after = browser->last_logical_size();
        X11_CHECK(browser_size_after.width != browser_size_before.width ||
                      browser_size_after.height != browser_size_before.height,
                  "the browser was told about the real resize (WasResized)");
        X11_CHECK(browser_size_after.width ==
                          to_logical(backend->client_size(), backend->dpi()).width &&
                      browser_size_after.height ==
                          to_logical(backend->client_size(), backend->dpi()).height,
                  "the browser was told the LOGICAL size the X server actually granted");
    }

    // The placement the SERVER reports, not the one we asked for — a reparenting window manager is
    // entitled to adjust it, and reading it back is what proves placement() talks to the server.
    const shell::WindowPlacement observed = backend->placement();
    X11_CHECK(!render::is_empty(observed.size()), "placement() read a real geometry back");
    X11_CHECK(observed.monitor.rfind("x11:", 0) == 0,
              "placement() names the X screen it read the geometry from");

    // --------------------------------------- 6b. INPUT the server carried back (e12a-x11-legs)
    //
    // The half e12a never proved and issue #408 is about: a pointer sample and a key press that
    // genuinely made the client -> SERVER -> client round trip and came back through
    // XNextEvent + translate_x11_event, rather than being appended to a queue the test owns.
    // Nothing here can pass with the decoder's pointer/key arms deleted, with the injection
    // deleted, or against the headless backend (the seam refuses that outright).
    //
    // WHY THE COUNTERS ARE READ AS A DELTA and not against a fixed number: the server is entitled
    // to have delivered other input already (a LeaveNotify, which the decoder carries as
    // `PointerAction::leave`), so an absolute count would be asserting whoever's desktop this ran on.
    // A delta of exactly 3 pins that these three samples arrived and that none of them was
    // duplicated by the decoder.
    const int pointer_dispatches_before = editor->input().pointer_dispatches();
    const int key_dispatches_before = editor->input().key_dispatches();

    shell::ShellEvent move;
    move.kind = shell::ShellEventKind::pointer;
    move.pointer.action = shell::PointerAction::move;
    // Inside the client area and OUTSIDE the "scene" viewport region published above, so the sample
    // is arbitrated to the browser rather than swallowed by a viewport — the arbitration is not what
    // this step is proving, but a swallowed sample would still count and hide a real routing break.
    move.pointer.position = shell::PointI{static_cast<std::int32_t>(client.width) - 20, 30};
    X11_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, move),
              "a pointer MOVE was accepted for injection through the X server");

    shell::ShellEvent press = move;
    press.pointer.action = shell::PointerAction::down;
    press.pointer.button = shell::MouseButton::left;
    X11_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, press),
              "a pointer PRESS was accepted for injection through the X server");

    shell::ShellEvent release = press;
    release.pointer.action = shell::PointerAction::up;
    // X reports the mask BEFORE the event, so a release still carries its own button down.
    release.pointer.modifiers.left_button_down = true;
    X11_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, release),
              "a pointer RELEASE was accepted for injection through the X server");

    const bool pointers_arrived = pump_until(manager, clock_us, [&] {
        return editor->input().pointer_dispatches() >= pointer_dispatches_before + 3;
    });
    X11_CHECK(pointers_arrived,
              "the three injected pointer samples came BACK from the real X server and were "
              "arbitrated");
    // ⚠ THE COUNTER IS ASSERTED WITH `>=`, AND THE PRECISION LIVES IN THE BUTTON COUNTS INSTEAD.
    // A real X server is entitled to deliver input this smoke did not inject — a LeaveNotify, which
    // `translate_x11_event` carries as `PointerAction::leave`, whenever the pointer leaves the
    // window (a sibling mapping over it is enough) — so `== before + 3` would be asserting the
    // state of somebody's desktop and would red at random on a developer box. An EnterNotify is
    // NOT such a source: the decoder has no arm for it, so a window merely mapping under the
    // pointer contributes nothing. Button presses and releases have no such source either, so
    // counting them is BOTH robust and strictly more precise than the total ever was (it pins
    // WHICH samples arrived, not merely how many).
    int downs = 0;
    int ups = 0;
    for (const shell::PointerEvent& sample : browser->pointers())
    {
        if (sample.button != shell::MouseButton::left)
        {
            continue;
        }
        if (sample.action == shell::PointerAction::down)
        {
            ++downs;
        }
        else if (sample.action == shell::PointerAction::up)
        {
            ++ups;
        }
    }
    X11_CHECK(downs == 1, "exactly one left-button PRESS arrived — no duplicate decode");
    X11_CHECK(ups == 1, "exactly one left-button RELEASE arrived — no duplicate decode");

    shell::ShellEvent key;
    key.kind = shell::ShellEventKind::key;
    key.key.action = shell::KeyAction::raw_key_down;
    key.key.windows_key_code = 0x09; // VK_TAB — the same key the live CEF boot smoke injects
    X11_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, key),
              "a key PRESS was accepted for injection through the X server");
    const bool key_arrived = pump_until(manager, clock_us, [&] {
        return editor->input().key_dispatches() > key_dispatches_before;
    });
    X11_CHECK(key_arrived,
              "the injected key came BACK from the real X server and was arbitrated");
    // ⚠ `>= 1`, deliberately, and NOT `== 1`: X carries no separate character event, so the decoder
    // SYNTHESIZES one alongside a press that produced text (window.cpp) — a real Tab therefore
    // yields the raw key AND its '\t'. Demanding exactly one would be asserting that this server's
    // keymap produces no text, which is a property of the host, not of the Shell.
    X11_CHECK(editor->input().key_dispatches() >= key_dispatches_before + 1,
              "the key press produced at least the raw-key dispatch");
    // THE KEYSYM ROUND TRIP, and the reason this step is not merely "a key arrived": an injection
    // table entry that is simply WRONG still delivers an event, so a presence-only assertion would
    // pass while the browser received a completely different key. The keycode this smoke sent was
    // derived from VK_TAB through smoke_window's inverse table, decoded back by the SHIPPING
    // `x11_keysym_to_windows_key_code`, and it must arrive as VK_TAB again.
    X11_CHECK(!browser->keys().empty(), "the browser received the injected key");
    if (!browser->keys().empty())
    {
        X11_CHECK(browser->keys().front().windows_key_code == 0x09,
                  "the browser was handed VK_TAB — the key that was actually injected");
        // CEF's Linux native_key_code IS the X11 hardware keycode (window.cpp), so a non-zero value
        // here is the server's own keycode surviving the whole round trip.
        X11_CHECK(browser->keys().front().native_key_code != 0,
                  "the key carried the X server's hardware keycode");
    }

    // The seam's RESIZE arm, which does not go through XSendEvent at all: it asks the window system
    // and waits for the server's own ConfigureNotify. Proven here so all three injection arms have
    // a CEF-free, locally-runnable proof and not just the two the live smokes exercise.
    const render::Extent2D before_injected_resize = backend->client_size();
    shell::ShellEvent shrink;
    shrink.kind = shell::ShellEventKind::resize;
    shrink.size = render::Extent2D{before_injected_resize.width - 40u,
                                   before_injected_resize.height - 30u};
    X11_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, shrink),
              "a resize REQUEST was accepted for injection");
    const bool observed_injected_resize = pump_until(manager, clock_us, [&] {
        const render::Extent2D now = backend->client_size();
        return now.width != before_injected_resize.width ||
               now.height != before_injected_resize.height;
    });
    X11_CHECK(observed_injected_resize,
              "the injected resize came back as a real ConfigureNotify, not a synthesized event");

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
