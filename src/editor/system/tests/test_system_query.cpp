// R-QA-013 tests for the (query, executor) tier's ENGINE-INDEPENDENT half: entity selection by
// declared component, and the gather/scatter round-trip between the World's archetype storage and a
// packed column buffer. These need no JS host, so they are a LOCAL gate on every leg (the full
// run_system path, which needs the V8 host, is in test_system_in_v8.cpp — CI-only for its V8 path).
//
// Covers: selection over multiple archetypes (all-components-present subset test), the empty query,
// canonical intra-archetype order, gather byte-fidelity vs get_raw, and a scatter round-trip that
// mutates the packed buffer and observes it in the derived World.

#include "context/editor/component/component_registry.h"
#include "context/editor/system/system.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"

#include "system_test.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace csys = context::editor::system;
namespace ccomp = context::editor::component;
namespace ck = context::kernel;

namespace
{

ccomp::ComponentTypeSchema compile_def(const std::string& def)
{
    std::vector<std::string> problems;
    std::optional<ccomp::ComponentTypeSchema> t = ccomp::compile_component_type(def, problems);
    CHECK(t.has_value());
    CHECK(problems.empty());
    return t.value_or(ccomp::ComponentTypeSchema{}); // degrade to empty on a failed compile (CHECK logs it)
}

// A "mover" component: pos (f32x3) + hits (u32). A "tag" component: flag (u8).
ccomp::ComponentTypeSchema mover_def()
{
    return compile_def(R"({
        "$id": "demo:mover", "version": 1,
        "fields": [
            {"name": "pos", "x-ctx-storage": "f32x3"},
            {"name": "hits", "x-ctx-storage": "u32"}
        ]
    })");
}

ccomp::ComponentTypeSchema tag_def()
{
    return compile_def(R"({
        "$id": "demo:tag", "version": 1,
        "fields": [{"name": "flag", "x-ctx-storage": "u8"}]
    })");
}

// Whether `e` appears in `entities` (selection sets are small).
[[nodiscard]] bool has_entity(const std::vector<ck::Entity>& entities, ck::Entity e)
{
    for (const ck::Entity x : entities)
    {
        if (x == e)
        {
            return true;
        }
    }
    return false;
}

void test_selection()
{
    ck::World world;
    ccomp::ComponentTypeRegistry reg;
    const ccomp::RegisteredComponentType& mover = reg.register_type(mover_def());
    const ccomp::RegisteredComponentType& tag = reg.register_type(tag_def());

    const ck::Entity e_mover = world.create();       // mover only
    const ck::Entity e_both = world.create();        // mover + tag
    const ck::Entity e_tag = world.create();         // tag only
    const ck::Entity e_none = world.create();        // no components
    (void)reg.add_default(world, e_mover, mover);
    (void)reg.add_default(world, e_both, mover);
    (void)reg.add_default(world, e_both, tag);
    (void)reg.add_default(world, e_tag, tag);

    // Query {mover}: the two entities carrying mover, nothing else.
    const std::vector<ck::Entity> movers = csys::select_entities(world, {mover.component_id});
    CHECK(movers.size() == 2);
    CHECK(has_entity(movers, e_mover));
    CHECK(has_entity(movers, e_both));
    CHECK(!has_entity(movers, e_tag));
    CHECK(!has_entity(movers, e_none));

    // Query {mover, tag}: only the entity carrying BOTH.
    const std::vector<ck::Entity> both =
        csys::select_entities(world, {mover.component_id, tag.component_id});
    CHECK(both.size() == 1);
    CHECK(has_entity(both, e_both));

    // Duplicate ids in the query collapse to one; an empty query selects nothing.
    const std::vector<ck::Entity> dup =
        csys::select_entities(world, {mover.component_id, mover.component_id});
    CHECK(dup.size() == 2);
    CHECK(csys::select_entities(world, {}).empty());
}

void test_intra_archetype_order()
{
    // Within one archetype, selection follows storage-row (creation) order — deterministic (L-54).
    ck::World world;
    ccomp::ComponentTypeRegistry reg;
    const ccomp::RegisteredComponentType& mover = reg.register_type(mover_def());

    std::vector<ck::Entity> created;
    for (int i = 0; i < 5; ++i)
    {
        const ck::Entity e = world.create();
        (void)reg.add_default(world, e, mover);
        created.push_back(e);
    }
    const std::vector<ck::Entity> selected = csys::select_entities(world, {mover.component_id});
    CHECK(selected.size() == created.size());
    for (std::size_t i = 0; i < created.size(); ++i)
    {
        CHECK(selected[i] == created[i]);
    }
}

void test_gather_scatter_roundtrip()
{
    ck::World world;
    ccomp::ComponentTypeRegistry reg;
    const ccomp::RegisteredComponentType& mover = reg.register_type(mover_def());
    const std::size_t rec = mover.schema.size;
    const ccomp::ComponentField* pos = mover.schema.field("pos");
    const ccomp::ComponentField* hits = mover.schema.field("hits");
    CHECK(pos != nullptr);
    CHECK(hits != nullptr);

    // Three entities with distinct known records, written straight into the archetype storage.
    std::vector<ck::Entity> ents;
    for (int i = 0; i < 3; ++i)
    {
        const ck::Entity e = world.create();
        auto* r = static_cast<unsigned char*>(reg.add_default(world, e, mover));
        CHECK(r != nullptr);
        const float px = static_cast<float>(i) + 0.5F;
        const std::uint32_t h = static_cast<std::uint32_t>(100 + i);
        std::memcpy(r + pos->offset, &px, sizeof(px)); // pos.x
        std::memcpy(r + hits->offset, &h, sizeof(h));
        ents.push_back(e);
    }

    // Gather: the packed buffer byte-matches each entity's live record.
    std::vector<unsigned char> packed(ents.size() * rec, 0xEE);
    csys::gather_column(world, ents, mover.component_id, rec, packed.data());
    for (std::size_t i = 0; i < ents.size(); ++i)
    {
        const void* live = world.get_raw(ents[i], mover.component_id);
        CHECK(live != nullptr);
        CHECK(std::memcmp(packed.data() + i * rec, live, rec) == 0);
    }

    // Mutate the packed buffer (bump hits, overwrite pos.x), scatter back, observe it in the World.
    for (std::size_t i = 0; i < ents.size(); ++i)
    {
        unsigned char* row = packed.data() + i * rec;
        std::uint32_t h = 0;
        std::memcpy(&h, row + hits->offset, sizeof(h));
        h += 1000;
        std::memcpy(row + hits->offset, &h, sizeof(h));
        const float nx = static_cast<float>(i) * 2.0F;
        std::memcpy(row + pos->offset, &nx, sizeof(nx));
    }
    csys::scatter_column(world, ents, mover.component_id, rec, packed.data());

    for (std::size_t i = 0; i < ents.size(); ++i)
    {
        const auto* live = static_cast<const unsigned char*>(world.get_raw(ents[i], mover.component_id));
        CHECK(live != nullptr);
        std::uint32_t h = 0;
        std::memcpy(&h, live + hits->offset, sizeof(h));
        CHECK(h == static_cast<std::uint32_t>(100 + i) + 1000);
        float x = 0.0F;
        std::memcpy(&x, live + pos->offset, sizeof(x));
        CHECK(x == static_cast<float>(i) * 2.0F);
    }
}

// Two components with DIFFERENT record sizes, gathered and scattered over the SAME selected entity set
// (exactly what run_system does one column at a time), prove the per-column packing keeps each entity's
// records aligned and never cross-contaminates one column with another. This is the multi-column
// analogue of the single-column round-trip above; the full run_system positional-view path needs the
// V8 host, so it is exercised by the CI-only keystone in test_system_in_v8.cpp — this LOCAL gate covers
// the World-storage column math for a multi-component query.
void test_multi_component_gather_scatter()
{
    ck::World world;
    ccomp::ComponentTypeRegistry reg;
    const ccomp::RegisteredComponentType& mover = reg.register_type(mover_def()); // pos f32x3 + hits u32
    const ccomp::RegisteredComponentType& tag = reg.register_type(tag_def());     // flag u8
    const std::size_t mrec = mover.schema.size;
    const std::size_t trec = tag.schema.size;
    const ccomp::ComponentField* hits = mover.schema.field("hits");
    const ccomp::ComponentField* flag = tag.schema.field("flag");
    CHECK(hits != nullptr);
    CHECK(flag != nullptr);

    // Four entities carrying BOTH components (one shared archetype), seeded with distinct known values.
    // Fetch the record pointers AFTER both components are added — the second add_default migrates the
    // entity to the {mover,tag} archetype, invalidating a pointer taken before it.
    std::vector<ck::Entity> ents;
    for (int i = 0; i < 4; ++i)
    {
        const ck::Entity e = world.create();
        (void)reg.add_default(world, e, mover);
        (void)reg.add_default(world, e, tag);
        auto* mr = static_cast<unsigned char*>(world.get_raw(e, mover.component_id));
        auto* tr = static_cast<unsigned char*>(world.get_raw(e, tag.component_id));
        CHECK(mr != nullptr);
        CHECK(tr != nullptr);
        const std::uint32_t h = static_cast<std::uint32_t>(200 + i);
        const std::uint8_t f = static_cast<std::uint8_t>(i + 1);
        std::memcpy(mr + hits->offset, &h, sizeof(h));
        std::memcpy(tr + flag->offset, &f, sizeof(f));
        ents.push_back(e);
    }

    const std::vector<ck::Entity> sel =
        csys::select_entities(world, {mover.component_id, tag.component_id});
    CHECK(sel.size() == ents.size());

    // Gather each column into its OWN packed buffer (per-column, like run_system's per-view gather).
    std::vector<unsigned char> mbuf(sel.size() * mrec, 0xEE);
    std::vector<unsigned char> tbuf(sel.size() * trec, 0xEE);
    csys::gather_column(world, sel, mover.component_id, mrec, mbuf.data());
    csys::gather_column(world, sel, tag.component_id, trec, tbuf.data());
    for (std::size_t i = 0; i < sel.size(); ++i)
    {
        CHECK(std::memcmp(mbuf.data() + i * mrec, world.get_raw(sel[i], mover.component_id), mrec) == 0);
        CHECK(std::memcmp(tbuf.data() + i * trec, world.get_raw(sel[i], tag.component_id), trec) == 0);
    }

    // Mutate BOTH columns independently, scatter both back into the World.
    for (std::size_t i = 0; i < sel.size(); ++i)
    {
        std::uint32_t h = 0;
        std::memcpy(&h, mbuf.data() + i * mrec + hits->offset, sizeof(h));
        h += 50;
        std::memcpy(mbuf.data() + i * mrec + hits->offset, &h, sizeof(h));
        tbuf[i * trec + flag->offset] = static_cast<unsigned char>(tbuf[i * trec + flag->offset] * 10);
    }
    csys::scatter_column(world, sel, mover.component_id, mrec, mbuf.data());
    csys::scatter_column(world, sel, tag.component_id, trec, tbuf.data());

    // Each column's mutation landed on the right entity, with no cross-column contamination.
    for (std::size_t i = 0; i < sel.size(); ++i)
    {
        const auto* mr = static_cast<const unsigned char*>(world.get_raw(sel[i], mover.component_id));
        const auto* tr = static_cast<const unsigned char*>(world.get_raw(sel[i], tag.component_id));
        CHECK(mr != nullptr);
        CHECK(tr != nullptr);
        std::uint32_t h = 0;
        std::memcpy(&h, mr + hits->offset, sizeof(h));
        CHECK(h == static_cast<std::uint32_t>(200 + i) + 50);
        CHECK(tr[flag->offset] == static_cast<unsigned char>((i + 1) * 10));
    }
}

} // namespace

int main()
{
    test_selection();
    test_intra_archetype_order();
    test_gather_scatter_roundtrip();
    test_multi_component_gather_scatter();
    if (systemtest::g_failures == 0)
    {
        std::printf("system query/gather/scatter: all checks passed\n");
    }
    SYSTEM_TEST_MAIN_END();
}
