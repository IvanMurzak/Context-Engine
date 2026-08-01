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
    return (generation << kViewportTargetSlotBits) | slot;
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
    // This is here for ORDERING, not for freeing, and the distinction is measured rather than
    // assumed: a plant that deleted this line stayed GREEN across the whole suite, because the
    // unique_ptr ASSIGNMENTS at the bottom of this function already destroy whatever the entry held.
    // What they do not do is destroy it in the right ORDER -- `entry.color = std::move(color)` frees
    // the old colour texture while `entry.color_view` still points at it, leaving a dangling view
    // until the next statement replaces it. Nothing dereferences it in between, so no test can catch
    // this and none pretends to; do not "simplify" the call away on the strength of a green suite.
    destroy_attachments(entry);

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

    std::unique_ptr<ITexture> color = device_.create_texture(color_desc);
    std::unique_ptr<ITexture> depth = device_.create_texture(depth_desc);
    if (color == nullptr || depth == nullptr)
    {
        // Fail CLOSED and leave nothing half-built: an entry holding a colour target but no depth
        // would render the scene in submission order and look almost right, which is worse than a
        // reported failure.
        return false;
    }
    textures_created_ += 2u;

    std::unique_ptr<ITextureView> color_view = color->create_view();
    std::unique_ptr<ITextureView> depth_view = depth->create_view();
    if (color_view == nullptr || depth_view == nullptr)
    {
        return false;
    }

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
    // one just released and the old handle stops resolving.
    ++entry->generation;
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
