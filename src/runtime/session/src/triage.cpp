// Determinism-divergence auto-triage (see triage.h).

#include "context/runtime/session/triage.h"

#include "context/runtime/session/session.h"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <utility>

namespace context::runtime::session
{

namespace
{
// Structural equality of two recordable input streams (the "were these even the same run?" check).
bool input_equal(const InputStream& a, const InputStream& b)
{
    if (a.ticks.size() != b.ticks.size())
        return false;
    for (std::size_t i = 0; i < a.ticks.size(); ++i)
    {
        const TickInputs& ta = a.ticks[i];
        const TickInputs& tb = b.ticks[i];
        if (ta.tick != tb.tick || ta.events.size() != tb.events.size() ||
            ta.actions.size() != tb.actions.size())
            return false;
        for (std::size_t e = 0; e < ta.events.size(); ++e)
            if (ta.events[e].device != tb.events[e].device ||
                ta.events[e].code != tb.events[e].code || ta.events[e].value != tb.events[e].value)
                return false;
        for (std::size_t c = 0; c < ta.actions.size(); ++c)
            if (ta.actions[c].action != tb.actions[c].action ||
                ta.actions[c].phase != tb.actions[c].phase ||
                ta.actions[c].value != tb.actions[c].value)
                return false;
    }
    return true;
}

// Build a fresh session for an artifact with its recorded input stream injected (not yet stepped).
Session build_session(const ReplayArtifact& artifact)
{
    SessionConfig config;
    config.seed = artifact.seed;
    config.scenario = artifact.scenario;
    Session session(config);
    for (const TickInputs& t : artifact.input_stream.ticks)
    {
        for (const InputEvent& e : t.events)
            session.inject_event_at(t.tick, e);
        for (const ActionActivation& a : t.actions)
            session.inject_action_at(t.tick, a);
    }
    return session;
}

// Re-run an artifact with tracing on and return the per-tick hierarchical hash trace.
HashTrace run_traced(const ReplayArtifact& artifact)
{
    Session session = build_session(artifact);
    session.set_trace(true);
    session.step(artifact.tick_count);
    return session.trace();
}

// The first system (by run order) whose post-run world hash differs, plus its index in `out_index`.
// Empty return + out_index unchanged when the two per-system ladders agree on their common prefix.
std::string first_divergent_system(const std::vector<SystemHash>& left,
                                   const std::vector<SystemHash>& right, std::size_t& out_index)
{
    const std::size_t n = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < n; ++i)
        if (left[i].hash != right[i].hash)
        {
            out_index = i;
            return left[i].system;
        }
    return {};
}

// Re-run an artifact and snapshot the world exactly after system `system_index` ran on `tick`.
WorldSnapshot snapshot_after(const ReplayArtifact& artifact, std::uint64_t tick,
                             std::size_t system_index)
{
    Session session = build_session(artifact);
    const SimComponentRegistry& registry = session.components();
    WorldSnapshot captured;
    bool done = false;
    session.set_system_observer(
        [&](std::uint64_t t, std::size_t idx, const std::string&, const kernel::World& world)
        {
            if (!done && t == tick && idx == system_index)
            {
                captured = snapshot_world(world, registry);
                done = true;
            }
        });
    // Step through the target tick (0-indexed: tick T is the (T+1)th step). Never step fewer than the
    // artifact recorded, so any injected input at the target tick is applied.
    const std::uint64_t needed = tick + 1;
    session.step(needed > artifact.tick_count ? needed : artifact.tick_count);
    return captured;
}
} // namespace

WorldSnapshot snapshot_world(const kernel::World& world, const SimComponentRegistry& registry)
{
    WorldSnapshot snap;

    world.for_each_archetype(
        [&](const kernel::World::ArchetypeView& view)
        {
            // Resolve + name-sort the columns (canonical, ComponentId-order-independent). An
            // unregistered column carries no named schema, so it is skipped for attribution.
            struct Column
            {
                std::size_t column = 0;
                std::string name;
                const SimComponentType* type = nullptr;
            };
            std::vector<Column> cols;
            cols.reserve(view.types().size());
            for (std::size_t c = 0; c < view.types().size(); ++c)
            {
                const SimComponentType* type = registry.by_id(view.types()[c]);
                if (type == nullptr)
                    continue; // unregistered — no named fields to attribute
                cols.push_back(Column{c, type->name, type});
            }
            std::sort(cols.begin(), cols.end(),
                      [](const Column& a, const Column& b) { return a.name < b.name; });

            std::string signature;
            for (const Column& col : cols)
            {
                if (!signature.empty())
                    signature += '+';
                signature += col.name;
            }

            // Canonical entity order: by (index, generation).
            std::vector<std::size_t> rows(view.entities().size());
            std::iota(rows.begin(), rows.end(), std::size_t{0});
            std::sort(rows.begin(), rows.end(),
                      [&](std::size_t a, std::size_t b)
                      {
                          const kernel::Entity& ea = view.entities()[a];
                          const kernel::Entity& eb = view.entities()[b];
                          if (ea.index != eb.index)
                              return ea.index < eb.index;
                          return ea.generation < eb.generation;
                      });

            for (std::size_t r : rows)
            {
                const kernel::Entity& e = view.entities()[r];
                for (const Column& col : cols)
                {
                    const std::vector<SimField> fields = col.type->read(view.component(col.column, r));
                    for (const SimField& f : fields)
                        snap.push_back(FieldValue{e.index, e.generation, signature, col.name, f.name,
                                                  f.value});
                }
            }
        });

    // Archetypes visit in the World's id-keyed order; canonicalize by (archetype, entity, component)
    // so two snapshots align positionally. stable_sort keeps each component's fields in declared
    // order (the read order they were pushed in).
    std::stable_sort(snap.begin(), snap.end(),
                     [](const FieldValue& a, const FieldValue& b)
                     {
                         if (a.archetype != b.archetype)
                             return a.archetype < b.archetype;
                         if (a.entity_index != b.entity_index)
                             return a.entity_index < b.entity_index;
                         if (a.entity_generation != b.entity_generation)
                             return a.entity_generation < b.entity_generation;
                         return a.component < b.component;
                     });
    return snap;
}

namespace
{
// The canonical identity of a snapshot slot (everything but the value) — two slots with the same key
// are "the same field of the same entity"; a value difference there is a value divergence.
bool same_slot(const FieldValue& a, const FieldValue& b)
{
    return a.entity_index == b.entity_index && a.entity_generation == b.entity_generation &&
           a.archetype == b.archetype && a.component == b.component && a.field == b.field;
}

FieldDivergence value_divergence(const FieldValue& l, const FieldValue& r)
{
    FieldDivergence d;
    d.found = true;
    d.structural = false;
    d.entity_index = l.entity_index;
    d.entity_generation = l.entity_generation;
    d.archetype = l.archetype;
    d.component = l.component;
    d.field = l.field;
    d.left_value = l.value;
    d.right_value = r.value;
    return d;
}

FieldDivergence structural_divergence(const FieldValue& present)
{
    FieldDivergence d;
    d.found = true;
    d.structural = true;
    d.entity_index = present.entity_index;
    d.entity_generation = present.entity_generation;
    d.archetype = present.archetype;
    d.component = present.component;
    d.field = present.field;
    d.left_value = present.value;
    d.right_value = present.value;
    return d;
}
} // namespace

FieldDivergence first_field_divergence(const WorldSnapshot& left, const WorldSnapshot& right)
{
    const std::size_t n = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        if (!same_slot(left[i], right[i]))
            return structural_divergence(left[i]); // shapes diverge at this canonical position
        if (left[i].value != right[i].value)
            return value_divergence(left[i], right[i]);
    }
    // Equal common prefix but one side has extra fields/entities == a structural divergence.
    if (left.size() != right.size())
        return structural_divergence(left.size() > right.size() ? left[n] : right[n]);
    return FieldDivergence{}; // identical
}

std::int64_t bisect_first_divergent_tick(const std::vector<std::uint64_t>& left,
                                         const std::vector<std::uint64_t>& right)
{
    // The per-tick root-hash ladder recorded in trace mode is the bisection substrate: a single
    // traced replay already yields EVERY tick's root, so locating the first divergent tick is a
    // direct walk of the two ladders — equivalent to, and cheaper than, re-running the replay to
    // binary-search midpoints. A walk (not a binary search over the ladder) is also correct when the
    // divergence is NOT monotonic — e.g. two artifacts recorded on different platforms whose stored
    // traces differ at a single tick — where a monotonic-boundary binary search would miss it.
    const std::size_t n = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < n; ++i)
        if (left[i] != right[i])
            return static_cast<std::int64_t>(i);
    // The common prefix is identical; a length mismatch diverges at the shorter length.
    if (left.size() != right.size())
        return static_cast<std::int64_t>(n);
    return -1;
}

DivergenceReport triage_divergence(const ReplayArtifact& left, const ReplayArtifact& right)
{
    DivergenceReport rep;
    rep.seed_match = left.seed == right.seed;
    rep.scenario_match = left.scenario == right.scenario;
    rep.input_match = input_equal(left.input_stream, right.input_stream);
    rep.left_ticks = left.tick_count;
    rep.right_ticks = right.tick_count;

    // Live reproduction: re-run both locally, capturing each tick's hierarchical hash. Determinism
    // makes the re-run bit-identical to the recorded run.
    const HashTrace lt = run_traced(left);
    const HashTrace rt = run_traced(right);
    const std::vector<std::uint64_t> lroots = trace_roots(lt);
    const std::vector<std::uint64_t> rroots = trace_roots(rt);
    const std::int64_t live_tick = bisect_first_divergent_tick(lroots, rroots);

    if (live_tick >= 0)
    {
        rep.diverged = true;
        rep.reproduced = true;
        rep.tick = live_tick;

        const std::size_t t = static_cast<std::size_t>(live_tick);
        const std::size_t common = std::min(lt.size(), rt.size());
        if (t < common)
        {
            // An in-range divergence: localize the system, then attribute the field.
            std::size_t sys_index = 0;
            rep.system = first_divergent_system(lt[t].per_system, rt[t].per_system, sys_index);
            const WorldSnapshot ls = snapshot_after(left, static_cast<std::uint64_t>(t), sys_index);
            const WorldSnapshot rs = snapshot_after(right, static_cast<std::uint64_t>(t), sys_index);
            rep.field = first_field_divergence(ls, rs);
        }
        // else: a pure tick-count (length) mismatch — tick-level divergence only, no system/field.
        return rep;
    }

    // No LOCAL divergence: the two runs are bit-identical on this host. If their STORED per-tick
    // traces still differ, the divergence is cross-platform (recorded on different platforms) —
    // bisect the stored ladders for the first divergent tick, but the field cannot be named locally.
    const std::int64_t stored_tick =
        bisect_first_divergent_tick(left.expected_hash_trace, right.expected_hash_trace);
    if (stored_tick >= 0)
    {
        rep.diverged = true;
        rep.reproduced = false;
        rep.tick = stored_tick;
        return rep;
    }

    rep.diverged = false;
    return rep;
}

} // namespace context::runtime::session
