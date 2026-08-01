// Per-viewport render targets with a REAL lifecycle -- create, resize, release (M9 e11b; design
// m9-editor D5). One entry owns the colour + depth attachments a viewport's render pass draws into,
// and a viewport keeps ONE entry for its whole life however often the panel is resized.
//
// WHY A SIBLING REGISTRY RATHER THAN EXTENDING DynamicTextureRegistry (the choice this task asks to
// justify). context/render/ui/dynamic_texture.h already keeps per-panel offscreen targets, so
// extending it looks like the smaller change. It is the wrong one, for three independent reasons and
// any single one of them settles it:
//
//   1. LAYERING, and this one is a hard build error rather than a preference. DynamicTextureRegistry
//      lives in context_render_ui, which links context_render. A viewport pass is context_render
//      code, so reaching that registry would make context_render depend on a library that links
//      AGAINST it. This is the same constraint that forced lit_math.h to be PROMOTED into
//      context/render/math.h for e11a -- see that header's own note.
//   2. ITS CONTRACT IS PERSISTENCE, and M7 a9/a10 rely on it. Its header states that each entry is
//      allocated ONCE and never reallocates mid-run, and its handles are stored in the L-39 snapshot
//      (render_world.h UiPanel::texture). Adding release+reuse there would let a snapshot handle
//      captured last frame silently resolve to a DIFFERENT panel's texture. Handle staleness is
//      solved here instead (see the generation bits below), which is a change that registry must not
//      take on behalf of its existing callers.
//   3. DIFFERENT RESOURCE SHAPE. A panel target is one sampled RGBA8 colour texture. A viewport
//      target is a colour texture PLUS a Depth32Float attachment, because a viewport draws 3D scene
//      geometry that has to depth-sort. Those allocate, resize and release together.
//
// What this registry adds over the append-only sibling, and what a per-viewport RT actually needs:
//
//   * RELEASE that frees the slot AND destroys the textures, so a closed viewport gives its GPU
//     memory back. The sibling's vector only grows.
//   * RESIZE IN PLACE, keeping the handle: the panel the caller already holds a handle for changes
//     size every drag frame. On the sibling's API the only way to change size is another
//     create_panel_target(), which LEAKS a handle and its texture per resize -- the defect this file
//     exists to remove.
//   * GENERATION-TAGGED HANDLES, which is what makes slot reuse safe. A handle packs
//     {generation:16 | slot+1:16}; releasing a slot bumps its generation, so a handle captured
//     before the release is REFUSED after the slot is reused rather than aliasing onto the new
//     target. That refusal is the property the sibling bought by never reusing at all.
//
// GPU-free of any concrete backend (it drives only rhi.h), so it builds and is unit-tested under
// every toolchain, exactly like the rest of context_render.

#pragma once

#include "context/render/rhi.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace context::render
{

// A handle into a ViewportTargetRegistry: {generation:16 | slot+1:16}. 0 is the reserved "no target"
// handle, matching the "0 = slot unused" convention render_world.h's texture handles use.
using ViewportTargetId = std::uint32_t;
inline constexpr ViewportTargetId kInvalidViewportTarget = 0;

inline constexpr std::uint32_t kViewportTargetSlotBits = 16u;
inline constexpr std::uint32_t kViewportTargetSlotMask = 0xFFFFu;

// The 1-based slot a handle names (0 for the invalid handle). Two handles sharing a slot name the
// same registry entry at different points in its life.
[[nodiscard]] constexpr std::uint32_t viewport_target_slot(ViewportTargetId id)
{
    return id & kViewportTargetSlotMask;
}

// How many times the handle's slot had been released when the handle was minted. This is the half
// that makes a stale handle detectable.
[[nodiscard]] constexpr std::uint32_t viewport_target_generation(ViewportTargetId id)
{
    return id >> kViewportTargetSlotBits;
}

// The most slots one registry can hold at once (the slot field is 16 bits and 0 is reserved). Far
// past D5's "N viewports bounded by GPU memory" -- memory runs out first, by orders of magnitude.
inline constexpr std::size_t kMaxViewportTargets = kViewportTargetSlotMask;

class ViewportTargetRegistry
{
public:
    // `device` must outlive the registry (targets are created on it and it is never owned).
    explicit ViewportTargetRegistry(IDevice& device);

    // Not copyable: the entries own device resources through unique_ptr, and a copy would hand two
    // registries the same viewport map. (FakeDevice takes the same position for the same reason.)
    ViewportTargetRegistry(const ViewportTargetRegistry&) = delete;
    ViewportTargetRegistry& operator=(const ViewportTargetRegistry&) = delete;

    // Allocate a viewport target of `size` (colour RGBA8Unorm + depth Depth32Float) and return its
    // handle. Reuses a released slot when one is free -- that reuse is the whole point -- with a
    // fresh generation, so the returned handle never equals a released one.
    //
    // Returns kInvalidViewportTarget for a DEGENERATE (zero-extent) size, and when the slot space or
    // the device is exhausted. A zero extent is a real transient state (a panel mid-resize, a
    // minimized window), so it is refused rather than allocated -- the same position ISwapchain::
    // resize takes in rhi.h.
    ViewportTargetId create(Extent2D size);

    // Destroy `id`'s textures and free its slot for reuse. Returns false for an unknown or
    // already-released handle, and in that case changes nothing -- releasing twice must not push one
    // slot onto the free list twice, which would hand the same entry out to two live callers.
    bool release(ViewportTargetId id);

    // Reallocate `id`'s attachments at `size`, KEEPING the handle (the caller's viewport did not
    // change identity, only extent). Returns false for an unknown handle.
    //
    // Two deliberate no-ops that both return true: `size` already matches (so a per-frame resize call
    // does not churn the device every frame), and a DEGENERATE `size` (the target keeps its last
    // valid configuration rather than being torn down, mirroring ISwapchain::resize).
    bool resize(ViewportTargetId id, Extent2D size);

    // The target `viewport_id` should render into, at `size` -- the per-frame call a viewport panel
    // makes. Creates the target on first sight, RESIZES it in place afterwards, and returns the same
    // handle throughout. This is the entry point whose absence made a per-viewport RT leak a handle
    // per resize on the append-only sibling registry.
    //
    // A degenerate `size` never allocates: it returns the viewport's existing handle unchanged, or
    // kInvalidViewportTarget when it has none yet.
    ViewportTargetId acquire_for(std::uint32_t viewport_id, Extent2D size);

    // Release `viewport_id`'s target (a closed viewport / a torn-out window) and forget the mapping.
    // Returns false when that viewport held no target.
    bool release_for(std::uint32_t viewport_id);

    // The handle `viewport_id` currently holds, or kInvalidViewportTarget.
    [[nodiscard]] ViewportTargetId target_for(std::uint32_t viewport_id) const;

    // Whether `id` names a LIVE entry. False for the invalid handle, an out-of-range slot, a
    // released slot, and a STALE handle whose slot has since been reused.
    [[nodiscard]] bool contains(ViewportTargetId id) const noexcept;

    // The colour attachment / its view, or nullptr when `contains(id)` is false. The colour texture
    // carries render_attachment (the pass draws into it), copy_src (golden/readback proofs) and
    // texture_binding (the compositor samples it onto the viewport layer -- e11e).
    [[nodiscard]] ITexture* get_color(ViewportTargetId id) const;
    [[nodiscard]] ITextureView* color_view(ViewportTargetId id) const;

    // The depth attachment / its view, or nullptr when `contains(id)` is false.
    [[nodiscard]] ITexture* get_depth(ViewportTargetId id) const;
    [[nodiscard]] ITextureView* depth_view(ViewportTargetId id) const;

    // The pixel size `id` currently holds ({0,0} when `contains(id)` is false).
    [[nodiscard]] Extent2D size_of(ViewportTargetId id) const;

    // How many targets are LIVE right now. Unlike the sibling registry's count() this can go DOWN.
    [[nodiscard]] std::size_t live_targets() const noexcept { return live_; }

    // How many slots exist (live + released-and-reusable). live_targets() == slot_count() means
    // nothing has been released; the gap is the free list.
    [[nodiscard]] std::size_t slot_count() const noexcept { return entries_.size(); }

    // CUMULATIVE device texture allocations (2 per create + 2 per real resize). Pairs with
    // live_targets(): together they distinguish "reallocated and freed the old one" from both a leak
    // and a silent no-op, which neither number can do alone.
    [[nodiscard]] std::size_t textures_created() const noexcept { return textures_created_; }

private:
    struct Entry
    {
        // DECLARATION ORDER IS LOAD-BEARING: members destroy in reverse, and a view holds a
        // non-owning pointer to its texture, so each view must be declared AFTER the texture it
        // views. destroy_attachments() resets them in that same order by hand.
        std::unique_ptr<ITexture> color;
        std::unique_ptr<ITextureView> color_view;
        std::unique_ptr<ITexture> depth;
        std::unique_ptr<ITextureView> depth_view;
        Extent2D size{};
        std::uint32_t generation = 0;
        bool live = false;
    };

    // The entry `id` names, or nullptr when the handle is invalid / out of range / released / STALE
    // (its generation no longer matches the slot's).
    [[nodiscard]] const Entry* resolve(ViewportTargetId id) const;
    [[nodiscard]] Entry* resolve(ViewportTargetId id);

    // Allocate colour + depth at `size` into `entry`, replacing whatever it held. Returns false when
    // the device refuses, leaving the entry with no attachments.
    bool allocate_attachments(Entry& entry, Extent2D size);
    static void destroy_attachments(Entry& entry);

    struct ViewportBinding
    {
        std::uint32_t viewport_id = 0;
        ViewportTargetId target = kInvalidViewportTarget;
    };

    IDevice& device_;
    std::vector<Entry> entries_;         // slot == index + 1; 0 stays the invalid handle
    std::vector<std::uint32_t> free_;    // released slot INDICES awaiting reuse
    // N viewports is small (D5's "N simultaneous viewports" is a handful in practice), so a flat
    // vector beats a hash map here and keeps iteration order deterministic.
    std::vector<ViewportBinding> bindings_;
    std::size_t live_ = 0;
    std::size_t textures_created_ = 0;
};

} // namespace context::render
