// A reusable, backend-agnostic UI-provider conformance harness (M7 a11, R-UI-002 pluggability proof +
// the R-UI-008 out-of-tree on-ramp). Any UiProvider — the in-repo null/GPU providers OR a downstream
// out-of-tree backend — proves it plugs into the engine by passing run_provider_conformance().
//
// Deliberately SELF-CONTAINED: it pulls in NO test framework (its checks tally into a Report and log to
// stderr), so a provider author drops this ONE header in, writes a three-line main() around their
// provider, and treats a non-zero return as non-conformant. It asserts ONLY the public UiProvider
// contract — capabilities() stability, the negotiate_repaint fallback coherence, and present() never
// mutating the tree (D6) — never any backend-specific introspection, so it is honestly reusable across
// backends (the null provider walks nothing; the GPU provider extracts + draws; both must pass the SAME
// checks). This is the R-UI-002 pluggability proof both in-repo providers satisfy and the R-UI-008 seam
// an out-of-tree provider runs.

#pragma once

#include "context/packages/ui/damage.h"
#include "context/packages/ui/provider.h"
#include "context/packages/ui/ui_node.h"
#include "context/packages/ui/ui_tree.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace context::packages::ui::conformance
{

// The canonical ORDERED (name, value) view of a Capabilities struct — the ONE source of truth for the
// capability field set, shared by this harness and the rots-if-broken capability-matrix cross-check. The
// structured binding pins the field COUNT at compile time: add a field to Capabilities (or remove one)
// and this decomposition fails to compile HERE, forcing the maintainer to extend the list — and, via the
// matrix cross-check that consumes it, to add the matching published-matrix column. The names MUST match
// the Capabilities member names AND the matrix column labels verbatim.
struct CapabilityEntry
{
    std::string_view name;
    bool value;
};

[[nodiscard]] inline std::array<CapabilityEntry, 6> capability_entries(const Capabilities& caps)
{
    const auto& [gpu_driver, damage_repaint, composited_transforms, text_shaping, bidi, ime] = caps;
    return {{{"gpu_driver", gpu_driver},
             {"damage_repaint", damage_repaint},
             {"composited_transforms", composited_transforms},
             {"text_shaping", text_shaping},
             {"bidi", bidi},
             {"ime", ime}}};
}

// A tiny self-contained result tally (NO external test framework — the R-UI-008 on-ramp must run with
// nothing but this header). expect() records + logs each failure with the provider label.
struct Report
{
    const char* label = "";
    int failures = 0;

    void expect(bool cond, const char* what)
    {
        if (!cond)
        {
            ++failures;
            std::fprintf(stderr, "conformance[%s]: FAILED -- %s\n", label, what);
        }
    }
};

namespace detail
{

// A structural fingerprint of the LIVE tree (pre-order over live nodes): role + text + visibility +
// child count per node. present() is presentation-only (D6), so it must leave this byte-identical.
[[nodiscard]] inline std::string fingerprint(const UiTree& tree, NodeId id)
{
    const UiNode* n = tree.node(id);
    if (n == nullptr)
    {
        return {};
    }
    std::string s;
    s += role_name(n->role);
    s += '|';
    s += n->text;
    s += '|';
    s += n->style.visible ? '1' : '0';
    s += '|';
    s += std::to_string(n->children.size());
    s += ';';
    for (const NodeId child : n->children)
    {
        s += fingerprint(tree, child);
    }
    return s;
}

// A small representative tree: a HUD-ish panel with a label + progress bar + a nested group holding an
// invisible item, with non-default styles/bounds so a mutating provider would perturb the fingerprint.
inline void build_reference_tree(UiTree& tree)
{
    const NodeId root = tree.root();
    const NodeId hud = tree.create_node(Role::Panel, root);
    tree.set_bounds(hud, Rect{0, 0, 128, 32});
    Style hs;
    hs.background = Color{20, 20, 28, 255};
    tree.set_style(hud, hs);

    const NodeId score = tree.create_node(Role::Label, hud);
    tree.set_text(score, "Score: 0");
    tree.set_bounds(score, Rect{4, 4, 60, 12});

    const NodeId health = tree.create_node(Role::ProgressBar, hud);
    tree.set_bounds(health, Rect{4, 18, 120, 8});

    const NodeId group = tree.create_node(Role::Group, hud);
    const NodeId item = tree.create_node(Role::ListItem, group);
    tree.set_text(item, "x1");
    tree.set_visible(item, false); // an invisible node — a provider must still not mutate it
}

} // namespace detail

// Run the backend-agnostic pluggability contract (R-UI-002) against `provider`. Returns the failure
// count (0 == conformant). The R-UI-008 on-ramp: an out-of-tree provider proves it plugs in by passing.
[[nodiscard]] inline int run_provider_conformance(UiProvider& provider, const char* label)
{
    Report r;
    r.label = label;

    // (1) capabilities() is const-stable: repeated reads agree field-for-field. The engine may query it
    //     many times per frame; a provider must not report a moving target.
    const Capabilities caps = provider.capabilities();
    {
        const std::array<CapabilityEntry, 6> a = capability_entries(caps);
        const std::array<CapabilityEntry, 6> b = capability_entries(provider.capabilities());
        bool stable = true;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            stable = stable && a[i].name == b[i].name && a[i].value == b[i].value;
        }
        r.expect(stable, "capabilities() is not stable across calls");
    }

    // (2) negotiate_repaint fallback coherence w.r.t. the provider's OWN declared caps (R-UI-005): a
    //     region-damage request yields a FULL repaint iff the provider lacks damage_repaint; a `full`
    //     damage request always yields a FULL repaint; a damage-capable provider gets the dirty regions.
    const Rect viewport{0, 0, 128, 32};
    {
        DamageList region;
        region.add(Rect{4, 4, 60, 12});
        const RepaintPlan plan = negotiate_repaint(caps, region, viewport);
        if (caps.damage_repaint)
        {
            r.expect(!plan.full_repaint,
                     "a damage-capable provider was forced to a full repaint on region damage");
            r.expect(!plan.regions.empty(), "a damage-capable provider dropped the dirty regions");
        }
        else
        {
            r.expect(plan.full_repaint,
                     "a provider without damage_repaint did not fall back to a full repaint");
        }

        DamageList full_damage;
        full_damage.mark_full();
        const RepaintPlan fplan = negotiate_repaint(caps, full_damage, viewport);
        r.expect(fplan.full_repaint, "a full damage request must always negotiate a full repaint");
    }

    // (3) present() is presentation-only (D6): it must NOT mutate the tree under ANY plan — a full
    //     repaint, the negotiated damage plan for this provider, or an empty plan. This is THE invariant
    //     every backend shares regardless of what its present() does internally.
    {
        UiTree tree;
        detail::build_reference_tree(tree);
        const std::string before = detail::fingerprint(tree, tree.root());

        RepaintPlan full;
        full.full_repaint = true;
        provider.present(tree, full);
        r.expect(detail::fingerprint(tree, tree.root()) == before, "present(full) mutated the tree");

        DamageList dmg;
        dmg.add(Rect{4, 4, 60, 12});
        const RepaintPlan negotiated = negotiate_repaint(caps, dmg, viewport);
        provider.present(tree, negotiated);
        r.expect(detail::fingerprint(tree, tree.root()) == before, "present(damage) mutated the tree");

        RepaintPlan empty;
        provider.present(tree, empty);
        r.expect(detail::fingerprint(tree, tree.root()) == before, "present(empty) mutated the tree");

        // (4) repeatable — many frames in a row don't crash and don't accumulate any mutation.
        for (int i = 0; i < 8; ++i)
        {
            provider.present(tree, (i % 2 == 0) ? full : empty);
        }
        r.expect(detail::fingerprint(tree, tree.root()) == before, "repeated present() mutated the tree");
    }

    // (5) robustness — an empty tree (root only), under both a full and an empty plan, must be accepted
    //     and left intact (a provider must not require a populated tree).
    {
        UiTree empty_tree;
        RepaintPlan full;
        full.full_repaint = true;
        provider.present(empty_tree, full);
        RepaintPlan none;
        provider.present(empty_tree, none);
        r.expect(empty_tree.node_count() == 1, "present() on an empty tree perturbed it");
    }

    return r.failures;
}

} // namespace context::packages::ui::conformance
