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
// The key-table half is the other thing that cannot be caught live: an injection table entry that is
// merely WRONG still sends an event, so the smoke sees a key arrive and passes while the browser
// receives a different key. BOTH tables — X11 keysyms and (since M9 e12c-3) macOS virtual keys — are
// pinned by SWEEPING the whole single-byte VK range back through their SHIPPING decoder map
// (`x11_keysym_to_windows_key_code` / `ns_key_code_to_windows_key_code`), so neither can drift from
// its decoder silently, including for a code a LATER task adds, which a hand-listed sample set would
// miss. A third sweep pins the two tables to EACH OTHER: a VK the Linux arm accepts and the macOS arm
// refuses is a smoke that works on one platform and silently cannot inject on the other.
//
// WHAT THIS SUITE DELIBERATELY DOES NOT COVER. Each platform encoder's own refusals (a wheel or leave
// sample, a press carrying MouseButton::none or MouseButton::middle on Cocoa, a `character` key
// action) are reached only with a real window — they live behind `CONTEXT_SHELL_SMOKE_HAS_X11` or
// inside the Objective-C++ TU — so they belong to the live `editor-shell-x11-window` /
// `editor-shell-cocoa-window` smokes, not here. Recorded so the gap is visible rather than assumed
// covered by `test_headless_injection_carries_what_the_x11_encoder_would_refuse`, which pins the
// opposite asymmetry.

#include "context/editor/shell/smoke/smoke_window.h"

#include "shell_test.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace context::editor::shell;
namespace smoke = context::editor::shell::smoke;
namespace render = context::render;

namespace
{

// `shelltest::fail` prints only the stringified expression, and where these suites assert an EMPTY
// diagnostic the string itself is the entire payload — on the Linux CI leg, the one place a real
// refusal can fire, there is no debugger to read it out of. So print it alongside.
void report_diagnostic(const char* what, const std::string& diagnostic)
{
    if (!diagnostic.empty())
    {
        std::fprintf(stderr, "  %s diagnostic: %s\n", what, diagnostic.c_str());
    }
}

WindowDesc desc_640x480()
{
    WindowDesc desc;
    desc.title = "smoke-window-suite";
    desc.logical_size = render::Extent2D{640, 480};
    // Deliberately the WRONG value for real mode, which forces it true. NOTE it is UNOBSERVABLE
    // here: HeadlessWindowBackend reads only title/placement/logical_size out of the desc, so no
    // assertion in this file can see the override — `visible` is load-bearing only in real mode,
    // where the window must actually map. Set wrong on purpose so a future display-capable test
    // does not have to remember to.
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
    report_diagnostic("construction", setup.diagnostic);
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

// The geometry claim, asserted where it can actually FAIL. At scale 1.0 every candidate
// implementation agrees — `to_logical`, `to_physical` and a raw pass-through of `client_size()` all
// return 640x480 — so the case above pins nothing about the conversion. That conversion is the
// seam's whole reason for existing (`smoke_window.h`: a 640x480 logical request on a 120-dpi
// display is NOT a 640x480 client area, and a CEF view rect sized from the request lays the
// document out at the wrong size), and its value is what the nine smokes thread into
// `cef_options.logical_size` / `cef_options.dpi`. So drive the DPI off 1.0 and assert the two
// extents DISAGREE, in the direction only `to_logical` produces.
void test_browser_geometry_converts_the_client_extent_at_a_non_identity_dpi()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    if (setup.backend == nullptr)
    {
        return;
    }
    IWindowBackend& backend = *setup.backend;

    ShellEvent dpi_changed;
    dpi_changed.kind = ShellEventKind::dpi_changed;
    dpi_changed.dpi = DpiScale{120}; // 1.25x — what a real per-monitor-v2 window reports
    CHECK(smoke::inject_event(backend, smoke::WindowMode::headless, dpi_changed));
    std::vector<ShellEvent> drained;
    CHECK(backend.pump(drained));

    // The backend applied it to ITSELF, exactly as an OS window does.
    CHECK(backend.dpi() == DpiScale{120});
    // The CLIENT extent is untouched by a scale change: those are physical pixels.
    CHECK(shelltest::extent_eq(backend.client_size(), render::Extent2D{640, 480}));

    const smoke::BrowserGeometry geometry = smoke::browser_geometry(backend);
    CHECK(geometry.dpi == DpiScale{120});
    // 640/1.25 x 480/1.25. A pass-through would report 640x480 and `to_physical` 800x600, so this
    // is the assertion that names which conversion ran.
    CHECK(shelltest::extent_eq(geometry.logical_size, render::Extent2D{512, 384}));
    CHECK(!shelltest::extent_eq(geometry.logical_size, backend.client_size()));
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
    report_diagnostic("headless present", present.diagnostic);
    CHECK(present.diagnostic.empty());
    CHECK(present.blitter_name == "memory");
    CHECK(window.compositor().path() == PresentPath::cpu_blit);
    // The compositor OWNS the blitter — the seam hands back its name, never a handle (see
    // `PresentSetup`), so this asserts one really attached rather than re-checking a pointer the
    // seam just returned.
    CHECK(window.compositor().blitter() != nullptr);
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

// The other half of the same rule, one level up: real mode's PRESENT attach must refuse a window it
// cannot really present from, rather than composing into a buffer and calling it a present.
//
// ⚠ NAMED FOR THE BRANCH IT ACTUALLY REACHES. A headless backend reports NativeWindowKind::None, so
// `make_present_blitter` returns a NULL blitter — it has no arm that can return a MemoryBlitter at
// all. `attach_smoke_present`'s `blitter_name == "memory"` guard is therefore defence in depth
// against a future memory fallback in that selection (see the comment there), NOT what this case
// exercises; the `blitter_name.empty()` assertion below is what pins which of the two fired, since
// the memory branch sets the name before returning and this one returns before it.
void test_real_mode_present_refuses_a_window_with_no_native_surface()
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
    CHECK(present.blitter_name.empty());
    // The compositor was left with NO blitter, so a smoke that ignored `ok` still could not present.
    CHECK(window.compositor().blitter() == nullptr);
}

// The asymmetry, pinned so a future reader does not "harmonize" the two modes: headless mode CARRIES
// any sample — it is a queue, not an encoder — including the ones the X11 encoder refuses outright.
// (Those refusals need a display and belong to the live smoke; see the file header.)
void test_headless_injection_carries_what_the_x11_encoder_would_refuse()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    if (setup.backend == nullptr)
    {
        return;
    }
    // A wheel sample: refused by the X11 encoder (it has no arm for it), carried here.
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
    // A SWEEP, not a sample list. Every code the table covers is fed back through the decoder's OWN
    // map, which is what makes the inverse structurally correct instead of hand-checked: a wrong
    // entry cannot survive here. Sweeping rather than listing is the load-bearing part — the table
    // is a `switch` plus two ranges, and a `case` a LATER task adds would simply not be round-tripped
    // by a hand-maintained array, which is exactly the silent drift this suite exists to forbid.
    // The single-byte range covers every VK code the table can name.
    int covered_count = 0;
    for (std::int32_t code = 0; code <= 0xFF; ++code)
    {
        const std::uint32_t keysym = smoke::x11_keysym_for_windows_key_code(code);
        if (keysym == 0)
        {
            continue; // not in the injection table — asserted loud-and-zero below
        }
        ++covered_count;
        CHECK(x11_keysym_to_windows_key_code(keysym) == code);
    }
    // A FLOOR, so the sweep cannot go vacuously green on a table that lost entries — and so that
    // ADDING one never reds this line (the round-trip above is what polices an addition).
    CHECK(covered_count >= 45); // 9 named + 26 letters + 10 digits as of e12a-x11-legs

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

// The macOS twin of the sweep above (M9 e12c-3), and it matters MORE than its X11 sibling for one
// structural reason: the macOS virtual key codes are POSITIONAL, so the inverse is a hand-written
// table all the way down with no run to fall back on — 0x00 is `A`, 0x06 is `Z`, and the digit row is
// TRANSPOSED at 5/6. A single transposed entry is exactly the defect that delivers an event carrying a
// different key, and only the round trip through the SHIPPING decoder can catch it.
void test_ns_virtual_key_table_round_trips_through_the_shipping_decoder_map()
{
    int covered_count = 0;
    for (std::int32_t code = 0; code <= 0xFF; ++code)
    {
        const smoke::NsVirtualKey key = smoke::ns_virtual_key_for_windows_key_code(code);
        if (!key.covered)
        {
            continue;
        }
        ++covered_count;
        CHECK(ns_key_code_to_windows_key_code(key.key_code) == code);
    }
    // A FLOOR, so the sweep cannot go vacuously green on a table that lost entries — and so that
    // ADDING one never reds this line (the round trip above is what polices an addition).
    CHECK(covered_count >= 45); // 9 named + 26 letters + 10 digits as of e12c-3

    // ⚠ THE SENTINEL CASE, and the whole reason NsVirtualKey carries a `covered` FLAG instead of
    // overloading a zero return the way the X11 table legitimately can: macOS key code 0x00 IS a real
    // key. So `covered` must be TRUE for VK_A while its `key_code` is 0 — a reader that treated 0 as
    // "not in the table" would refuse to inject `A` forever, silently.
    const smoke::NsVirtualKey letter_a = smoke::ns_virtual_key_for_windows_key_code('A');
    CHECK(letter_a.covered);
    CHECK(letter_a.key_code == 0u);

    // The two positional hazards, pinned explicitly. "It round-trips" would ALSO hold for a table that
    // had swapped a pair consistently in both directions, and the decoder is the shipping side — so
    // these name the actual macOS codes rather than deriving them.
    CHECK(smoke::ns_virtual_key_for_windows_key_code('Z').key_code == 0x06u); // the ASDF row, not Z=25
    CHECK(smoke::ns_virtual_key_for_windows_key_code('5').key_code == 0x17u); // 5 AFTER 6...
    CHECK(smoke::ns_virtual_key_for_windows_key_code('6').key_code == 0x16u); // ...which is 0x16

    // The `text` column. A wrong character still delivers an event, so it is pinned separately — and
    // in macOS's OWN spellings, which are not the Windows VK intuition: Backspace produces U+007F
    // (NSDeleteCharacter), Return U+000D, and an arrow key its AppKit private-use function-key code
    // point. The letters inject the UNSHIFTED (lowercase) character, exactly as an unmodified press
    // produces — the same claim the X11 sweep makes about the lowercase keysym.
    CHECK(smoke::ns_virtual_key_for_windows_key_code(0x09).text == U'\t');     // VK_TAB
    CHECK(smoke::ns_virtual_key_for_windows_key_code(0x0D).text == U'\r');     // VK_RETURN
    CHECK(smoke::ns_virtual_key_for_windows_key_code(0x08).text == U'\u007F'); // VK_BACK -> DELETE
    CHECK(smoke::ns_virtual_key_for_windows_key_code(0x25).text == U'\uF702'); // NSLeftArrowFunctionKey
    CHECK(smoke::ns_virtual_key_for_windows_key_code('A').text == U'a');
    CHECK(smoke::ns_virtual_key_for_windows_key_code('Z').text == U'z');
    CHECK(smoke::ns_virtual_key_for_windows_key_code('7').text == U'7');

    // Uncovered codes report `covered == false` so `inject_event` fails LOUDLY. VK_F1 and VK_NUMPAD0
    // are real keys the decoder maps in the OTHER direction — they are simply not in the injection
    // table yet, and a smoke that needs one is told so rather than injecting nothing.
    CHECK(!smoke::ns_virtual_key_for_windows_key_code(0x70).covered); // VK_F1
    CHECK(!smoke::ns_virtual_key_for_windows_key_code(0x60).covered); // VK_NUMPAD0
    CHECK(!smoke::ns_virtual_key_for_windows_key_code(0).covered);
    CHECK(!smoke::ns_virtual_key_for_windows_key_code(-1).covered);
}

// ⚠ THE CROSS-ARM CONSISTENCY SWEEP — the case neither per-arm sweep above can make, and the one that
// catches the most likely FUTURE defect in this seam. Both real-mode arms exist to serve the SAME
// smokes, so a VK the X11 arm accepts and the Cocoa arm refuses (or vice versa) is a smoke that
// injects fine on one OS and silently cannot on the other — a per-platform capability gap that no
// single-platform test and no live smoke can see, because each live smoke only ever runs on its own
// OS. Sweeping the whole VK range pins the two tables to each other, so extending one without the
// other reds HERE, on all three legs, instead of on somebody's next macOS-only CI round.
void test_both_real_mode_arms_accept_exactly_the_same_key_codes()
{
    int agreed = 0;
    for (std::int32_t code = 0; code <= 0xFF; ++code)
    {
        const bool x11_covers = smoke::x11_keysym_for_windows_key_code(code) != 0;
        const bool cocoa_covers = smoke::ns_virtual_key_for_windows_key_code(code).covered;
        CHECK(x11_covers == cocoa_covers);
        if (x11_covers && cocoa_covers)
        {
            ++agreed;
        }
    }
    // The floor again, so a pair of tables that BOTH lost their entries cannot pass this vacuously
    // (two empty tables agree perfectly).
    CHECK(agreed >= 45);
}

} // namespace

// THE UNIT BOUNDARY THE RESIZE ARM CROSSES, asserted where it can actually FAIL — and it could not,
// until e12c-3's review. `inject_event`'s resize arm writes a PHYSICAL-PIXEL `event.size` into a
// `WindowPlacement`, which is physical pixels on X11/Win32 but COCOA POINTS on macOS
// (`CocoaWindowBackend::placement()`'s own "⚠ IN COCOA POINTS" note). The seam assigned it straight
// through, so on a 2x display it asked for TWICE the requested size.
//
// ⚠ WHY THIS IS A UNIT TEST AND NOT A LIVE-SMOKE ASSERTION, which is the whole reason the defect
// survived a green live gate: `editor-shell-cocoa-window` only asserts that the granted geometry
// CHANGED, which a 2x-too-large request satisfies just as well as a correct one. And at scale 1.0 —
// what a non-Retina runner reports — the conversion is a literal no-op, so no assertion on such a
// host can distinguish a present divide from a missing one. So the DPI is driven off 1.0 here and
// the expected values are spelled as LITERALS, the same discipline
// `test_browser_geometry_converts_the_client_extent_at_a_non_identity_dpi` above records.
void test_ns_extent_to_points_inverts_the_shipping_physical_conversion()
{
    // Identity at the reference DPI — the case that must NOT change, since it is every non-Retina
    // display and every X11/Win32 backend.
    CHECK(smoke::ns_extent_to_points(640, DpiScale{96}) == 640u);

    // 2x: the case a pass-through gets wrong by a factor of two.
    CHECK(smoke::ns_extent_to_points(640, DpiScale{192}) == 320u);
    CHECK(smoke::ns_extent_to_points(400, DpiScale{192}) == 200u);
    // 1.25x and 1.5x — factors that are not exactly representable as a binary float, which is why
    // the implementation divides by `dpi.dpi` (the source of truth) rather than multiplying by
    // 1/factor().
    CHECK(smoke::ns_extent_to_points(800, DpiScale{120}) == 640u);
    CHECK(smoke::ns_extent_to_points(960, DpiScale{144}) == 640u);
    // 0.5x, the other side of 1.0: fewer physical pixels than points.
    CHECK(smoke::ns_extent_to_points(320, DpiScale{48}) == 640u);

    // ROUND TRIP against the SHIPPING forward conversion, so the two can never drift: the same
    // single-source-of-truth discipline the key tables get, one axis over.
    //
    // ⚠ THE EXACTNESS CLAIM IS CONDITIONAL, and stating it loosely would have been the vacuous
    // version. `ns_extent_to_physical` ROUNDS, so at a factor below 1.0 it is not injective — at
    // 0.5x both 17 and 18 points map to 9 physical pixels, and no inverse can recover which. (Found
    // by this very sweep over-claiming: 1@48 and 17@48 failed an unconditional equality.) So: EXACT
    // wherever the forward direction lost nothing, within ONE point everywhere else, and a counted
    // floor so the exact arm cannot quietly stop running.
    int exact_cases = 0;
    for (const std::uint32_t dpi_value : {48u, 96u, 120u, 144u, 192u, 288u})
    {
        const DpiScale dpi{dpi_value};
        for (const std::uint32_t points : {1u, 17u, 320u, 480u, 640u, 1440u})
        {
            const std::uint32_t physical = ns_extent_to_physical(static_cast<double>(points), dpi);
            CHECK(physical != 0u);
            const std::uint32_t back = smoke::ns_extent_to_points(physical, dpi);
            if ((points * dpi_value) % kReferenceDpi == 0u)
            {
                // The forward conversion was exact, so the inverse must recover `points` exactly.
                CHECK(back == points);
                ++exact_cases;
            }
            else
            {
                const std::uint32_t drift = back > points ? back - points : points - back;
                CHECK(drift <= 1u);
            }
        }
    }
    // Non-vacuity floor: 30 of the 36 pairs above have an exact forward conversion today. If a
    // future edit turned the exact arm off, this reds instead of silently degrading to the ±1
    // claim.
    CHECK(exact_cases >= 30);

    // A non-empty extent NEVER becomes empty. `apply_placement` refuses an empty rect outright, so
    // rounding 1 physical pixel at 10x down to 0 points would turn a resize REQUEST into a silent
    // no-op — the shape this seam exists to make impossible.
    CHECK(smoke::ns_extent_to_points(1, DpiScale{960}) == 1u);
    // ...but a genuinely empty input stays empty, so `is_empty` still discriminates upstream.
    CHECK(smoke::ns_extent_to_points(0, DpiScale{192}) == 0u);
}

// The OTHER half of the same rule, and the more dangerous direction: converting for a backend whose
// placement is ALREADY physical pixels would break the Linux `editor-shell-x11-window` gate and the
// nine CEF smokes. The discrimination is on the published native window KIND, so a headless backend
// must come back UNCHANGED even at a non-identity DPI — which is exactly what this asserts, on all
// three legs, with no display and no Cocoa.
//
// Honest scope: the `MetalLayer` arm's BRANCH SELECTION is reachable only with a real NSWindow, so
// it belongs to the live `editor-shell-cocoa-window` smoke. Its ARITHMETIC is
// `ns_extent_to_points`, swept above on every leg — so the part that can silently drift is fully
// covered here.
void test_placement_extent_for_physical_leaves_a_non_cocoa_backend_alone()
{
    smoke::WindowSetup setup = smoke::make_smoke_window(desc_640x480(), smoke::WindowMode::headless);
    CHECK(setup.backend != nullptr);
    if (setup.backend == nullptr)
    {
        return;
    }
    IWindowBackend& backend = *setup.backend;

    ShellEvent dpi_changed;
    dpi_changed.kind = ShellEventKind::dpi_changed;
    dpi_changed.dpi = DpiScale{192}; // 2x — where an unwanted conversion HALVES the request
    CHECK(smoke::inject_event(backend, smoke::WindowMode::headless, dpi_changed));
    std::vector<ShellEvent> drained;
    CHECK(backend.pump(drained));
    CHECK(backend.dpi() == DpiScale{192});
    // The premise: this backend publishes no layer, so it must take the identity arm.
    CHECK(backend.native_window().kind != render::NativeWindowKind::MetalLayer);

    const render::Extent2D asked{640, 400};
    const render::Extent2D got = smoke::placement_extent_for_physical(backend, asked);
    // 640x400, NOT 320x200 — a `#if defined(__APPLE__)` implementation would report the latter on
    // this very leg when built on macOS, which is the reason the discriminator is the window kind.
    CHECK(shelltest::extent_eq(got, asked));
    CHECK(!shelltest::extent_eq(got, render::Extent2D{320, 200}));
}

// The COORDINATE inverse, swept back through the shipping decoder — the sibling of the extent sweep
// above, and the case with the sharpest provenance in this suite.
//
// ⚠ THIS EXISTS BECAUSE THE LIVE SMOKE PROVABLY CANNOT CATCH IT. `editor-shell-cocoa-window`
// asserts the flip DIRECTION, a separation floor and a half-plane — never an equality, because
// AppKit scales the delivered location about the window centre (smoke_window.h, Cocoa limit 1).
// MEASURED during e12c-3's review: mutating the inverse to divide AFTER the flip rather than before
// — the exact error `ns_view_point_to_physical` warns about — left that live gate GREEN on a 2x
// host, because all three of its claims survive a uniformly wrong scale. At 1.0 the mutation is a
// literal no-op, so a 1x runner cannot see it either. Hence: driven off 1.0, round-tripped against
// the SHIPPING decoder.
void test_ns_view_point_for_physical_inverts_the_shipping_decoder_at_a_non_identity_dpi()
{
    // 2x, worked by hand so a reader can check the arithmetic without running it: a 400-point-tall
    // view is 800 physical pixels. Shell-space y=100 (near the TOP) must become 400 - 50 = 350
    // points (near Cocoa's TOP, which is the HIGH point value), and x=600 physical is 300 points.
    const NsViewGeometry two_x{400.0, DpiScale{192}, 0u};
    const smoke::NsViewPointPoints high =
        smoke::ns_view_point_for_physical(PointI{600, 100}, two_x.height_points, two_x.dpi);
    CHECK(high.x_points == 300.0);
    CHECK(high.y_points == 350.0);
    // Divide-after-flip would give (400 - 100) / 2 = 150. Pinned as a literal NON-value so that
    // specific mutation cannot pass.
    CHECK(high.y_points != 150.0);

    // ROUND TRIP against the shipping decoder, at four scales including 1.0 as a control. Every
    // coordinate is chosen to be exactly divisible by the scale, so the decoder's round-to-nearest
    // cannot blur the claim (the extent sweep above records why that matters).
    int non_identity_cases = 0;
    for (const std::uint32_t dpi_value : {96u, 144u, 192u, 288u})
    {
        const DpiScale dpi{dpi_value};
        const NsViewGeometry view{300.0, dpi, 0u}; // a 300-point-tall view at every scale
        for (const PointI position : {PointI{0, 0}, PointI{144, 288}, PointI{288, 144},
                                      PointI{576, 576}})
        {
            const smoke::NsViewPointPoints in_points =
                smoke::ns_view_point_for_physical(position, view.height_points, dpi);
            const PointI back =
                ns_view_point_to_physical(in_points.x_points, in_points.y_points, view);
            CHECK(back == position);
            if (dpi_value != 96u)
            {
                ++non_identity_cases;
            }
        }
    }
    // Non-vacuity floor: the 1.0 control alone would pass with the scale dropped entirely, so the
    // sweep is only meaningful if the non-identity scales really ran.
    CHECK(non_identity_cases >= 12);

    // A hand-constructed 0 dpi is treated as 1x rather than dividing the location away — the same
    // refuse-to-1x regime `ns_dpi_from_backing_scale` uses for a scale that names no display.
    const smoke::NsViewPointPoints degenerate =
        smoke::ns_view_point_for_physical(PointI{10, 20}, 100.0, DpiScale{0});
    CHECK(degenerate.x_points == 10.0);
    CHECK(degenerate.y_points == 80.0);
}

// The DELIVERY-TIME correction, and the case with the sharpest provenance in this suite after the
// one above: it is the arithmetic that was missing when `main` went red on both macOS jobs.
//
// ⚠ WHY THE LIVE SMOKE CANNOT PIN THIS EITHER, which is the same argument the sweep above makes.
// `editor-shell-cocoa-window` exercises this function with a ZERO origin delta on every run where
// the window holds still — and at a zero delta EVERY spelling of it is a no-op, including the two
// wrong ones that matter: dropping the dpi scale, and giving the two axes the SAME sign instead of
// opposite ones. So the live gate can only ever see this correct by accident. The two conditions
// below are exactly the two no CI runner reliably exercises: a NON-IDENTITY scale (the runner's
// virtual display is 1x, where the scale is invisible) and a NON-ZERO screen origin with a non-zero
// delta between the post and the delivery (this Mac's window holds still, where the delta is 0).
void test_ns_delivered_shift_for_window_move_corrects_a_moved_window_at_a_non_identity_dpi()
{
    // 1. THE ORDINARY CASE IS A NO-OP, at every scale. This is what makes the correction safe to
    //    apply unconditionally in the live smoke: a window that did not move changes no claim.
    for (const std::uint32_t dpi_value : {96u, 144u, 192u, 288u})
    {
        const smoke::NsDeliveredShift still = smoke::ns_delivered_shift_for_window_move(
            PointI{640, 480}, PointI{640, 480}, DpiScale{dpi_value});
        CHECK(still.dx == 0);
        CHECK(still.dy == 0);
    }

    // 2. THE SIGN ASYMMETRY, worked by hand at 1x so a reader can check it: the window moved RIGHT 10
    //    points and UP 10 points. A delivered locationInWindow gains (post - delivery) on BOTH axes,
    //    but the decoder subtracts y from the view height and passes x through, so the Shell-space
    //    displacements come out with OPPOSITE signs. Giving both axes the same sign would DOUBLE the
    //    error it is meant to cancel, and at a zero delta the two spellings are indistinguishable.
    const smoke::NsDeliveredShift diagonal =
        smoke::ns_delivered_shift_for_window_move(PointI{0, 0}, PointI{10, 10}, DpiScale{96});
    CHECK(diagonal.dx == -10);
    CHECK(diagonal.dy == 10);
    // The two equalities above are what catch the same-sign mutation; these restate the wrong answer
    // explicitly so a reader sees WHICH mutation is being excluded without recomputing it. They are
    // redundant by construction, and deliberately so — documentation, not extra discrimination.
    CHECK(diagonal.dy != -10);
    CHECK(diagonal.dx != 10);

    // 3. THE SCALE, driven off 1.0. A 2x window moved DOWN 80 POINTS displaces its already-queued
    //    samples by 160 PHYSICAL PIXELS, and the measured failure was exactly this shape. At 1.0 a
    //    dropped scale is a literal no-op, so only a non-identity case can catch it.
    const smoke::NsDeliveredShift moved_down_two_x = smoke::ns_delivered_shift_for_window_move(
        PointI{100, 300}, PointI{100, 220}, DpiScale{192});
    CHECK(moved_down_two_x.dx == 0);
    CHECK(moved_down_two_x.dy == -160);
    CHECK(moved_down_two_x.dy != -80); // the scale-dropped answer

    // 4. IT DEPENDS ON THE DELTA ALONE, NEVER ON THE ABSOLUTE SCREEN ORIGIN — the second condition
    //    neither host exercises. A window near the screen origin and one 800 points away must be
    //    corrected identically for the same movement, which is what makes the correction a property
    //    of the window rather than of the display arrangement. (The forward conversion
    //    `ns_view_point_to_physical` takes no screen origin at all, by construction; this is where a
    //    screen origin actually enters the round trip, so this is where it has to be pinned.)
    const smoke::NsDeliveredShift near_origin = smoke::ns_delivered_shift_for_window_move(
        PointI{0, 40}, PointI{-24, 8}, DpiScale{192});
    const smoke::NsDeliveredShift far_from_origin = smoke::ns_delivered_shift_for_window_move(
        PointI{800, 840}, PointI{776, 808}, DpiScale{192});
    CHECK(near_origin.dx == far_from_origin.dx);
    CHECK(near_origin.dy == far_from_origin.dy);
    // ...and the shared answer is the RIGHT one, not merely a consistent one: moved LEFT 24 points
    // and DOWN 32 points, at 2x.
    CHECK(near_origin.dx == 48);
    CHECK(near_origin.dy == -64);

    // 5. A hand-constructed 0 dpi is treated as 1x rather than scaling the correction away — the same
    //    refuse-to-1x regime `ns_view_point_for_physical` and `ns_dpi_from_backing_scale` use.
    const smoke::NsDeliveredShift degenerate =
        smoke::ns_delivered_shift_for_window_move(PointI{0, 0}, PointI{-5, -7}, DpiScale{0});
    CHECK(degenerate.dx == 5);
    CHECK(degenerate.dy == -7);
}

void test_region_mid_centres_a_live_rect_with_floor_division()
{
    // Odd extents on purpose: the midpoint divides in unsigned pixels, so a 31x41 rect centres at
    // origin + (15, 20) — the same floor the two CEF smokes' inline copies computed before this
    // helper replaced them.
    ShellRegion region;
    region.id = "chrome.caption";
    region.rect = render::Rect2D{render::Origin2D{10, 20}, render::Extent2D{31, 41}};
    const PointI mid = smoke::region_mid(region);
    CHECK(mid == (PointI{25, 40}));
}

int main()
{
    test_flag_parsing();
    test_headless_construction_is_offscreen_and_cannot_fail();
    test_browser_geometry_converts_the_client_extent_at_a_non_identity_dpi();
    test_headless_present_attaches_the_memory_blitter_at_the_client_extent();
    test_headless_injection_reaches_the_pump();
    test_real_mode_refuses_the_headless_backend_rather_than_degrading();
    test_real_mode_present_refuses_a_window_with_no_native_surface();
    test_headless_injection_carries_what_the_x11_encoder_would_refuse();
    test_mode_of_reports_what_a_backend_actually_is();
    test_keysym_table_round_trips_through_the_shipping_decoder_map();
    test_ns_virtual_key_table_round_trips_through_the_shipping_decoder_map();
    test_both_real_mode_arms_accept_exactly_the_same_key_codes();
    test_ns_extent_to_points_inverts_the_shipping_physical_conversion();
    test_placement_extent_for_physical_leaves_a_non_cocoa_backend_alone();
    test_ns_view_point_for_physical_inverts_the_shipping_decoder_at_a_non_identity_dpi();
    test_ns_delivered_shift_for_window_move_corrects_a_moved_window_at_a_non_identity_dpi();
    test_region_mid_centres_a_live_rect_with_floor_division();
    SHELL_TEST_MAIN_END();
}
