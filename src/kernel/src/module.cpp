// Module registry implementation (R-KERNEL-003/004).

#include "context/kernel/module.h"

#include <algorithm>

namespace context::kernel
{

Module& ModuleRegistry::register_module(std::unique_ptr<Module> module)
{
    const std::string_view name = module->name();
    if (contains(name))
        throw DuplicateModuleError(std::string{name});
    modules_.push_back(std::move(module));
    return *modules_.back();
}

Module* ModuleRegistry::find(std::string_view name)
{
    for (auto& m : modules_)
    {
        if (m->name() == name)
            return m.get();
    }
    return nullptr;
}

const Module* ModuleRegistry::find(std::string_view name) const
{
    for (const auto& m : modules_)
    {
        if (m->name() == name)
            return m.get();
    }
    return nullptr;
}

bool ModuleRegistry::contains(std::string_view name) const { return find(name) != nullptr; }

std::vector<std::string> ModuleRegistry::missing_dependencies() const
{
    std::vector<std::string> missing;
    for (const auto& m : modules_)
    {
        for (const std::string& dep : m->dependencies())
        {
            if (!contains(dep) &&
                std::find(missing.begin(), missing.end(), dep) == missing.end())
                missing.push_back(dep);
        }
    }
    return missing;
}

} // namespace context::kernel
