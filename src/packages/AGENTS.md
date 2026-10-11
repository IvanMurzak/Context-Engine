# src/packages/ — first-party feature packages

Every engine feature is a package over the microkernel; `README.md` here lists them, and each
package has its own `README.md` with seam/API detail.

- Sim core: `simmath/` (Q16 fixed-point math every sim package shares), `spatial/` (broad-phase AABB tree)
- Simulation: `physics3d/`, `physics2d/`, `particles/`, `animation/`, `spline/`
- Observers / front-ends: `audio/` (pure presentation observer, vendored miniaudio), `input/` (action maps feeding the one `InputState` sink in `src/runtime/session/`)
- `ui/` — runtime UI (`ui-*` tests); `ui/text/` is `context_ui_text` (FreeType + HarfBuzz + SheenBidi + libunibreak)

## Package rules

- A package is a STATIC library `context_<name>` in `src/packages/<name>/`, composing on
  `context_kernel` (and `context_session` for sim components). **The kernel never links back.**
- Deterministic sim state is integer/fixed-point only (`context_simmath` Q16) and folds into the
  state hash; cosmetic/presentation state is a float observer that folds into NO hash.
- `ui/text/` links its text deps PRIVATE (public headers stay dependency-free) and fetches FreeType +
  HarfBuzz from source at configure time; that fetch sits in nearly every CI leg's closure, so its
  pins must keep verified mirrors (`tools/AGENTS.md`).
