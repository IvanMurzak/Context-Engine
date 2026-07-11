// Problems observer panel: R-FILE-003 diagnostics projected into a headless uitree Panel (list +
// inline markers) with click-to-navigate + R-BRIDGE-008 provisional->stable promotion / stale discard.

#include "context/editor/gui/panels/problems/problems_panel.h"

#include "context/editor/gui/panels/problems/problems_model.h"
#include "context/editor/gui/uitree/node.h"

#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace context::editor::gui::panels::problems
{

namespace
{

// The status-line text: total + per-severity breakdown + provisional count + the R-BRIDGE-008
// stability + the derived-world generation. Deterministic (identical state -> identical string).
[[nodiscard]] std::string status_text(const ProblemsModel& model, bridge::Stability stability,
                                      std::uint64_t generation)
{
    std::ostringstream out;
    if (model.empty())
    {
        out << "No problems";
    }
    else
    {
        out << model.total << (model.total == 1 ? " problem" : " problems");

        std::vector<std::string> parts;
        const auto add = [&parts](std::size_t n, const char* singular, const char* plural)
        {
            if (n > 0)
            {
                parts.push_back(std::to_string(n) + " " + (n == 1 ? singular : plural));
            }
        };
        add(model.errors, "error", "errors");
        add(model.warnings, "warning", "warnings");
        add(model.infos, "info", "infos");
        add(model.hints, "hint", "hints");

        if (!parts.empty())
        {
            out << " - ";
            for (std::size_t i = 0; i < parts.size(); ++i)
            {
                if (i > 0)
                {
                    out << ", ";
                }
                out << parts[i];
            }
        }
        if (model.provisional > 0)
        {
            out << " - " << model.provisional << " provisional";
        }
    }
    out << " - " << bridge::stability_name(stability) << " - generation " << generation;
    return out.str();
}

} // namespace

void ProblemsPanel::set_diagnostics(std::vector<ProblemDiagnostic> diagnostics)
{
    // Collapse duplicate identities (last wins = the promotion path), preserving first-seen order.
    std::vector<ProblemDiagnostic> unique;
    unique.reserve(diagnostics.size());
    std::unordered_map<std::string, std::size_t> index_of;
    for (ProblemDiagnostic& d : diagnostics)
    {
        const std::string id = d.identity();
        const auto it = index_of.find(id);
        if (it == index_of.end())
        {
            index_of.emplace(id, unique.size());
            unique.push_back(std::move(d));
        }
        else
        {
            unique[it->second] = std::move(d);
        }
    }
    diagnostics_ = std::move(unique);

    if (navigation_.diagnostic_identity.empty())
    {
        return;
    }
    if (const ProblemDiagnostic* d = find(navigation_.diagnostic_identity);
        d != nullptr && d->nav.navigable())
    {
        // Preserve navigation across the refresh (identity survives -> no listener notification),
        // refreshing the stored target in case the diagnostic's location moved.
        navigation_.target = d->nav;
    }
    else
    {
        // The navigated diagnostic vanished from the new set -> clear and notify.
        navigation_ = ProblemNavigation{};
        notify();
    }
}

bool ProblemsPanel::ingest(ProblemDiagnostic diagnostic)
{
    const std::string id = diagnostic.identity();
    for (ProblemDiagnostic& d : diagnostics_)
    {
        if (d.identity() == id)
        {
            d = std::move(diagnostic); // same problem re-emitted: promote / update in place
            return true;
        }
    }
    diagnostics_.push_back(std::move(diagnostic));
    return false;
}

void ProblemsPanel::on_derivation_settled(std::uint64_t generation, bridge::Stability stability)
{
    generation_ = generation;
    stability_ = stability;

    std::vector<ProblemDiagnostic> kept;
    kept.reserve(diagnostics_.size());
    for (ProblemDiagnostic& d : diagnostics_)
    {
        if (d.provisional() && d.generation < generation)
        {
            continue; // stale: a later settling pass superseded this provisional diagnostic (R-FILE-003)
        }
        if (d.provisional() && d.generation == generation)
        {
            d.stability = bridge::Stability::stable; // promote: this generation settled (R-BRIDGE-008)
        }
        kept.push_back(std::move(d));
    }
    diagnostics_ = std::move(kept);

    if (!navigation_.diagnostic_identity.empty() && find(navigation_.diagnostic_identity) == nullptr)
    {
        navigation_ = ProblemNavigation{};
        notify();
    }
}

bool ProblemsPanel::navigate(const std::string& identity)
{
    const ProblemDiagnostic* d = find(identity);
    if (d == nullptr || !d->nav.navigable())
    {
        return false;
    }
    navigation_ = ProblemNavigation{d->identity(), d->nav};
    notify();
    return true;
}

void ProblemsPanel::clear_navigation()
{
    navigation_ = ProblemNavigation{};
    notify();
}

void ProblemsPanel::add_navigation_listener(NavigationListener listener)
{
    listeners_.push_back(std::move(listener));
}

void ProblemsPanel::notify() const
{
    for (const NavigationListener& listener : listeners_)
    {
        if (listener)
        {
            listener(navigation_);
        }
    }
}

const ProblemDiagnostic* ProblemsPanel::find(const std::string& identity) const
{
    for (const ProblemDiagnostic& d : diagnostics_)
    {
        if (d.identity() == identity)
        {
            return &d;
        }
    }
    return nullptr;
}

uitree::Panel ProblemsPanel::build_panel() const
{
    using uitree::Role;
    using uitree::UiNode;

    const ProblemsModel model = build_problems_model(diagnostics_);

    bool any_navigable = false;
    for (const ProblemGroup& group : model.groups)
    {
        for (const ProblemDiagnostic& d : group.diagnostics)
        {
            if (d.nav.navigable())
            {
                any_navigable = true;
                break;
            }
        }
        if (any_navigable)
        {
            break;
        }
    }

    uitree::Panel panel("problems", "Problems");
    if (any_navigable)
    {
        // Exposed only when a navigable row binds it, so it is never an unreachable a11y orphan.
        panel.add_command(kNavigateCommand, "Go to source");
    }

    UiNode root(Role::region, "problems.panel");
    root.set_label("Problems");

    root.add_child(
        UiNode(Role::heading, "problems.heading").set_label("Problems").set_text("Problems"));

    root.add_child(UiNode(Role::status, "problems.status")
                       .set_label("Problems status")
                       .set_text(status_text(model, stability_, generation_)));

    UiNode list(Role::list, "problems.list");
    list.set_label("Problem list");

    std::size_t row_index = 0;
    for (std::size_t gi = 0; gi < model.groups.size(); ++gi)
    {
        const ProblemGroup& group = model.groups[gi];
        UiNode group_node(Role::group, "problems.group." + std::to_string(gi));

        // A visible, inert file header so grouping is legible to sighted + assistive-tech users.
        const std::string file_label = group.file.empty() ? "(no file)" : group.file;
        group_node.add_child(
            UiNode(Role::text, "problems.file." + std::to_string(gi)).set_text(file_label));

        for (const ProblemDiagnostic& d : group.diagnostics)
        {
            UiNode item(Role::listitem, "problems.row." + std::to_string(row_index));
            ++row_index;

            // Accessible name: stable + navigation-independent, so it does not churn as the current
            // navigation target moves.
            std::string label = std::string(severity_name(d.severity)) + ": " + d.message;
            const std::string loc = d.nav.display();
            if (!loc.empty())
            {
                label += " (" + loc + ")";
            }
            item.set_label(label);
            item.set_focusable(true);
            if (d.nav.navigable())
            {
                item.set_command(kNavigateCommand);
            }

            // Visible text: the label plus the dynamic markers a squiggle would carry.
            std::string text = label;
            if (d.provisional())
            {
                text += " (provisional)";
            }
            if (!navigation_.diagnostic_identity.empty() &&
                d.identity() == navigation_.diagnostic_identity)
            {
                text += " (active)";
            }
            item.set_text(text);

            group_node.add_child(std::move(item));
        }
        list.add_child(std::move(group_node));
    }
    root.add_child(std::move(list));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::panels::problems
