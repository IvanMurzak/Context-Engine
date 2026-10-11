# src/editor/ — EditorKernel and the editor application

The headless, file-authoritative project "language server" plus the GUI tiers that are clients of it.
Most modules carry a `README.md` with their full seam/API detail.

- File-truth core: `serializer/` (canonical JSON), `filesync/`, `derivation/`, `editorkernel/` (composition), `merge/`, `migrate/`, `compose/`, `schema/`, `component/`, `kinds/`, `assetdb/`, `import/`, `pkg/`, `system/`, `schedule/`, `tilemap/`
- Build side: `build/` (build orchestration), `pack/` (content-pack writer)
- Contract surface: `contract/` (the one registry CLI/RPC/MCP project), `bridge/`
- `client/` — the installed/exported `context_client` SDK (+ `consumer/`, the out-of-tree build test) — `docs/client-sdk.md`
- `gui/` — headless panel/UI-logic tiers + the CEF host (`gui/host/`); `cef/` — the CEF fetch/link substrate
- `shell/` — the native Shell `context_editor` (windows, compositor, input) — `docs/shell.md`
- `webui/` — the editor-core web workspace (TypeScript, bundled at build time; see its README — no Node at runtime, never `npm install`)

## Area rules

- **CEF-linking targets** (`cef/`, `gui/host/`, `shell/cef/` + the CEF-linked `context_editor`) do
  NOT link `context_warnings` (CEF ships its own warning set); their headless dependencies still do.
  Build them only with `-DCONTEXT_BUILD_GUI_CEF=ON` on MSVC/clang, or rely on CI.
- **A new editor panel registers its a11y coverage in the same PR**: `gui/a11y/src/registry.cpp` +
  `gui/a11y/coverage.manifest.jsonl` + the a11y CMake link.
- **`editor-boundary` CI job** (D10): installs the exported `context_client` closure, builds the
  out-of-tree consumer against it via `find_package(ContextClient)`, and runs
  `tools/check_include_graph.py` (no kernel-internal headers on the published surface); it also
  builds `context_editor` for the `editor-shell-boundary` audit. **Adding a library to the exported
  install set means adding it to that job's `--target` list in `ci.yml`.**
- A web-layer npm dependency is declared in `webui/core/package.json` (read by the license gate) and
  arrives through a SHA-pinned fetcher in `tools/` — the declaration is not an install manifest.
- `webui/core/src/generated/` is generated (`tools/gen_client_typings.py`) — never hand-edit.
