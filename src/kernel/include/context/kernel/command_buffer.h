// CommandBuffer: deferred structural changes for the archetype/SoA World (the R-LANG-009
// structural-changes-via-command-buffers clause; L-60).
//
// A structural change — add-component, remove-component, entity create, entity destroy — migrates an
// entity between archetypes (or grows a column), which MOVES component storage. Under the R-LANG-009
// view protocol a running system holds live zero-copy views over that storage, so structural changes
// issued by a running system are RECORDED here instead of applied: the buffer owns the deferred
// commands (including add-component payload bytes) and `apply()` executes them against the World
// AFTER the system returns — between systems, never during an invocation. A running system therefore
// observes a stable World for its whole invocation, and memory never moves under a live view.
//
// Determinism (L-37 composed-id discipline / L-54): `apply()` executes commands in exactly the order
// they were recorded (FIFO), so one buffer's outcome is a pure function of its recording sequence.
// Cross-system determinism is the flush-ordering contract of the caller — the safe parallel scheduler
// (editor/schedule) applies per-system buffers at batch boundaries in ascending registration order,
// independent of thread completion order.
//
// Thread-safety: a CommandBuffer is NOT internally synchronized. The intended shape is one buffer per
// system per tick — each running system records only into its OWN buffer (concurrently with sibling
// systems recording into theirs), and the scheduler applies every buffer from the join thread after
// the fork-join barrier. Recording into one buffer from two threads is a caller error.

#pragma once

#include "component.h"
#include "entity.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace context::kernel
{

class World;

class CommandBuffer
{
public:
    // A placeholder handle for an entity whose creation is deferred. Minted by create(); resolves to
    // a real Entity when apply() runs, and is a valid target for add/remove/destroy commands recorded
    // LATER in the SAME buffer (so a system can create an entity and populate its components in one
    // deferred batch). A PendingEntity is meaningful only within the buffer that minted it and only
    // until that buffer's next apply()/clear(); a stale or foreign handle resolves to the invalid
    // entity at apply time, making its commands deterministic no-ops.
    struct PendingEntity
    {
        std::uint32_t index = 0;
    };

    CommandBuffer();
    ~CommandBuffer(); // discards unapplied commands (destroying owned payloads)

    CommandBuffer(CommandBuffer&&) noexcept;
    CommandBuffer& operator=(CommandBuffer&&) noexcept;
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    // --- deferred entity lifecycle --------------------------------------------------------------

    // Record a deferred entity creation and return its placeholder handle.
    [[nodiscard]] PendingEntity create();

    // Record a deferred destroy of a live entity (a no-op at apply time if `e` is dead by then —
    // World::destroy semantics) or of a pending entity minted by this buffer.
    void destroy(Entity e);
    void destroy(PendingEntity p);

    // --- deferred components ---------------------------------------------------------------------

    // Record a deferred add (or overwrite) of component T. The value is moved into the buffer NOW and
    // moved into World storage at apply time — recording never touches the World.
    template <class T>
    void add(Entity e, T value)
    {
        add_raw(e, component_id<T>(), ops_for<T>(), &value);
    }
    template <class T>
    void add(PendingEntity p, T value)
    {
        add_raw(p, component_id<T>(), ops_for<T>(), &value);
    }

    // Type-erased deferred add — the runtime-typed analog (R-LANG-010), mirroring World::add_raw:
    // `src == nullptr` records a zero-initialized add; for POD ops the payload is byte-copied into
    // the buffer; for ops with a real move_construct, `src` is consumed as if moved-from.
    void add_raw(Entity e, ComponentId id, const ComponentOps& ops, const void* src);
    void add_raw(PendingEntity p, ComponentId id, const ComponentOps& ops, const void* src);

    // Record a deferred remove of component T / component `id` (a no-op at apply time if the target
    // does not have it — World::remove semantics).
    template <class T>
    void remove(Entity e)
    {
        remove_raw(e, component_id<T>());
    }
    template <class T>
    void remove(PendingEntity p)
    {
        remove_raw(p, component_id<T>());
    }
    void remove_raw(Entity e, ComponentId id);
    void remove_raw(PendingEntity p, ComponentId id);

    // --- inspection / lifecycle ------------------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept;

    // Number of recorded, not-yet-applied commands (creates included).
    [[nodiscard]] std::size_t size() const noexcept;

    // Number of pending entity creations recorded since the last apply()/clear().
    [[nodiscard]] std::size_t pending_count() const noexcept;

    // Discard every recorded command WITHOUT applying (destroying owned add-payloads). Pending
    // handles minted before clear() are invalidated.
    void clear() noexcept;

    // Apply every recorded command to `world`, in exactly the order recorded (FIFO — deterministic,
    // see the header comment). Commands whose target is dead by execution time (destroyed earlier in
    // this buffer, or externally) degrade to the corresponding World no-op. Returns the real Entity
    // created for each PendingEntity, index-aligned with PendingEntity::index. The buffer is left
    // empty and reusable; pending indices restart at 0.
    std::vector<Entity> apply(World& world);

private:
    enum class Kind : std::uint8_t
    {
        Create,
        Destroy,
        Add,
        Remove,
    };

    // One recorded command. For a pending target, `target.index` holds the PendingEntity index and
    // `target.generation` is 0 with `pending_target` set (a real Entity target always has a nonzero
    // generation — World never mints generation 0). `payload` (Add only) is an owned, aligned block
    // holding the component value, or nullptr for a zero-initialized add.
    struct Command
    {
        Kind kind = Kind::Create;
        bool pending_target = false;
        Entity target;
        ComponentId id = 0;
        ComponentOps ops;
        void* payload = nullptr;
    };

    void record_add(Command cmd, const void* src);
    static void destroy_payload(Command& cmd) noexcept;

    std::vector<Command> commands_;
    std::uint32_t pending_count_ = 0;
};

} // namespace context::kernel
