// The Shell-published chrome facts (editor-window-chrome a1) — see chrome_facts.h for the model.

#include "context/editor/shell/chrome_facts.h"

namespace context::editor::shell
{

contract::Json chrome_maximized_envelope(WindowId window, bool maximized, std::uint64_t seq)
{
    // Both payload members written unconditionally (write_notice_envelope's rationale): there is no
    // case where "missing" and a default would mean different things, and an optional member makes
    // the renderer's parser choose between them for nothing.
    contract::Json payload = contract::Json::object();
    payload.set("windowId", contract::Json(static_cast<std::uint64_t>(window)));
    payload.set("maximized", contract::Json(maximized));

    // The shared Shell-published seal (ui_mirror.h `shell_ui_envelope`) — origin NEVER a window id;
    // see chrome_facts.h § the origin (write_notice.h's rationale).
    return shell_ui_envelope(kUiTopicChrome, std::move(payload), seq);
}

std::size_t ChromeFactRelay::publish_maximized(WindowId window, bool maximized)
{
    ++published_;
    if (store_ == nullptr)
    {
        // No transport in this build (a T1 caller, a smoke with no mirror session). Counted, not
        // silent: `published() > delivered()` is exactly "the flip happened and reached nobody" —
        // the state a wiring regression would produce and the one a test must be able to see.
        return 0;
    }
    ++seq_;
    // UNICAST to the affected window (chrome_facts.h § UNICAST): its own titlebar is the fact's
    // only renderer, and the payload's `windowId` still names the subject.
    store_->enqueue(window, chrome_maximized_envelope(window, maximized, seq_));
    ++delivered_;
    return 1;
}

} // namespace context::editor::shell
