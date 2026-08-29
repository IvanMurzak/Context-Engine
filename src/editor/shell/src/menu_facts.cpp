// The Shell-published menu activation fact (editor-window-chrome d3) — see menu_facts.h for the model.

#include "context/editor/shell/menu_facts.h"

namespace context::editor::shell
{

contract::Json menu_activation_envelope(WindowId window, const std::string& command_id,
                                        std::uint64_t seq)
{
    // Both payload members written unconditionally (write_notice_envelope's rationale): there is no
    // case where "missing" and a default would mean different things, and an optional member makes
    // the renderer's parser choose between them for nothing.
    contract::Json payload = contract::Json::object();
    payload.set("windowId", contract::Json(static_cast<std::uint64_t>(window)));
    payload.set("commandId", contract::Json(command_id));

    // The shared Shell-published seal (ui_mirror.h `shell_ui_envelope`) — origin NEVER a window id;
    // see menu_facts.h § the origin (write_notice.h's rationale).
    return shell_ui_envelope(kUiTopicMenu, std::move(payload), seq);
}

std::size_t MenuActivationRelay::publish_activation(WindowId window, const std::string& command_id)
{
    if (command_id.empty())
    {
        // A nameless activation dispatches nothing by construction — refused HERE, before an
        // envelope exists, so the renderer's registry never sees an empty id (menu_facts.h).
        ++refused_;
        return 0;
    }
    ++published_;
    if (store_ == nullptr)
    {
        // No transport in this build (a T1 caller, a smoke with no mirror session). Counted, not
        // silent: `published() > delivered()` is exactly "the activation happened and reached
        // nobody" — the state a wiring regression would produce and the one a test must see.
        return 0;
    }
    ++seq_;
    // UNICAST to the affected window (menu_facts.h § the precedent): its own editor-core is the
    // fact's only executor, and the payload's `windowId` still names the subject.
    store_->enqueue(window, menu_activation_envelope(window, command_id, seq_));
    ++delivered_;
    return 1;
}

} // namespace context::editor::shell
