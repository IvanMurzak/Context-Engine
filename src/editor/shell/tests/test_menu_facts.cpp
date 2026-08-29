// T1 for the Shell-published menu activation fact (editor-window-chrome d3, menu structure 03) —
// and for the off-platform cocoa_menu.h refusals, which every leg can assert as a VALUE.
//
// WHAT THIS PROVES:
//
//   1. THE ENVELOPE IS ONE `editor.ui` FACT the renderer's bus will accept — the `editor.ui.menu`
//      topic exactly as uibus.ts spells it, and every payload member present.
//   2. THE ORIGIN IS NEVER A WINDOW ID (write_notice.h's rule, inherited via chrome_facts).
//   3. DELIVERY IS UNICAST to the affected window, while the payload still names the subject.
//   4. AN EMPTY COMMAND ID IS REFUSED before an envelope exists — a nameless activation could only
//      dispatch nothing, and the registry must never be handed one.
//   5. UNBOUND IS HONEST: with no store the publish reaches nobody, visibly.
//   6. THE cocoa_menu SURFACE REFUSES for every non-Cocoa backend — which on this leg is every
//      backend there is — without touching its out-params, so the composition root's unconditional
//      `bind_menu` degrades to `accepted:false` on Windows/Linux by construction.

#include "context/editor/shell/menu_facts.h"

#include "context/editor/shell/cocoa_menu.h"
#include "context/editor/shell/menu_model.h"
#include "context/editor/shell/ui_mirror.h"
#include "context/editor/shell/window.h"
#include "context/editor/shell/window_registry.h"

#include "shell_test.h"

#include <string>
#include <vector>

using namespace context::editor::shell;
using Json = context::editor::contract::Json;

namespace
{

void the_envelope_is_one_menu_fact_with_every_member_present()
{
    const Json envelope = menu_activation_envelope(3, "help.about", 7);
    // The LITERAL topic and origin, not the constants: this pins the wire strings, and a constant
    // compared to itself would hold for whatever value it drifted to.
    CHECK(envelope.at("topic").as_string() == "editor.ui.menu");
    CHECK(envelope.at("origin").as_string() == "shell");
    // Never the subject window's id (write_notice.h § THE ORIGIN IS `shell`): every bus origin is
    // `String(windowId)`, so a fact stamped with the TARGET's id would be swallowed by exactly the
    // window it is for.
    CHECK(envelope.at("origin").as_string() != "3");
    CHECK(envelope.at("seq").as_int() == 7);
    CHECK(envelope.at("payload").at("windowId").as_int() == 3);
    CHECK(envelope.at("payload").at("commandId").as_string() == "help.about");
}

void delivery_is_unicast_to_the_affected_window()
{
    UiMirrorStore store;
    MenuActivationRelay relay;
    relay.bind_store(&store);
    CHECK(relay.has_store());

    CHECK(relay.publish_activation(0, "workbench.palette.toggle") == 1);
    // Window 0 got exactly one envelope; nobody else got anything — an activation is a fact about
    // one window's menu, and only its own editor-core executes it.
    CHECK(store.pending(0) == 1);
    CHECK(store.pending(1) == 0);

    const std::vector<Json> drained = store.take(0);
    CHECK(drained.size() == 1);
    CHECK(drained[0].at("payload").at("commandId").as_string() == "workbench.palette.toggle");

    // A second activation queues a SECOND envelope with a growing seq — publish-order history,
    // exactly what a poll-driven drain needs to replay activations faithfully.
    CHECK(relay.publish_activation(0, "view.theme.toggle") == 1);
    const std::vector<Json> second = store.take(0);
    CHECK(second.size() == 1);
    CHECK(second[0].at("seq").as_int() == 2);
    CHECK(relay.published() == 2);
    CHECK(relay.delivered() == 2);
    CHECK(relay.refused() == 0);
    CHECK(relay.seq() == 2);
}

void an_empty_command_id_is_refused_before_an_envelope_exists()
{
    UiMirrorStore store;
    MenuActivationRelay relay;
    relay.bind_store(&store);
    CHECK(relay.publish_activation(0, "") == 0);
    CHECK(store.pending(0) == 0); // nothing was queued — the registry never sees a nameless dispatch
    CHECK(relay.refused() == 1);
    CHECK(relay.published() == 0);
    CHECK(relay.seq() == 0); // no envelope was minted, so no seq was spent
}

void unbound_is_honest_not_silent()
{
    MenuActivationRelay relay;
    CHECK(!relay.has_store());
    CHECK(relay.publish_activation(1, "help.docs") == 0);
    // `published() > delivered()` is the observable a wiring regression produces.
    CHECK(relay.published() == 1);
    CHECK(relay.delivered() == 0);
    CHECK(relay.seq() == 0);
}

void the_cocoa_menu_surface_refuses_every_non_cocoa_backend()
{
    WindowDesc desc;
    HeadlessWindowBackend headless(desc);

    MenuModel model;
    MenuDefinition file;
    file.id = "file";
    file.label = "File";
    MenuItem about;
    about.kind = MenuItem::Kind::command;
    about.command_id = "help.about";
    about.label = "About";
    file.items.push_back(about);
    model.menus.push_back(file);

    // The install refuses — on THIS leg every backend is non-Cocoa (off macOS by the stub, on
    // macOS because a headless backend is not the live Cocoa one), which is exactly the honest
    // false the unconditional `bind_menu` in editor_main degrades on.
    bool activated = false;
    CHECK(!cocoa_install_menu(headless, model,
                              [&activated](const std::string&) { activated = true; }));
    CHECK(!activated);

    CocoaMenuStats stats;
    stats.installs = 123u; // must be left untouched by a refusal
    CHECK(!cocoa_menu_stats(headless, stats));
    CHECK(stats.installs == 123u);

    CHECK(!cocoa_menu_perform(headless, "help.about"));

    // Still fully functional as a window afterwards — the refusal really was a no-op.
    std::vector<ShellEvent> drained;
    CHECK(headless.pump(drained));
}

} // namespace

int main()
{
    the_envelope_is_one_menu_fact_with_every_member_present();
    delivery_is_unicast_to_the_affected_window();
    an_empty_command_id_is_refused_before_an_envelope_exists();
    unbound_is_honest_not_silent();
    the_cocoa_menu_surface_refuses_every_non_cocoa_backend();
    SHELL_TEST_MAIN_END();
}
