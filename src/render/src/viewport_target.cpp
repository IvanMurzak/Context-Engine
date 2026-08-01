// Per-viewport render targets with a real create/resize/release lifecycle -- see
// context/render/viewport_target.h for the design and for why this is a sibling of
// context_render_ui's DynamicTextureRegistry rather than an extension of it.

#include "context/render/viewport_target.h"

#include <utility>

namespace context::render
{
namespace
{

// The handle for slot INDEX `index` at `generation`. The stored slot field is index + 1 so that a
// zero handle can stay reserved for "no target".
[[nodiscard]] ViewportTargetId make_handle(std::size_t index, std::uint32_t generation)
{
    const std::uint32_t slot = static_cast<std::uint32_t>(index) + 1u;
    // Masked to the 16 bits a handle can carry. ⚠ This mask is BELT-AND-BRACES, and saying so is
    // load-bearing for anyone who later "simplifies" one of the two: the mask that actually bounds
    // the counter is the one in release(), which keeps entry.generation itself inside 16 bits. With
    // that one in place this mask can never fire, which is exactly why a plant that deletes ONLY
    // this one comes back GREEN -- the claim is still true, just defended twice. Delete release()'s
    // mask and the defect returns in full: the stored generation runs past 65535 while the handle
    // can only report the low 16 bits, so resolve() compares 65536 against 0, refuses every handle
    // the slot ever mints again, and leaves it permanently dead with live_ over-counted.
    return ((generation & kViewportTargetSlotMask) << kViewportTargetSlotBits) | slot;
}

} // namespace

ViewportTargetRegistry::ViewportTargetRegistry(IDevice& device) : device_(device) {}

void ViewportTargetRegistry::destroy_attachments(Entry& entry)
{
    // Views first, in both pairs: a view holds a non-owning pointer to its texture, so dropping the
    // texture while its view is still alive would leave a dangling one for however long the entry
    // lives. Same reason the Entry members are declared in this order.
    entry.color_view.reset();
    entry.color.reset();
    entry.depth_view.reset();
    entry.depth.reset();
    entry.size = Extent2D{};
}

bool ViewportTargetRegistry::allocate_attachments(Entry& entry, Extent2D size)
{
    TextureDesc color_desc;
    color_desc.size = size;
    color_desc.format = TextureFormat::RGBA8Unorm;
    color_desc.render_attachment = true; // the viewport pass draws into it
    color_desc.copy_src = true;          // read back by an offscreen / golden proof
    color_desc.texture_binding = true;   // sampled onto the compositor's viewport layer (e11e)

    TextureDesc depth_desc;
    depth_desc.size = size;
    depth_desc.format = TextureFormat::Depth32Float;
    depth_desc.render_attachment = true; // 3D scene geometry has to depth-sort

    // BUILD INTO LOCALS AND COMMIT ONLY ON FULL SUCCESS. Every early return below leaves `entry`
    // byte-untouched, so a device that refuses mid-way leaves the caller's target exactly as it was
    // rather than half-built. Freeing first and allocating second would instead leave a LIVE entry
    // whose attachments are all null -- contains(id) true while color_view(id) is nullptr, which is
    // the one state the accessors' contract does not describe and which a caller doing
    // `render_viewport_view(..., *registry.color_view(h), ...)` dereferences. Keeping the old target
    // on failure is also the recovery posture resize() already takes for a degenerate extent.
    // The cost is that a resize holds both pairs briefly; that is the standard trade and it is
    // bounded by one target.
    std::unique_ptr<ITexture> color = device_.create_texture(color_desc);
    std::unique_ptr<ITexture> depth = device_.create_texture(depth_desc);
    if (color == nullptr || depth == nullptr)
    {
        // Fail CLOSED and leave nothing half-built: an entry holding a colour target but no depth
        // would render the scene in submission order and look almost right, which is worse than a
        // reported failure.
        return false;
    }

    std::unique_ptr<ITextureView> color_view = color->create_view();
    std::unique_ptr<ITextureView> depth_view = depth->create_view();
    if (color_view == nullptr || depth_view == nullptr)
    {
        return false;
    }
    // Counted only once the pair is COMPLETE, so the counter cannot over-report a failed round.
    textures_created_ += 2u;

    // Now that the new pair is in hand, drop the old one. This call is here for ORDERING, not for
    // freeing: the unique_ptr assignments below already destroy whatever the entry held, but they do
    // it in the wrong order -- `entry.color = std::move(color)` frees the old colour texture while
    // `entry.color_view` still points at it, leaving a dangling view until the next statement
    // replaces it. Nothing dereferences it in between, so no test can observe this and none pretends
    // to; do not "simplify" the call away on the strength of a green suite.
    destroy_attachments(entry);

    entry.color = std::move(color);
    entry.color_view = std::move(color_view);
    entry.depth = std::move(depth);
    entry.depth_view = std::move(depth_view);
    entry.size = size;
    return true;
}

ViewportTargetId ViewportTargetRegistry::create(Extent2D size)
{
    if (is_empty(size))
    {
        return kInvalidViewportTarget;
    }

    std::size_t index = 0;
    if (!free_.empty())
    {
        index = free_.back();
        free_.pop_back();
    }
    else
    {
        if (entries_.size() >= kMaxViewportTargets)
        {
            return kInvalidViewportTarget;
        }
        entries_.emplace_back();
        index = entries_.size() - 1u;
    }

    Entry& entry = entries_[index];
    if (!allocate_attachments(entry, size))
    {
        // The device refused. Hand the slot straight back so a transient allocation failure does not
        // permanently burn a slot.
        entry.live = false;
        free_.push_back(static_cast<std::uint32_t>(index));
        return kInvalidViewportTarget;
    }
    entry.live = true;
    ++live_;
    return make_handle(index, entry.generation);
}

bool ViewportTargetRegistry::release(ViewportTargetId id)
{
    Entry* entry = resolve(id);
    if (entry == nullptr)
    {
        // Unknown, already released, or stale. Returning false WITHOUT touching the free list is the
        // point: a second release of the same handle would otherwise queue one slot twice and hand
        // two live callers the same entry.
        return false;
    }

    destroy_attachments(*entry);
    entry->live = false;
    // Bump BEFORE the slot can be reused, so every handle minted from it afterwards differs from the
    // one just released and the old handle stops resolving. Kept inside the 16 bits a handle carries
    // so the stored generation and the one make_handle() encodes can never disagree.
    entry->generation = (entry->generation + 1u) & kViewportTargetSlotMask;
    --live_;
    free_.push_back(viewport_target_slot(id) - 1u);

    // A viewport still pointing at this target must not keep a handle that no longer resolves.
    for (std::size_t i = 0; i < bindings_.size(); ++i)
    {
        if (bindings_[i].target == id)
        {
            bindings_.erase(bindings_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    return true;
}

bool ViewportTargetRegistry::resize(ViewportTargetId id, Extent2D size)
{
    Entry* entry = resolve(id);
    if (entry == nullptr)
    {
        return false;
    }
    if (is_empty(size))
    {
        // A minimized window / a panel mid-drag keeps its last valid configuration rather than being
        // torn down -- the position ISwapchain::resize takes in rhi.h.
        return true;
    }
    if (entry->size.width == size.width && entry->size.height == size.height)
    {
        // Already the right size. A viewport calls this every frame, so reallocating here would churn
        // two device textures per frame for nothing.
        return true;
    }
    return allocate_attachments(*entry, size);
}

ViewportTargetId ViewportTargetRegistry::acquire_for(std::uint32_t viewport_id, Extent2D size)
{
    for (ViewportBinding& binding : bindings_)
    {
        if (binding.viewport_id != viewport_id)
        {
            continue;
        }
        if (is_empty(size))
        {
            return binding.target; // keep the last valid configuration
        }
        if (!resize(binding.target, size))
        {
            return kInvalidViewportTarget;
        }
        // The handle is deliberately UNCHANGED across a resize: the viewport did not change
        // identity, only extent. Returning a new handle here is precisely the per-resize leak this
        // registry exists to remove.
        return binding.target;
    }

    if (is_empty(size))
    {
        return kInvalidViewportTarget; // nothing bound yet and nothing sensible to allocate
    }
    const ViewportTargetId target = create(size);
    if (target == kInvalidViewportTarget)
    {
        return kInvalidViewportTarget;
    }
    ViewportBinding binding;
    binding.viewport_id = viewport_id;
    binding.target = target;
    bindings_.push_back(binding);
    return target;
}

bool ViewportTargetRegistry::release_for(std::uint32_t viewport_id)
{
    for (std::size_t i = 0; i < bindings_.size(); ++i)
    {
        if (bindings_[i].viewport_id != viewport_id)
        {
            continue;
        }
        const ViewportTargetId target = bindings_[i].target;
        // release() erases the binding itself (it has to, for a handle released directly), so the
        // erase is not repeated here.
        return release(target);
    }
    return false;
}

ViewportTargetId ViewportTargetRegistry::target_for(std::uint32_t viewport_id) const
{
    for (const ViewportBinding& binding : bindings_)
    {
        if (binding.viewport_id == viewport_id)
        {
            return binding.target;
        }
    }
    return kInvalidViewportTarget;
}

const ViewportTargetRegistry::Entry* ViewportTargetRegistry::resolve(ViewportTargetId id) const
{
    const std::uint32_t slot = viewport_target_slot(id);
    if (slot == 0u || slot > entries_.size())
    {
        return nullptr;
    }
    const Entry& entry = entries_[slot - 1u];
    if (!entry.live)
    {
        return nullptr;
    }
    if (entry.generation != viewport_target_generation(id))
    {
        return nullptr; // a handle from before this slot was recycled
    }
    return &entry;
}

ViewportTargetRegistry::Entry* ViewportTargetRegistry::resolve(ViewportTargetId id)
{
    const ViewportTargetRegistry* self = this;
    return const_cast<Entry*>(self->resolve(id));
}

bool ViewportTargetRegistry::contains(ViewportTargetId id) const noexcept
{
    return resolve(id) != nullptr;
}

ITexture* ViewportTargetRegistry::get_color(ViewportTargetId id) const
{
    const Entry* entry = resolve(id);
    return entry == nullptr ? nullptr : entry->color.get();
}

ITextureView* ViewportTargetRegistry::color_view(ViewportTargetId id) const
{
    const Entry* entry = resolve(id);
    return entry == nullptr ? nullptr : entry->color_view.get();
}

ITexture* ViewportTargetRegistry::get_depth(ViewportTargetId id) const
{
    const Entry* entry = resolve(id);
    return entry == nullptr ? nullptr : entry->depth.get();
}

ITextureView* ViewportTargetRegistry::depth_view(ViewportTargetId id) const
{
    const Entry* entry = resolve(id);
    return entry == nullptr ? nullptr : entry->depth_view.get();
}

Extent2D ViewportTargetRegistry::size_of(ViewportTargetId id) const
{
    const Entry* entry = resolve(id);
    return entry == nullptr ? Extent2D{} : entry->size;
}

} // namespace context::render
