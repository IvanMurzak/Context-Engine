// Operational-argument parsing implementation (see args.h).

#include "context/cli/args.h"

#include <limits>

namespace context::cli
{

std::optional<std::string> flag_value(const std::vector<std::string>& args, const std::string& name)
{
    const std::string prefix = "--" + name;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == prefix && i + 1 < args.size())
            return args[i + 1];
        const std::string eq = prefix + "=";
        if (args[i].rfind(eq, 0) == 0)
            return args[i].substr(eq.size());
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parse_u64(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
            return std::nullopt; // would overflow
        value = value * 10u + digit;
    }
    return value;
}

bool has_flag(const std::vector<std::string>& args, const std::string& name)
{
    const std::string f = "--" + name;
    for (const std::string& a : args)
        if (a == f)
            return true;
    return false;
}

} // namespace context::cli
