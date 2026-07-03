// EventBus implementation: type-erased, snapshot-dispatched pub/sub (R-KERNEL-001).

#include "context/kernel/event_bus.h"

#include <unordered_map>
#include <vector>

namespace context::kernel
{

struct EventBus::Impl
{
    struct Entry
    {
        SubscriptionId id;
        std::function<void(const void*)> handler;
    };

    std::unordered_map<std::type_index, std::vector<Entry>> by_type;
    std::unordered_map<SubscriptionId, std::type_index> id_to_type;
    SubscriptionId next_id = 1;
    std::size_t count = 0;
};

EventBus::EventBus() : impl_(std::make_unique<Impl>()) {}
EventBus::~EventBus() = default;
EventBus::EventBus(EventBus&&) noexcept = default;
EventBus& EventBus::operator=(EventBus&&) noexcept = default;

SubscriptionId EventBus::subscribe_erased(std::type_index type,
                                          std::function<void(const void*)> handler)
{
    const SubscriptionId id = impl_->next_id++;
    impl_->by_type[type].push_back(Impl::Entry{id, std::move(handler)});
    impl_->id_to_type.emplace(id, type);
    ++impl_->count;
    return id;
}

void EventBus::publish_erased(std::type_index type, const void* payload)
{
    auto it = impl_->by_type.find(type);
    if (it == impl_->by_type.end())
        return;
    // Snapshot the handlers so a handler that (un)subscribes mid-dispatch cannot invalidate the
    // vector we are iterating.
    std::vector<std::function<void(const void*)>> handlers;
    handlers.reserve(it->second.size());
    for (const auto& entry : it->second)
        handlers.push_back(entry.handler);
    for (const auto& handler : handlers)
        handler(payload);
}

void EventBus::unsubscribe(SubscriptionId id)
{
    auto type_it = impl_->id_to_type.find(id);
    if (type_it == impl_->id_to_type.end())
        return;
    auto bucket_it = impl_->by_type.find(type_it->second);
    if (bucket_it != impl_->by_type.end())
    {
        auto& entries = bucket_it->second;
        for (auto e = entries.begin(); e != entries.end(); ++e)
        {
            if (e->id == id)
            {
                entries.erase(e);
                --impl_->count;
                break;
            }
        }
    }
    impl_->id_to_type.erase(type_it);
}

std::size_t EventBus::subscription_count() const noexcept { return impl_->count; }

} // namespace context::kernel
