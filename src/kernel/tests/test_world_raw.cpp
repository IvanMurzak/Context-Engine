// Data-driven (runtime-typed) World storage tests: the non-template add_raw/get_raw/has_raw/
// remove_raw API + kernel::pod_ops trivially-relocatable columns (R-LANG-010, L-60). Proves a
// component type registered at RUNTIME (a ComponentId + POD ops, no C++ type) lands in the same
// archetype/SoA storage as the compile-time add<T> path — including archetype migration + growth.

#include "context/kernel/component.h"
#include "context/kernel/world.h"
#include "kernel_test.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace context::kernel;

namespace
{
// A 12-byte runtime record: three int32 fields (x, y, z), addressed purely by byte offset.
std::int32_t field(const void* record, std::size_t offset)
{
    std::int32_t v = 0;
    std::memcpy(&v, static_cast<const std::byte*>(record) + offset, sizeof(v));
    return v;
}
void set_field(void* record, std::size_t offset, std::int32_t v)
{
    std::memcpy(static_cast<std::byte*>(record) + offset, &v, sizeof(v));
}
} // namespace

int main()
{
    // Two distinct runtime component ids, both stored as POD blobs of different sizes.
    const ComponentId kVec3 = detail::next_component_id();
    const ComponentId kByte = detail::next_component_id();
    const ComponentOps vec3_ops = pod_ops(/*size=*/12, /*align=*/4);
    const ComponentOps byte_ops = pod_ops(/*size=*/1, /*align=*/1);

    // --- zero-init add + read/write ------------------------------------------------------------
    {
        World w;
        const Entity e = w.create();
        void* rec = w.add_raw(e, kVec3, vec3_ops, nullptr); // zero-initialized
        CHECK(rec != nullptr);
        CHECK(field(rec, 0) == 0);
        CHECK(field(rec, 4) == 0);
        CHECK(field(rec, 8) == 0);
        CHECK(w.has_raw(e, kVec3));
        set_field(rec, 0, 7);
        set_field(rec, 4, -3);
        set_field(rec, 8, 100);

        // get_raw returns the SAME storage (values persist).
        void* again = w.get_raw(e, kVec3);
        CHECK(again == rec);
        CHECK(field(again, 0) == 7);
        CHECK(field(again, 4) == -3);
        CHECK(field(again, 8) == 100);
    }

    // --- add from a source blob (memcpy relocate) ----------------------------------------------
    {
        World w;
        const Entity e = w.create();
        std::byte src[12];
        set_field(src, 0, 11);
        set_field(src, 4, 22);
        set_field(src, 8, 33);
        void* rec = w.add_raw(e, kVec3, vec3_ops, src);
        CHECK(rec != nullptr);
        CHECK(field(rec, 0) == 11);
        CHECK(field(rec, 8) == 33);
    }

    // --- archetype migration + column growth preserve POD bytes --------------------------------
    {
        World w;
        std::vector<Entity> es;
        for (int i = 0; i < 50; ++i)
        {
            const Entity e = w.create();
            void* rec = w.add_raw(e, kVec3, vec3_ops, nullptr);
            set_field(rec, 0, i);
            set_field(rec, 4, i * 2);
            set_field(rec, 8, i * 3);
            es.push_back(e);
        }
        // Add a SECOND component to every other entity — migrates them to a new archetype, relocating
        // the vec3 column bytes (exercises pod_ops memcpy relocation across archetypes + growth).
        for (std::size_t i = 0; i < es.size(); i += 2)
            w.add_raw(es[i], kByte, byte_ops, nullptr);

        for (int i = 0; i < 50; ++i)
        {
            const void* rec = w.get_raw(es[static_cast<std::size_t>(i)], kVec3);
            CHECK(rec != nullptr);
            CHECK(field(rec, 0) == i);
            CHECK(field(rec, 4) == i * 2);
            CHECK(field(rec, 8) == i * 3);
        }
    }

    // --- overwrite + remove --------------------------------------------------------------------
    {
        World w;
        const Entity e = w.create();
        void* rec = w.add_raw(e, kVec3, vec3_ops, nullptr);
        set_field(rec, 0, 5);
        // Re-add overwrites in place (same archetype).
        std::byte src[12] = {};
        set_field(src, 0, 99);
        void* rec2 = w.add_raw(e, kVec3, vec3_ops, src);
        CHECK(field(rec2, 0) == 99);

        CHECK(w.remove_raw(e, kVec3));
        CHECK(!w.has_raw(e, kVec3));
        CHECK(w.get_raw(e, kVec3) == nullptr);
        CHECK(!w.remove_raw(e, kVec3)); // second remove is a no-op
    }

    // --- dead-entity + absent-component safety -------------------------------------------------
    {
        World w;
        const Entity e = w.create();
        w.destroy(e);
        CHECK(w.add_raw(e, kVec3, vec3_ops, nullptr) == nullptr);
        CHECK(w.get_raw(e, kVec3) == nullptr);
        CHECK(!w.has_raw(e, kVec3));
        CHECK(!w.remove_raw(e, kVec3));
    }

    KERNEL_TEST_MAIN_END();
}
