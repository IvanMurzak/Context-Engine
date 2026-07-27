// The M9 e12c-3 LIVE WINDOWED macOS SMOKE — the DoD line "Editor boots windowed with live panels on
// the macOS GH runner", proven against a REAL NSWindow.
//
// WHY THIS EXISTS, and why e12b could not supply it. e12b landed the whole Cocoa backend (NSWindow +
// a CAMetalLayer-backed NSView, the pure NSEvent decoder, the CALayer.contents CPU blitter) and said
// so honestly in its own record: "a window appears / a frame is visible is asserted NOWHERE". macOS
// runs all nine `editor-cef-smoke-shell*` smokes HEADLESS, so nothing in the tree proved a real macOS
// window ever opens, ever receives pixels, or ever delivers an OS event. This smoke is the exact
// mirror of `context_editor_shell_x11_smoke` (M9 e12a) for macOS:
//
//   * a REAL NSWindow created by the REAL make_window_backend (not a test double),
//   * the REAL CALayer.contents present blitter, selected by the SHIPPING attach_cpu_present() from
//     the REAL native layer,
//   * the REAL AppKit queue as the event source — the pointer and key samples below are posted with
//     -[NSApplication postEvent:atStart:] and come BACK out of the backend's own
//     nextEventMatchingMask pump through the real translate_ns_event,
//   * the REAL e05d1 panel composition root, so "with live panels" is a checked claim.
//
// NOTHING HERE LINKS CEF, and that is the point: it is unaffected by the CEF keychain class (x7 /
// issue #437), it costs the CI job only a build, and it runs on the local macOS dev host in seconds.
//
// FOUR CLAIMS THIS SMOKE DELIBERATELY DOES NOT MAKE, each because the platform cannot support it —
// recorded so the gap is visible rather than assumed covered:
//
//   1. NO OS-DRIVEN REPAINT. The x11 smoke asserts an Expose coming back from the X server. Cocoa has
//      no window-event QUEUE at all (cocoa_window.mm shape 2): geometry is POLLED and diffed, and
//      there is no paint event to observe — `request_redraw()` is `setNeedsDisplay:`, which drives
//      AppKit's own drawing, not a ShellEvent. The OS-sourced claim here is the RESIZE instead, which
//      IS observed coming back through the geometry diff.
//   2. NO EXACT POINTER POSITION. MEASURED: AppKit round-trips a posted mouse location through the
//      window server and returns it scaled about the window's centre (~1.4% on the reproduction host;
//      a 640-point window's edges came back 4.5 points out). So the position claim is the Y-FLIP
//      DIRECTION plus ORDER and SEPARATION — properties no such scaling can invert — and never an
//      equality. See smoke_window.h.
//   3. NO PRESSED-BUTTON MODIFIER. `+[NSEvent pressedMouseButtons]` is a live HID query, so an
//      injected press cannot set `Modifiers::left_button_down`. The button IDENTITY still travels (it
//      is the event TYPE), which is what the press/release counts below assert.
//   4. NO FOCUS ASSERTION. `windowDidBecomeKey` needs the app to actually become active, which is not
//      a property of the Shell and not one a CI runner owes us. Asserting it would be the flaky
//      blocking gate e12a already shipped once.
//
// EXIT CODES. 0 = pass. 1 = a real failure. 77 = ctest's SKIP: no GUI (Aqua) session. That is the
// ordinary state of the `build (macos-latest)` leg — the ONLY default leg this target exists on at
// all, since it is created inside `if(APPLE)` while both `sanitize` legs are ubuntu. The skip is
// non-vacuous because the CI job that DOES have a session runs this with --require-cocoa
// --require-display, under which a missing session, a refused window and a missing OS blitter are
// all hard failures.

#include "context/editor/shell/dpi.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/smoke/smoke_window.h"

#include <chrono>
#include <cstddef>
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
    std::fprintf(stderr, "[editor-shell-cocoa] FAIL (line %d): %s\n", line, what);
    ++g_failures;
}

#define COCOA_CHECK(cond, what) check((cond), (what), __LINE__)

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
// Session-0 and X11 smokes use, and for the same reason: a uniformly-filled padded allocation would
// catch none of the UV/stride/origin mistakes it exists to catch.
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
// allocation padding that must never reach the window: seeing it means the compose path presented the
// ALLOCATION instead of the visible rect. Sampling a single interior texel would pass against a
// stride, origin or right/bottom-edge bleed, which is the class the padded shape exists to catch.
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

// Pump the owner loop until `predicate` holds or the budget runs out.
//
// WARNING THE BUDGET IS WALL-CLOCK, AND MUST BE — the same lesson e12a paid for on the X11 side.
// `clock_us` is a FAKE clock the owner loop is merely TOLD, and nothing inside pump_once() blocks
// (Cocoa's nextEventMatchingMask uses distantPast), so a budget counted in ITERATIONS burns all of
// them in microseconds and "times out" before AppKit has had any chance to deliver the posted event
// or the window server to grant the resize. Sleeping between pumps makes the budget mean what it
// says; the deadline, not the iteration count, is what ends the wait.
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
    const bool require_cocoa = has_flag(argc, argv, "--require-cocoa");
    const bool require_display = has_flag(argc, argv, "--require-display");

    std::printf("[editor-shell-cocoa] live windowed macOS smoke: real NSWindow + real CALayer "
                "present\n");

    // ----------------------------------------------------- 1. a REAL window, or an honest skip
    //
    // Through the SHARED smoke-tier seam, not a private copy of the construction — this smoke is the
    // CEF-FREE, locally-runnable proving ground for the Cocoa arm of the seam. `WindowMode::real`
    // never degrades: a null backend here is a refused window or a box with no GUI session, never a
    // headless substitution.
    //
    // NOTE the SKIP path is also how the window-server-session question gets answered with real
    // output rather than an assumption: `make_smoke_window`'s diagnostic is the Cocoa backend's own
    // CGSessionCopyCurrentDictionary report, printed verbatim below, and the CI step that must have a
    // session passes --require-display so the same condition is a hard failure there.
    shell::WindowDesc desc;
    desc.title = "Context Editor (cocoa smoke)";
    desc.logical_size = kWindowSize;

    smoke::WindowSetup selection = smoke::make_smoke_window(desc, smoke::WindowMode::real);
    if (selection.backend == nullptr)
    {
        if (require_cocoa || require_display)
        {
            std::fprintf(stderr,
                         "[editor-shell-cocoa] FAIL: --require-cocoa/--require-display was passed "
                         "but no native window could be created: %s\n",
                         selection.diagnostic.c_str());
            return 1;
        }
        std::printf("[editor-shell-cocoa] SKIP: %s\n", selection.diagnostic.c_str());
        return kSkipExitCode;
    }

    shell::IWindowBackend* backend = selection.backend.get();
    COCOA_CHECK(std::strcmp(backend->name(), "cocoa") == 0,
                "make_window_backend resolved to the Cocoa backend on macOS");
    const render::NativeWindowDesc native = backend->native_window();
    // rhi.h's MetalLayer contract: the handle is the CAMetalLayer backing the NSView, and it is the
    // LAYER not the view because both the GPU surface and the CPU present fallback address the layer.
    COCOA_CHECK(native.kind == render::NativeWindowKind::MetalLayer,
                "the backend reports a Metal layer as its native window");
    COCOA_CHECK(native.handle != nullptr, "the native window carries the CAMetalLayer");
    // Unlike X11's (Display*, Window) pair a CAMetalLayer is self-contained, so the second handle
    // must stay EMPTY — a non-null one would mean the Cocoa backend had grown an X11-shaped contract.
    COCOA_CHECK(native.display == nullptr, "the Metal layer needs no second (display) handle");
    COCOA_CHECK(!render::is_empty(backend->client_size()),
                "AppKit reported a non-empty client area");
    // Derived from a REAL -backingScaleFactor; either way it must be a usable scale rather than the
    // zero an unclamped read of a not-yet-on-screen window would produce.
    COCOA_CHECK(backend->dpi().dpi >= shell::kMinDpi && backend->dpi().dpi <= shell::kMaxDpi,
                "the backingScaleFactor derivation produced a clamped, usable scale");

    std::error_code ec;
    const fs::path project = fs::temp_directory_path(ec) / "context-editor-shell-cocoa-smoke";
    fs::remove_all(project, ec);
    fs::create_directories(project, ec);

    // ----------------------------------------------------- 2. the real shell over that window
    auto browser_owned = std::make_unique<shell::ScriptedBrowserHost>();
    shell::ScriptedBrowserHost* browser = browser_owned.get();

    shell::EditorWindowConfig config;
    // The L-41 switch, set deliberately: this smoke proves the SOFTWARE-OSR + CPU-present path, which
    // is exactly what a Mac with no usable adapter falls back to (C-F2).
    config.compositor.import_options.force_software = true;
    config.placement_poll_us = 0;

    const render::Extent2D client = backend->client_size();
    auto window = std::make_unique<shell::EditorWindow>(std::move(selection.backend),
                                                        std::move(browser_owned), config);

    // ----------------------------------------------------- 3. the REAL CALayer present blitter
    //
    // Through the seam again, which routes real mode into `EditorWindow::attach_cpu_present()` — the
    // SHIPPING call `context_editor` makes on a GPU-less boot. It selects the blitter from the
    // window's own native layer, so a break there breaks the product and not merely this test, and it
    // refuses the in-memory blitter outright (the degrade real mode exists to forbid).
    const smoke::PresentSetup present_setup =
        smoke::attach_smoke_present(*window, smoke::WindowMode::real);
    if (!present_setup.ok)
    {
        if (require_cocoa)
        {
            std::fprintf(stderr,
                         "[editor-shell-cocoa] FAIL: --require-cocoa was passed but no macOS present "
                         "blitter exists in this build: %s\n",
                         present_setup.diagnostic.c_str());
            return 1;
        }
        std::printf("[editor-shell-cocoa] SKIP: %s\n", present_setup.diagnostic.c_str());
        return kSkipExitCode;
    }
    const std::string blitter_name = present_setup.blitter_name;
    COCOA_CHECK(blitter_name == "cocoa-calayer",
                "attach_cpu_present resolved to the CALayer.contents blitter on macOS");
    COCOA_CHECK(window->compositor().path() == shell::PresentPath::cpu_blit,
                "the compositor took the C-F2 CPU present path");
    COCOA_CHECK(window->compositor().diagnostic().empty(),
                "the macOS CPU present path attached with no diagnostic");

    shell::WindowManager manager(project);
    manager.add(std::move(window));
    shell::EditorWindow* editor = manager.window(0);
    COCOA_CHECK(editor != nullptr, "the manager adopted the window");
    if (editor == nullptr)
    {
        return 1;
    }
    // A viewport region over the LEFT half, so an injected sample on the RIGHT is arbitrated to the
    // browser rather than swallowed. Sized in halves rather than in absolute pixels precisely because
    // the injected position is only approximately delivered (see the file header, claim 2): the
    // samples below sit deep inside the right half, far from this boundary.
    editor->input().regions().publish(
        {shell::ShellRegion{"scene",
                            render::Rect2D{render::Origin2D{0, 0},
                                           render::Extent2D{client.width / 2u, client.height}},
                            shell::RegionKind::viewport}});

    // ----------------------------------------------------- 4. LIVE PANELS (the e05d1 root)
    // The same composition root `context_editor` uses, over the same real roster. Rendering each
    // hostable panel is what makes "boots WITH LIVE PANELS" a checked claim.
    shell::PanelHost panel_host;
    panels::BuiltinPanels builtin = panels::install_builtin_panels(panel_host);
    COCOA_CHECK(builtin.bound == panels::hostable_panel_ids().size(),
                "every hostable panel provider bound on the real roster");
    COCOA_CHECK(panel_host.roster_size() > 0u, "the built-in roster is non-empty");
    COCOA_CHECK(panel_host.hosted_count() == panels::hostable_panel_ids().size(),
                "the host reports exactly the hostable panels as hosted");
    for (const std::string& panel_id : panels::hostable_panel_ids())
    {
        std::string error_code;
        const auto rendered = panel_host.render(panel_id, error_code);
        COCOA_CHECK(rendered.has_value(), "a hosted panel rendered a live model");
        if (!rendered.has_value())
        {
            std::fprintf(stderr, "[editor-shell-cocoa]   panel '%s' -> %s\n", panel_id.c_str(),
                         error_code.c_str());
        }
    }

    // ----------------------------------------------------- 5. a frame reaches the real window
    const render::Extent2D coded{client.width + 24, client.height + 16};
    const std::uint32_t kPaddedStride = coded.width * 4u + 64u;
    const render::Rect2D kViewVisible{render::Origin2D{8, 6}, client};
    const Texel kViewColor{20, 40, 60, 255};
    const Texel kMarginColor{7, 7, 7, 255};

    std::uint64_t clock_us = 1'000;
    browser->queue_frame(shell::BrowserLayer::view, coded, kViewVisible,
                         padded_frame(coded, kPaddedStride, kViewVisible, kViewColor, kMarginColor),
                         kPaddedStride);
    COCOA_CHECK(manager.pump_once(clock_us), "the owner loop ran over the real NSWindow");
    COCOA_CHECK(editor->compositor().stats().view_frames == 1, "the view frame was adopted");
    COCOA_CHECK(editor->compositor().stats().frames_presented >= 1,
                "a composited frame was PRESENTED through the CALayer blitter");
    {
        const std::vector<std::uint8_t>& surface = editor->compositor().cpu_surface();
        COCOA_CHECK(surface.size() == static_cast<std::size_t>(client.width) * client.height * 4u,
                    "the composed surface is the window's client extent");
        const Texel texel = sample(surface, client, 5, 5);
        COCOA_CHECK(texel.b == 20 && texel.g == 40 && texel.r == 60,
                    "the composed surface carries the browser's premultiplied BGRA pixels");
        COCOA_CHECK(free_of(surface, client, kMarginColor),
                    "no texel of the padded allocation's margin reached the window");
    }

    // ----------------------------------------------- 6. the WINDOW SERVER granted a real resize
    //
    // Cocoa has no ConfigureNotify: geometry is POLLED once per pump and diffed by
    // translate_ns_window_geometry (cocoa_window.mm shape 2). So this asks the window system for a
    // new frame and waits for the size to come back through THAT diff — the size the window server
    // GRANTED, which `constrainFrameRect:toScreen:` or a screen constraint is entitled to adjust
    // (there is no window manager on macOS), never the one we asked for.
    const render::Extent2D before_resize = backend->client_size();
    // The size the browser was last told about BEFORE the resize. It is already non-zero — the owner
    // loop syncs it on the very first pump_once (the browser_size_synced_ latch) — so asserting
    // merely that it is non-zero afterwards proves nothing and would pass with sync_browser_size()
    // deleted from the resize arm. The claim is that it CHANGED.
    const render::Extent2D browser_size_before = browser->last_logical_size();
    shell::WindowPlacement resized = backend->placement();
    // ⚠ THE PHYSICAL-PIXELS -> COCOA-POINTS CONVERSION IS LOAD-BEARING HERE, not decoration.
    // `client_size()` is PHYSICAL PIXELS; `placement()` is COCOA POINTS (smoke_window.h states the
    // rule and why). Assigning `before_resize.width + 120` straight into `resized` — which this
    // smoke did until e12c-3's review — asked a 2x window for ~2.4x its size while reading as
    // "+120", and read as exactly right on a 1x display, so no assertion here could have caught it.
    const render::Extent2D grown = smoke::placement_extent_for_physical(
        *backend, render::Extent2D{before_resize.width + 120u, before_resize.height + 80u});
    resized.width = grown.width;
    resized.height = grown.height;
    backend->apply_placement(resized);
    // ⚠ THE PREDICATE IS DIRECTIONAL (`>`), NOT MERELY "CHANGED" (`!=`), and that is the assertion
    // that pins the physical-pixels -> points conversion above. A `!=` is satisfied by a resize
    // that went the WRONG WAY by a factor of the DPI scale, which is exactly how the unit-mismatch
    // this review fixed stayed invisible on a green gate. Both dimensions, because a one-axis clamp
    // must not be able to satisfy it either. The size is still the GRANTED one, never the requested
    // one — this claims the direction the window server moved in, not the magnitude.
    const bool observed_resize = pump_until(manager, clock_us, [&] {
        const render::Extent2D now = backend->client_size();
        return now.width > before_resize.width && now.height > before_resize.height;
    });
    COCOA_CHECK(observed_resize,
                "AppKit's own geometry reported the granted resize to the shell, and it GREW as "
                "requested");
    if (observed_resize)
    {
        COCOA_CHECK(editor->compositor().size().width == backend->client_size().width &&
                        editor->compositor().size().height == backend->client_size().height,
                    "the compositor took the size the window system actually granted");
        const render::Extent2D browser_size_after = browser->last_logical_size();
        COCOA_CHECK(browser_size_after.width != browser_size_before.width ||
                        browser_size_after.height != browser_size_before.height,
                    "the browser was told about the real resize (WasResized)");
        COCOA_CHECK(browser_size_after.width ==
                            to_logical(backend->client_size(), backend->dpi()).width &&
                        browser_size_after.height ==
                            to_logical(backend->client_size(), backend->dpi()).height,
                    "the browser was told the LOGICAL size the window system actually granted");
    }

    // The placement the WINDOW reports, not the one we asked for — reading it back is what proves
    // placement() talks to AppKit.
    const shell::WindowPlacement observed = backend->placement();
    COCOA_CHECK(!render::is_empty(observed.size()), "placement() read a real geometry back");
    // The display's CGDirectDisplayID, not its localized name (which is not an identity a later
    // launch can match against).
    COCOA_CHECK(observed.monitor.rfind("cocoa:", 0) == 0,
                "placement() names the CGDirectDisplayID it read the geometry from");

    // ------------------------------------ 6b. INPUT AppKit carried back (the e12c-3 core claim)
    //
    // The half e12b never proved: a pointer sample and a key press that genuinely made the
    // post -> APPKIT QUEUE -> nextEventMatchingMask round trip and came back through the real
    // translate_ns_event, rather than being appended to a queue this test owns. Nothing here can pass
    // with the decoder's pointer/key arms deleted, with the injection deleted, or against the headless
    // backend (the seam refuses that outright).
    //
    // The counters are read as a DELTA, never against a fixed number: AppKit is entitled to have
    // delivered other input already (a MouseExited, which the decoder carries as
    // `PointerAction::leave`) on whatever desktop this runs on.
    const render::Extent2D live = backend->client_size();
    const int pointer_dispatches_before = editor->input().pointer_dispatches();
    const int key_dispatches_before = editor->input().key_dispatches();
    // ⚠ THE BROWSER'S SAMPLE LOG IS SCANNED AS A TAIL, for exactly the reason the dispatch counters
    // are read as a delta one line up. Everything before this index belongs to whatever the desktop
    // delivered earlier — a MouseExited during the up-to-10 s resize pump above is the realistic
    // case — and the position claims below identify their samples by ORDER, so folding a hardware
    // sample in as "the first move" would compare it against an injected one and red on a clean
    // tree.
    const std::size_t pointers_before = browser->pointers().size();

    // TWO moves, at the TOP and the BOTTOM of the right half. Two rather than one because the strong
    // position claim available here is a RELATIVE one: an exact equality is impossible (file header,
    // claim 2), but the Y-FLIP DIRECTION and the SEPARATION are properties no centre-scaling can
    // invert. `high_in_shell_space` is near the shell-space TOP, which is Cocoa's BOTTOM — so if the
    // flip were dropped these two would arrive in the opposite order.
    const std::int32_t x_in_browser_half = static_cast<std::int32_t>(live.width) -
                                           static_cast<std::int32_t>(live.width / 8u);
    const std::int32_t high_in_shell_space = static_cast<std::int32_t>(live.height / 8u);
    const std::int32_t low_in_shell_space = static_cast<std::int32_t>(live.height) -
                                            static_cast<std::int32_t>(live.height / 8u);

    shell::ShellEvent move_high;
    move_high.kind = shell::ShellEventKind::pointer;
    move_high.pointer.action = shell::PointerAction::move;
    move_high.pointer.position = shell::PointI{x_in_browser_half, high_in_shell_space};
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, move_high),
                "a pointer MOVE near the TOP was accepted for injection through AppKit");

    shell::ShellEvent move_low = move_high;
    move_low.pointer.position = shell::PointI{x_in_browser_half, low_in_shell_space};
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, move_low),
                "a pointer MOVE near the BOTTOM was accepted for injection through AppKit");

    // The press/release PAIR. Pairing is not cosmetic: the Shell's pump forwards every dequeued event
    // to AppKit, and a burst of UNPAIRED synthesized presses drove AppKit into a nested mouse-tracking
    // loop that never returned (measured, e12c-3 probe). The x11 smoke pairs them too.
    shell::ShellEvent press = move_low;
    press.pointer.action = shell::PointerAction::down;
    press.pointer.button = shell::MouseButton::left;
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, press),
                "a pointer PRESS was accepted for injection through AppKit");

    shell::ShellEvent release = press;
    release.pointer.action = shell::PointerAction::up;
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, release),
                "a pointer RELEASE was accepted for injection through AppKit");

    const bool pointers_arrived = pump_until(manager, clock_us, [&] {
        return editor->input().pointer_dispatches() >= pointer_dispatches_before + 4;
    });
    COCOA_CHECK(pointers_arrived,
                "the four injected pointer samples came BACK from the AppKit queue and were "
                "arbitrated");
    // WARNING THE TOTAL IS ASSERTED WITH `>=` AND THE PRECISION LIVES IN THE PER-ACTION COUNTS, the
    // same split the x11 smoke records: AppKit may deliver a MouseExited this smoke never injected
    // (the decoder carries it as `PointerAction::leave`), so `== before + 4` would be asserting the
    // state of somebody's desktop. Presses and releases have no such source, so counting them is BOTH
    // robust and strictly more precise than the total ever was.
    int downs = 0;
    int ups = 0;
    int moves = 0;
    std::int32_t first_move_y = 0;
    std::int32_t second_move_y = 0;
    std::int32_t move_x = 0;
    const std::vector<shell::PointerEvent>& all_pointers = browser->pointers();
    for (std::size_t i = pointers_before; i < all_pointers.size(); ++i)
    {
        const shell::PointerEvent& pointer = all_pointers[i];
        if (pointer.action == shell::PointerAction::move)
        {
            ++moves;
            if (moves == 1)
            {
                first_move_y = pointer.position.y;
                move_x = pointer.position.x;
            }
            else if (moves == 2)
            {
                second_move_y = pointer.position.y;
            }
            continue;
        }
        if (pointer.button != shell::MouseButton::left)
        {
            continue;
        }
        if (pointer.action == shell::PointerAction::down)
        {
            ++downs;
        }
        else if (pointer.action == shell::PointerAction::up)
        {
            ++ups;
        }
    }
    COCOA_CHECK(downs == 1, "exactly one left-button PRESS arrived - no duplicate decode");
    COCOA_CHECK(ups == 1, "exactly one left-button RELEASE arrived - no duplicate decode");
    COCOA_CHECK(moves >= 2, "both injected moves arrived at the browser");

    // THE Y-FLIP AND THE SEPARATION. The strongest position claim the platform allows, and it is a
    // real one: the first move was injected NEAR THE SHELL-SPACE TOP, so it must arrive with the
    // SMALLER y. `ns_view_point_to_physical` is what flips Cocoa's bottom-left origin into the
    // Shell's top-left one; drop that flip and these two swap. The separation floor rules out both
    // samples collapsing onto one point (which an ignored location would do).
    //
    // ⚠ THE ONLY GUARD IS `moves >= 2`, which the CHECK above already asserts — so this block
    // cannot be SKIPPED on a passing run. It previously also required both delivered y values to be
    // `>= 0`, which meant a negative delivered coordinate (the one shape that says the flip or the
    // scaling went wrong) silently dropped the whole flip claim while the smoke still reported
    // PASS. A coordinate out of range is something to FAIL on, never a reason to stop asserting.
    if (moves >= 2)
    {
        COCOA_CHECK(first_move_y >= 0 && second_move_y >= 0,
                    "both delivered sample positions are inside the window - a negative y means "
                    "the flip or the location round trip is broken");
        COCOA_CHECK(first_move_y < second_move_y,
                    "the TOP-injected sample arrived above the BOTTOM-injected one - the Cocoa "
                    "bottom-left -> Shell top-left flip survived the round trip");
        const std::int32_t requested_gap = low_in_shell_space - high_in_shell_space;
        COCOA_CHECK(second_move_y - first_move_y >= requested_gap / 2,
                    "the two samples arrived separated by at least half the injected gap - the "
                    "location really travelled");
    }
    // The x coordinate is asserted as a HALF-PLANE, never an equality: the delivered location is
    // scaled about the window centre (file header, claim 2), and a half-plane is what the region
    // arbitration above actually depends on.
    if (moves >= 1)
    {
        COCOA_CHECK(move_x > static_cast<std::int32_t>(live.width / 2u),
                    "the sample arrived in the RIGHT half, outside the published viewport region");
    }

    shell::ShellEvent key;
    key.kind = shell::ShellEventKind::key;
    key.key.action = shell::KeyAction::raw_key_down;
    key.key.windows_key_code = 0x09; // VK_TAB - the same key the live CEF boot smoke injects
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, key),
                "a key PRESS was accepted for injection through AppKit");
    const bool key_arrived = pump_until(manager, clock_us, [&] {
        return editor->input().key_dispatches() > key_dispatches_before;
    });
    COCOA_CHECK(key_arrived, "the injected key came BACK from the AppKit queue and was arbitrated");
    // `>= 1`, deliberately, and NOT `== 1`: macOS carries the character ON the key event, so the
    // decoder emits the raw key AND its '\t' character for a real Tab.
    COCOA_CHECK(editor->input().key_dispatches() >= key_dispatches_before + 1,
                "the key press produced at least the raw-key dispatch");
    // THE VIRTUAL-KEY ROUND TRIP, and the reason this step is not merely "a key arrived": an
    // injection-table entry that is simply WRONG still delivers an event, so a presence-only
    // assertion would pass while the browser received a completely different key. The keyCode this
    // smoke sent was derived from VK_TAB through the seam's inverse table and decoded back by the
    // SHIPPING `ns_key_code_to_windows_key_code`; it must arrive as VK_TAB again.
    COCOA_CHECK(!browser->keys().empty(), "the browser received the injected key");
    if (!browser->keys().empty())
    {
        COCOA_CHECK(browser->keys().front().windows_key_code == 0x09,
                    "the browser was handed VK_TAB - the key that was actually injected");
    }

    // The seam's RESIZE arm, which posts no NSEvent at all: it asks the window system and waits for
    // the geometry diff. Proven here so all three injection arms have a CEF-free, locally-runnable
    // macOS proof and not just the two the live CEF smokes exercise.
    //
    // `shrink.size` is PHYSICAL PIXELS, like every other `ShellEvent` extent; `inject_event`
    // converts it to the backend's placement units itself (smoke_window.h), so this really does ask
    // for a SMALLER window rather than — as it did before e12c-3's review — a near-doubled one that
    // only satisfied the "geometry changed" predicate by accident.
    const render::Extent2D before_injected_resize = backend->client_size();
    shell::ShellEvent shrink;
    shrink.kind = shell::ShellEventKind::resize;
    shrink.size = render::Extent2D{before_injected_resize.width - 40u,
                                   before_injected_resize.height - 30u};
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, shrink),
                "a resize REQUEST was accepted for injection");
    // Directional for the same reason as step 6, and here it is the seam's OWN conversion under
    // test: a resize arm that wrote physical pixels into a points-valued placement would GROW this
    // window while the smoke asked it to SHRINK, and a `!=` predicate would have called that a
    // pass.
    const bool observed_injected_resize = pump_until(manager, clock_us, [&] {
        const render::Extent2D now = backend->client_size();
        return now.width < before_injected_resize.width &&
               now.height < before_injected_resize.height;
    });
    COCOA_CHECK(observed_injected_resize,
                "the injected resize came back through AppKit's own geometry, not a synthesized "
                "event, and the window really SHRANK");

    // ----------------------------------------------------- 7. teardown persists the session
    manager.shutdown();
    COCOA_CHECK(manager.state_store().write_count() >= 1,
                "the shutdown flushed the pending session state");
    COCOA_CHECK(fs::exists(shell::editor_state_path(project)),
                ".editor/editor-state.json was written by the Shell");
    fs::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-shell-cocoa] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("[editor-shell-cocoa] PASS: real NSWindow, %s present, %zu live panels rendered, "
                "granted resize observed, AppKit-carried pointer + key round trip, session "
                "persisted\n",
                blitter_name.c_str(), panels::hostable_panel_ids().size());
    return 0;
}
