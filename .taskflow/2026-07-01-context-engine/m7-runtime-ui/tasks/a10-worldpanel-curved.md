---
id: a10-worldpanel-curved
title: Curved-surface world-space UI — mesh UV mapping + raycast→UV→events (owner ruling d)
group: A
sequence: 10
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [T8-split, ruling-d, L-16]
---
## Goal
Owner ruling (d): world-space RTT includes CURVED surfaces in M7. Extend a9's panel binding from
flat quads to arbitrary UV-mapped meshes (the L-16 raycast→UV→events model, non-XR half): the
panel texture binds to a mesh's UV set; pointer interaction raycasts the mesh, maps hit → UV →
panel-space coordinates → the a2 hit-test path. OpenXR/stereo/XR-input stay v2.
**GREENFIELD warning (verified against the repo):** NO raycast API exists anywhere (spatial
exposes only `query_aabb`/`query_radius` — broad-phase), physics3d has sphere/box colliders
only, and there is NO runtime mesh/UV data seam (`Renderable.mesh_id` is an opaque "later wave"
handle; lit proofs bake geometry into WGSL). This task BUILDS: ray traversal (spatial used only
for broad-phase candidate pruning), ray-vs-triangle + UV interpolation, and a minimal mesh+UV
data seam for panel meshes. Keep the mesh seam panel-scoped — do not accrete the M8 asset-mesh
registry.

## Scope & seams
`src/render/ui/` (mesh binding + the panel-mesh data seam), `src/packages/ui/` (panel-space
pointer mapping + ray math), `src/packages/spatial/` consumed for broad-phase pruning only (no
kernel changes). Golden scene extension or a dedicated `ui-curvedpanel` golden (decide in-task;
if new: native-blocking per the a9 precedent).

## Definition of Done
- [ ] UV-mapping unit tests: hit → UV → panel coords on a curved mesh (cylinder-class), edge
      wrap/clamp cases.
- [ ] Interaction ctest: click on the curved panel routes through the SAME a3 capture path.
- [ ] Golden (new or extended) SSIM-gated native + web; reviewed baselines.
