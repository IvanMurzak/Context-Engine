// EventBus: the kernel-internal, in-process, typed pub/sub (R-KERNEL-001).
//
// This is the ENGINE-INTERNAL event bus — RuntimeKernel systems talk to each other over it. It is
// deliberately distinct from the client-facing event STREAM on the bridge (R-BRIDGE-008), which
// carries post-derivation facts to clients; nothing here is exposed to clients. The `log` topic
// (LogEvent) is a first-class seam so subsystems can emit diagnostics that the bridge's `log` topic
// later forwards.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>

namespace context::kernel
{

// Handle to one subscription; pass to unsubscribe(). 0 is never a valid id.
using SubscriptionId = std::uint64_t;

enum class LogLevel
{
    trace,
    debug,
    info,
    warn,
    error,
};

// The kernel's built-in `log` topic payload.
struct LogEvent
{
    LogLevel level = LogLevel::info;
    std::string message;
};

class EventBus
{
public:
    EventBus();
    ~EventBus();
    EventBus(EventBus&&) noexcept;
    EventBus& operator=(EventBus&&) noexcept;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Subscribe to every event of type E. Handlers fire in subscription order. Returns an id for
    // unsubscribe().
    template <class E>
    SubscriptionId subscribe(std::function<void(const E&)> handler)
    {
        return subscribe_erased(std::type_index(typeid(E)),
                                [h = std::move(handler)](const void* payload)
                                { h(*static_cast<const E*>(payload)); });
    }

    // Publish an event to all current subscribers of type E. Safe to call with no subscribers.
    template <class E>
    void publish(const E& event)
    {
        publish_erased(std::type_index(typeid(E)), &event);
    }

    // Remove a subscription. A no-op for an already-removed / never-issued id.
    void unsubscribe(SubscriptionId id);

    // Number of live subscriptions across all event types.
    [[nodiscard]] std::size_t subscription_count() const noexcept;

    // Convenience for the built-in `log` topic.
    void log(LogLevel level, std::string message)
    {
        publish(LogEvent{level, std::move(message)});
    }

private:
    SubscriptionId subscribe_erased(std::type_index type,
                                    std::function<void(const void*)> handler);
    void publish_erased(std::type_index type, const void* payload);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace context::kernel
