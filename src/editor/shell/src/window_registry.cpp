// The pure half of the window registry (window_registry.h) — M9 e10a.
//
// Everything here is a total function over plain values: the outcome vocabulary, the failure
// report's wording, and the parts contract. The stateful half (id minting, create/destroy, the
// retired-session graveyard) lives on WindowManager in shell.cpp, because it owns the windows.

#include "context/editor/shell/window_registry.h"

namespace context::editor::shell
{

const char* to_string(WindowCreateOutcome outcome)
{
    switch (outcome)
    {
    case WindowCreateOutcome::created:
        return "created";
    case WindowCreateOutcome::no_factory:
        return "no-factory";
    case WindowCreateOutcome::factory_failed:
        return "factory-failed";
    case WindowCreateOutcome::incomplete_parts:
        return "incomplete-parts";
    case WindowCreateOutcome::limit_reached:
        return "limit-reached";
    }
    return "unknown";
}

const char* to_string(WindowDestroyOutcome outcome)
{
    switch (outcome)
    {
    case WindowDestroyOutcome::destroyed:
        return "destroyed";
    case WindowDestroyOutcome::unknown_window:
        return "unknown-window";
    case WindowDestroyOutcome::primary_refused:
        return "primary-refused";
    }
    return "unknown";
}

std::string describe(const WindowCreateFailure& failure)
{
    // Shape: what failed, for whom, and why — in that order, because the first two are what a
    // consumer routes on and the third is what a human acts on.
    std::string text = "secondary window could not be created (";
    text += to_string(failure.outcome);
    text += ") for window ";
    text += std::to_string(static_cast<unsigned long long>(failure.source));
    if (!failure.title.empty())
    {
        text += " titled '";
        text += failure.title;
        text += "'";
    }
    text += ": ";
    // A failure with no stated reason is itself a defect worth naming: an empty tail would read as
    // "it failed, shrug", which is exactly the silent degradation 03 §7 forbids.
    text += failure.error.empty() ? "no reason reported by the window factory" : failure.error;
    return text;
}

bool validate_window_parts(const WindowSessionParts& parts, std::string& error)
{
    if (parts.backend == nullptr)
    {
        error = "the window factory produced no window backend";
        return false;
    }
    if (parts.browser == nullptr)
    {
        error = "the window factory produced no browser host";
        return false;
    }
    error.clear();
    return true;
}

} // namespace context::editor::shell
