// FakeWatcher implementation (the fault-injection watcher).

#include "context/editor/filesync/watcher.h"

#include <algorithm>
#include <utility>

namespace context::editor::filesync
{

void FakeWatcher::emit(WatchEvent event)
{
    pending_.push_back(std::move(event));
}

void FakeWatcher::emit(std::string path, ChangeKind kind)
{
    pending_.push_back(WatchEvent{std::move(path), kind});
}

void FakeWatcher::drop_all()
{
    pending_.clear();
}

void FakeWatcher::duplicate_all()
{
    const std::size_t original = pending_.size();
    for (std::size_t i = 0; i < original; ++i)
        pending_.push_back(pending_[i]);
}

void FakeWatcher::reverse()
{
    std::reverse(pending_.begin(), pending_.end());
}

std::vector<WatchEvent> FakeWatcher::poll()
{
    std::vector<WatchEvent> drained;
    drained.swap(pending_);
    return drained;
}

} // namespace context::editor::filesync
