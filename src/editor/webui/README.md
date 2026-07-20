# `src/editor/webui/` — the editor-core web workspace

The M9 editor's web layer: an npm workspace bundled at **build time** into static assets the native
Shell serves to CEF. Design authority: `04-editor-web-app-docking-panels.md` §1–2,
`05-contract-events-commands.md` §3, `08-security.md` §3.

## The one rule

**No Node at runtime, and no `npm install` — ever.** Every third-party input arrives through a
SHA-pinned, fail-closed fetch channel under `tools/`, and CMake drives the build with pinned native
binaries. `core/package.json` is a **declaration** (so the deny-by-default license gate sees the
dependency), *not* an install manifest — nothing reads it to resolve packages.

## Layout

| Path | What it is |
|---|---|
| `core/` | `@context-engine/editor-core` — the app package |
| `core/src/index.ts` | The bundle entry point: re-exports the whole surface, then boots the bridge |
| `core/src/info.ts` | The contract-surface projection (was `index.ts` through e05b) |
| `core/src/bridge.ts` | The privileged Shell bridge, JS side (e05c) |
| `core/src/boot.ts` | The boot handshake the live CEF smoke asserts natively (e05c) |
| `core/src/generated/client-schema.ts` | **Generated** client typings — never hand-edit |
| `core/tsconfig.json` | The typecheck config (strict; `--noEmit`) |
| `app/index.html` | The document served at `context-editor://app/index.html` (e05c) |
| `app/app.css` | The served stylesheet — a FILE, not a `<style>` block (the CSP blocks inline) |
| `CMakeLists.txt` | The build wiring: fetch → typecheck → bundle → stage |

## The three pinned tools

| Tool | Pin manifest | Role |
|---|---|---|
| esbuild | `tools/ts-toolchain.json` | Bundles the workspace (shared with `src/runtime/ts`) |
| tsgo | `tools/tsc-toolchain.json` | Semantic typecheck — esbuild strips types without checking them |
| dockview-core | `tools/dockview-toolchain.json` | The docking engine (D2) |

> ⚠ **dockview-core is owner-consent-gated and version-pinned at `7.0.2`** (`08` §3, ratified at
> spike s1). Bumping past 7.0.2, **or adding any further `dockview-*` package**, re-triggers the
> standing consent gate. It is not blanket Dockview approval — do not bump it as routine
> maintenance.

## Build

Nothing separate to run — `context_editor_webui` is an `ALL` target, so the normal gate builds it:

```sh
cmake -S src --preset dev && cd src && cmake --build --preset dev && ctest --preset dev -R '^webui-'
```

Outputs land in `<build>/editor-webui/app/`: `editor-core.js` plus the staged, verified
`dockview-core.min.js` + `dockview.css`, plus the served document set `index.html` + `app.css`.
That directory IS the asset root the Shell serves over `context-editor://app/` — it is published to
the Shell as the `CONTEXT_WEBUI_ASSET_DIR` cache entry.

**A type error is a build failure**, not a test failure: the bundle's custom command depends on the
typecheck stamp, so a bad type stops the build before esbuild ever runs.

### Ordering constraint (load-bearing)

`add_subdirectory(editor/webui)` **must** follow `add_subdirectory(runtime/ts)` — which stages the
pinned esbuild/tsgo and publishes `CONTEXT_ESBUILD_BIN` / `CONTEXT_TSGO_BIN` — and
`add_subdirectory(editor/client)`, which publishes `CONTEXT_CLIENT_SCHEMA_GENERATED`. `src/editor/`
is otherwise configured long before `src/runtime/ts`, which is exactly why this directory is wired
in its own stanza next to `runtime/ts` rather than in the editor cluster. All three are
`CACHE INTERNAL`; `CMakeLists.txt` `FATAL_ERROR`s with the reason if any is empty, so a future
reorder fails loudly at configure instead of silently invoking an empty program.

## Generated client typings — do not hand-write

Hand-written client typings are **prohibited** (design 05 §3, the R-CLI-009 spirit): the contract
registry is the single source of truth. The projection chain is:

```
contract::Registry::describe
  → context_client_schema_gen          (e02, a build-time C++ tool)
  → context-client-schema.json         (build artifact + committed copy)
  → tools/gen_client_typings.py
  → core/src/generated/client-schema.ts   (committed)
```

Regenerate after any registry change:

```sh
python3 tools/gen_client_typings.py
```

The committed copy exists so the workspace type-checks as a self-contained unit (a fresh checkout,
an IDE, and the typecheck build step all read it without first building C++).

The drift comparison is **byte-exact**, so the generator emits pure ASCII with LF endings — and the
repo's `.gitattributes` (`* text=auto eol=lf`) keeps LF through checkout on Windows too. Both halves
are load-bearing: a CRLF-normalising checkout would red the drift gate on the Windows leg only.

**Three gates keep the typings honest, with no gap between them:**

| Gate | Where | Compares |
|---|---|---|
| `client-test_schema` (e02) | build matrix | committed schema ↔ live registry |
| `test_gen_client_typings.py` | `python-tests` (~10s) | committed typings ↔ committed schema |
| `webui-client-typings-drift` | build matrix | committed typings ↔ **build-generated** schema |

## Tests

`webui-*` is a plain package test family — not a milestone gate — so it is not in the `build` job's
gate-exclusion regex and runs automatically in the general ctest step on all three OS legs. **No
`ci.yml` edit is needed to add a `webui-*` test.**

- `webui-assets` — the bundle is real, non-empty, exports the entry symbols, and references no Node
  builtin; every staged dockview file is re-hashed against its pin (verify-**at**-use, one step
  beyond fetch-time verification).
- `webui-scheme-contract` — three facts that live in two languages and would otherwise drift
  silently: the served document carries no inline script, inline style or inline event handler (the
  CSP would BLOCK them, so the failure mode is a blank editor rather than a build error); the scheme
  vocabulary the **built** bundle compiled in is byte-identical to the C++ constants the Shell serves
  and routes with (scheme, origin, endpoint, query + cancel function names); and no `file://` URL
  reached the asset set (a DoD line). Checked against the BUILT asset, so it sees what the renderer
  will actually get, and fails closed when the bundle is missing.
- `webui-panel-contract` — the e05d1 gate over the WIDEST cross-language seam in the editor: the six
  `panel.*` method names, the two D6 state member names and the four gesture verbs, each read out of
  the BUILT bundle and compared to the C++ constants. A rename on either side unbinds the panel layer
  SILENTLY (the editor comes up with no panels and nothing reports an error), which is the same
  no-local-signal failure class e05c's `nosniff` break taught. It also asserts the two facts that
  would make the panel layer non-functional in a way no unit test can see: that `index.html` LOADS the
  pinned docking engine (PanelHost reads it off the UMD global that script publishes) rather than
  bundling it — bundling would void `webui-assets`' SHA re-hash of the shipped artifact — and that
  editor-core's declared dependency set is still exactly the owner-ratified, version-pinned s1 set.
- `webui-client-typings-drift` — re-projects the build-generated schema and refuses any byte
  difference.

Python-side unit tests live in `tools/tests/test_gen_client_typings.py`,
`tools/tests/test_check_webui_assets.py`, and `tools/tests/test_fetch_dockview.py`.

## Scope boundary (after e05d1)

e05a made this the **toolchain substrate**; e05c added the **served document set** (`app/`) and the
**JS half of the privileged bridge**; e05d1 added the **app**: `PanelHost` over Dockview and the
hydration runtime. The docking engine is now consumed through its API (loaded as the pinned UMD
script by `index.html`, typed structurally in `dockview.ts`), and every Shell-hostable panel mounts
and hydrates. Deliberately **not** here:

- ~~the `context-editor://` app scheme and the privileged IPC bridge~~ → ✅ **e05c**. The scheme's
  NATIVE half (registration, resource handler, message router) lives in `src/editor/shell/`, not
  here; this directory owns only what is served and what runs in the renderer.
- ~~`PanelHost` (the dockview wrapper that owns panel lifecycle, design 04 §2)~~ → ✅ **e05d1**
- ~~the panel hydration runtime~~ → ✅ **e05d1**
- layout persistence + region maps → **e05d2**. `PanelHost.captureLayout()` / `restoreLayout()` exist
  because they are geometry operations this layer owns, but NOTHING here writes them anywhere: the
  Shell is the single writer of `.editor/editor-state.json` (C-F3), and publishing layout to it over
  the bridge is e05d2's. A direct write from editor-core is a defect even if it works.
- live Scene tree + Inspector → **e05d3**. Both are LISTED by `panel.list` and report
  `hosted: false` today, because their C++ libraries link `context_compose` and the D10 shell-boundary
  gate forbids it. Nothing in this directory special-cases that — the runtime renders whatever the
  roster says is hosted, which is exactly what lets e05d3 land them with no change here.
- OS-window tear-out → **e10**. Dockview's popout API is deliberately UNUSED (B-F2): v7 rejects
  non-http(s) popout URLs, and its popout is opener-owned DOM transfer, which cannot produce the
  independent per-window editor-core instances design 04 §1 requires.

Grow the app along those seams. `index.ts` is the entry (re-export + boot) and should stay that
thin — put new surface in its own module and re-export it.
