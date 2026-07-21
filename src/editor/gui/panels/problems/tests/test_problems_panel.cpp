// The Problems panel: click-to-navigate events, R-BRIDGE-008 provisional->stable promotion + stale
// discard on derivation.settled, stable re-render determinism, navigation preservation/clearing across
// a snapshot refresh, and the inline markers reflecting stability (R-HUX-005 / R-FILE-003 / R-BRIDGE-008).

#include "context/editor/gui/panels/problems/problems_model.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/uitree/panel.h"

#include "problems_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::problems;
namespace bridge = context::editor::bridge;
namespace uitree = context::editor::gui::uitree;

namespace
{

[[nodiscard]] ProblemDiagnostic diag(std::string key, std::string message, Severity sev,
                                     std::string file, std::uint32_t line,
                                     bridge::Stability stability, std::uint64_t generation)
{
    ProblemDiagnostic d;
    d.key = std::move(key); // an explicit stable identity so navigate() targets are legible
    d.code = "E-CODE";
    d.message = std::move(message);
    d.severity = sev;
    d.nav.file = std::move(file);
    d.nav.line = line;
    d.stability = stability;
    d.generation = generation;
    return d;
}

} // namespace

int main()
{
    using bridge::Stability;

    // --- Click-to-navigate emits an event; unknown / non-navigable are ignored -------------------
    {
        ProblemsPanel panel;
        panel.set_diagnostics({
            diag("P1", "bad ref", Severity::error, "a.json", 1, Stability::stable, 1),
            diag("P2", "project wide", Severity::hint, "", 0, Stability::stable, 1), // non-navigable
        });

        int fired = 0;
        ProblemNavigation last;
        panel.add_navigation_listener(
            [&](const ProblemNavigation& nav)
            {
                ++fired;
                last = nav;
            });

        CHECK(panel.navigate("P1"));
        CHECK(fired == 1);
        CHECK(last.diagnostic_identity == "P1");
        CHECK(last.target.file == "a.json");
        CHECK(panel.navigation().diagnostic_identity == "P1");

        // Unknown identity: ignored, no event.
        CHECK(!panel.navigate("nope"));
        CHECK(fired == 1);

        // Non-navigable diagnostic: cannot be a navigation target.
        CHECK(!panel.navigate("P2"));
        CHECK(fired == 1);

        panel.clear_navigation();
        CHECK(fired == 2);
        CHECK(panel.navigation().diagnostic_identity.empty());
    }

    // --- provisional -> stable promotion on the settled generation ------------------------------
    {
        ProblemsPanel panel;
        CHECK(!panel.ingest(diag("P1", "bad ref", Severity::error, "a.json", 1, Stability::settling, 5)));
        CHECK(panel.model().provisional == 1);
        CHECK(panel.inline_markers().size() == 1);
        CHECK(panel.inline_markers()[0].provisional);

        panel.on_derivation_settled(5, Stability::stable);

        CHECK(panel.generation() == 5);
        CHECK(panel.stability() == Stability::stable);
        CHECK(panel.diagnostics().size() == 1);
        CHECK(panel.diagnostics()[0].stability == Stability::stable); // promoted
        CHECK(panel.model().provisional == 0);
        CHECK(!panel.inline_markers()[0].provisional);
    }

    // --- stale provisional diagnostics of an OLDER generation are discarded on settle ------------
    {
        ProblemsPanel panel;
        panel.set_diagnostics({
            diag("OLD", "stale mid-derivation", Severity::warning, "a.json", 2, Stability::settling, 4),
            diag("NEW", "current", Severity::error, "a.json", 1, Stability::settling, 5),
            diag("KEEP", "already stable", Severity::info, "b.json", 3, Stability::stable, 4),
        });
        CHECK(panel.diagnostics().size() == 3);

        panel.on_derivation_settled(5, Stability::stable);

        // OLD (provisional, gen 4 < 5) discarded; NEW (gen 5) promoted; KEEP (already stable) retained.
        CHECK(panel.diagnostics().size() == 2);
        CHECK(panel.model().provisional == 0);

        bool has_new = false;
        bool has_keep = false;
        bool has_old = false;
        for (const ProblemDiagnostic& d : panel.diagnostics())
        {
            has_new = has_new || d.key == "NEW";
            has_keep = has_keep || d.key == "KEEP";
            has_old = has_old || d.key == "OLD";
        }
        CHECK(has_new);
        CHECK(has_keep);
        CHECK(!has_old);
    }

    // --- a navigation whose diagnostic is discarded on settle is cleared (notifying) -------------
    {
        ProblemsPanel panel;
        panel.set_diagnostics({
            diag("OLD", "stale", Severity::warning, "a.json", 2, Stability::settling, 4),
        });
        CHECK(panel.navigate("OLD"));

        int fired = 0;
        panel.add_navigation_listener([&](const ProblemNavigation&) { ++fired; });

        panel.on_derivation_settled(5, Stability::stable); // discards OLD -> navigation cleared
        CHECK(panel.navigation().diagnostic_identity.empty());
        CHECK(fired == 1);
    }

    // --- stable re-render is deterministic (byte-identical for identical state) -------------------
    {
        ProblemsPanel panel;
        panel.set_diagnostics({
            diag("P1", "bad ref", Severity::error, "a.json", 1, Stability::stable, 7),
            diag("P2", "unused", Severity::warning, "b.json", 4, Stability::stable, 7),
        });
        panel.on_derivation_settled(7, Stability::stable); // establish generation 7
        panel.navigate("P1");
        const std::string first = uitree::render_html(panel.build_panel());
        // A settle that changes nothing observable (same generation, already-stable diagnostics)
        // re-renders identically.
        panel.on_derivation_settled(7, Stability::stable);
        const std::string second = uitree::render_html(panel.build_panel());
        CHECK(first == second);

        // The rendered surface shows the active-navigation marker + the status generation.
        CHECK(first.find("(active)") != std::string::npos);
        CHECK(first.find("generation 7") != std::string::npos);
    }

    // --- navigation preserved across a refresh that keeps it; cleared when it vanishes -----------
    {
        ProblemsPanel panel;
        panel.set_diagnostics({
            diag("P1", "bad ref", Severity::error, "a.json", 1, Stability::stable, 1),
            diag("P2", "unused", Severity::warning, "b.json", 4, Stability::stable, 1),
        });
        CHECK(panel.navigate("P1"));

        int fired = 0;
        panel.add_navigation_listener([&](const ProblemNavigation&) { ++fired; });

        // Refresh that still contains P1: navigation preserved, no notification.
        panel.set_diagnostics({
            diag("P1", "bad ref", Severity::error, "a.json", 1, Stability::stable, 2),
            diag("P3", "new", Severity::info, "c.json", 9, Stability::stable, 2),
        });
        CHECK(panel.navigation().diagnostic_identity == "P1");
        CHECK(fired == 0);

        // Refresh WITHOUT P1: navigation cleared + notified.
        panel.set_diagnostics({
            diag("P3", "new", Severity::info, "c.json", 9, Stability::stable, 3),
        });
        CHECK(panel.navigation().diagnostic_identity.empty());
        CHECK(fired == 1);
    }

    // --- the provisional marker is visible in the rendered surface -------------------------------
    {
        ProblemsPanel panel;
        panel.ingest(diag("P1", "settling diag", Severity::error, "a.json", 1, Stability::settling, 2));
        const std::string html = uitree::render_html(panel.build_panel());
        CHECK(html.find("(provisional)") != std::string::npos);
        CHECK(html.find("a.json") != std::string::npos); // the file group header is legible
    }

    // --- the grouped model is CACHED: reads share one build; only mutators invalidate -------------
    // (M9 e05d3 inherited perf fix: one click previously cost 3 full builds, 2 wasted.)
    {
        ProblemsPanel panel;
        panel.set_diagnostics({diag("k1", "boom", Severity::error, "a.json", 3,
                                    bridge::Stability::stable, 1)});
        CHECK(panel.model_builds() == 0u); // lazy: nothing built until someone reads

        (void)panel.model();
        (void)panel.model();
        (void)panel.build_panel(); // the render path reads the SAME cache
        CHECK(panel.model_builds() == 1u);

        // A click (navigate) mutates NAVIGATION state only — the grouped model is untouched, so the
        // whole click-to-render loop costs ZERO additional builds on an unchanged diagnostic set.
        CHECK(panel.navigate("k1"));
        (void)panel.model();
        (void)panel.build_panel();
        CHECK(panel.model_builds() == 1u);

        // Every diagnostic-set mutator invalidates: the NEXT read rebuilds, exactly once.
        (void)panel.ingest(diag("k2", "warn", Severity::warning, "b.json", 1,
                                bridge::Stability::stable, 1));
        (void)panel.model();
        CHECK(panel.model_builds() == 2u);
        panel.on_derivation_settled(2, bridge::Stability::stable);
        (void)panel.model();
        CHECK(panel.model_builds() == 3u);
        panel.set_diagnostics({});
        (void)panel.model();
        CHECK(panel.model_builds() == 4u);
    }

    PROBLEMS_TEST_MAIN_END();
}
