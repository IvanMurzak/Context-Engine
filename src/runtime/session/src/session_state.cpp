// Session-state (de)serialization (see session_state.h).

#include "context/runtime/session/session_state.h"

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/runtime/session/json_build.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace context::runtime::session
{

namespace
{
constexpr std::int64_t kStateVersion = 1;

// One entity's serialized form, collected across archetypes then sorted into canonical id order.
struct EntityRecordJson
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    serializer::JsonValue components; // object: {componentName: {field: value}}
};

serializer::JsonValue components_of(const kernel::World::ArchetypeView& view, std::size_t row,
                                    const SimComponentRegistry& registry)
{
    serializer::JsonValue obj = jb::object();
    for (std::size_t col = 0; col < view.types().size(); ++col)
    {
        const SimComponentType* type = registry.by_id(view.types()[col]);
        if (type == nullptr)
            continue; // unregistered components are not serializable state (none in the demo)
        serializer::JsonValue fields = jb::object();
        for (const SimField& f : type->read(view.component(col, row)))
            jb::set(fields, f.name, jb::integer(f.value));
        jb::set(obj, type->name, std::move(fields));
    }
    return obj;
}
} // namespace

serializer::JsonValue session_state_to_json(const Session& session)
{
    serializer::JsonValue doc = jb::object();
    jb::set(doc, "$schema", jb::str("ctx:session-state"));
    jb::set(doc, "version", jb::integer(kStateVersion));
    jb::set(doc, "seed", jb::uinteger(session.seed()));
    jb::set(doc, "tickHz", jb::uinteger(session.tick_hz()));
    jb::set(doc, "scenario", jb::str(session.scenario()));
    jb::set(doc, "simTick", jb::uinteger(session.sim_tick()));
    jb::set(doc, "rngState", jb::uinteger(session.rng_state()));
    jb::set(doc, "traceEnabled", jb::boolean(session.trace_enabled()));

    std::vector<EntityRecordJson> records;
    session.world().for_each_archetype(
        [&](const kernel::World::ArchetypeView& view)
        {
            for (std::size_t row = 0; row < view.entities().size(); ++row)
            {
                const kernel::Entity e = view.entities()[row];
                records.push_back(
                    EntityRecordJson{e.index, e.generation, components_of(view, row, session.components())});
            }
        });
    std::sort(records.begin(), records.end(),
              [](const EntityRecordJson& a, const EntityRecordJson& b)
              {
                  if (a.index != b.index)
                      return a.index < b.index;
                  return a.generation < b.generation;
              });

    serializer::JsonValue entities = jb::array();
    for (EntityRecordJson& r : records)
    {
        serializer::JsonValue entry = jb::object();
        jb::set(entry, "components", std::move(r.components));
        jb::set(entry, "generation", jb::uinteger(r.generation));
        jb::set(entry, "index", jb::uinteger(r.index));
        jb::push(entities, std::move(entry));
    }
    jb::set(doc, "entities", std::move(entities));
    jb::set(doc, "inputLog", input_stream_to_json(session.input_log()));
    return doc;
}

std::string session_state_dump(const Session& session)
{
    std::string out;
    if (!serializer::serialize_canonical(session_state_to_json(session), out))
        out.clear(); // unreachable: session-state trees carry only integers/strings
    return out;
}

namespace
{
LoadResult invalid(std::string message)
{
    LoadResult r;
    r.ok = false;
    r.error_code = "session.state_invalid";
    r.message = std::move(message);
    return r;
}
} // namespace

LoadResult session_from_json(const serializer::JsonValue& doc)
{
    if (doc.type != serializer::JsonValue::Type::object)
        return invalid("session state root is not an object");
    if (jb::as_int(jb::member(doc, "version")) != kStateVersion)
        return invalid("unsupported session-state version");

    SessionConfig config;
    config.seed = jb::as_uint(jb::member(doc, "seed"));
    config.tick_hz = jb::as_uint(jb::member(doc, "tickHz"), 60);
    config.scenario = jb::as_str(jb::member(doc, "scenario"), "demo");

    // Build with the systems wired but the world left empty; we restore the world from the doc.
    Session session(config, /*run_setup=*/false);
    const SimComponentRegistry& registry = session.components();

    const serializer::JsonValue* entities = jb::member(doc, "entities");
    if (entities == nullptr || entities->type != serializer::JsonValue::Type::array)
        return invalid("session state has no entities array");

    // Restore entities in ascending-index order so plain World::create() reproduces the exact ids.
    std::vector<const serializer::JsonValue*> ordered;
    ordered.reserve(entities->elements.size());
    for (const serializer::JsonValue& e : entities->elements)
        ordered.push_back(&e);
    std::sort(ordered.begin(), ordered.end(),
              [](const serializer::JsonValue* a, const serializer::JsonValue* b)
              { return jb::as_uint(jb::member(*a, "index")) < jb::as_uint(jb::member(*b, "index")); });

    for (const serializer::JsonValue* entry : ordered)
    {
        const auto want_index = static_cast<std::uint32_t>(jb::as_uint(jb::member(*entry, "index")));
        const auto want_gen = static_cast<std::uint32_t>(jb::as_uint(jb::member(*entry, "generation")));
        const kernel::Entity created = session.world().create();
        if (created.index != want_index || created.generation != want_gen)
            return invalid("session state entity ids are not restorable (id recycling is not "
                           "supported by restore yet)");

        const serializer::JsonValue* components = jb::member(*entry, "components");
        if (components == nullptr || components->type != serializer::JsonValue::Type::object)
            continue;
        for (const serializer::JsonMember& comp : components->members)
        {
            const SimComponentType* type = registry.by_name(comp.key);
            if (type == nullptr)
                return invalid("session state names an unknown component: " + comp.key);
            std::vector<std::int64_t> values;
            values.reserve(type->fields.size());
            for (const std::string& field : type->fields)
                values.push_back(jb::as_int(jb::member(comp.value, field)));
            void* storage = type->add_fn(session.world(), created);
            type->write(storage, values);
        }
    }

    InputStream input_log;
    if (const serializer::JsonValue* log = jb::member(doc, "inputLog"); log != nullptr)
        input_log = input_stream_from_json(*log);

    session.restore_runtime(config.seed, jb::as_uint(jb::member(doc, "rngState")),
                            jb::as_uint(jb::member(doc, "simTick")), std::move(input_log));
    session.set_trace(jb::as_bool(jb::member(doc, "traceEnabled")));

    LoadResult result;
    result.ok = true;
    result.session = std::move(session);
    return result;
}

LoadResult session_state_parse(std::string_view text)
{
    serializer::ParseResult parsed = serializer::parse_json(text);
    if (!parsed.ok)
        return invalid("session state is not well-formed JSON");
    return session_from_json(parsed.root);
}

} // namespace context::runtime::session
