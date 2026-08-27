# d1 — Visual direction mockups

Live-HTML mockups porting the owner's shipped **monochrome-glow-ui** design system (design doc
[`06-theme-design-system.md`](../06-theme-design-system.md) §2) to the Context Editor, so the
owner can pick the visual direction at the d1 gate before task **e06** hardcodes token values.
Nothing here is a screenshot or a static image — every page is the real HTML/CSS/JS, themeable
and interactive, meant to be opened straight off disk.

## Start here

Open **[`index.html`](index.html)** by double-clicking it (no server, no build step) — or jump
straight to **[`flourishes.html`](flourishes.html)**, which now carries the one required decision
(the signature flourish) front and center with a live side-by-side comparison of six options.

If you'd rather jump straight to a surface:

| File | What it shows |
|---|---|
| [`index.html`](index.html) | Landing hub — decisions front and center, links to everything |
| [`flourishes.html`](flourishes.html) | **The signature-flourish gate** — six live, fully-executed treatments for the Play button (plus the retired original for comparison), each on a realistic play-bar strip, in both themes and reduced-motion |
| [`editor.html`](editor.html) | Full editor window: Dockview-style dock chrome (splits / tab strips / a floating torn-out group), Scene viewport (grid, gizmo, selection, camera widget), Inspector, Scene Tree, Problems, and the play bar — now with a live **Flourish** selector so you can try any of the six in the full editor chrome |
| [`welcome.html`](welcome.html) | Mini-welcome screen (D13): recent projects, open folder, new-from-template |
| [`viewport-palette.html`](viewport-palette.html) | The D12 "legal chroma exception" swatch/legend proposal: axis X/Y/Z, grid major/minor, selection outline, gizmo hover/active, camera widget |
| [`tokens-preview.html`](tokens-preview.html) | A visual specimen sheet — every color/type/shape/motion value rendered live, for fast QA of the port |
| [`TOKENS.md`](TOKENS.md) | The structured value sheet for e06 to consume — every value labeled GROUND TRUTH / PROPOSAL / OWNER PICK |

Every page carries the same **"Mockup controls"** overlay (bottom-right) — Dark/Light and a
reduced-motion simulator, plus a page-specific control where relevant (`editor.html` gets the new
**Flourish** selector) — so you can flip between states without leaving the page. It's clearly
boxed off and labeled "not part of the product UI."

## ✅ The signature-flourish decision — RESOLVED: #6 Pulse of Work (2026-07-19)

The first pass presented the signature flourish (design doc `06-theme-design-system.md` §2 / doc
`02-target-architecture.md` §7's open O1 question) as two aurora placement variants — Play button
only, or Play button + welcome CTA — both using the same rotating monochrome ink-alpha conic halo.
**The owner reviewed that pass and rejected both: the rotating halo itself "looks bad."** The
placement question (Play-only vs. Play+CTA) is now moot until a treatment is picked at all.

The gate is therefore reframed: **pick 1 of 6 new flourish treatments** on
**[`flourishes.html`](flourishes.html)** —

1. **Ink Bloom** — crisp monochrome glow (bloom + static ring + inner sheen, no rotation)
2. **Northern Veil** — a real, hushed northern-lights wash (the one option allowed to break monochrome)
3. **Signal Ring** — a resting ring that pings outward on a slow interval (crisp, never blurred)
4. **Living Line** — the button's own 1px border quietly breathes (no added element at all)
5. **Passing Light** — a soft diagonal sheen sweeps across the ink, then rests
6. **Pulse of Work** — the glow's color/rhythm mirrors the button's real state (idle/compiling/running)

Compare all six side by side on `flourishes.html`, each on a realistic play-bar strip, in both
themes and with a reduced-motion toggle. Then try your favorite live in the full editor chrome via
the new **Flourish** control in `editor.html`'s Mockup Controls panel — same six options, wired to
the real Play button, so you can judge it in situ before committing. The retired rotating halo is
still viewable (labeled "Original — rejected") on both pages for direct comparison, but it is not a
candidate. Whichever you pick, `data-flourish="<name>"` on the Play button is the one-line default
for e06 to carry into the real theme config — no rebuild.

Also worth 30 seconds: flip **Motion → Reduced** on `flourishes.html` or `editor.html`. That toggle
simulates `prefers-reduced-motion: reduce`, which is also honored for real for every treatment — if
your OS/browser already has reduced motion on, you're seeing the shipped fallback right now without
touching anything.

## Other things worth a look (not blocking, but cheap to weigh in on)

- [`viewport-palette.html`](viewport-palette.html) proposes concrete colors for the one other
  place chroma is allowed (D12) — axis colors follow the Unity/Unreal/Blender/Maya red/green/blue
  convention on purpose (see its "Rationale" section for why, and why selection/active-drag get
  their own distinct hues instead of reusing a status color).
- [`TOKENS.md`](TOKENS.md) §9 lists a short punch-list of values doc 06 names but doesn't fix
  (a Light-theme `muted2`, the spacing/density scale, the icon set) — none of these block e06,
  they're just flagged rather than silently invented. §1.5 now also lists the new Northern Veil
  color tokens, the only new token values this respin introduces.

## Recording the pick

Per the task's Definition of Done, the owner's decision (which flourish, plus any other O1-adjacent
calls) is recorded in the M9 ROADMAP progress log — that log entry, not this folder, is the
authoritative record once made. This folder then stays as e06's reference for the actual values
(`TOKENS.md` is written to be consumed directly, with everything provisional clearly marked).

## Notes on how this was built

- No build step, no framework, no external network requests — `shared/tokens.css` /
  `base.css` / `components.css` / `viewport.css` / `flourishes.css` / `main.js`, loaded via plain
  `<link>`/`<script>` tags (not ES modules — some browsers block module `import`/`fetch` on
  `file://`, classic scripts don't have that restriction).
  Icons are inline SVG `<symbol>` sprites (also `file://`-safe; no icon font, no CDN).
- `shared/aurora.css` (the rejected rotating-halo treatment) is kept in the repo and still governs
  `index.html` and `welcome.html`'s original variant-A/B exploration — those two pages were left
  exactly as first authored (per the task's "only redesign the flourish itself" scope) and are now
  historical context rather than a live decision surface; `flourishes.html` is the current gate.
  `editor.html`'s Play button has fully migrated off `.aurora`/`aurora.css` onto the new
  `.flourish-btn` / `data-flourish="<name>"` contract in `shared/flourishes.css`, which is what
  every one of the six new options (and the "original — rejected" reference reproduction) is built
  on. See `shared/flourishes.css`'s own header comment for the contract and the house rules it
  carries over from `aurora.css` (no `box-shadow`, ever — glows are pseudo-element gradients +
  `filter: blur()`).
- Best viewed at a reasonably wide window (1280px+) — like the real app, the editor mockup's dock
  layout isn't designed to survive a phone-width viewport.
- Theme/flourish/motion choices persist via `localStorage` where the browser allows it across local
  files; if a fresh tab doesn't remember your last pick, that's a `file://` storage-partitioning
  quirk in the browser, not a bug — the toggle is instant either way.
- Scope is deliberately narrow: no engine code, no `@context-engine/editor-tokens` package, no
  build tooling. `shared/tokens.css` says as much at its own top — it exists only to render these
  pages, not to be copied into the engine repo as source.
