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
//      ⚠ AND THE POSITION IS ONLY IN RANGE IN THE FRAME IT WAS ADDRESSED TO. A posted location is
//      resolved against the window's frame origin at DEQUEUE time, not at post time, so a window
//      AppKit moved after the post delivers displaced samples — which is what reddened `main` on
//      both macOS jobs with a NEGATIVE y and no defect anywhere in the flip. The range claim is
//      therefore corrected by `ns_delivered_shift_for_window_move` (a no-op on a window that held
//      still) and the window is settled on a real predicate before anything is posted; the full
//      measurements are smoke_window.h's Cocoa limit 3. The samples are also identified by a modifier
//      MARKER rather than by their ORDER in the stream, because the desktop is entitled to deliver
//      moves of its own.
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

#include "context/editor/shell/cocoa_chrome.h"
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
#include <optional>
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

// Pump until `read()` yields the SAME value on `required` CONSECUTIVE pumps — "this quantity has come
// to rest". Lives here, next to `pump_until`, because the hazard it exists to avoid is `pump_until`'s
// own contract:
//
// ⚠ THE FIRST READ IS SEEDED INSIDE THE PREDICATE, AND THAT IS LOAD-BEARING. `pump_until` evaluates
// its predicate BEFORE the first `pump_once`, so seeding from a read taken before the loop would make
// the FIRST of the comparisons span ZERO pumps — and two live geometry reads with no run-loop turn
// between them are identical by construction, so that first "stable" observation would be free and an
// N-interval claim would silently rest on N-1. The `std::optional` is what makes the seed a state the
// caller cannot forget rather than a comment asking the next reader not to break it.
template <typename Read>
bool pump_until_stable(shell::WindowManager& manager, std::uint64_t& clock_us, Read read,
                       int required = 2)
{
    std::optional<decltype(read())> previous;
    int stable = 0;
    return pump_until(manager, clock_us, [&] {
        auto now = read();
        stable = (previous && *previous == now) ? stable + 1 : 0;
        previous = std::move(now);
        return stable >= required;
    });
}

// THE INJECTED-SAMPLE MARKER: Shift+Control+Option, set on the way out and tested on the way back.
// The pair lives in ONE place so "these exact flags round-tripped" is structurally true rather than
// true only while two literal lists happen to agree.
//
// WHY A MARKER AT ALL. The position claims below name their samples ("the FIRST move", "the SECOND
// move"), but the stream they are read out of is the desktop's, not this smoke's — the same reason the
// dispatch counters are read as a delta and the log is scanned as a tail. A MouseExited was already
// anticipated there; a foreign MouseMoved is the shape that was not, and it is indistinguishable from
// an injected one by position alone (MEASURED: a probe of this exact injection channel folded a real
// cursor-derived move in as "the first move" and compared a desktop coordinate against an injected
// one). These flags travel ON the event — the injectable half of the modifier inverse
// (smoke_inject_cocoa.mm, shape 2) — so they survive the AppKit round trip and come back through the
// SHIPPING `make_ns_modifiers`, which makes the identification positive rather than positional AND
// strengthens the modifier claim from "flags are encodable" to "these exact flags round-tripped".
//
// ⚠ WHY THIS MASK, AND WHY IT IS NOT REUSABLE BLIND. Command is out because a Cmd-click carries
// app-level meaning AppKit would be entitled to act on. Control is NOT inert either: Control-click is
// macOS's canonical SECONDARY-click chord, so as far as AppKit is concerned the marked press below is
// a context-menu gesture. It is harmless HERE for a checked reason and not by luck — this smoke's
// content view carries no `menu`, so `-menuForEvent:` yields nil and nothing opens. A view that DOES
// carry one would need a chord-free mask (Shift+Option alone).
void apply_marker(shell::Modifiers& modifiers)
{
    modifiers.shift = true;
    modifiers.control = true;
    modifiers.alt = true;
}

[[nodiscard]] bool has_marker(const shell::Modifiers& modifiers)
{
    return modifiers.shift && modifiers.control && modifiers.alt;
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
    // ⚠ THE WINDOW IS SETTLED ON A REAL PREDICATE BEFORE ANYTHING IS POSTED, and that is a
    // CORRECTNESS step, not tidiness. A posted location is resolved against the frame origin at
    // DEQUEUE time (smoke_window.h, Cocoa limit 3), so a window that is still coming to rest after
    // the granted resize above displaces every queued sample by the remainder of its own movement.
    // The predicate is the geometry itself — the WHOLE `placement()`, compared with its own
    // `operator==` so the EXTENT, the monitor and the maximized flag are all in scope rather than just
    // the origin. That breadth is deliberate: the decoder flips y against the view HEIGHT, so a height
    // still settling displaces a delivered sample exactly as a moving origin does. Never a sleep,
    // which would assert the runner's speed. `pump_until_stable` owns the seeding subtlety.
    const bool window_settled =
        pump_until_stable(manager, clock_us, [&] { return backend->placement(); });
    COCOA_CHECK(window_settled, "the window geometry came to rest before any event was injected");

    // The counters are read as a DELTA, never against a fixed number: AppKit is entitled to have
    // delivered other input already (a MouseExited, which the decoder carries as
    // `PointerAction::leave`) on whatever desktop this runs on.
    const render::Extent2D live = backend->client_size();
    const int pointer_dispatches_before = editor->input().pointer_dispatches();
    const int key_dispatches_before = editor->input().key_dispatches();
    // ⚠ THE BROWSER'S SAMPLE LOG IS SCANNED AS A TAIL, for exactly the reason the dispatch counters
    // are read as a delta one line up. Everything before this index belongs to whatever the desktop
    // delivered earlier — a MouseExited during the up-to-10 s resize pump above is the realistic
    // case. ⚠ SAMPLE IDENTITY IS NO LONGER POSITIONAL — the four samples below carry a modifier
    // MARKER and are selected by it (see the injection), so this tail is now a NARROWING rather than
    // the whole guard it used to be: it keeps the unmarked-sample counters below scoped to this
    // smoke's own window of time instead of to everything the desktop ever delivered.
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

    // The frame origin the four samples below are ENCODED AGAINST — read after the settle and before
    // the first post. The first half of the limit-3 correction the range claim below applies.
    const shell::WindowPlacement placement_at_post = backend->placement();

    shell::ShellEvent move_high;
    move_high.kind = shell::ShellEventKind::pointer;
    move_high.pointer.action = shell::PointerAction::move;
    move_high.pointer.position = shell::PointI{x_in_browser_half, high_in_shell_space};
    // The marker the delivered-sample loop below selects on — the mask, and why the samples are no
    // longer identified by their ORDINAL position, are `apply_marker`'s own comment. The three
    // remaining samples inherit it by copy-construction from this one.
    apply_marker(move_high.pointer.modifiers);
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

    // The frame origin the samples were actually DELIVERED against — the second half. Read here and
    // not later because `pump()` drains the WHOLE AppKit queue in one call and there is no run-loop
    // turn between that drain and this read, so `[window frame]` cannot move in the gap: one uniform
    // `shift` really does describe all four samples. It is ZERO on a window that did not move — the
    // ordinary case, in which every claim below is bit-identical to what it was before this existed.
    const shell::WindowPlacement placement_at_delivery = backend->placement();
    const smoke::NsDeliveredShift shift = smoke::ns_delivered_shift_for_window_move(
        shell::PointI{placement_at_post.x, placement_at_post.y},
        shell::PointI{placement_at_delivery.x, placement_at_delivery.y}, backend->dpi());
    // ⚠ "HELD STILL" IS THE WHOLE PLACEMENT, NOT JUST THE ORIGIN `shift` CORRECTS FOR. The decoder
    // flips y against the view HEIGHT, so a height that changed between the post and the delivery
    // displaces every delivered y by that change — and in Cocoa a window can GROW with its bottom-left
    // origin untouched, which would leave `shift` at (0,0) and that displacement both uncorrected AND
    // invisible. Rather than invent a second correction for a case the settle predicate already
    // prevents, the extent enters the PREMISE: any placement change at all makes this false, so the
    // report below fires and the run says out loud which axis moved. Same `operator==` the settle
    // predicate uses, for the same reason — one definition of "the geometry is at rest" per file.
    const bool window_held_still = placement_at_post == placement_at_delivery;

    int downs = 0;
    int ups = 0;
    int moves = 0;
    int unmarked_moves = 0;
    int unmarked_others = 0;
    std::int32_t first_move_y = 0;
    std::int32_t second_move_y = 0;
    std::int32_t move_x = 0;
    const std::vector<shell::PointerEvent>& all_pointers = browser->pointers();
    for (std::size_t i = pointers_before; i < all_pointers.size(); ++i)
    {
        const shell::PointerEvent& pointer = all_pointers[i];
        // THE MARKER FILTER (`apply_marker`). EVERY unmarked sample is COUNTED rather than dropped
        // silently, and both counts are reported below, so a desktop delivering its own input stays
        // VISIBLE. Split so a foreign MOVE — the shape the old positional identification could have
        // confused with one of ours — stays legible separately from the already-anticipated `leave`.
        if (!has_marker(pointer.modifiers))
        {
            if (pointer.action == shell::PointerAction::move)
            {
                ++unmarked_moves;
            }
            else
            {
                ++unmarked_others;
            }
            continue;
        }
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
    //
    // ⚠ THE RANGE CLAIM IS MADE IN THE FRAME THE SAMPLE WAS ADDRESSED TO, WHICH IS WHAT MADE `main`
    // RED. It is the ONE claim here that is not invariant under a uniform translation of all four
    // samples, and a posted location is resolved against the frame origin at DEQUEUE time — so a
    // window that AppKit moved after the post delivers an out-of-range coordinate that says nothing
    // whatever about the flip or the scale (smoke_window.h, Cocoa limit 3, with the measurements).
    // `shift` is that displacement, derived from the window ORIGIN — an independent quantity read off
    // `placement()`, never from the delivered sample — so correcting by it cannot launder a genuinely
    // wrong coordinate: a mis-flipped or mis-scaled sample is still out of range afterwards. It is
    // ZERO whenever the window held still, which is the ordinary case.
    //
    // ⚠ AND IT IS NOW TWO-SIDED. `>= 0` alone passes a sample delivered BELOW the window in silence,
    // which is exactly what the opposite-direction window move produces — see the moved-UP-80 row of
    // smoke_window.h's Cocoa limit 3 for the measurement. (Read that table's units note before
    // comparing its figures against anything here: it is in Cocoa POINTS, while the `height` below is
    // `live.height` in PHYSICAL PIXELS.) The upper bound is the half of this claim that the shape
    // which reddened `main` happened not to exercise.
    if (moves >= 2)
    {
        const std::int32_t first_addressed = first_move_y - shift.dy;
        const std::int32_t second_addressed = second_move_y - shift.dy;
        const std::int32_t height = static_cast<std::int32_t>(live.height);
        COCOA_CHECK(first_addressed >= 0 && first_addressed < height && second_addressed >= 0 &&
                        second_addressed < height,
                    "both delivered sample positions are inside the window - a y outside "
                    "[0, height) means the flip or the location round trip is broken");
        // ORDER and SEPARATION are asserted on the RAW delivered values, deliberately: both are
        // invariant under the uniform translation `shift` describes, so correcting them would add a
        // term that provably cancels and would only obscure that they need no correction at all.
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
    // arbitration above actually depends on. Corrected on the same axis-specific term as the range
    // claim — x keeps the origin delta's sign where y inverts it, which is the y-flip itself.
    if (moves >= 1)
    {
        COCOA_CHECK(move_x - shift.dx > static_cast<std::int32_t>(live.width / 2u),
                    "the sample arrived in the RIGHT half, outside the published viewport region");
    }
    // WHATEVER HAPPENED TO THE GEOMETRY AND TO THE STREAM IS REPORTED, PASS OR FAIL. A correction
    // that is silent when it fires is a correction nobody can audit, and "the window held still" is
    // the premise the two-sided range claim above is cheapest to read under. Neither line is an
    // assertion: a desktop is allowed to move our window and to deliver its own input — what is not
    // allowed is doing either INVISIBLY. The geometry is printed as BOTH endpoints (origin AND
    // extent, in the Cocoa POINTS `placement()` speaks) rather than only the derived `shift`, because
    // the extent half is deliberately uncorrected — see `window_held_still` — so a reader has to be
    // able to see WHICH axis moved, not just that something did.
    if (!window_held_still || unmarked_moves > 0 || unmarked_others > 0)
    {
        std::printf("[editor-shell-cocoa]   note: geometry between post and delivery: origin "
                    "%d,%d -> %d,%d points, extent %ux%u -> %ux%u points => corrected samples by "
                    "(%d,%d) physical px (origin term only); excluded %d unmarked move(s) and %d "
                    "other unmarked sample(s) from the desktop; delivered y %d / %d, client %ux%u "
                    "physical px\n",
                    placement_at_post.x, placement_at_post.y, placement_at_delivery.x,
                    placement_at_delivery.y, placement_at_post.width, placement_at_post.height,
                    placement_at_delivery.width, placement_at_delivery.height, shift.dx, shift.dy,
                    unmarked_moves, unmarked_others, first_move_y, second_move_y, live.width,
                    live.height);
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

    // ------------------------------------ 6c. the c1 HYBRID CHROME (style, inset, caption consult)
    //
    // editor-window-chrome c1 (target design 02 §4), proven against the REAL NSWindow — the ONE CI
    // context that has one (the live CEF smokes run headless on macOS, so `mode:"hybrid"` is
    // assertable nowhere else). Three claims, each with a real failure path:
    //
    //   * THE STYLE IS LIVE, NOT ASSUMED. `cocoa_hybrid_chrome` re-reads FullSizeContentView +
    //     titlebarAppearsTransparent off the window at call time (interim honesty made structural),
    //     so reverting the create()-time flip reddens this line — no self-report can satisfy it.
    //   * THE INSET IS MEASURED, AND POSITIVE. It comes from the real standardWindowButton frames
    //     through the leg-tested arithmetic; a nil-button or dropped-measurement regression reads 0
    //     and fails the `> 0` half the DoD names.
    //   * THE CAPTION CONSULT CONSUMES THE PRESS WHOLE — AND ONLY THE PRESS. The suppression claim
    //     is two-sided: no marked press reaches the browser and no implicit capture leaks (the
    //     stuck-hover shape, ROADMAP risk 3), while the very NEXT event (the release) flows through
    //     ordinary arbitration and a non-caption press still lands in the browser afterwards.
    shell::CocoaChromeState chrome;
    COCOA_CHECK(shell::cocoa_hybrid_chrome(*backend, chrome),
                "the live window reports HYBRID chrome - FullSizeContentView + the transparent "
                "titlebar are really set on the NSWindow");
    COCOA_CHECK(chrome.inset_left > 0u,
                "controlsInset.left measured from the real traffic-light frames is positive");
    COCOA_CHECK(chrome.inset_left < backend->client_size().width / 2u,
                "the measured inset is sane - less than half the window");
    // The GH macOS runners run an en-US (LTR) session, so the cluster sits LEFT; the RTL mirror is
    // pinned by the leg-tested arithmetic (test_cocoa_chrome), not asserted against this desktop.
    COCOA_CHECK(chrome.inset_right == 0u,
                "an LTR desktop insets the left edge only");

    // The consult reads the arbiter's LIVE region map — wired by the EditorWindow constructor
    // itself (shell.cpp), so this smoke exercises the production wiring, not a private copy of it.
    shell::CocoaCaptionStats caption_before;
    COCOA_CHECK(shell::cocoa_caption_stats(*backend, caption_before),
                "the caption stats are readable off the live cocoa backend");

    // Settle, then publish a caption drag surface over the TOP HALF — generous, because a posted
    // location is only approximately delivered (file header, claim 2) and every sample below sits
    // deep inside its half. Wholesale replace, per the region contract: the viewport region from
    // step 3 is gone, which is exactly what frees the bottom half for the forwarding drill.
    const bool settled_for_chrome =
        pump_until_stable(manager, clock_us, [&] { return backend->placement(); });
    COCOA_CHECK(settled_for_chrome, "the geometry came to rest before the caption drills");
    const render::Extent2D chrome_client = backend->client_size();
    editor->input().regions().publish({shell::ShellRegion{
        "chrome.caption",
        render::Rect2D{render::Origin2D{0, 0},
                       render::Extent2D{chrome_client.width, chrome_client.height / 2u}},
        shell::RegionKind::caption}});
    // Everything the browser receives from here on is judged against this baseline — the two
    // consumed presses below must never appear after it.
    const std::size_t chrome_samples_baseline = browser->pointers().size();

    // --- double-click on the caption = zoom: (the platform convention) ---------------------------
    shell::ShellEvent zoom_press;
    zoom_press.kind = shell::ShellEventKind::pointer;
    zoom_press.pointer.action = shell::PointerAction::down;
    zoom_press.pointer.button = shell::MouseButton::left;
    zoom_press.pointer.click_count = 2; // Cocoa counts clicks ON the event; 2 is the double-click's press
    zoom_press.pointer.position =
        shell::PointI{static_cast<std::int32_t>(chrome_client.width / 2u),
                      static_cast<std::int32_t>(chrome_client.height / 4u)};
    apply_marker(zoom_press.pointer.modifiers);
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, zoom_press),
                "a double-click caption press was accepted for injection");
    shell::ShellEvent zoom_release = zoom_press;
    zoom_release.pointer.action = shell::PointerAction::up;
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, zoom_release),
                "the double-click's release was accepted for injection");
    const bool zoom_consumed = pump_until(manager, clock_us, [&] {
        shell::CocoaCaptionStats stats;
        return shell::cocoa_caption_stats(*backend, stats) &&
               stats.zooms >= caption_before.zooms + 1;
    });
    COCOA_CHECK(zoom_consumed, "the double-click reached the pump's caption consult as zoom:");
    const bool zoom_observed =
        pump_until(manager, clock_us, [&] { return backend->placement().maximized; });
    COCOA_CHECK(zoom_observed, "zoom: really zoomed - placement().maximized flipped true");

    // Restore the frame before the drag drill, so its caption rect is published against a settled
    // geometry (a posted location is resolved against the frame at DEQUEUE time — claim 2).
    backend->set_maximized(false);
    const bool unzoomed =
        pump_until(manager, clock_us, [&] { return !backend->placement().maximized; });
    COCOA_CHECK(unzoomed, "set_maximized(false) un-zoomed the window for the drag drill");
    const bool settled_after_zoom =
        pump_until_stable(manager, clock_us, [&] { return backend->placement(); });
    COCOA_CHECK(settled_after_zoom, "the geometry came to rest after the zoom round trip");

    // --- a single caption press hands the drag to the OS, consumed WHOLE -------------------------
    const render::Extent2D drag_client = backend->client_size();
    editor->input().regions().publish({shell::ShellRegion{
        "chrome.caption",
        render::Rect2D{render::Origin2D{0, 0},
                       render::Extent2D{drag_client.width, drag_client.height / 2u}},
        shell::RegionKind::caption}});
    shell::ShellEvent drag_press = zoom_press;
    drag_press.pointer.click_count = 1;
    drag_press.pointer.position =
        shell::PointI{static_cast<std::int32_t>(drag_client.width / 2u),
                      static_cast<std::int32_t>(drag_client.height / 4u)};
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, drag_press),
                "a single caption press was accepted for injection");
    // `caption_before` is still the drag baseline: the only drill since that read was the zoom
    // drill, whose press carries `click_count 2` by construction and so can only bump `zooms`.
    const bool drag_consumed = pump_until(manager, clock_us, [&] {
        shell::CocoaCaptionStats stats;
        return shell::cocoa_caption_stats(*backend, stats) &&
               stats.drags >= caption_before.drags + 1;
    });
    COCOA_CHECK(drag_consumed,
                "the caption press was handed to performWindowDragWithEvent: by the pump");
    // The matching release is NOT consumed: it flows through ordinary arbitration (the caption
    // region claims it, the native arm drops it) — the positive half of "for THAT press only".
    const int dispatches_before_release = editor->input().pointer_dispatches();
    shell::ShellEvent drag_release = drag_press;
    drag_release.pointer.action = shell::PointerAction::up;
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, drag_release),
                "the caption press's release was accepted for injection");
    const bool release_arbitrated = pump_until(manager, clock_us, [&] {
        return editor->input().pointer_dispatches() > dispatches_before_release;
    });
    COCOA_CHECK(release_arbitrated,
                "the release flowed through ordinary arbitration - only the press was consumed");

    // "One of OUR presses reached the browser", counted from a baseline: a marked pointer-down at
    // or past it. The load-bearing predicate for both claims below — suppression needs zero of
    // them, forwarding waits for one — so it is defined exactly once.
    const auto marked_downs_since = [&](std::size_t baseline) {
        int marked_downs = 0;
        const std::vector<shell::PointerEvent>& samples = browser->pointers();
        for (std::size_t i = baseline; i < samples.size(); ++i)
        {
            if (has_marker(samples[i].modifiers) &&
                samples[i].action == shell::PointerAction::down)
            {
                ++marked_downs;
            }
        }
        return marked_downs;
    };

    // --- the suppression claims (ROADMAP risk 3: no stuck hover) ---------------------------------
    COCOA_CHECK(marked_downs_since(chrome_samples_baseline) == 0,
                "neither consumed caption press reached the browser - no half-press, no stuck "
                "hover");
    COCOA_CHECK(!editor->input().has_pointer_capture(),
                "no implicit pointer capture leaked from a consumed press");

    // --- and a NON-caption press still reaches the browser afterwards ----------------------------
    const std::size_t forward_baseline = browser->pointers().size();
    shell::ShellEvent forward_press = drag_press;
    forward_press.pointer.position =
        shell::PointI{static_cast<std::int32_t>(drag_client.width -
                                                drag_client.width / 4u),
                      static_cast<std::int32_t>(drag_client.height -
                                                drag_client.height / 4u)};
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, forward_press),
                "a non-caption press was accepted for injection");
    shell::ShellEvent forward_release = forward_press;
    forward_release.pointer.action = shell::PointerAction::up;
    COCOA_CHECK(smoke::inject_event(*backend, smoke::WindowMode::real, forward_release),
                "the non-caption release was accepted for injection");
    const bool forwarded = pump_until(manager, clock_us, [&] {
        return marked_downs_since(forward_baseline) > 0;
    });
    COCOA_CHECK(forwarded,
                "a press OUTSIDE the caption still reaches the browser - the always-forward rule "
                "holds for every other event");

    // ------------------------------------ 6d. the f1 FACTORY-WINDOW hybrid chrome (02 §9)
    //
    // editor-window-chrome f1: c1's style mask must hold for EVERY window the factory creates, not
    // just window 0. The app's window factory (editor_main.cpp) builds a second window through the
    // SAME make_window_backend path window 0 took — same WindowDesc shape, filled from a
    // WindowSpec — so creating one here and re-reading its REAL style is the macOS-leg proof:
    // `cocoa_hybrid_chrome` reads FullSizeContentView + the transparent titlebar back off the
    // NSWindow at call time, and the inset comes from that window's OWN measured traffic-light
    // frames. A create path that armed the mask only for the first window would answer false (or
    // a zero inset) here; the live CEF tear-out smoke runs headless on macOS, so this is the one
    // CI context that can pin it.
    {
        shell::WindowDesc second_desc;
        second_desc.title = "Context Editor (cocoa smoke, factory window)";
        second_desc.logical_size = kWindowSize;
        smoke::WindowSetup second_setup =
            smoke::make_smoke_window(second_desc, smoke::WindowMode::real);
        COCOA_CHECK(second_setup.backend != nullptr,
                    "a second real window came up through the same creation seam the factory uses");
        if (second_setup.backend != nullptr)
        {
            shell::CocoaChromeState second_chrome;
            COCOA_CHECK(shell::cocoa_hybrid_chrome(*second_setup.backend, second_chrome),
                        "the factory-path window reports HYBRID chrome too - the c1 style mask "
                        "holds for every window the create path mints, not just window 0");
            COCOA_CHECK(second_chrome.inset_left > 0u,
                        "its inset is measured from its OWN traffic-light frames and positive");
            COCOA_CHECK(second_chrome.inset_left < second_setup.backend->client_size().width / 2u,
                        "and sane - less than half that window");
        }
        // The unique_ptr closes the second window on scope exit; nothing below references it.
    }

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
                "granted resize observed, AppKit-carried pointer + key round trip, hybrid chrome "
                "(measured inset %u px) + caption drag/zoom consult + factory-window style mask, "
                "session persisted\n",
                blitter_name.c_str(), panels::hostable_panel_ids().size(),
                static_cast<unsigned>(chrome.inset_left));
    return 0;
}
