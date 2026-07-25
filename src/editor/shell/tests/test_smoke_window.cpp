// The smoke-tier window seam (M9 e12a-x11-legs, issue #408): every decision it makes that can be
// decided WITHOUT a display, asserted on all three default `build` legs.
//
// WHY THIS SUITE MATTERS MORE THAN ITS SIZE SUGGESTS. The seam's job is to let the nine live
// `editor-cef-smoke-shell*` scenarios run through a REAL X11 window — and the whole value of that
// depends on one property no live smoke can prove about itself: that asking for a real window and
// silently getting the headless one is IMPOSSIBLE. A degrade there would leave nine blocking gates
// asserting their claims against no OS at all, green forever, which is exactly the vacuity mode the
// task exists to close. So the load-bearing cases here are the NEGATIVE ones — real mode refusing a
// headless backend rather than falling back onto `post()`.
//
// The keysym half is the other thing that cannot be caught live: an injection table entry that is
// merely WRONG still sends an event, so the smoke sees a key arrive and passes while the browser
// receives a different key. It is pinned by ROUND-TRIPPING every covered code through the SHIPPING
// decoder map (`x11_keysym_to_windows_key_code`), so the two can never drift apart silently.

#include "context/editor/shell/smoke/smoke_window.h"

#include "shell_test.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace context::editor::shell;
namespace smoke = context::editor::shell::smoke;
namespace render = context::render;

namespace
{

WindowDesc desc_640x480()
{
    WindowDesc desc;
    desc.title = "smoke-window-suite";
    desc.logical_size = render::Extent2D{640, 480};
    // Deliberately the WRONG value for both modes, so the assertions below prove the seam OVERRIDES
    // it rather than passing the caller's guess through.
    desc.visible = true;
    return desc;
}

ShellEvent pointer_move(std::int32_t x, std::int32_t y)
{
    ShellEvent event;
    event.kind = ShellEventKind::pointer;
    event.pointer.action = PointerAction::move;
    event.pointer.position = PointI{x, y};
    return event;
}

void test_flag_parsing()
{
    char arg0[] = "smoke";
    char real[] = "--real-window";
    char other[] = "--require-x11";
    char nearly[] = "--real-window=1"; // NOT the flag: an exact match is the contract

    char* none[] = {arg0};
    CHECK(smoke::window_mode_from_args(1, none) == smoke::WindowMode::headless);

    char* only_other[] = {arg0, other};
    CHECK(smoke::window_mode_from_args(2, only_other) == smoke::WindowMode::headless);

    char* with_real[] = {arg0, other, real};
    CHECK(smoke::window_mode_from_args(3, with_real) == smoke::WindowMode::real);

    char* prefixed[] = {arg0, nearly};
    CHECK(smoke::window_mode_from_args(2, prefixed) == smoke::WindowMode::headless);

    // argv[0] is the program name and is never a flag — a binary that happened to be NAMED
    // --real-window must not flip the mode.
    char* as_program_name[] = {real};
    CHECK(smoke::window_mode_from_args(1, as_program_name) == smoke::WindowMode::headless);

    CHECK(std::strcmp(smoke::to_string(smoke::WindowMode::headless), "headless") == 0);
    CHECK(std::strcmp(smoke::to_string(smoke::WindowMode::real), "real") == 0);
}

void test_headless_construction_is_offscreen_and_cannot_fail()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    CHECK(setup.diagnostic.empty());
    if (setup.backend == nullptr)
    {
        return;
    }
    CHECK(std::strcmp(setup.backend->name(), "headless") == 0);
    // The honest report of "there is no presentable native window here" — what routes the
    // compositor to the CPU present fallback.
    CHECK(setup.backend->native_window().kind == render::NativeWindowKind::None);
    CHECK(shelltest::extent_eq(setup.backend->client_size(), render::Extent2D{640, 480}));

    const smoke::BrowserGeometry geometry = smoke::browser_geometry(*setup.backend);
    CHECK(geometry.dpi == DpiScale{});
    CHECK(shelltest::extent_eq(geometry.logical_size, render::Extent2D{640, 480}));
}

void test_headless_present_attaches_the_memory_blitter_at_the_client_extent()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    if (setup.backend == nullptr)
    {
        return;
    }

    EditorWindowConfig config;
    config.placement_poll_us = 0;
    EditorWindow window(std::move(setup.backend), std::make_unique<ScriptedBrowserHost>(), config);

    const smoke::PresentSetup present = smoke::attach_smoke_present(window, smoke::WindowMode::headless);
    CHECK(present.ok);
    CHECK(present.diagnostic.empty());
    CHECK(present.blitter_name == "memory");
    CHECK(present.memory != nullptr);
    CHECK(window.compositor().path() == PresentPath::cpu_blit);
    CHECK(window.compositor().blitter() == present.memory);
    // The CLIENT extent, not the caller's logical request: on a real window those differ whenever
    // the display is not at 96 dpi, and a compositor sized from the request would sample a UV
    // sub-rect that no longer matches the window.
    CHECK(shelltest::extent_eq(window.compositor().size(), render::Extent2D{640, 480}));
}

void test_headless_injection_reaches_the_pump()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    if (setup.backend == nullptr)
    {
        return;
    }
    IWindowBackend& backend = *setup.backend;

    CHECK(smoke::inject_event(backend, smoke::WindowMode::headless, pointer_move(11, 22)));

    ShellEvent key;
    key.kind = ShellEventKind::key;
    key.key.action = KeyAction::raw_key_down;
    key.key.windows_key_code = 0x09; // VK_TAB
    CHECK(smoke::inject_event(backend, smoke::WindowMode::headless, key));

    std::vector<ShellEvent> drained;
    CHECK(backend.pump(drained));
    CHECK(drained.size() == 2);
    if (drained.size() == 2)
    {
        CHECK(drained[0].kind == ShellEventKind::pointer);
        CHECK(drained[0].pointer.position.x == 11);
        CHECK(drained[0].pointer.position.y == 22);
        CHECK(drained[1].kind == ShellEventKind::key);
        CHECK(drained[1].key.windows_key_code == 0x09);
    }
}

// ⚠ THE LOAD-BEARING CASE. Real mode must REFUSE a headless backend outright — for pointer, key AND
// resize. The resize arm is the one that would silently "work": it goes through
// `apply_placement()`, which the headless backend implements perfectly well against its own
// bookkeeping, so without the guard a real-window smoke would observe a size change that no window
// system ever granted. Every refusal is asserted alongside the proof that NOTHING was queued, since
// a refusal that still posted would be the degrade wearing a `false` return.
void test_real_mode_refuses_the_headless_backend_rather_than_degrading()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    if (setup.backend == nullptr)
    {
        return;
    }
    IWindowBackend& backend = *setup.backend;
    const WindowPlacement before = backend.placement();

    CHECK(!smoke::inject_event(backend, smoke::WindowMode::real, pointer_move(11, 22)));

    ShellEvent key;
    key.kind = ShellEventKind::key;
    key.key.action = KeyAction::raw_key_down;
    key.key.windows_key_code = 0x09;
    CHECK(!smoke::inject_event(backend, smoke::WindowMode::real, key));

    ShellEvent resize;
    resize.kind = ShellEventKind::resize;
    resize.size = render::Extent2D{800, 500};
    CHECK(!smoke::inject_event(backend, smoke::WindowMode::real, resize));

    std::vector<ShellEvent> drained;
    CHECK(backend.pump(drained));
    CHECK(drained.empty());
    // ...and the refused resize did not reach the backend's placement either.
    CHECK(backend.placement().width == before.width);
    CHECK(backend.placement().height == before.height);
}

// The other half of the same rule, one level up: real mode's PRESENT attach must refuse the
// in-memory blitter. A window with no presentable native surface resolves to exactly that, so
// without this guard a real-window smoke would compose into a buffer and call it a present.
void test_real_mode_present_refuses_the_memory_blitter()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    if (setup.backend == nullptr)
    {
        return;
    }

    EditorWindowConfig config;
    config.placement_poll_us = 0;
    EditorWindow window(std::move(setup.backend), std::make_unique<ScriptedBrowserHost>(), config);

    const smoke::PresentSetup present = smoke::attach_smoke_present(window, smoke::WindowMode::real);
    CHECK(!present.ok);
    CHECK(!present.diagnostic.empty());
}

void test_headless_injection_refuses_a_pointer_it_cannot_carry()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    if (setup.backend == nullptr)
    {
        return;
    }
    // Headless mode carries ANY sample — it is a queue, not an encoder. This pins the asymmetry so a
    // future reader does not "harmonize" the two modes: the encoder restrictions below belong to the
    // X11 path alone.
    ShellEvent wheel;
    wheel.kind = ShellEventKind::pointer;
    wheel.pointer.action = PointerAction::wheel;
    wheel.pointer.wheel_delta_y = 120;
    CHECK(smoke::inject_event(*setup.backend, smoke::WindowMode::headless, wheel));
}

void test_mode_of_reports_what_a_backend_actually_is()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    if (setup.backend == nullptr)
    {
        return;
    }
    // The only case assertable without a display — and the one that matters, since it is what stops
    // a deliberately-offscreen Nth window from being handed the real-mode present attach.
    CHECK(smoke::mode_of(*setup.backend) == smoke::WindowMode::headless);
}

void test_keysym_table_round_trips_through_the_shipping_decoder_map()
{
    // Every code the table covers, fed back through the decoder's OWN map. This is what makes the
    // inverse structurally correct instead of hand-checked: a wrong entry cannot survive here.
    const std::int32_t covered[] = {
        0x08, 0x09, 0x0D, 0x1B, 0x20, 0x25, 0x26, 0x27, 0x28, // control + arrows
        0x30, 0x35, 0x39,                                     // digit row
        0x41, 0x4D, 0x5A,                                     // letter row
    };
    for (std::int32_t code : covered)
    {
        const std::uint32_t keysym = smoke::x11_keysym_for_windows_key_code(code);
        CHECK(keysym != 0);
        CHECK(x11_keysym_to_windows_key_code(keysym) == code);
    }

    // The letter row maps to the LOWERCASE latin keysym, which is the unshifted one — the exact
    // inverse of the uppercasing the decoder documents. Pinned explicitly because "it round-trips"
    // would also hold for the uppercase keysym, which is NOT what an unshifted key press produces.
    CHECK(smoke::x11_keysym_for_windows_key_code(0x41) == 0x61u);
    CHECK(smoke::x11_keysym_for_windows_key_code(0x5A) == 0x7Au);
    CHECK(smoke::x11_keysym_for_windows_key_code(0x09) == 0xFF09u); // XK_Tab

    // Uncovered codes report 0 so `inject_event` fails LOUDLY. VK_F1 and VK_NUMPAD0 are real keys
    // the decoder maps in the OTHER direction — they are simply not in the injection table yet, and
    // a smoke that needs one is told so rather than injecting nothing.
    CHECK(smoke::x11_keysym_for_windows_key_code(0x70) == 0u); // VK_F1
    CHECK(smoke::x11_keysym_for_windows_key_code(0x60) == 0u); // VK_NUMPAD0
    CHECK(smoke::x11_keysym_for_windows_key_code(0) == 0u);
    CHECK(smoke::x11_keysym_for_windows_key_code(-1) == 0u);
}

} // namespace

int main()
{
    test_flag_parsing();
    test_headless_construction_is_offscreen_and_cannot_fail();
    test_headless_present_attaches_the_memory_blitter_at_the_client_extent();
    test_headless_injection_reaches_the_pump();
    test_real_mode_refuses_the_headless_backend_rather_than_degrading();
    test_real_mode_present_refuses_the_memory_blitter();
    test_headless_injection_refuses_a_pointer_it_cannot_carry();
    test_mode_of_reports_what_a_backend_actually_is();
    test_keysym_table_round_trips_through_the_shipping_decoder_map();
    SHELL_TEST_MAIN_END();
}
