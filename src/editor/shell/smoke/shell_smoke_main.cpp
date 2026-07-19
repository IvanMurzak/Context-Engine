// The M9 e04 SESSION-0-SAFE SHELL SMOKE — the task's BLOCKING CI requirement.
//
// WHAT "Session-0-safe" MEANS AND WHY THIS EXISTS. The Windows CI legs run on a self-hosted runner
// installed as a LocalSystem service, i.e. in Session 0: there is no interactive desktop, and native
// GPU windowed teardown crashes there (the repo has a standing "never add a Windows native-GPU
// render leg" rule for exactly this — offscreen_main.cpp:75-84, editor_host.cpp:376-385). So the
// blocking proof that the Shell WORKS cannot open a window or create a GPU device.
//
// It does not need to. Everything between the OS and the pixels is the Shell's own code, and this
// runs ALL of it against the real objects:
//
//   * the REAL owner loop (WindowManager -> EditorWindow::pump_once), over the honest offscreen
//     window backend;
//   * REAL software-OSR frames — premultiplied BGRA8 at a PADDED row stride, with an allocation
//     LARGER than the visible rect and the visible area at a non-zero origin inside it, and with
//     the padding filled in a contrasting colour so any compose path that presents the allocation
//     instead of the visible rect is caught per-pixel. CEF's own OnPaint happens to report
//     coded == visible, so this deliberately drives the WIDER shape the OSR contract permits —
//     that is where the UV, stride and origin bugs hide;
//   * the REAL compositor: damage-driven redraw, the resize protocol, the full-window CEF layer and
//     the PET_POPUP second layer;
//   * the REAL C-F2 CPU present path, presenting through e03's MemoryBlitter — which present_blit.h
//     documents as "the honest present target for a headless/offscreen shell";
//   * the REAL input arbitration, asserted as a ROUND TRIP: OS event in, browser event out.
//
// and it asserts the COMPOSITED PRESENT PER-PIXEL. Nothing here opens a window, creates a device, or
// links CEF, so it is safe on every OS leg including Session 0. The live CEF binding is proven
// separately by the `editor-cef-smoke` job.
//
// Exits 0 on success; any failed assertion prints its line and exits 1.

#include "context/editor/shell/shell.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace shell = context::editor::shell;
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
    std::fprintf(stderr, "[editor-shell-smoke] FAIL (line %d): %s\n", line, what);
    ++g_failures;
}

#define SMOKE_CHECK(cond, what) check((cond), (what), __LINE__)

const render::Extent2D kWindowSize{320, 200};
const render::Rect2D kPopupRect{render::Origin2D{40, 30}, render::Extent2D{80, 60}};

// A texel of the composed surface (BGRA8, tightly packed at the frame's coded width).
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

// A genuinely honest software-OSR producer frame: an allocation LARGER than the visible rect, at a
// PADDED row stride, with the visible area at a NON-ZERO origin inside it — and with the margin in a
// DIFFERENT colour from the content.
//
// The contrasting margin is the whole point. A uniformly-filled padded allocation catches nothing:
// its margin is byte-identical to its content, so a compose path that ignored visible_rect, walked
// the wrong stride, or copied the entire allocation would still yield exactly the expected pixels.
// With a distinct margin each of those bugs lands margin colour in the presented surface, and the
// per-pixel assertions below fail. (The first version of this smoke filled the allocation solid and
// set coded == visible, so its "allocation larger than the visible rect" claim asserted nothing.)
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
// the allocation instead of the visible rect.
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

shell::ShellEvent pointer_event(shell::PointerAction action, std::int32_t x, std::int32_t y,
                                shell::MouseButton button = shell::MouseButton::none)
{
    shell::ShellEvent event;
    event.kind = shell::ShellEventKind::pointer;
    event.pointer.action = action;
    event.pointer.position = shell::PointI{x, y};
    event.pointer.button = button;
    return event;
}

} // namespace

int main()
{
    std::error_code ec;
    const fs::path project = fs::temp_directory_path(ec) / "context-editor-shell-smoke";
    fs::remove_all(project, ec);
    fs::create_directories(project, ec);

    std::printf("[editor-shell-smoke] Session-0-safe shell smoke: software OSR + composited present\n");

    // ---------------------------------------------------------------- 1. build the shell
    shell::WindowDesc desc;
    desc.title = "Context Editor (smoke)";
    desc.logical_size = kWindowSize;
    desc.visible = false;

    auto backend_owned = std::make_unique<shell::HeadlessWindowBackend>(desc);
    auto browser_owned = std::make_unique<shell::ScriptedBrowserHost>();
    shell::HeadlessWindowBackend* backend = backend_owned.get();
    shell::ScriptedBrowserHost* browser = browser_owned.get();

    shell::EditorWindowConfig config;
    // The L-41 switch, set deliberately: this smoke proves the SOFTWARE-OSR path, which is also the
    // shipping Windows path per the owner ruling of 2026-07-19.
    config.compositor.import_options.force_software = true;
    config.placement_poll_us = 0;

    auto window = std::make_unique<shell::EditorWindow>(std::move(backend_owned),
                                                        std::move(browser_owned), config);

    // The C-F2 CPU present path with e03's portable blitter: no adapter, no swapchain, no window.
    auto blitter_owned = std::make_unique<present::MemoryBlitter>();
    present::MemoryBlitter* blitter = blitter_owned.get();
    window->compositor().attach_cpu(std::move(blitter_owned), kWindowSize);
    SMOKE_CHECK(window->compositor().path() == shell::PresentPath::cpu_blit,
                "the compositor took the CPU present path");
    SMOKE_CHECK(window->compositor().diagnostic().empty(),
                "the CPU present path attached with no diagnostic");

    shell::WindowManager manager(project);
    manager.add(std::move(window));
    shell::EditorWindow* editor = manager.window(0);
    SMOKE_CHECK(editor != nullptr, "the manager adopted the window");
    if (editor == nullptr)
    {
        return 1;
    }

    // The window publishes a viewport region: editor-core does this on every layout change, and it
    // is what the input arbitration hits against.
    editor->input().regions().publish(
        {shell::ShellRegion{"scene",
                            render::Rect2D{render::Origin2D{0, 0}, render::Extent2D{160, 200}},
                            shell::RegionKind::viewport}});

    // ------------------------------------------------- 2. a software-OSR frame is composited
    // An allocation LARGER than the visible rect, at a PADDED stride, with the visible area at a
    // NON-ZERO origin inside it — the shape that catches the UV/stride/origin bugs. See padded_frame.
    const render::Extent2D coded{kWindowSize.width + 24, kWindowSize.height + 16};
    const std::uint32_t kPaddedStride = coded.width * 4u + 64u;
    const render::Rect2D kViewVisible{render::Origin2D{8, 6}, kWindowSize};
    const Texel kViewColor{20, 40, 60, 255};
    // Distinct from every content colour asserted below, so a stray margin texel is unambiguous.
    const Texel kMarginColor{7, 7, 7, 255};

    browser->queue_frame(shell::BrowserLayer::view, coded, kViewVisible,
                         padded_frame(coded, kPaddedStride, kViewVisible, kViewColor, kMarginColor),
                         kPaddedStride);
    SMOKE_CHECK(manager.pump_once(1'000), "the owner loop ran");
    SMOKE_CHECK(blitter->blit_count() == 1, "the first composited frame was presented");
    SMOKE_CHECK(editor->compositor().stats().view_frames == 1, "the view frame was adopted");

    {
        // The composed surface is WINDOW-sized (the visible rect), not allocation-sized.
        const std::vector<std::uint8_t>& surface = editor->compositor().cpu_surface();
        SMOKE_CHECK(surface.size() == static_cast<std::size_t>(kWindowSize.width) *
                                          kWindowSize.height * 4u,
                    "the composed surface is the VISIBLE extent, not the coded allocation");
        const Texel texel = sample(surface, kWindowSize, 5, 5);
        SMOKE_CHECK(texel.b == 20 && texel.g == 40 && texel.r == 60 && texel.a == 255,
                    "the composed surface carries the browser's premultiplied BGRA pixels");
        SMOKE_CHECK(free_of(surface, kWindowSize, kMarginColor),
                    "no allocation-margin texel reached the presented surface");
    }

    // ------------------------------------------------- 3. damage-driven redraw skips an idle frame
    SMOKE_CHECK(manager.pump_once(2'000), "the loop ran with nothing damaged");
    SMOKE_CHECK(blitter->blit_count() == 1, "an undamaged frame was SKIPPED, not re-presented");
    SMOKE_CHECK(editor->compositor().stats().frames_skipped_no_damage >= 1,
                "the skip was accounted for");

    // ------------------------------------------------- 4. PET_POPUP composites as a second layer
    // CEF reports the rect and the visibility separately, and the rect commonly lands first.
    browser->queue_popup_state(true, kPopupRect);
    // The popup allocation is padded too, and its visible area sits at a non-zero origin inside it:
    // the popup source offset is a SECOND place the visible rect must be honoured, and reading from
    // the allocation's top-left would composite margin into the dropdown.
    const render::Extent2D kPopupCoded{kPopupRect.size.width + 12, kPopupRect.size.height + 8};
    const std::uint32_t kPopupStride = kPopupCoded.width * 4u + 32u;
    const render::Rect2D kPopupVisible{render::Origin2D{5, 3}, kPopupRect.size};
    const Texel kPopupColor{240, 200, 160, 255};
    browser->queue_frame(
        shell::BrowserLayer::popup, kPopupCoded, kPopupVisible,
        padded_frame(kPopupCoded, kPopupStride, kPopupVisible, kPopupColor, kMarginColor),
        kPopupStride);
    SMOKE_CHECK(manager.pump_once(3'000), "the loop ran with a popup");
    SMOKE_CHECK(editor->compositor().popup_visible(), "the popup is visible");
    SMOKE_CHECK(editor->compositor().stats().popup_draws == 1, "the popup layer was composited");
    SMOKE_CHECK(blitter->blit_count() == 2, "the popup frame was presented");

    {
        const std::vector<std::uint8_t>& surface = editor->compositor().cpu_surface();
        // INSIDE the popup rect: the popup's pixels.
        const Texel inside =
            sample(surface, kWindowSize, kPopupRect.origin.x + 2, kPopupRect.origin.y + 2);
        SMOKE_CHECK(inside.b == 240 && inside.g == 200 && inside.r == 160,
                    "the popup's pixels are composited at the popup rect");
        // OUTSIDE it: still the view's. Asserting BOTH is what distinguishes a real second layer
        // from a popup that was dropped (which would leave the view everywhere) or one drawn
        // full-window (which would leave the popup everywhere).
        const Texel outside = sample(surface, kWindowSize, 5, 5);
        SMOKE_CHECK(outside.b == 20 && outside.g == 40 && outside.r == 60,
                    "the view's pixels survive outside the popup rect");
        SMOKE_CHECK(free_of(surface, kWindowSize, kMarginColor),
                    "neither the view nor the popup leaked allocation margin into the present");
    }

    // The popup hiding DROPS the layer — CEF reuses that texture for the next dropdown.
    browser->queue_popup_state(false, render::Rect2D{});
    browser->queue_frame(shell::BrowserLayer::view, coded, kViewVisible,
                         padded_frame(coded, kPaddedStride, kViewVisible, kViewColor, kMarginColor),
                         kPaddedStride);
    SMOKE_CHECK(manager.pump_once(4'000), "the loop ran after the popup closed");
    SMOKE_CHECK(!editor->compositor().popup_visible(), "the popup is hidden");
    SMOKE_CHECK(editor->compositor().stats().popup_draws == 1, "no stale popup layer was drawn");
    {
        const Texel where_popup_was = sample(editor->compositor().cpu_surface(), kWindowSize,
                                             kPopupRect.origin.x + 2, kPopupRect.origin.y + 2);
        SMOKE_CHECK(where_popup_was.b == 20 && where_popup_was.g == 40 && where_popup_was.r == 60,
                    "the view repainted over where the popup was");
    }

    // ------------------------------------------------- 5. the input round trip
    backend->post(pointer_event(shell::PointerAction::move, 200, 100)); // chrome -> the browser
    backend->post(pointer_event(shell::PointerAction::down, 200, 100, shell::MouseButton::left));
    backend->post(pointer_event(shell::PointerAction::up, 200, 100, shell::MouseButton::left));
    backend->post(pointer_event(shell::PointerAction::move, 20, 20)); // the viewport -> native path

    shell::ShellEvent wheel;
    wheel.kind = shell::ShellEventKind::pointer;
    wheel.pointer.action = shell::PointerAction::wheel;
    wheel.pointer.position = shell::PointI{200, 100};
    wheel.pointer.wheel_delta_y = -120;
    backend->post(wheel);

    shell::ShellEvent key;
    key.kind = shell::ShellEventKind::key;
    key.key.action = shell::KeyAction::raw_key_down;
    key.key.windows_key_code = 'K';
    backend->post(key);

    shell::ShellEvent character;
    character.kind = shell::ShellEventKind::key;
    character.key.action = shell::KeyAction::character;
    character.key.character = U'K';
    backend->post(character);

    SMOKE_CHECK(manager.pump_once(5'000), "the loop ran with input");
    SMOKE_CHECK(editor->input().pointer_dispatches() == 5, "every pointer sample was arbitrated");
    // Four of the five reached the browser; the viewport sample took the native path.
    SMOKE_CHECK(browser->pointers().size() == 4u, "mouse + wheel round-tripped to the browser");
    SMOKE_CHECK(browser->pointers()[3].wheel_delta_y == -120, "the wheel delta round-tripped");
    SMOKE_CHECK(browser->keys().size() == 2u, "keyboard round-tripped to the browser");

    // ------------------------------------------------- 6. resize + DPI handled live
    shell::ShellEvent resize;
    resize.kind = shell::ShellEventKind::resize;
    resize.size = render::Extent2D{400, 240};
    backend->post(resize);
    SMOKE_CHECK(manager.pump_once(6'000), "the loop ran through a resize");
    SMOKE_CHECK(editor->compositor().size().width == 400u, "the compositor took the new size");
    SMOKE_CHECK(browser->last_logical_size().width == 400u,
                "the browser was told about the resize (WasResized)");

    shell::ShellEvent dpi;
    dpi.kind = shell::ShellEventKind::dpi_changed;
    dpi.dpi = shell::DpiScale{192}; // 2x
    backend->post(dpi);
    SMOKE_CHECK(manager.pump_once(7'000), "the loop ran through a DPI change");
    SMOKE_CHECK(browser->last_dpi().dpi == 192u, "the browser got the new device scale factor");
    // The browser's view rect is DIP: 400 physical at 2x is 200 logical. Reporting the physical
    // size would lay the document out at twice the intended size.
    SMOKE_CHECK(browser->last_logical_size().width == 200u,
                "the browser's view rect is in DIP, not physical pixels");

    // ------------------------------------------------- 7. placement persists (the Shell's own file)
    backend->apply_placement(shell::WindowPlacement{"smoke-monitor", 64, 48, 400, 240, false});
    SMOKE_CHECK(manager.pump_once(8'000), "the loop ran after a window move");
    manager.shutdown();
    SMOKE_CHECK(manager.state_store().write_count() >= 1,
                "the shutdown flushed the pending session state");
    SMOKE_CHECK(fs::exists(shell::editor_state_path(project)),
                ".editor/editor-state.json was written by the Shell");

    {
        shell::EditorStateStore reopened(project);
        bool loaded = false;
        reopened.load(&loaded);
        SMOKE_CHECK(loaded, "the persisted editor state reloads");
        SMOKE_CHECK(!reopened.state().windows.empty(), "a window placement was persisted");
        if (!reopened.state().windows.empty())
        {
            SMOKE_CHECK(reopened.state().windows[0].x == 64, "the placement round-tripped");
            SMOKE_CHECK(reopened.state().windows[0].width == 400u, "the placement size round-tripped");
        }
    }

    fs::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-shell-smoke] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("[editor-shell-smoke] PASS: software-OSR composited + presented (%d frames), "
                "PET_POPUP layer composited, input round-tripped, resize + DPI handled live, "
                "window placement persisted\n",
                blitter->blit_count());
    return 0;
}
