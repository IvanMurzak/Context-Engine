// Composite-time UI math (M7 a6, R-UI-005 composited_transforms). The pure, GPU-free arithmetic the UI
// backend applies at COMPOSITE time — transforms and opacity fold into the emitted quad without ever
// forcing a relayout (the layout `bounds` are the input, never mutated). Kept as free functions over the
// a1 POD types so the fold is unit-tested exactly (the fake-backend "composite math" ctest) and shared
// by the extract (snapshot.cpp) and any future presentation path.

#pragma once

#include "context/packages/ui/ui_node.h"

namespace context::render::ui
{

// Apply a node's presentation transform to its computed bounds at composite time (no relayout). Scale
// is about the rect's CENTRE (the natural HUD/widget pivot) and translate shifts the centre; the
// resulting rect stays axis-aligned. Rotation is deferred at T1 (it would break the axis-aligned quad
// path + the analytic golden) and is intentionally ignored here — `transform.rotation` is carried on the
// node for a later task. A non-positive scale collapses the rect to empty (nothing to draw).
[[nodiscard]] packages::ui::Rect apply_transform(const packages::ui::Rect& bounds,
                                                 const packages::ui::Transform& transform) noexcept;

// Compose opacity DOWN the tree: the effective opacity of a node is its ancestors' product times its own
// (each a presentation float in [0,1]). Clamped to [0,1]. This is what lets a group fade its whole
// subtree with a single style change and no relayout.
[[nodiscard]] float effective_opacity(float ancestor_opacity, float node_opacity) noexcept;

// Fold `src` at `opacity` OVER an opaque `backdrop` (standard source-over with a scaled source alpha),
// returning an OPAQUE color — the composite-time opacity resolution the T1 backend uses where a backdrop
// is known (no RHI blend state yet, so a translucent quad is pre-resolved against what sits behind it).
// `opacity` scales `src`'s own alpha; the result's alpha is always 255. src alpha 255 + opacity 1 ⇒ src
// unchanged; opacity 0 (or src alpha 0) ⇒ backdrop unchanged.
[[nodiscard]] packages::ui::Color blend_over(const packages::ui::Color& backdrop,
                                             const packages::ui::Color& src, float opacity) noexcept;

} // namespace context::render::ui
