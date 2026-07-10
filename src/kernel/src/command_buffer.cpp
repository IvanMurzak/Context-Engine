// CommandBuffer — deferred structural changes, applied in recorded order. See command_buffer.h for
// the contract.

#include "context/kernel/command_buffer.h"

#include "context/kernel/world.h"

#include <cstring>
#include <new>
#include <utility>

namespace context::kernel
{
namespace
{

// Allocate an aligned block for one component payload. ComponentOps alignments are real alignments
// (alignof(T), or the schema-declared align of a POD record), so aligned operator new handles every
// case uniformly; the matching aligned delete is in free_block.
[[nodiscard]] void* alloc_block(const ComponentOps& ops)
{
    const std::size_t align = ops.align == 0 ? 1 : ops.align;
    return ::operator new(ops.size, std::align_val_t{align});
}

void free_block(void* p, const ComponentOps& ops) noexcept
{
    const std::size_t align = ops.align == 0 ? 1 : ops.align;
    ::operator delete(p, std::align_val_t{align});
}

// Move-construct (or byte-copy, for POD ops with a null move_construct) one component value from
// `src` into `dst` — the same null-hook convention World's storage uses (component.h).
void relocate_into(const ComponentOps& ops, void* dst, void* src) noexcept
{
    if (ops.move_construct != nullptr)
    {
        ops.move_construct(dst, src);
    }
    else
    {
        std::memcpy(dst, src, ops.size);
    }
}

} // namespace

CommandBuffer::CommandBuffer() = default;

CommandBuffer::~CommandBuffer()
{
    clear();
}

CommandBuffer::CommandBuffer(CommandBuffer&& other) noexcept
    : commands_(std::move(other.commands_)), pending_count_(other.pending_count_)
{
    other.commands_.clear();
    other.pending_count_ = 0;
}

CommandBuffer& CommandBuffer::operator=(CommandBuffer&& other) noexcept
{
    if (this != &other)
    {
        clear();
        commands_ = std::move(other.commands_);
        pending_count_ = other.pending_count_;
        other.commands_.clear();
        other.pending_count_ = 0;
    }
    return *this;
}

CommandBuffer::PendingEntity CommandBuffer::create()
{
    const PendingEntity p{pending_count_};
    ++pending_count_;
    Command cmd;
    cmd.kind = Kind::Create;
    cmd.pending_target = true;
    cmd.target = Entity{p.index, 0};
    commands_.push_back(cmd);
    return p;
}

void CommandBuffer::destroy(Entity e)
{
    Command cmd;
    cmd.kind = Kind::Destroy;
    cmd.target = e;
    commands_.push_back(cmd);
}

void CommandBuffer::destroy(PendingEntity p)
{
    Command cmd;
    cmd.kind = Kind::Destroy;
    cmd.pending_target = true;
    cmd.target = Entity{p.index, 0};
    commands_.push_back(cmd);
}

void CommandBuffer::add_raw(Entity e, ComponentId id, const ComponentOps& ops, const void* src)
{
    Command cmd;
    cmd.kind = Kind::Add;
    cmd.target = e;
    cmd.id = id;
    cmd.ops = ops;
    record_add(cmd, src);
}

void CommandBuffer::add_raw(PendingEntity p, ComponentId id, const ComponentOps& ops,
                            const void* src)
{
    Command cmd;
    cmd.kind = Kind::Add;
    cmd.pending_target = true;
    cmd.target = Entity{p.index, 0};
    cmd.id = id;
    cmd.ops = ops;
    record_add(cmd, src);
}

void CommandBuffer::remove_raw(Entity e, ComponentId id)
{
    Command cmd;
    cmd.kind = Kind::Remove;
    cmd.target = e;
    cmd.id = id;
    commands_.push_back(cmd);
}

void CommandBuffer::remove_raw(PendingEntity p, ComponentId id)
{
    Command cmd;
    cmd.kind = Kind::Remove;
    cmd.pending_target = true;
    cmd.target = Entity{p.index, 0};
    cmd.id = id;
    commands_.push_back(cmd);
}

bool CommandBuffer::empty() const noexcept
{
    return commands_.empty();
}

std::size_t CommandBuffer::size() const noexcept
{
    return commands_.size();
}

std::size_t CommandBuffer::pending_count() const noexcept
{
    return pending_count_;
}

void CommandBuffer::clear() noexcept
{
    for (Command& cmd : commands_)
    {
        destroy_payload(cmd);
    }
    commands_.clear();
    pending_count_ = 0;
}

std::vector<Entity> CommandBuffer::apply(World& world)
{
    // Pre-sized so a (caller-error) stale/foreign pending handle resolves to the default-constructed
    // INVALID entity — every World operation on it is a documented no-op, keeping apply deterministic.
    std::vector<Entity> created(pending_count_);

    for (Command& cmd : commands_)
    {
        // Resolve the target: a pending handle maps through `created` (its Create command always
        // precedes its uses — create() minted the handle at recording time).
        Entity target = cmd.target;
        if (cmd.pending_target && cmd.kind != Kind::Create)
        {
            target = cmd.target.index < created.size() ? created[cmd.target.index] : Entity{};
        }

        switch (cmd.kind)
        {
        case Kind::Create:
            created[cmd.target.index] = world.create();
            break;
        case Kind::Destroy:
            world.destroy(target);
            break;
        case Kind::Add:
            // payload == nullptr records a zero-initialized add (World::add_raw's nullptr path);
            // otherwise the owned block is consumed as moved-from and destroyed below.
            world.add_raw(target, cmd.id, cmd.ops, cmd.payload);
            destroy_payload(cmd);
            break;
        case Kind::Remove:
            world.remove_raw(target, cmd.id);
            break;
        }
    }

    commands_.clear();
    pending_count_ = 0;
    return created;
}

void CommandBuffer::record_add(Command cmd, const void* src)
{
    if (src != nullptr)
    {
        void* block = alloc_block(cmd.ops);
        // The const_cast mirrors World::add_raw: POD ops only memcpy-read `src`, and a concrete
        // move_construct means the caller opted into move semantics (documented on add_raw).
        relocate_into(cmd.ops, block, const_cast<void*>(src));
        cmd.payload = block;
    }
    commands_.push_back(cmd);
}

void CommandBuffer::destroy_payload(Command& cmd) noexcept
{
    if (cmd.payload == nullptr)
    {
        return;
    }
    if (cmd.ops.destroy != nullptr)
    {
        cmd.ops.destroy(cmd.payload);
    }
    free_block(cmd.payload, cmd.ops);
    cmd.payload = nullptr;
}

} // namespace context::kernel
