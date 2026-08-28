// T1 for the Shell-published chrome facts (editor-window-chrome a1, target design 02 §1).
//
// WHAT THIS PROVES. The a2 titlebar's max/restore glyph flips on a fact only the C++ side can
// observe (a maximize can come from Win+Up or the WM, not just the button), and 02 §1 binds the
// channel: the placement poll's observation becomes ONE `editor.ui` envelope on the EXISTING mirror
// relay. The properties below are the ones a regression would take away silently:
//
//   1. THE ENVELOPE IS ONE `editor.ui` FACT the renderer's bus will accept — the `editor.ui.chrome`
//      topic exactly as uibus.ts spells it, and every payload member present.
//   2. THE ORIGIN IS NEVER A WINDOW ID (write_notice.h's rule, inherited with its rationale):
//      `receiveMirrored` drops an envelope whose origin equals the receiving bus's own, and every
//      bus origin is a window id — a fact stamped with the TARGET's id would be swallowed by
//      exactly the window it is for.
//   3. DELIVERY IS UNICAST to the affected window — a flip in window 2 must not land in window 0's
//      queue — while the payload still names the subject.
//   4. UNBOUND IS HONEST: with no store the publish reaches nobody, and the counters make that
//      state visible instead of silent.
//
// It drives the REAL relay against the REAL `UiMirrorStore` the Shell runs; the flip DETECTION that
// feeds it is proven one suite over, in test_shell.cpp's WindowManager case, end to end through the
// placement poll.

#include "context/editor/shell/chrome_facts.h"

#include "context/editor/shell/ui_mirror.h"
#include "context/editor/shell/window_registry.h"

#include "shell_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::shell;
using Json = context::editor::contract::Json;

namespace
{

void the_envelope_is_one_chrome_fact_with_every_member_present()
{
    const Json envelope = chrome_maximized_envelope(3, true, 7);
    // The LITERAL topic and origin, not the constants: this pins the wire strings, and a constant
    // compared to itself would hold for whatever value it drifted to.
    CHECK(envelope.at("topic").as_string() == "editor.ui.chrome");
    CHECK(envelope.at("origin").as_string() == "shell");
    CHECK(envelope.at("seq").as_int() == 7);
    CHECK(envelope.at("payload").at("windowId").as_int() == 3);
    CHECK(envelope.at("payload").at("maximized").as_bool());

    // Both payload members are written UNCONDITIONALLY (write_notice_envelope's rule) — a restore
    // (false) is a value, never an absence.
    const Json restored = chrome_maximized_envelope(0, false, 8);
    CHECK(restored.at("payload").contains("maximized"));
    CHECK(restored.at("payload").at("maximized").as_bool() == false);
    CHECK(restored.at("payload").at("windowId").as_int() == 0);
}

void the_origin_is_never_a_window_id()
{
    // The loop-breaker collision (write_notice.h § THE ORIGIN IS `shell`): every bus origin is
    // `String(windowId)`, so the origin must not parse as the subject window's id — or any id.
    const Json envelope = chrome_maximized_envelope(5, true, 1);
    const std::string origin = envelope.at("origin").as_string();
    CHECK(origin != "5");
    CHECK(origin != "0");
    CHECK(!origin.empty());
    bool numeric = true;
    for (const char c : origin)
    {
        numeric = numeric && c >= '0' && c <= '9';
    }
    CHECK(!numeric);
}

void delivery_is_unicast_to_the_affected_window()
{
    UiMirrorStore store;
    ChromeFactRelay relay;
    relay.bind_store(&store);
    CHECK(relay.has_store());

    CHECK(relay.publish_maximized(2, true) == 1);
    // Window 2 got exactly one envelope; window 0 (and everyone else) got NOTHING — the fact is
    // about one window's chrome and only its own titlebar renders it.
    CHECK(store.pending(2) == 1);
    CHECK(store.pending(0) == 0);
    CHECK(store.pending(1) == 0);

    const std::vector<Json> drained = store.take(2);
    CHECK(drained.size() == 1);
    CHECK(drained[0].at("payload").at("windowId").as_int() == 2);
    CHECK(drained[0].at("payload").at("maximized").as_bool());

    // A second flip (the restore) queues a SECOND envelope with a growing seq — the queue carries
    // history in publish order, exactly what a poll-driven drain needs to replay flips faithfully.
    CHECK(relay.publish_maximized(2, false) == 1);
    const std::vector<Json> second = store.take(2);
    CHECK(second.size() == 1);
    CHECK(second[0].at("payload").at("maximized").as_bool() == false);
    CHECK(second[0].at("seq").as_int() == 2);
    CHECK(relay.published() == 2);
    CHECK(relay.delivered() == 2);
    CHECK(relay.seq() == 2);
}

void unbound_is_honest_not_silent()
{
    ChromeFactRelay relay;
    CHECK(!relay.has_store());
    // No transport: the publish reaches nobody and SAYS so — `published() > delivered()` is the
    // observable a wiring regression produces, and the one this case keeps observable.
    CHECK(relay.publish_maximized(1, true) == 0);
    CHECK(relay.published() == 1);
    CHECK(relay.delivered() == 0);
    CHECK(relay.seq() == 0); // no envelope was minted, so no seq was spent
}

} // namespace

int main()
{
    the_envelope_is_one_chrome_fact_with_every_member_present();
    the_origin_is_never_a_window_id();
    delivery_is_unicast_to_the_affected_window();
    unbound_is_honest_not_silent();
    SHELL_TEST_MAIN_END();
}
