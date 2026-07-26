// T1 for the LOUD write-notice relay (M9 e09b-3, design 05 §8 / 10 § Non-negotiable UX invariants).
//
// WHAT THIS PROVES. e09b-2 made a concurrent-writer collision DROP the gesture instead of clobbering
// the co-writer; this relay is what turns that refusal into something the human can see. The three
// properties below are the ones a regression would take away silently, so each has a case that fails
// without it:
//
//   1. THE ENVELOPE IS ONE `editor.ui` FACT the renderer's bus will accept — the `write.notice` topic
//      exactly as `uibus.ts` spells it, and every payload member present.
//   2. THE ORIGIN IS NEVER A WINDOW ID. `EditorUiBus.receiveMirrored` drops an envelope whose origin
//      equals the receiving bus's own origin, and every bus origin is a window id — so a notice
//      stamped with one would be swallowed by the very window it was meant for. Asserted directly.
//   3. THE BROADCAST REACHES EVERY WINDOW, deduplicated, and still reaches the PRIMARY when no
//      windows provider is bound (or when one throws) — a single-window editor must not lose its
//      notices because nobody wired an enumerator.
//
// It drives the REAL relay against the REAL `UiMirrorStore` the Shell runs, so this is the same code
// path the live editor takes, minus only the browser.

#include "context/editor/shell/write_notice.h"

#include "context/editor/shell/ui_mirror.h"
#include "context/editor/shell/window_registry.h"

#include "shell_test.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace context::editor::shell;
using Json = context::editor::contract::Json;

namespace
{

WriteNotice drop_notice()
{
    WriteNotice notice;
    notice.kind = kWriteNoticeKindDrop;
    notice.action = "edit";
    notice.code = "cas.mismatch";
    notice.message = "another writer changed this field; your edit was not applied";
    notice.pointer = "/components/camera/fov";
    return notice;
}

// ------------------------------------------------- 1. the envelope is a well-formed editor.ui fact

void test_the_envelope_carries_the_topic_origin_and_every_payload_member()
{
    const Json envelope = write_notice_envelope(drop_notice(), 3u);

    CHECK(envelope.is_object());
    CHECK(envelope.at("seq").as_int() == 3);
    CHECK(envelope.at("topic").as_string() == kUiTopicWriteNotice);
    // The literal is spelled out ONCE here on purpose. The constant is what the Shell publishes and
    // what `webui-panel-contract` compares against the TS side; asserting only `== kUiTopicWriteNotice`
    // would be true of ANY value the constant took, including one the renderer's closed topic set
    // does not contain — which is precisely the drift that would silence every notice.
    CHECK(envelope.at("topic").as_string() == "editor.ui.write-notice");

    // Property 2: the origin is the Shell's, and it is NOT parseable as a window id.
    CHECK(envelope.at("origin").as_string() == kWriteNoticeOrigin);
    CHECK(envelope.at("origin").as_string() == "shell");
    CHECK(envelope.at("origin").as_string() != std::to_string(kPrimaryWindowId));

    const Json& payload = envelope.at("payload");
    CHECK(payload.is_object());
    CHECK(payload.at("kind").as_string() == kWriteNoticeKindDrop);
    CHECK(payload.at("action").as_string() == "edit");
    CHECK(payload.at("code").as_string() == "cas.mismatch");
    CHECK(shelltest::mentions(payload.at("message").as_string(), "your edit was not applied"));
    CHECK(payload.at("pointer").as_string() == "/components/camera/fov");
}

void test_empty_members_are_written_rather_than_omitted()
{
    // A refusal with no pointer (a write path that failed before it resolved a field) still carries
    // every member: the renderer's parser must never have to distinguish "absent" from "empty", and a
    // member that is sometimes missing is how such a parser grows a branch nobody tests.
    WriteNotice notice;
    notice.kind = kWriteNoticeKindRefusal;
    notice.action = "undo";
    notice.code = "shell.no_daemon";
    notice.message = "no daemon connection";

    const Json envelope = write_notice_envelope(notice, 1u);
    const Json& payload = envelope.at("payload");
    CHECK(payload.contains("pointer"));
    CHECK(payload.at("pointer").is_string());
    CHECK(payload.at("pointer").as_string().empty());
    CHECK(payload.at("kind").as_string() == "refusal");
}

// ------------------------------------------------------------------ 2. the broadcast, and its edges

void test_a_notice_is_broadcast_to_every_live_window_exactly_once()
{
    UiMirrorStore mirror;
    WriteNoticeRelay relay;
    relay.bind_store(&mirror);
    // The duplicate 0 is deliberate: a provider that reports the primary twice must not queue two
    // envelopes, which would show the human two identical toasts for ONE refused write.
    relay.bind_windows([]() -> std::vector<WindowId> { return {0u, 1u, 1u, 0u}; });

    CHECK(relay.publish(drop_notice()) == 2u);
    CHECK(relay.published() == 1u);
    CHECK(relay.delivered() == 2u);
    CHECK(relay.seq() == 1u);

    CHECK(mirror.pending(0u) == 1u);
    CHECK(mirror.pending(1u) == 1u);

    const std::vector<Json> for_window_1 = mirror.take(1u);
    // GUARDED, and the guard is not ceremony: CHECK only RECORDS a failure (shell_test.h) — it does
    // not abort — so indexing straight after a size assertion is out of bounds on exactly the run that
    // assertion exists to catch, trading a legible "size == 1 failed" for UB, and on the ASan leg for
    // a heap-buffer-overflow report that names the test harness instead of the regression.
    CHECK(for_window_1.size() == 1u);
    if (for_window_1.size() == 1u)
    {
        CHECK(for_window_1[0].at("topic").as_string() == kUiTopicWriteNotice);
        CHECK(for_window_1[0].at("payload").at("code").as_string() == "cas.mismatch");
    }
}

void test_with_no_windows_provider_the_primary_still_receives_it()
{
    UiMirrorStore mirror;
    WriteNoticeRelay relay;
    relay.bind_store(&mirror);
    // No windows provider bound — the single-window editor, and the shape a wiring omission produces.
    CHECK(relay.publish(drop_notice()) == 1u);
    CHECK(mirror.pending(kPrimaryWindowId) == 1u);
}

void test_a_throwing_windows_provider_degrades_to_the_primary_rather_than_propagating()
{
    UiMirrorStore mirror;
    WriteNoticeRelay relay;
    relay.bind_store(&mirror);
    relay.bind_windows([]() -> std::vector<WindowId> { throw std::runtime_error("no registry"); });

    // The notice still lands somewhere, and the exception never reaches the write path that is
    // already handling a refusal.
    CHECK(relay.publish(drop_notice()) == 1u);
    CHECK(mirror.pending(kPrimaryWindowId) == 1u);
    CHECK(relay.delivered() == 1u);
}

void test_an_unbound_relay_is_inert_but_counted()
{
    WriteNoticeRelay relay; // no store — a T1 caller, or a smoke with no mirror session
    CHECK(!relay.has_store());
    CHECK(relay.publish(drop_notice()) == 0u);
    // COUNTED, not silent: `published > delivered` is exactly "the notice happened and reached
    // nobody", which is the state a wiring regression produces and the one a test must be able to see.
    CHECK(relay.published() == 1u);
    CHECK(relay.delivered() == 0u);
    // A refusal consumes no seq, so a later bound publish still starts at 1.
    CHECK(relay.seq() == 0u);
}

void test_successive_notices_are_drained_in_publish_order()
{
    UiMirrorStore mirror;
    WriteNoticeRelay relay;
    relay.bind_store(&mirror);

    WriteNotice first = drop_notice(); // keeps drop_notice()'s pointer — the contrast is on `second`
    WriteNotice second = drop_notice();
    second.kind = kWriteNoticeKindRefusal;
    second.pointer = "/components/light/intensity";

    CHECK(relay.publish(first) == 1u);
    CHECK(relay.publish(second) == 1u);
    CHECK(relay.seq() == 2u);

    const std::vector<Json> drained = mirror.take(kPrimaryWindowId);
    CHECK(drained.size() == 2u); // guarded below — CHECK records, it does not abort
    if (drained.size() == 2u)
    {
        CHECK(drained[0].at("payload").at("pointer").as_string() == "/components/camera/fov");
        CHECK(drained[1].at("payload").at("pointer").as_string() == "/components/light/intensity");
        CHECK(drained[0].at("seq").as_int() == 1);
        CHECK(drained[1].at("seq").as_int() == 2);
    }
}

} // namespace

int main()
{
    test_the_envelope_carries_the_topic_origin_and_every_payload_member();
    test_empty_members_are_written_rather_than_omitted();
    test_a_notice_is_broadcast_to_every_live_window_exactly_once();
    test_with_no_windows_provider_the_primary_still_receives_it();
    test_a_throwing_windows_provider_degrades_to_the_primary_rather_than_propagating();
    test_an_unbound_relay_is_inert_but_counted();
    test_successive_notices_are_drained_in_publish_order();
    SHELL_TEST_MAIN_END();
}
