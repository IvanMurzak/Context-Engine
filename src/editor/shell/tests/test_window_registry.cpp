// The window REGISTRY (M9 e10a, design 03 §1 / §7): N windows addressable by id, created and
// destroyed on demand, with the CE #319 session-lifetime rule asserted rather than assumed.
//
// EVERYTHING HERE RUNS ON ALL THREE `build` LEGS AND THE LOCAL DEV GATE. The registry is CEF-free by
// construction — a window's browser reaches it through `IBrowserHost`, so the create/destroy
// machinery, the failure seam and the lifetime rule are exercised by a plain ctest instead of only
// by the one CI job that can link CEF. The live CEF half (a REAL second browser, a REAL
// `window.open`) is `editor-cef-smoke-shell-multiwindow`; it proves the things a fake cannot, and
// nothing else.
//
// THE ASSERTION THAT MATTERS MOST is the repeated create/destroy cycle: CE #319 was a
// teardown-ordering use-after-free with ONE window, and N windows multiply the number of such
// lifetimes AND add a mid-process destroy that did not exist before. So the test does not check
// that a destroy "works" — it checks WHEN each object dies, in both directions: the browser must be
// gone the instant the window is destroyed, and the session's bridge-side objects must NOT be, until
// the manager itself is destroyed (which the app sequences after `shell::cef::shutdown()`).

#include "context/editor/shell/shell.h"
#include "context/editor/shell/window_registry.h"

#include "shell_test.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace context::editor::shell;
namespace render = context::render;
namespace fs = std::filesystem;

namespace
{

// ------------------------------------------------------------------------------- the instruments

// Two shared counters make "when did this die" assertable. A test that only checks the registry's
// own bookkeeping would pass just as happily if the objects were leaked or freed too early — which
// is precisely the defect class CE #319 belongs to.
struct Deaths
{
    int browsers = 0;
    int surfaces = 0;
};

// A browser host that records its own destruction. Everything else is the minimum IBrowserHost
// needs; the compositor never sees a frame from it, which is fine — this suite is about lifetimes
// and identity, and the composite path has its own suite (test_compositor).
class CountingBrowserHost final : public IBrowserHost
{
public:
    CountingBrowserHost(Deaths& deaths, int* alive) : deaths_(deaths), alive_(alive)
    {
        if (alive_ != nullptr)
        {
            ++*alive_;
        }
    }

    ~CountingBrowserHost() override
    {
        ++deaths_.browsers;
        if (alive_ != nullptr)
        {
            --*alive_;
        }
    }

    [[nodiscard]] const char* name() const override { return "counting"; }
    void resize(render::Extent2D, DpiScale) override {}
    void send_pointer(const PointerDispatch&, const PointerEvent&) override {}
    void send_key(const KeyEvent&) override {}
    void set_focus(bool) override {}
    bool pump(IBrowserFrameSink&) override { return alive_flag_; }
    void execute_script(std::string_view source) override { scripts_.emplace_back(source); }
    void close() override { alive_flag_ = false; }

    [[nodiscard]] const std::vector<std::string>& scripts() const { return scripts_; }

private:
    Deaths& deaths_;
    int* alive_ = nullptr;
    std::vector<std::string> scripts_;
    bool alive_flag_ = true;
};

// Stands in for whatever per-window surface a bridge's handlers captured (a handshake today, a
// panel host in e10b). Its only job is to say when it died.
struct SurfaceProbe
{
    explicit SurfaceProbe(Deaths& deaths) : deaths_(deaths) {}
    ~SurfaceProbe() { ++deaths_.surfaces; }
    Deaths& deaths_;
};

// The app's window factory, scripted. `fail_with` non-empty makes it refuse; `drop_browser` makes it
// claim success while producing unusable parts (a DIFFERENT defect, reported differently).
struct ScriptedFactory
{
    Deaths* deaths = nullptr;
    int* browsers_alive = nullptr;
    std::string fail_with;
    bool succeed_without_error_text = false;
    bool drop_browser = false;
    int calls = 0;

    bool operator()(const WindowSpec& spec, WindowSessionParts& parts, std::string& error)
    {
        ++calls;
        if (succeed_without_error_text)
        {
            // The pathological factory: says "no" and states no reason.
            error.clear();
            return false;
        }
        if (!fail_with.empty())
        {
            error = fail_with;
            return false;
        }
        WindowDesc desc;
        desc.title = spec.title;
        desc.logical_size = spec.logical_size;
        parts.backend = std::make_unique<HeadlessWindowBackend>(desc);
        if (!drop_browser)
        {
            parts.browser = std::make_unique<CountingBrowserHost>(*deaths, browsers_alive);
        }
        parts.bridge = std::make_unique<BridgeRouter>();
        parts.surfaces.push_back(std::make_shared<SurfaceProbe>(*deaths));
        error.clear();
        return true;
    }
};

// The window the app builds itself and adopts as the primary (exactly as editor_main.cpp does).
std::unique_ptr<EditorWindow> make_primary_window(Deaths& deaths, int* browsers_alive)
{
    WindowDesc desc;
    auto backend = std::make_unique<HeadlessWindowBackend>(desc);
    auto browser = std::make_unique<CountingBrowserHost>(deaths, browsers_alive);
    EditorWindowConfig config;
    config.compositor.import_options.force_software = true;
    config.placement_poll_us = 0;
    return std::make_unique<EditorWindow>(std::move(backend), std::move(browser), config);
}

WindowSpec secondary_spec(const char* title = "Context Editor — 2")
{
    WindowSpec spec;
    spec.title = title;
    spec.logical_size = render::Extent2D{640, 480};
    spec.headless = true;
    spec.state_index = 1;
    return spec;
}

// -------------------------------------------------------------------- 1. the pure vocabulary

void test_outcome_names_are_distinct_and_stable()
{
    // The names travel into the failure report a human reads, so they are asserted rather than
    // eyeballed. All-distinct matters: two outcomes that stringify the same are indistinguishable in
    // a log, which is how a `no_factory` build gets diagnosed as a broken browser.
    const WindowCreateOutcome creates[] = {
        WindowCreateOutcome::created, WindowCreateOutcome::no_factory,
        WindowCreateOutcome::factory_failed, WindowCreateOutcome::incomplete_parts,
        WindowCreateOutcome::limit_reached};
    for (std::size_t i = 0; i < std::size(creates); ++i)
    {
        for (std::size_t j = i + 1; j < std::size(creates); ++j)
        {
            CHECK(std::string(to_string(creates[i])) != std::string(to_string(creates[j])));
        }
    }
    CHECK(std::string(to_string(WindowCreateOutcome::limit_reached)) == "limit-reached");
    CHECK(std::string(to_string(WindowDestroyOutcome::primary_refused)) == "primary-refused");
    CHECK(std::string(to_string(WindowDestroyOutcome::destroyed)) == "destroyed");
    CHECK(std::string(to_string(WindowDestroyOutcome::unknown_window)) == "unknown-window");
}

void test_the_failure_report_names_the_class_the_source_and_the_reason()
{
    WindowCreateFailure failure;
    failure.outcome = WindowCreateOutcome::factory_failed;
    failure.source = 3;
    failure.title = "Inspector";
    failure.error = "no native window backend on this platform";
    const std::string text = describe(failure);
    CHECK(shelltest::mentions(text, "factory-failed"));
    CHECK(shelltest::mentions(text, "window 3"));
    CHECK(shelltest::mentions(text, "Inspector"));
    CHECK(shelltest::mentions(text, "no native window backend"));

    // A failure with no stated reason must still SAY that. An empty tail reads as a shrug, and 03 §7
    // requires the degradation to be loud — a report nobody can act on is the silent failure wearing
    // a message.
    WindowCreateFailure silent;
    silent.error.clear();
    const std::string silent_text = describe(silent);
    CHECK(shelltest::mentions(silent_text, "no reason reported"));
}

void test_parts_validation_names_the_missing_piece()
{
    Deaths deaths;
    std::string error;

    WindowSessionParts empty;
    CHECK(!validate_window_parts(empty, error));
    CHECK(shelltest::mentions(error, "backend"));

    WindowSessionParts no_browser;
    no_browser.backend = std::make_unique<HeadlessWindowBackend>(WindowDesc{});
    CHECK(!validate_window_parts(no_browser, error));
    CHECK(shelltest::mentions(error, "browser"));

    WindowSessionParts complete;
    complete.backend = std::make_unique<HeadlessWindowBackend>(WindowDesc{});
    complete.browser = std::make_unique<CountingBrowserHost>(deaths, nullptr);
    CHECK(validate_window_parts(complete, error));
    CHECK(error.empty());

    // A bridge and a daemon client are OPTIONAL by design: a CEF-free build has no browser bridge
    // and a detached window has no connection. Requiring them would make those honest states fail.
    CHECK(complete.bridge == nullptr);
    CHECK(complete.daemon_client == nullptr);
}

// ------------------------------------------------------------------- 2. ids, peers, the primary

void test_window_zero_is_primary_and_ids_are_minted_in_order()
{
    const fs::path project = shelltest::make_temp_project("shell-registry", "ids");
    Deaths deaths;
    int alive = 0;
    ScriptedFactory factory{&deaths, &alive, {}, false, false, 0};
    {
        WindowManager manager(project);
        manager.bind_window_factory(std::ref(factory));
        manager.add(make_primary_window(deaths, &alive));

        CHECK(manager.window_count() == 1);
        CHECK(manager.window(kPrimaryWindowId) != nullptr);
        CHECK(manager.is_primary(kPrimaryWindowId));

        const WindowCreateResult second = manager.create_window(secondary_spec());
        CHECK(second.ok());
        CHECK(second.id == 1);
        CHECK(!manager.is_primary(second.id));
        CHECK(second.error.empty());

        const WindowCreateResult third = manager.create_window(secondary_spec("third"));
        CHECK(third.ok());
        CHECK(third.id == 2);

        // Peers, addressable by id — and every id resolves to a DIFFERENT window.
        const std::vector<WindowId> ids = manager.window_ids();
        CHECK(ids.size() == 3);
        CHECK(ids[0] == kPrimaryWindowId);
        CHECK(manager.window(0) != manager.window(1));
        CHECK(manager.window(1) != manager.window(2));
        CHECK(manager.window_count() == 3);
        // An id that was never minted is nullptr, not the nearest window.
        CHECK(manager.window(99) == nullptr);
    }
    shelltest::cleanup(project);
}

void test_ids_are_never_reused_after_a_destroy()
{
    const fs::path project = shelltest::make_temp_project("shell-registry", "no-reuse");
    Deaths deaths;
    int alive = 0;
    ScriptedFactory factory{&deaths, &alive, {}, false, false, 0};
    {
        WindowManager manager(project);
        manager.bind_window_factory(std::ref(factory));
        manager.add(make_primary_window(deaths, &alive));

        const WindowCreateResult first = manager.create_window(secondary_spec());
        CHECK(first.id == 1);
        CHECK(manager.destroy_window(first.id).ok());

        const WindowCreateResult second = manager.create_window(secondary_spec());
        // 2, NOT 1. Reusing an id would let a stale reference (e10b will hold plenty, one per moved
        // panel) silently address a DIFFERENT window instead of resolving to nothing.
        CHECK(second.id == 2);
        CHECK(manager.window(first.id) == nullptr);
        CHECK(manager.window(second.id) != nullptr);
    }
    shelltest::cleanup(project);
}

void test_the_primary_cannot_be_destroyed_and_an_unknown_id_is_refused()
{
    const fs::path project = shelltest::make_temp_project("shell-registry", "destroy-guard");
    Deaths deaths;
    int alive = 0;
    ScriptedFactory factory{&deaths, &alive, {}, false, false, 0};
    {
        WindowManager manager(project);
        manager.bind_window_factory(std::ref(factory));
        manager.add(make_primary_window(deaths, &alive));

        const WindowDestroyResult primary = manager.destroy_window(kPrimaryWindowId);
        CHECK(!primary.ok());
        CHECK(primary.outcome == WindowDestroyOutcome::primary_refused);
        CHECK(shelltest::mentions(primary.error, "primary"));
        // Refused means REFUSED: the window is still there and still pumping.
        CHECK(manager.window(kPrimaryWindowId) != nullptr);
        CHECK(manager.window_count() == 1);

        const WindowDestroyResult unknown = manager.destroy_window(41);
        CHECK(!unknown.ok());
        CHECK(unknown.outcome == WindowDestroyOutcome::unknown_window);
        CHECK(shelltest::mentions(unknown.error, "41"));
    }
    shelltest::cleanup(project);
}

// ------------------------------------------------------- 3. the 03 §7 create-failure degradation

void test_a_create_failure_is_reported_once_with_the_source_and_leaves_the_registry_usable()
{
    const fs::path project = shelltest::make_temp_project("shell-registry", "failure");
    Deaths deaths;
    int alive = 0;
    {
        WindowManager manager(project);
        manager.add(make_primary_window(deaths, &alive));

        std::vector<WindowCreateFailure> reports;
        manager.on_window_create_failed([&reports](const WindowCreateFailure& failure)
                                        { reports.push_back(failure); });

        // (a) No factory bound at all — an honest state (a build with no browser binding), not a
        // crash and not a silent no-op.
        const WindowCreateResult none = manager.create_window(secondary_spec("popout"), 0);
        CHECK(!none.ok());
        CHECK(none.outcome == WindowCreateOutcome::no_factory);
        CHECK(none.id == kInvalidWindowId);
        CHECK(reports.size() == 1);
        CHECK(reports[0].source == 0);
        CHECK(reports[0].title == "popout");
        CHECK(manager.create_failures() == 1);
        CHECK(manager.last_create_failure() != nullptr);
        CHECK(manager.window_count() == 1);

        // (b) The factory refuses, with a reason.
        ScriptedFactory refusing{&deaths, &alive, "the OS refused to create a window", false, false,
                                 0};
        manager.bind_window_factory(std::ref(refusing));
        const WindowCreateResult refused = manager.create_window(secondary_spec(), 7);
        CHECK(refused.outcome == WindowCreateOutcome::factory_failed);
        CHECK(shelltest::mentions(refused.error, "the OS refused"));
        CHECK(reports.size() == 2);
        CHECK(reports[1].source == 7);

        // (c) The factory refuses and states NO reason — reported as its own thing.
        ScriptedFactory mute{&deaths, &alive, {}, true, false, 0};
        manager.bind_window_factory(std::ref(mute));
        const WindowCreateResult silent = manager.create_window(secondary_spec());
        CHECK(silent.outcome == WindowCreateOutcome::factory_failed);
        CHECK(shelltest::mentions(silent.error, "no reason"));

        // (d) The factory says YES and hands back unusable parts. A different defect from (b), and
        // reported as one instead of crashing on the null browser.
        ScriptedFactory hollow{&deaths, &alive, {}, false, true, 0};
        manager.bind_window_factory(std::ref(hollow));
        const WindowCreateResult incomplete = manager.create_window(secondary_spec());
        CHECK(incomplete.outcome == WindowCreateOutcome::incomplete_parts);
        CHECK(shelltest::mentions(incomplete.error, "browser"));
        CHECK(reports.size() == 4);
        CHECK(manager.create_failures() == 4);

        // THE REGISTRY IS STILL USABLE. Four failures in a row must not have left it wedged — a
        // degradation seam that poisons the Shell is worse than the failure it reports.
        ScriptedFactory working{&deaths, &alive, {}, false, false, 0};
        manager.bind_window_factory(std::ref(working));
        const WindowCreateResult ok = manager.create_window(secondary_spec());
        CHECK(ok.ok());
        CHECK(manager.window_count() == 2);
        CHECK(reports.size() == 4); // no report on success
    }
    shelltest::cleanup(project);
}

void test_the_live_window_count_is_capped()
{
    const fs::path project = shelltest::make_temp_project("shell-registry", "cap");
    Deaths deaths;
    int alive = 0;
    ScriptedFactory factory{&deaths, &alive, {}, false, false, 0};
    {
        WindowManager manager(project);
        manager.bind_window_factory(std::ref(factory));
        manager.add(make_primary_window(deaths, &alive));

        while (manager.window_count() < kMaxEditorWindows)
        {
            CHECK(manager.create_window(secondary_spec()).ok());
        }
        const WindowCreateResult over = manager.create_window(secondary_spec());
        CHECK(!over.ok());
        CHECK(over.outcome == WindowCreateOutcome::limit_reached);
        CHECK(manager.window_count() == kMaxEditorWindows);

        // The cap is on LIVE windows, so destroying one makes room again — it is containment, not a
        // lifetime budget.
        CHECK(manager.destroy_window(1).ok());
        CHECK(manager.create_window(secondary_spec()).ok());
    }
    shelltest::cleanup(project);
}

// ---------------------------------------------------- 4. THE LIFETIME RULE (CE #319 generalised)

void test_a_destroyed_window_kills_its_browser_but_retires_its_session()
{
    const fs::path project = shelltest::make_temp_project("shell-registry", "lifetime");
    Deaths deaths;
    int browsers_alive = 0;
    ScriptedFactory factory{&deaths, &browsers_alive, {}, false, false, 0};
    {
        WindowManager manager(project);
        manager.bind_window_factory(std::ref(factory));
        manager.add(make_primary_window(deaths, &browsers_alive));

        const WindowCreateResult second = manager.create_window(secondary_spec());
        CHECK(second.ok());
        CHECK(browsers_alive == 2);
        CHECK(deaths.surfaces == 0);

        CHECK(manager.destroy_window(second.id).ok());

        // The BROWSER is gone immediately — it is the thing holding the OS resources and the raw
        // router pointer, and leaving it alive is what "destroy" must not mean.
        CHECK(browsers_alive == 1);
        CHECK(deaths.browsers == 1);

        // The SESSION is not. CEF finishes tearing a closed browser down inside CefShutdown, still
        // dispatching to the client that holds this window's `BridgeRouter*` (CE #319), so freeing
        // it here is the use-after-free. It is retired instead.
        CHECK(deaths.surfaces == 0);
        CHECK(manager.retired_session_count() == 1);
    }
    // ...and freed when the MANAGER dies, which the app sequences after shell::cef::shutdown().
    CHECK(deaths.surfaces == 1);
    shelltest::cleanup(project);
}

void test_repeated_create_destroy_tears_down_cleanly_and_leaks_nothing()
{
    // The CE #319 hazard class asserted, not assumed: 25 full create/destroy cycles in ONE manager.
    // Every browser must die on its cycle; every session must survive to the end; nothing may
    // accumulate in the live set.
    const fs::path project = shelltest::make_temp_project("shell-registry", "cycles");
    Deaths deaths;
    int browsers_alive = 0;
    constexpr int kCycles = 25;
    ScriptedFactory factory{&deaths, &browsers_alive, {}, false, false, 0};
    {
        WindowManager manager(project);
        manager.bind_window_factory(std::ref(factory));
        manager.add(make_primary_window(deaths, &browsers_alive));

        WindowId previous = kPrimaryWindowId;
        for (int cycle = 0; cycle < kCycles; ++cycle)
        {
            const WindowCreateResult created = manager.create_window(secondary_spec());
            CHECK(created.ok());
            // Ids keep climbing across cycles — the no-reuse rule holds under churn, not just once.
            CHECK(created.id > previous);
            previous = created.id;
            CHECK(manager.window_count() == 2);
            CHECK(browsers_alive == 2);

            // Pump with both windows live: the loop must not care how many there are.
            CHECK(manager.pump_once(static_cast<std::uint64_t>(cycle) * 1000u));

            CHECK(manager.destroy_window(created.id).ok());
            CHECK(manager.window_count() == 1);
            CHECK(browsers_alive == 1);           // this cycle's browser is gone NOW
            CHECK(deaths.browsers == cycle + 1);  // ...exactly one per cycle, never two, never zero
            CHECK(deaths.surfaces == 0);          // ...and no session freed early, ever
        }

        CHECK(manager.retired_session_count() == static_cast<std::size_t>(kCycles));
        CHECK(factory.calls == kCycles);

        // The primary is still alive and pumping after 25 cycles of churn around it.
        CHECK(manager.window(kPrimaryWindowId) != nullptr);
        CHECK(manager.pump_once(1'000'000u));

        // shutdown() closes the remaining window — and still frees NO session, because it runs
        // BEFORE shell::cef::shutdown() in the app.
        manager.shutdown();
        CHECK(manager.window_count() == 0);
        CHECK(browsers_alive == 0);
        CHECK(deaths.surfaces == 0);
        CHECK(manager.retired_session_count() == static_cast<std::size_t>(kCycles));

        // Idempotent, as documented — a second shutdown must not double-retire.
        manager.shutdown();
        CHECK(manager.retired_session_count() == static_cast<std::size_t>(kCycles));
    }
    // Only now: every session freed, exactly once each.
    CHECK(deaths.surfaces == kCycles);
    CHECK(deaths.browsers == kCycles + 1);
    shelltest::cleanup(project);
}

void test_shutdown_retires_the_session_of_every_window_still_open()
{
    // FOUND BY PLANTING, and it is the sharpest form of the CE #319 hazard: the app quits with
    // several windows OPEN, and `shutdown()` runs BEFORE `shell::cef::shutdown()`. Freeing a router
    // there is the original use-after-free, once per open window.
    //
    // The cycle test above did not catch a `shutdown()` that frees eagerly, because by then the only
    // window still open was the PRIMARY — which the app builds itself and adopts with no session
    // objects at all, so there was nothing to free. Two live secondaries is the shape that bites.
    const fs::path project = shelltest::make_temp_project("shell-registry", "shutdown-open");
    Deaths deaths;
    int browsers_alive = 0;
    ScriptedFactory factory{&deaths, &browsers_alive, {}, false, false, 0};
    {
        WindowManager manager(project);
        manager.bind_window_factory(std::ref(factory));
        manager.add(make_primary_window(deaths, &browsers_alive));
        CHECK(manager.create_window(secondary_spec("second")).ok());
        CHECK(manager.create_window(secondary_spec("third")).ok());
        CHECK(browsers_alive == 3);

        manager.shutdown();

        // Every browser closed — that half is what shutdown has always done.
        CHECK(manager.window_count() == 0);
        CHECK(browsers_alive == 0);
        // ...and NOT ONE session freed, because CEF has not been shut down yet.
        CHECK(deaths.surfaces == 0);
        CHECK(manager.retired_session_count() == 2);
    }
    CHECK(deaths.surfaces == 2);
    shelltest::cleanup(project);
}

void test_a_window_that_dies_on_its_own_is_retired_not_freed()
{
    // The CEF-renderer-crash / user-closed-the-window path (03 §7): pump_once drops the window. It
    // must take the same care as an explicit destroy — the browser is gone, but CEF may still be
    // finishing with the client that holds this window's router.
    const fs::path project = shelltest::make_temp_project("shell-registry", "self-death");
    Deaths deaths;
    int browsers_alive = 0;
    ScriptedFactory factory{&deaths, &browsers_alive, {}, false, false, 0};
    {
        WindowManager manager(project);
        manager.bind_window_factory(std::ref(factory));
        manager.add(make_primary_window(deaths, &browsers_alive));
        const WindowCreateResult second = manager.create_window(secondary_spec());
        CHECK(second.ok());

        // Close the secondary window's OS backend from underneath the loop.
        manager.window(second.id)->backend().close();
        CHECK(manager.pump_once(1000u)); // the primary keeps the loop alive
        CHECK(manager.window(second.id) == nullptr);
        CHECK(manager.window_count() == 1);
        CHECK(manager.retired_session_count() == 1);
        CHECK(deaths.surfaces == 0);

        // The surviving window is untouched: one window's death is not the loop's.
        CHECK(manager.window(kPrimaryWindowId) != nullptr);
    }
    CHECK(deaths.surfaces == 1);
    shelltest::cleanup(project);
}

// ------------------------------------------------------------------------ 5. per-window `origin`

void test_each_window_reports_its_own_origin()
{
    // e08a mints `origin` per WIRE CONNECTION, so N windows with N connections are N origins — the
    // property e10b/e10d's cross-window drills stand on. The registry's job is to REPORT each
    // window's own identity and never to invent one; the ids themselves come from the daemon, which
    // the `editor-session-multiclient-t2` integration drill proves over a real wire.
    const fs::path project = shelltest::make_temp_project("shell-registry", "origins");
    Deaths deaths;
    int alive = 0;
    ScriptedFactory factory{&deaths, &alive, {}, false, false, 0};
    {
        WindowManager manager(project);
        manager.bind_window_factory(std::ref(factory));
        manager.add(make_primary_window(deaths, &alive));

        // Unattached is 0 — which e08a ALSO spells "not attached", so it is never counted as an
        // identity. Two unattached windows are not two origins.
        CHECK(manager.window_origin(kPrimaryWindowId) == 0);
        const WindowCreateResult second = manager.create_window(secondary_spec());
        CHECK(second.ok());
        CHECK(manager.window_origin(second.id) == 0);
        CHECK(manager.distinct_origins() == 0);

        manager.set_window_origin(kPrimaryWindowId, 7);
        manager.set_window_origin(second.id, 9);
        CHECK(manager.window_origin(kPrimaryWindowId) == 7);
        CHECK(manager.window_origin(second.id) == 9);
        CHECK(manager.distinct_origins() == 2);

        // The failure this guards: two windows sharing ONE connection would report ONE origin, and
        // every cross-window echo-suppression drill built on it would silently pass by accident.
        manager.set_window_origin(second.id, 7);
        CHECK(manager.distinct_origins() == 1);

        // An unknown window has no origin, and setting one on it is a no-op rather than a resurrect.
        CHECK(manager.window_origin(404) == 0);
        manager.set_window_origin(404, 11);
        CHECK(manager.window_origin(404) == 0);
    }
    shelltest::cleanup(project);
}

// ------------------------------------------------------------------- 6. the script-injection seam

void test_the_script_seam_is_recorded_by_the_scripted_host()
{
    // The seam the popup-suppression proof needs. The scripted host has no JS engine, so it RECORDS
    // rather than pretends — the real proof is the live CEF smoke, and this asserts only that the
    // call reaches the host it was aimed at.
    ScriptedBrowserHost host;
    CHECK(host.scripts().empty());
    host.execute_script("window.open('about:blank', '_blank');");
    CHECK(host.scripts().size() == 1);
    CHECK(shelltest::mentions(host.scripts()[0], "window.open"));
}

} // namespace

int main()
{
    test_outcome_names_are_distinct_and_stable();
    test_the_failure_report_names_the_class_the_source_and_the_reason();
    test_parts_validation_names_the_missing_piece();

    test_window_zero_is_primary_and_ids_are_minted_in_order();
    test_ids_are_never_reused_after_a_destroy();
    test_the_primary_cannot_be_destroyed_and_an_unknown_id_is_refused();

    test_a_create_failure_is_reported_once_with_the_source_and_leaves_the_registry_usable();
    test_the_live_window_count_is_capped();

    test_a_destroyed_window_kills_its_browser_but_retires_its_session();
    test_repeated_create_destroy_tears_down_cleanly_and_leaks_nothing();
    test_shutdown_retires_the_session_of_every_window_still_open();
    test_a_window_that_dies_on_its_own_is_retired_not_freed();

    test_each_window_reports_its_own_origin();
    test_the_script_seam_is_recorded_by_the_scripted_host();
    SHELL_TEST_MAIN_END();
}
