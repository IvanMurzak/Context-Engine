// Kernel: the minimal RuntimeKernel facade (R-KERNEL-001).
//
// R-KERNEL-001 fixes the kernel's contents exactly: the World (component storage), the fixed-
// timestep Scheduler, the module registry, the event bus, the resource-handle registry, and the
// platform seam — and NOTHING with game-feature semantics. This type bundles precisely those and
// no more; every feature (rendering, physics, audio, ...) composes on top as a Module (R-KERNEL-002).

#pragma once

#include "context/kernel/event_bus.h"
#include "context/kernel/module.h"
#include "context/kernel/platform.h"
#include "context/kernel/resource.h"
#include "context/kernel/scheduler.h"
#include "context/kernel/world.h"

#include <memory>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>

namespace context::kernel
{

// Owns the concrete headless-default platform impls and exposes a Platform view over them.
class DefaultPlatform
{
public:
    [[nodiscard]] Platform view() noexcept { return Platform{&clock_, &fs_, &tasks_}; }
    [[nodiscard]] SteadyClock& clock() noexcept { return clock_; }
    [[nodiscard]] MemoryFileSystem& fs() noexcept { return fs_; }
    [[nodiscard]] InlineTaskRunner& tasks() noexcept { return tasks_; }

private:
    SteadyClock clock_;
    MemoryFileSystem fs_;
    InlineTaskRunner tasks_;
};

class Kernel
{
public:
    // Default: an owned headless platform (SteadyClock + MemoryFileSystem + InlineTaskRunner).
    Kernel();
    // Inject a platform seam (e.g. a ManualClock + MemoryFileSystem for the fault-injection harness).
    explicit Kernel(Platform platform);
    ~Kernel();

    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;
    Kernel(Kernel&&) = delete;
    Kernel& operator=(Kernel&&) = delete;

    [[nodiscard]] World& world() noexcept { return world_; }
    [[nodiscard]] const World& world() const noexcept { return world_; }

    [[nodiscard]] Scheduler& scheduler() noexcept { return scheduler_; }
    [[nodiscard]] const Scheduler& scheduler() const noexcept { return scheduler_; }

    [[nodiscard]] ModuleRegistry& modules() noexcept { return modules_; }
    [[nodiscard]] const ModuleRegistry& modules() const noexcept { return modules_; }

    [[nodiscard]] EventBus& events() noexcept { return events_; }
    [[nodiscard]] const EventBus& events() const noexcept { return events_; }

    [[nodiscard]] const Platform& platform() const noexcept { return platform_; }

    // Register a module and run its on_register lifecycle hook against this kernel's world + events.
    Module& add_module(std::unique_ptr<Module> module)
    {
        Module& m = modules_.register_module(std::move(module));
        m.on_register(world_, events_);
        return m;
    }

    // The resource-handle registry: one ResourcePool per resource type, created on first request.
    template <class T>
    [[nodiscard]] ResourcePool<T>& resources()
    {
        const std::type_index ti{typeid(T)};
        auto it = resource_pools_.find(ti);
        if (it == resource_pools_.end())
        {
            auto pool = std::make_unique<TypedPool<T>>();
            TypedPool<T>* raw = pool.get();
            resource_pools_.emplace(ti, std::move(pool));
            return raw->pool;
        }
        return static_cast<TypedPool<T>*>(it->second.get())->pool;
    }

private:
    struct IPool
    {
        virtual ~IPool() = default;
    };
    template <class T>
    struct TypedPool final : IPool
    {
        ResourcePool<T> pool;
    };

    std::unique_ptr<DefaultPlatform> owned_platform_;
    Platform platform_;
    World world_;
    Scheduler scheduler_;
    ModuleRegistry modules_;
    EventBus events_;
    std::unordered_map<std::type_index, std::unique_ptr<IPool>> resource_pools_;
};

} // namespace context::kernel
