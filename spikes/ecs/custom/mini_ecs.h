// spikes/ecs — THIN custom archetype/SoA sketch ("mini"), just enough to compare against
// EnTT/flecs. NOT a real ECS: no alignment beyond operator-new, no chunking, no parallel
// executor, POD components only, all types registered before the first archetype exists.
//
// It intentionally exercises the design locks the libraries are being scored against:
//   * R-LANG-010 / L-37 — component types are RUNTIME-REGISTERED (size only, no templates):
//     storage layout is interpreted from data, the schema-driven model.
//   * R-SIM-003        — archetype/SoA: per-archetype contiguous component columns.
//   * R-LANG-008/012   — columnView() exposes a column as {ptr, elemSize, count, version}:
//     exactly the shape a JS typed-array view binds to, per archetype.
//   * L-38 / R-SIM-006 — systems carry declared R/W sets; buildScheduleBatches() derives a
//     parallel schedule ONCE from the static declarations (derivation only — no executor).
//   * L-39             — per-archetype-column monotonic versions = cheap dirty tracking for
//     extract-to-render-world.

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace mini {

using TypeId = std::uint32_t;

struct Handle
{
    std::uint32_t slot = 0xFFFFFFFFu;
    std::uint32_t gen = 0;

    std::uint64_t pack() const
    {
        return (static_cast<std::uint64_t>(gen) << 32) | slot;
    }
    static Handle unpack(std::uint64_t v)
    {
        return Handle{static_cast<std::uint32_t>(v & 0xFFFFFFFFu),
                      static_cast<std::uint32_t>(v >> 32)};
    }
};

// The zero-copy exposure shape: raw contiguous memory + the layout metadata a JS/WASM
// seam needs to alias it as a typed array (R-LANG-008), plus the change-detection version.
struct ColumnView
{
    void* data = nullptr;
    std::uint32_t elemSize = 0;
    std::uint32_t count = 0;
    std::uint64_t version = 0;
};

class World
{
public:
    struct Archetype
    {
        std::vector<TypeId> sig;                   // sorted, unique
        std::vector<std::vector<std::byte>> cols;  // parallel to sig (elemSize-strided bytes)
        std::vector<std::uint32_t> slotOfRow;      // row -> entity slot
        std::vector<std::uint64_t> colVersion;     // parallel to sig — dirty tracking (L-39)
        std::vector<std::int32_t> colOfType;       // TypeId -> column index, -1 if absent

        std::uint32_t count() const { return static_cast<std::uint32_t>(slotOfRow.size()); }
    };

    TypeId registerType(std::uint32_t size, const char* name)
    {
        assert(archetypes_.empty() && "sketch constraint: register all types first");
        types_.push_back(TypeInfo{size, name});
        return static_cast<TypeId>(types_.size() - 1);
    }

    std::uint32_t typeSize(TypeId t) const { return types_[t].size; }

    Archetype* archetypeFor(std::vector<TypeId> sig) // pre-sorted, unique
    {
        if (auto it = bySig_.find(sig); it != bySig_.end()) return it->second;
        auto arch = std::make_unique<Archetype>();
        arch->sig = sig;
        arch->cols.resize(sig.size());
        arch->colVersion.assign(sig.size(), 0);
        arch->colOfType.assign(types_.size(), -1);
        for (std::size_t i = 0; i < sig.size(); ++i)
            arch->colOfType[sig[i]] = static_cast<std::int32_t>(i);
        Archetype* raw = arch.get();
        archetypes_.push_back(std::move(arch));
        bySig_.emplace(std::move(sig), raw);
        return raw;
    }

    // data[i] initializes column i (arch->sig order).
    Handle createIn(Archetype& a, const void* const* data)
    {
        std::uint32_t slot;
        if (!freeSlots_.empty())
        {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
        }
        else
        {
            slot = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back(Slot{});
        }
        Slot& s = slots_[slot];
        s.arch = &a;
        s.row = a.count();
        for (std::size_t i = 0; i < a.sig.size(); ++i)
            appendElem(a.cols[i], data[i], types_[a.sig[i]].size);
        a.slotOfRow.push_back(slot);
        ++aliveCount_;
        return Handle{slot, s.gen};
    }

    bool alive(Handle h) const
    {
        return h.slot < slots_.size() && slots_[h.slot].gen == h.gen
               && slots_[h.slot].arch != nullptr;
    }

    void destroy(Handle h)
    {
        Slot& s = slots_[h.slot];
        swapRemoveRow(*s.arch, s.row);
        s.arch = nullptr;
        ++s.gen;
        freeSlots_.push_back(h.slot);
        --aliveCount_;
    }

    bool has(Handle h, TypeId t) const
    {
        const Slot& s = slots_[h.slot];
        return s.arch->colOfType[t] >= 0;
    }

    const void* get(Handle h, TypeId t) const
    {
        const Slot& s = slots_[h.slot];
        const std::int32_t c = s.arch->colOfType[t];
        if (c < 0) return nullptr;
        return s.arch->cols[static_cast<std::size_t>(c)].data()
               + static_cast<std::size_t>(s.row) * types_[t].size;
    }

    // Mutable random access; bumps the column's change version (coarse, chunk-level dirty
    // tracking — the L-39 extract consumer asks "did this column change since version V?").
    void* getMut(Handle h, TypeId t)
    {
        Slot& s = slots_[h.slot];
        const std::int32_t c = s.arch->colOfType[t];
        if (c < 0) return nullptr;
        s.arch->colVersion[static_cast<std::size_t>(c)] = ++worldVersion_;
        return s.arch->cols[static_cast<std::size_t>(c)].data()
               + static_cast<std::size_t>(s.row) * types_[t].size;
    }

    // Archetype migration: move the entity's row to the archetype with `t` added.
    void addComponent(Handle h, TypeId t, const void* data)
    {
        Slot& s = slots_[h.slot];
        Archetype& src = *s.arch;
        std::vector<TypeId> sig = src.sig;
        sig.insert(std::lower_bound(sig.begin(), sig.end(), t), t);
        Archetype& dst = *archetypeFor(std::move(sig));
        moveRow(src, s, dst, t, data);
    }

    void removeComponent(Handle h, TypeId t)
    {
        Slot& s = slots_[h.slot];
        Archetype& src = *s.arch;
        std::vector<TypeId> sig = src.sig;
        sig.erase(std::lower_bound(sig.begin(), sig.end(), t));
        Archetype& dst = *archetypeFor(std::move(sig));
        moveRow(src, s, dst, t, nullptr);
    }

    // Iteration fast path: for every archetype whose signature contains `required`, hand the
    // callback the raw column pointers (in `required` order), the row count, and the
    // row->slot map. Columns listed in `writes` get their change version bumped ONCE per
    // archetype visit (chunk-level dirty granularity).
    template <typename F>
    void forEach(std::span<const TypeId> required, std::span<const TypeId> writes, F&& f)
    {
        void* colPtr[8];
        assert(required.size() <= 8);
        for (auto& archPtr : archetypes_)
        {
            Archetype& a = *archPtr;
            if (a.count() == 0) continue;
            bool match = true;
            for (const TypeId t : required)
                if (a.colOfType[t] < 0) { match = false; break; }
            if (!match) continue;
            for (std::size_t i = 0; i < required.size(); ++i)
                colPtr[i] = a.cols[static_cast<std::size_t>(a.colOfType[required[i]])].data();
            for (const TypeId t : writes)
                a.colVersion[static_cast<std::size_t>(a.colOfType[t])] = ++worldVersion_;
            f(a, a.count(), colPtr, a.slotOfRow.data());
        }
    }

    // Zero-copy exposure of one archetype column (R-LANG-008 demo).
    ColumnView columnView(Archetype& a, TypeId t) const
    {
        const std::int32_t c = a.colOfType[t];
        if (c < 0) return ColumnView{};
        const auto ci = static_cast<std::size_t>(c);
        return ColumnView{const_cast<std::byte*>(a.cols[ci].data()), types_[t].size, a.count(),
                          a.colVersion[ci]};
    }

    std::uint32_t genOf(std::uint32_t slot) const { return slots_[slot].gen; }
    std::uint64_t aliveCount() const { return aliveCount_; }
    std::uint64_t worldVersion() const { return worldVersion_; }

private:
    struct TypeInfo
    {
        std::uint32_t size;
        const char* name;
    };

    struct Slot
    {
        Archetype* arch = nullptr;
        std::uint32_t row = 0;
        std::uint32_t gen = 0;
    };

    static void appendElem(std::vector<std::byte>& col, const void* data, std::uint32_t size)
    {
        const std::size_t off = col.size();
        col.resize(off + size);
        std::memcpy(col.data() + off, data, size);
    }

    void swapRemoveRow(Archetype& a, std::uint32_t row)
    {
        const std::uint32_t last = a.count() - 1;
        if (row != last)
        {
            for (std::size_t i = 0; i < a.sig.size(); ++i)
            {
                const std::uint32_t sz = types_[a.sig[i]].size;
                std::memcpy(a.cols[i].data() + static_cast<std::size_t>(row) * sz,
                            a.cols[i].data() + static_cast<std::size_t>(last) * sz, sz);
            }
            const std::uint32_t movedSlot = a.slotOfRow[last];
            a.slotOfRow[row] = movedSlot;
            slots_[movedSlot].row = row;
        }
        for (std::size_t i = 0; i < a.sig.size(); ++i)
            a.cols[i].resize(a.cols[i].size() - types_[a.sig[i]].size);
        a.slotOfRow.pop_back();
    }

    // Move slot s's row from src to dst; `added` (with `data`) is the new component on add,
    // or the dropped component on remove (data == nullptr).
    void moveRow(Archetype& src, Slot& s, Archetype& dst, TypeId added, const void* data)
    {
        const std::uint32_t row = s.row;
        for (std::size_t i = 0; i < dst.sig.size(); ++i)
        {
            const TypeId t = dst.sig[i];
            const std::uint32_t sz = types_[t].size;
            if (data != nullptr && t == added)
            {
                appendElem(dst.cols[i], data, sz);
            }
            else
            {
                const auto sc = static_cast<std::size_t>(src.colOfType[t]);
                appendElem(dst.cols[i], src.cols[sc].data() + static_cast<std::size_t>(row) * sz,
                           sz);
            }
        }
        dst.slotOfRow.push_back(src.slotOfRow[row]);
        swapRemoveRow(src, row);
        s.arch = &dst;
        s.row = dst.count() - 1;
    }

    std::vector<TypeInfo> types_;
    std::vector<std::unique_ptr<Archetype>> archetypes_;
    std::map<std::vector<TypeId>, Archetype*> bySig_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::uint64_t aliveCount_ = 0;
    std::uint64_t worldVersion_ = 0;
};

// ---- L-38 sketch: derive the parallel schedule ONCE from static declared access sets -----
struct SystemDecl
{
    const char* name;
    std::vector<TypeId> reads;
    std::vector<TypeId> writes;
};

inline bool accessConflicts(const SystemDecl& a, const SystemDecl& b)
{
    auto intersects = [](const std::vector<TypeId>& x, const std::vector<TypeId>& y) {
        for (const TypeId t : x)
            for (const TypeId u : y)
                if (t == u) return true;
        return false;
    };
    return intersects(a.writes, b.writes) || intersects(a.writes, b.reads)
           || intersects(b.writes, a.reads);
}

// Greedy layering honoring declaration order: each system lands one batch after the last
// conflicting earlier system. Batches are the sets a parallel executor may run concurrently.
// Computed once per composition change and cached — never per frame (L-38).
inline std::vector<std::vector<std::size_t>> buildScheduleBatches(
    std::span<const SystemDecl> systems)
{
    std::vector<int> batchOf(systems.size(), 0);
    int maxBatch = 0;
    for (std::size_t i = 0; i < systems.size(); ++i)
    {
        int b = 0;
        for (std::size_t j = 0; j < i; ++j)
            if (accessConflicts(systems[i], systems[j]) && batchOf[j] + 1 > b)
                b = batchOf[j] + 1;
        batchOf[i] = b;
        if (b > maxBatch) maxBatch = b;
    }
    std::vector<std::vector<std::size_t>> batches(static_cast<std::size_t>(maxBatch) + 1);
    for (std::size_t i = 0; i < systems.size(); ++i)
        batches[static_cast<std::size_t>(batchOf[i])].push_back(i);
    return batches;
}

} // namespace mini
