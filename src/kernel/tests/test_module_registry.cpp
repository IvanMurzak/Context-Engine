// Module registry tests: explicit registration, duplicate rejection, dependency reporting
// (R-QA-013, R-KERNEL-003/004).

#include "context/kernel/module.h"
#include "kernel_test.h"

#include <string>
#include <string_view>
#include <vector>

using namespace context::kernel;

namespace
{
class NamedModule final : public Module
{
public:
    explicit NamedModule(std::string name, std::vector<std::string> deps = {})
        : name_(std::move(name)), deps_(std::move(deps))
    {
    }
    [[nodiscard]] std::string_view name() const override { return name_; }
    [[nodiscard]] std::vector<std::string> dependencies() const override { return deps_; }

private:
    std::string name_;
    std::vector<std::string> deps_;
};
} // namespace

int main()
{
    // --- register / find / contains / size -----------------------------------------------------
    {
        ModuleRegistry reg;
        CHECK(reg.size() == 0);
        Module& r = reg.register_module(std::make_unique<NamedModule>("render"));
        CHECK(reg.size() == 1);
        CHECK(reg.contains("render"));
        CHECK(reg.find("render") == &r);
        CHECK(reg.find("physics") == nullptr);
        CHECK(!reg.contains("physics"));
    }

    // --- duplicate name is rejected ------------------------------------------------------------
    {
        ModuleRegistry reg;
        reg.register_module(std::make_unique<NamedModule>("audio"));
        bool threw = false;
        try
        {
            reg.register_module(std::make_unique<NamedModule>("audio"));
        }
        catch (const DuplicateModuleError&)
        {
            threw = true;
        }
        CHECK(threw);
        CHECK(reg.size() == 1);
    }

    // --- declared dependencies: missing then satisfied -----------------------------------------
    {
        ModuleRegistry reg;
        reg.register_module(std::make_unique<NamedModule>("render", std::vector<std::string>{"gpu"}));
        std::vector<std::string> missing = reg.missing_dependencies();
        CHECK(missing.size() == 1);
        CHECK(missing.front() == "gpu");

        reg.register_module(std::make_unique<NamedModule>("gpu"));
        CHECK(reg.missing_dependencies().empty());
    }

    // --- a dependency named by two modules is reported once ------------------------------------
    {
        ModuleRegistry reg;
        reg.register_module(std::make_unique<NamedModule>("a", std::vector<std::string>{"core"}));
        reg.register_module(std::make_unique<NamedModule>("b", std::vector<std::string>{"core"}));
        CHECK(reg.missing_dependencies().size() == 1);
    }

    KERNEL_TEST_MAIN_END();
}
