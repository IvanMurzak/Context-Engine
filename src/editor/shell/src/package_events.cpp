// The bounded per-package event fan-out buffer (see package_events.h).

#include "context/editor/shell/package_events.h"

#include <cstdio>
#include <utility>

namespace context::editor::shell
{

PackageEventBuffer::PackageEventBuffer(std::size_t capacity)
    // 0 would mean "buffer nothing", i.e. a subscription that can never deliver — a disabled feature
    // wearing a configuration error's clothes. Clamped to 1, matching PackageSessionHost's own cap.
    : capacity_(capacity == 0 ? 1 : capacity)
{
}

void PackageEventBuffer::push(const std::string& package_id, contract::Json event)
{
    Queue& queue = queues_[package_id];
    while (queue.events.size() >= capacity_)
    {
        // DROP-OLDEST: these are facts, and the freshest one is the one a panel must render. See the
        // header for why refuse-newest would leave a panel rendering the past forever.
        // ⚠ THE EPISODE LATCH IS `dropped`, NOT `gapped`. Both an overflow and a DAEMON gap set
        // `gapped`, so latching the log on it silenced this line entirely whenever an `event.gap`
        // arrived first — losing the hint in the compound case (daemon behind AND editor behind),
        // which is the most diagnostic one there is. `dropped` is touched ONLY by eviction and is
        // reset by every drain (`take` erases the queue), so `dropped == 0` is exactly "first
        // eviction of this episode" and needs no extra state.
        const bool first_of_episode = queue.dropped == 0;
        queue.events.pop_front();
        ++queue.dropped;
        ++dropped_total_;
        if (first_of_episode)
        {
            // ONE LINE PER EPISODE, not per event: under overflow a per-event line IS the flood this
            // buffer exists to survive. Developer-facing only — the deliverable observable is the
            // `gapped` / `dropped` pair that travels to the panel (header, observable 1).
            std::fprintf(stderr,
                         "context_editor: package '%s' fell behind its daemon event buffer (cap %zu) "
                         "- dropping oldest events; the panel is told to re-snapshot\n",
                         package_id.c_str(), capacity_);
        }
        queue.gapped = true;
    }
    queue.events.push_back(std::move(event));
}

void PackageEventBuffer::mark_gap(const std::string& package_id)
{
    // `dropped` is deliberately UNTOUCHED: the loss happened in the DAEMON's ring, not here, and a
    // counter that conflated the two would make a slow daemon read as an editor-side leak.
    queues_[package_id].gapped = true;
}

PackageEventDrain PackageEventBuffer::take(const std::string& package_id)
{
    const auto it = queues_.find(package_id);
    if (it == queues_.end())
    {
        // An ordinary empty drain. editor-core polls unconditionally on its tick, so "this package
        // never subscribed" is the COMMON case and must not read as a fault.
        return {};
    }

    PackageEventDrain drain;
    drain.events.reserve(it->second.events.size());
    for (contract::Json& event : it->second.events)
    {
        drain.events.push_back(std::move(event));
    }
    drain.dropped = it->second.dropped;
    drain.gapped = it->second.gapped;
    // ERASED, not merely cleared: a package that subscribed once and never again should stop costing
    // a map node, exactly as UiMirrorStore::take erases a drained window's queue.
    queues_.erase(it);
    return drain;
}

void PackageEventBuffer::forget(const std::string& package_id)
{
    queues_.erase(package_id);
}

void PackageEventBuffer::clear()
{
    queues_.clear();
}

std::size_t PackageEventBuffer::pending(const std::string& package_id) const
{
    const auto it = queues_.find(package_id);
    return it == queues_.end() ? 0u : it->second.events.size();
}

std::uint64_t PackageEventBuffer::dropped(const std::string& package_id) const
{
    const auto it = queues_.find(package_id);
    return it == queues_.end() ? 0u : it->second.dropped;
}

} // namespace context::editor::shell
