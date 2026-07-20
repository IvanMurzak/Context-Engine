// T1 for the LIVE diagnostics feed (M9 e05d1): the wire->model projection, the R-BRIDGE-008
// promotion/settle path, the node-id -> diagnostic-identity mapping, and the host revision coupling.
//
// This is the test that makes "Problems hydrates from the LIVE daemon" assertable WITHOUT a daemon.
// The parsers are pure (JSON in, model out), so every shape the `diagnostics` topic legitimately
// carries — and every hostile shape a compromised renderer or a malformed publisher could produce —
// is driven here, on all three default `build` legs. What is left for the live CEF smoke is only
// that the wiring is connected, which is exactly the split the rest of the Shell already uses.

#include "context/editor/shell/panels/problems_feed.h"

#include "context/editor/gui/contract/extension.h"
#include "context/editor/shell/panel_host.h"

#include "panels_test.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace problems = context::editor::gui::panels::problems;
namespace gc = context::editor::gui::contract;
namespace bridge = context::editor::bridge;
using Json = context::editor::contract::Json;

namespace
{

constexpr const char* kPanelId = "builtin.problems";

Json diagnostic_payload(const std::string& code, const std::string& message,
                        const std::string& severity, const std::string& file,
                        const std::string& stability)
{
    Json payload = Json::object();
    payload.set("code", Json(code));
    payload.set("message", Json(message));
    if (!severity.empty())
    {
        payload.set("severity", Json(severity));
    }
    if (!file.empty())
    {
        payload.set("file", Json(file));
        payload.set("line", Json(12));
        payload.set("column", Json(4));
    }
    if (!stability.empty())
    {
        payload.set("stability", Json(stability));
    }
    return payload;
}

std::vector<gc::Contribution> roster_with_problems()
{
    gc::Contribution c;
    c.id = kPanelId;
    c.kind = gc::ContributionKind::panel;
    c.title = "Problems";
    c.content.type = gc::ContentType::uitree;
    c.state.schema_version = 1;
    return {c};
}

// --- cases ----------------------------------------------------------------------------------------

void maps_severity_and_stability_tokens()
{
    CHECK(panels::parse_severity("error") == problems::Severity::error);
    CHECK(panels::parse_severity("warning") == problems::Severity::warning);
    CHECK(panels::parse_severity("warn") == problems::Severity::warning);
    CHECK(panels::parse_severity("info") == problems::Severity::info);
    CHECK(panels::parse_severity("information") == problems::Severity::info);
    CHECK(panels::parse_severity("hint") == problems::Severity::hint);
    // An unreadable severity ESCALATES rather than degrades — see problems_feed.h on why.
    CHECK(panels::parse_severity("") == problems::Severity::error);
    CHECK(panels::parse_severity("catastrophe") == problems::Severity::error);

    CHECK(panels::parse_stability("stable") == bridge::Stability::stable);
    CHECK(panels::parse_stability("unstable") == bridge::Stability::unstable);
    CHECK(panels::parse_stability("settling") == bridge::Stability::settling);
    CHECK(panels::parse_stability("") == bridge::Stability::stable);
    CHECK(panels::parse_stability("nonsense") == bridge::Stability::stable);
}

void projects_a_diagnostic_payload()
{
    const Json payload =
        diagnostic_payload("file.malformed", "unterminated object", "warning", "scenes/a.json",
                           "settling");
    const std::optional<problems::ProblemDiagnostic> parsed = panels::parse_diagnostic(payload, 42);
    CHECK(parsed.has_value());
    if (!parsed.has_value())
    {
        return;
    }
    CHECK(parsed->code == "file.malformed");
    CHECK(parsed->message == "unterminated object");
    CHECK(parsed->severity == problems::Severity::warning);
    CHECK(parsed->stability == bridge::Stability::settling);
    CHECK(parsed->provisional());
    CHECK(parsed->nav.file == "scenes/a.json");
    CHECK(parsed->nav.line == 12);
    CHECK(parsed->nav.column == 4);
    CHECK(parsed->nav.navigable());
    // The generation comes from the EVENT ENVELOPE, never the payload.
    CHECK(parsed->generation == 42);
}

void tolerates_every_shape_the_topic_legitimately_carries()
{
    // The crash-recovery publisher's shape (EditorKernel::start): code + opId + message, no severity,
    // no location. It must still render.
    Json recovery = Json::object();
    recovery.set("code", Json("recovery.unresumable"));
    recovery.set("opId", Json("op-17"));
    recovery.set("message", Json("write could not be resumed"));
    recovery.set("event", Json("recovery.diagnostic"));
    const std::optional<problems::ProblemDiagnostic> parsed_recovery =
        panels::parse_diagnostic(recovery, 3);
    CHECK(parsed_recovery.has_value());
    if (parsed_recovery.has_value())
    {
        CHECK(parsed_recovery->severity == problems::Severity::error);
        CHECK(!parsed_recovery->nav.navigable()); // a project-wide diagnostic, not a file one
        // `opId` must NOT become the identity: one crashed op publishes one diagnostic PER PLANNED
        // WRITE, all carrying the same op id, and adopting it would collapse them onto one row.
        CHECK(parsed_recovery->key.empty());
    }

    // The identity of two diagnostics from the SAME crashed op stays DISTINCT (R-FILE-004:
    // reported, never silently dropped). Same `opId`, different code/message — the composite
    // fallback identity must keep them apart.
    Json sibling = Json::object();
    sibling.set("code", Json("filesync.intent.cas"));
    sibling.set("opId", Json("op-17"));
    sibling.set("message", Json("target changed under the write"));
    const std::optional<problems::ProblemDiagnostic> parsed_sibling =
        panels::parse_diagnostic(sibling, 3);
    CHECK(parsed_sibling.has_value());
    if (parsed_recovery.has_value() && parsed_sibling.has_value())
    {
        CHECK(parsed_recovery->identity() != parsed_sibling->identity());
    }
    // A publisher that DOES supply its own `key` still gets it honoured verbatim, so a genuine
    // re-emission still collapses in place.
    Json keyed = Json::object();
    keyed.set("code", Json("schema.invalid"));
    keyed.set("message", Json("bad kind"));
    keyed.set("key", Json("stable-key-1"));
    const std::optional<problems::ProblemDiagnostic> parsed_keyed =
        panels::parse_diagnostic(keyed, 3);
    CHECK(parsed_keyed.has_value() && parsed_keyed->key == "stable-key-1");

    // A NESTED location container.
    Json nested = Json::object();
    nested.set("code", Json("schema.invalid"));
    nested.set("message", Json("bad kind"));
    Json nav = Json::object();
    nav.set("file", Json("entities/e.json"));
    nav.set("pointer", Json("/components/0"));
    nested.set("nav", nav);
    const std::optional<problems::ProblemDiagnostic> parsed_nested =
        panels::parse_diagnostic(nested, 1);
    CHECK(parsed_nested.has_value());
    CHECK(parsed_nested.has_value() && parsed_nested->nav.file == "entities/e.json");
    CHECK(parsed_nested.has_value() && parsed_nested->nav.pointer == "/components/0");

    // A `path` spelling instead of `file`.
    Json pathy = Json::object();
    pathy.set("code", Json("x"));
    pathy.set("path", Json("assets/tex.png"));
    const std::optional<problems::ProblemDiagnostic> parsed_path = panels::parse_diagnostic(pathy, 1);
    CHECK(parsed_path.has_value() && parsed_path->nav.file == "assets/tex.png");

    // --- the hostile / degenerate shapes. None may throw; the ONE rejection is "no code and no
    // message", because that is the only payload with nothing to render.
    CHECK(!panels::parse_diagnostic(Json(), 1).has_value());
    CHECK(!panels::parse_diagnostic(Json("a bare string"), 1).has_value());
    CHECK(!panels::parse_diagnostic(Json::array(), 1).has_value());
    CHECK(!panels::parse_diagnostic(Json::object(), 1).has_value());

    // A message with no code still renders (and vice versa).
    Json message_only = Json::object();
    message_only.set("message", Json("something went wrong"));
    CHECK(panels::parse_diagnostic(message_only, 1).has_value());

    // Wrong-typed and out-of-range members degrade to "unspecified" rather than wrapping.
    Json junk = Json::object();
    junk.set("code", Json("j"));
    junk.set("file", Json("f.json"));
    junk.set("line", Json(-5));
    junk.set("column", Json("not a number"));
    junk.set("severity", Json(7));
    const std::optional<problems::ProblemDiagnostic> parsed_junk = panels::parse_diagnostic(junk, 1);
    CHECK(parsed_junk.has_value());
    if (parsed_junk.has_value())
    {
        CHECK(parsed_junk->nav.line == 0);
        CHECK(parsed_junk->nav.column == 0);
        CHECK(parsed_junk->severity == problems::Severity::error);
    }
}

void parses_all_three_snapshot_shapes()
{
    const Json a = diagnostic_payload("a", "first", "error", "one.json", "");
    const Json b = diagnostic_payload("b", "second", "warning", "two.json", "");

    // Shape 1: a bare array.
    Json bare = Json::array();
    bare.push_back(a);
    bare.push_back(b);
    const auto bare_parsed = panels::parse_diagnostics_snapshot(bare, 9);
    CHECK(bare_parsed.has_value() && bare_parsed->size() == 2);

    // Shape 2: {"diagnostics": [...]}.
    Json wrapped = Json::object();
    Json list = Json::array();
    list.push_back(a);
    wrapped.set("diagnostics", list);
    const auto wrapped_parsed = panels::parse_diagnostics_snapshot(wrapped, 9);
    CHECK(wrapped_parsed.has_value() && wrapped_parsed->size() == 1);

    // Shape 3: {"events": [envelope, ...]} — the topic is filtered and each envelope's OWN
    // generation wins.
    Json enveloped = Json::object();
    Json events = Json::array();
    Json diag_event = Json::object();
    diag_event.set("topic", Json("diagnostics"));
    diag_event.set("generation", Json(77));
    diag_event.set("payload", a);
    events.push_back(diag_event);
    Json other_event = Json::object();
    other_event.set("topic", Json("files"));
    other_event.set("payload", b);
    events.push_back(other_event);
    enveloped.set("events", events);
    const std::optional<std::vector<problems::ProblemDiagnostic>> parsed =
        panels::parse_diagnostics_snapshot(enveloped, 9);
    CHECK(parsed.has_value());
    CHECK(parsed.has_value() && parsed->size() == 1); // the `files` envelope is not a diagnostic
    CHECK(parsed.has_value() && !parsed->empty() && parsed->front().generation == 77);

    // A NEGATIVE envelope generation falls back to the caller's stamp rather than wrapping to
    // ~1.8e19, which would leave the row permanently un-discardable by `on_derivation_settled`.
    Json negative_gen = Json::object();
    Json neg_events = Json::array();
    Json neg_event = Json::object();
    neg_event.set("topic", Json("diagnostics"));
    neg_event.set("generation", Json(-1));
    neg_event.set("payload", a);
    neg_events.push_back(neg_event);
    negative_gen.set("events", neg_events);
    const std::optional<std::vector<problems::ProblemDiagnostic>> neg_parsed =
        panels::parse_diagnostics_snapshot(negative_gen, 9);
    CHECK(neg_parsed.has_value() && !neg_parsed->empty() && neg_parsed->front().generation == 9);

    // An unrecognized container yields NULLOPT — "this snapshot said nothing about the diagnostic
    // set" — which is what stops the caller clearing the panel on a cursor-shaped snapshot.
    CHECK(!panels::parse_diagnostics_snapshot(Json("nope"), 1).has_value());
    CHECK(!panels::parse_diagnostics_snapshot(Json::object(), 1).has_value());
    // The REAL subscription snapshot shape (`EventStream::snapshot()`) is exactly that cursor.
    Json cursor = Json::object();
    cursor.set("incarnationId", Json("inc-1"));
    cursor.set("generation", Json(4));
    cursor.set("lastSeq", Json(12));
    CHECK(!panels::parse_diagnostics_snapshot(cursor, 0).has_value());
    // An engaged-but-EMPTY container is the distinct "the set is genuinely empty" answer.
    CHECK(panels::parse_diagnostics_snapshot(Json::array(), 1).has_value());
    CHECK(panels::parse_diagnostics_snapshot(Json::array(), 1)->empty());
    // A container with unparseable members SKIPS them instead of failing the whole snapshot.
    Json mixed = Json::array();
    mixed.push_back(a);
    mixed.push_back(Json("garbage"));
    mixed.push_back(Json::object());
    const auto mixed_parsed = panels::parse_diagnostics_snapshot(mixed, 1);
    CHECK(mixed_parsed.has_value() && mixed_parsed->size() == 1);
}

void drives_the_panel_and_touches_the_host()
{
    shell::PanelHost host(roster_with_problems());
    panels::ProblemsFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    const std::uint64_t initial = host.revision(kPanelId);

    Json snapshot = Json::array();
    snapshot.push_back(diagnostic_payload("a", "first", "error", "one.json", ""));
    snapshot.push_back(diagnostic_payload("b", "second", "warning", "two.json", ""));
    feed.apply_snapshot(snapshot, 5);
    CHECK(feed.snapshots_applied() == 1);
    CHECK(feed.panel().diagnostics().size() == 2);
    // The host revision moved, which is what makes the hydration runtime re-render.
    CHECK(host.revision(kPanelId) > initial);

    // The rendered panel carries both rows — the READ PATH, end to end through the host.
    std::string error_code;
    const std::optional<shell::PanelRender> rendered = host.render(kPanelId, error_code);
    CHECK(rendered.has_value());
    if (rendered.has_value())
    {
        CHECK(panelstest::mentions(rendered->html, "first"));
        CHECK(panelstest::mentions(rendered->html, "one.json"));
        CHECK(panelstest::mentions(rendered->html, "problems.row.0"));
        CHECK(panelstest::mentions(rendered->html, "data-command=\"problems.navigate\""));
        CHECK(!rendered->focus_order.empty());
    }

    // A CURSOR-SHAPED SNAPSHOT PRESERVES THE SET. This is the shape the daemon actually sends
    // (`EventStream::snapshot()` -> {incarnationId, generation, lastSeq}); a resnapshot after a
    // wire gap or a daemon restart must NOT wipe the panel at exactly the recovery point the panel
    // exists to survive. It says nothing about the set, so it moves no revision either.
    const std::uint64_t before_cursor = host.revision(kPanelId);
    Json cursor = Json::object();
    cursor.set("incarnationId", Json("inc-1"));
    cursor.set("generation", Json(6));
    cursor.set("lastSeq", Json(12));
    feed.apply_snapshot(cursor, 0);
    CHECK(feed.snapshots_applied() == 2);
    CHECK(feed.panel().diagnostics().size() == 2); // preserved, NOT cleared
    CHECK(host.revision(kPanelId) == before_cursor);

    // AN EMPTY *CONTAINER* STILL TOUCHES: "everything cleared" is a change the renderer must see.
    // This is the distinction the nullopt return exists to draw — engaged-but-empty still clears.
    const std::uint64_t before_clear = host.revision(kPanelId);
    feed.apply_snapshot(Json::array(), 6);
    CHECK(feed.panel().diagnostics().empty());
    CHECK(host.revision(kPanelId) > before_clear);
}

void promotes_provisional_diagnostics_on_settle()
{
    shell::PanelHost host(roster_with_problems());
    panels::ProblemsFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    // A provisional diagnostic arrives on the stream at generation 4.
    Json provisional = diagnostic_payload("p", "still churning", "error", "a.json", "settling");
    provisional.set("key", Json("stable-key"));
    CHECK(feed.apply_event("diagnostics", provisional, 4));
    CHECK(feed.panel().diagnostics().size() == 1);
    CHECK(!feed.panel().diagnostics().empty() && feed.panel().diagnostics().front().provisional());

    // The same diagnostic re-emitted as stable PROMOTES IN PLACE rather than duplicating.
    Json promoted = provisional;
    promoted.set("stability", Json("stable"));
    CHECK(feed.apply_event("diagnostics", promoted, 4));
    CHECK(feed.panel().diagnostics().size() == 1);
    CHECK(!feed.panel().diagnostics().empty() && !feed.panel().diagnostics().front().provisional());

    // `derivation.settled` at the same generation records it on the panel.
    Json settled = Json::object();
    settled.set("event", Json("derivation.settled"));
    CHECK(feed.apply_event("derivation", settled, 4));
    CHECK(feed.panel().generation() == 4);

    // An unrelated topic and an unrelated derivation event are IGNORED, not misapplied.
    CHECK(!feed.apply_event("files", Json::object(), 5));
    Json other = Json::object();
    other.set("event", Json("derivation.started"));
    CHECK(!feed.apply_event("derivation", other, 5));
    // A diagnostics payload with nothing renderable is dropped rather than counted.
    CHECK(!feed.apply_event("diagnostics", Json::object(), 5));
}

void maps_a_row_node_id_to_a_diagnostic_identity()
{
    shell::PanelHost host(roster_with_problems());
    panels::ProblemsFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    Json snapshot = Json::array();
    snapshot.push_back(diagnostic_payload("a", "first", "error", "one.json", ""));
    snapshot.push_back(diagnostic_payload("b", "second", "warning", "two.json", ""));
    feed.apply_snapshot(snapshot, 1);

    // The mapping resolves in the SAME grouped/row order build_panel walks.
    const std::optional<std::string> row0 = panels::problems_row_identity(feed.panel(), "problems.row.0");
    const std::optional<std::string> row1 = panels::problems_row_identity(feed.panel(), "problems.row.1");
    CHECK(row0.has_value());
    CHECK(row1.has_value());
    CHECK(row0.has_value() && row1.has_value() && *row0 != *row1);

    // --- everything else is refused, TOTALLY: these ids come off the bridge.
    CHECK(!panels::problems_row_identity(feed.panel(), "problems.row.99").has_value());
    CHECK(!panels::problems_row_identity(feed.panel(), "problems.row.").has_value());
    CHECK(!panels::problems_row_identity(feed.panel(), "problems.row.-1").has_value());
    CHECK(!panels::problems_row_identity(feed.panel(), "problems.row.1x").has_value());
    CHECK(!panels::problems_row_identity(feed.panel(), "problems.heading").has_value());
    CHECK(!panels::problems_row_identity(feed.panel(), "").has_value());
    // An index that would overflow a size_t is refused rather than wrapped into a valid row.
    CHECK(!panels::problems_row_identity(feed.panel(),
                                          "problems.row.99999999999999999999999999")
               .has_value());

    // --- activation THROUGH the host: the runtime sends a node id and the model navigates.
    Json params = Json::object();
    params.set("nodeId", Json("problems.row.0"));
    bool dispatched = false;
    std::string error_code;
    CHECK(host.invoke(kPanelId, problems::kNavigateCommand, params, dispatched, error_code));
    CHECK(dispatched);
    CHECK(feed.panel().navigation().diagnostic_identity == *row0);

    // A node id that resolves to nothing is a DECLINED dispatch, not a protocol error — an ordinary
    // click on a dead row.
    Json ghost = Json::object();
    ghost.set("nodeId", Json("problems.row.99"));
    bool ghost_dispatched = true;
    CHECK(host.invoke(kPanelId, problems::kNavigateCommand, ghost, ghost_dispatched, error_code));
    CHECK(!ghost_dispatched);
}

// A non-navigable diagnostic binds no command, so its row must not be navigable through the mapping
// either — otherwise a stale mounted DOM could navigate to a diagnostic that has lost its source.
void refuses_a_non_navigable_row()
{
    shell::PanelHost host(roster_with_problems());
    panels::ProblemsFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    Json snapshot = Json::array();
    snapshot.push_back(diagnostic_payload("p", "project-wide", "error", "", ""));
    feed.apply_snapshot(snapshot, 1);

    CHECK(feed.panel().diagnostics().size() == 1);
    CHECK(!panels::problems_row_identity(feed.panel(), "problems.row.0").has_value());
}

} // namespace

int main()
{
    maps_severity_and_stability_tokens();
    projects_a_diagnostic_payload();
    tolerates_every_shape_the_topic_legitimately_carries();
    parses_all_three_snapshot_shapes();
    drives_the_panel_and_touches_the_host();
    promotes_provisional_diagnostics_on_settle();
    maps_a_row_node_id_to_a_diagnostic_identity();
    refuses_a_non_navigable_row();
    PANELS_TEST_MAIN_END();
}
