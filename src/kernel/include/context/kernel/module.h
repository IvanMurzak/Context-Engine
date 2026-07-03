// Module registry: the uniform package contract + explicit registration seam (R-KERNEL-003/004).
//
// Every engine feature is a package (R-KERNEL-002); a package is a Module. Registration is
// ALWAYS EXPLICIT — the registry offers no static-initializer self-registration hook, because
// self-registering objects are reachable from their own initializers and structurally defeat
// linker dead-code elimination (R-KERNEL-003). In shipped builds a generated translation unit
// calls register_module() for exactly the referenced packages; nothing else is linked in.

#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace context::kernel
{

class World;
class EventBus;

// The uniform package contract: a name, declared dependencies, and register/unregister lifecycle
// hooks. Concrete packages subclass this. Kept intentionally tiny — feature semantics live in the
// package, never in the kernel.
class Module
{
public:
    virtual ~Module() = default;

    // Stable, unique package name (e.g. "render", "physics2d"). Used as the registry key.
    [[nodiscard]] virtual std::string_view name() const = 0;

    // Names of other modules this one requires. Default: none.
    [[nodiscard]] virtual std::vector<std::string> dependencies() const { return {}; }

    // Lifecycle: called once when the module is registered onto / removed from the kernel. Default
    // no-ops so trivial packages need not override them.
    virtual void on_register(World& /*world*/, EventBus& /*events*/) {}
    virtual void on_unregister() {}
};

// Thrown when a module is registered under a name that is already taken.
class DuplicateModuleError : public std::runtime_error
{
public:
    explicit DuplicateModuleError(const std::string& module_name)
        : std::runtime_error("module already registered: " + module_name)
    {
    }
};

class ModuleRegistry
{
public:
    ModuleRegistry() = default;
    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;
    ModuleRegistry(ModuleRegistry&&) = default;
    ModuleRegistry& operator=(ModuleRegistry&&) = default;

    // Register a module. Throws DuplicateModuleError if its name is already present. Returns a
    // reference to the stored module (ownership stays with the registry).
    Module& register_module(std::unique_ptr<Module> module);

    [[nodiscard]] Module* find(std::string_view name);
    [[nodiscard]] const Module* find(std::string_view name) const;
    [[nodiscard]] bool contains(std::string_view name) const;
    [[nodiscard]] std::size_t size() const noexcept { return modules_.size(); }

    // Every declared dependency name that is NOT itself registered — empty when the composition is
    // self-consistent. (Ordering/topological composition is a package-layer concern; the kernel
    // only reports the gaps.)
    [[nodiscard]] std::vector<std::string> missing_dependencies() const;

private:
    std::vector<std::unique_ptr<Module>> modules_;
};

} // namespace context::kernel
