// The LOUD write-notice relay (M9 e09b-3) — see write_notice.h for the whole rationale.

#include "context/editor/shell/write_notice.h"

#include <set>
#include <utility>

namespace context::editor::shell
{

contract::Json write_notice_envelope(const WriteNotice& notice, std::uint64_t seq)
{
    // The PAYLOAD is opaque to the transport (D7 tier 2) but not to the renderer, which narrows it in
    // `parseWriteNotice` (notifications.ts). Every member is written unconditionally, including the
    // empty ones: a member that is sometimes absent makes the renderer's parser choose between
    // "missing" and "empty", and there is no case here where those mean different things.
    contract::Json payload = contract::Json::object();
    payload.set("kind", contract::Json(notice.kind));
    payload.set("action", contract::Json(notice.action));
    payload.set("code", contract::Json(notice.code));
    payload.set("message", contract::Json(notice.message));
    payload.set("pointer", contract::Json(notice.pointer));

    contract::Json envelope = contract::Json::object();
    envelope.set("seq", contract::Json(seq));
    envelope.set("topic", contract::Json(kUiTopicWriteNotice));
    // NEVER a window id — see write_notice.h § THE ORIGIN IS `shell`.
    envelope.set("origin", contract::Json(kWriteNoticeOrigin));
    envelope.set("payload", std::move(payload));
    return envelope;
}

std::size_t WriteNoticeRelay::publish(const WriteNotice& notice)
{
    ++published_;
    if (store_ == nullptr)
    {
        // No transport in this build (a T1 caller, a smoke with no mirror session). Counted, not
        // silent: `published() > delivered()` is exactly "the notice happened and reached nobody",
        // which is the state a wiring regression would produce and the one a test must be able to see.
        return 0;
    }

    // The target set is deduplicated for the same reason `WindowBridge::ui_mirror` deduplicates its
    // broadcast: a provider that reports the primary twice must not queue the notice twice, which
    // would show the human two identical toasts for one refused write.
    //
    // kPrimaryWindowId is inserted UNCONDITIONALLY. A relay with no windows provider (or one that
    // threw) still has somewhere honest to deliver: a single-window editor is the common shape, and
    // losing its notices because nobody bound an enumerator would reproduce the silence this whole
    // file exists to end.
    std::set<WindowId> targets;
    targets.insert(kPrimaryWindowId);
    if (windows_)
    {
        try
        {
            for (const WindowId id : windows_())
            {
                targets.insert(id);
            }
        }
        catch (...)
        {
            // A throwing provider costs the OTHER windows their notice, never the write path its
            // completion: this is called from the owner loop right after a refused commit, and an
            // exception escaping here would propagate into a code path whose whole job is to fail
            // gracefully.
        }
    }

    ++seq_;
    const contract::Json envelope = write_notice_envelope(notice, seq_);
    for (const WindowId id : targets)
    {
        store_->enqueue(id, envelope);
        ++delivered_;
    }
    return targets.size();
}

} // namespace context::editor::shell
