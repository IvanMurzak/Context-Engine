<div align="center">

<img src="docs/img/readme/hero.svg" alt="Context Engine — AI-first game engine, pre-alpha. Project files are the single source of truth; GUI, CLI and AI agents are equal clients. Every feature is a package." width="100%"/>

<br/>

[![CI](https://img.shields.io/github/actions/workflow/status/IvanMurzak/Context-Engine/ci.yml?branch=main&label=CI&style=for-the-badge&logo=github&logoColor=white&labelColor=0F172A&color=15803D)](https://github.com/IvanMurzak/Context-Engine/actions/workflows/ci.yml) ![Status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white) ![C++20](https://img.shields.io/badge/C%2B%2B-20-0E7490?style=for-the-badge&labelColor=0F172A&logoColor=white&logo=cplusplus) ![CMake 3.25+](https://img.shields.io/badge/CMake-3.25%2B-0E7490?style=for-the-badge&labelColor=0F172A&logoColor=white&logo=cmake) [![License: Context Engine EULA (draft)](https://img.shields.io/badge/license-EULA%20%28draft%29-047857?style=for-the-badge&labelColor=0F172A&logoColor=white)](LICENSE.md) [![Stand with Ukraine](https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-badge.svg)](https://stand-with-ukraine.pp.ua)

</div>

**Context** is a C++ game engine built AI-first without making humans second-class. **Project files are the single source of truth**: a headless project daemon, the **EditorKernel**, keeps a live index over them, every edit is a file write, and the editor GUI, the `context` CLI and AI agents are equal clients of one contract. The whole authoring loop runs with no GPU and no GUI.

<p align="center">
  <img src="docs/img/readme/architecture.svg" alt="Project files feed the EditorKernel daemon, which serves one contract registry (CLI, RPC, MCP) to the editor app, the context CLI and AI agents; the RuntimeKernel runs deterministic sessions and the WebGPU renderer draws them" width="100%"/>
</p>

> [!WARNING]
> **Pre-alpha.** There are no versioned releases yet: `main` is the only supported line, and things change without notice. Milestones M0–M8.5 of the [roadmap](.taskflow/2026-07-01-context-engine/core/ROADMAP.md) are done; M9, the interactive editor application, is in progress.

## What is in the repository today

<p>
✅ <b>Every feature is a package</b> — physics, animation, particles, audio, input, UI on a microkernel<br>
✅ <b>File-authoritative</b> — canonical JSON, stable ids, schema migration, structural 3-way merge<br>
✅ <b>One contract</b> — each verb is a CLI command, an RPC method and an MCP tool definition<br>
✅ <b>TypeScript gameplay</b> — on an embedded V8, with a C++/WASM tier for hot paths<br>
✅ <b>WebGPU rendering</b> — native and in the browser; 2D is first-class<br>
✅ <b>Deterministic sessions</b> — fixed ticks, state hash and replays, checked on 3 OSes in CI<br>
✅ <b>Headless builds</b> — <code>context build --target linux</code>, <code>windows</code>, <code>macos</code> or <code>web</code><br>
✅ <b>Windowed editor</b> — dockable panels, command palette, themes, multi-window tear-out<br>
</p>

## Build the engine and the CLI

Needs CMake ≥ 3.25 and a C++20 compiler. No GPU or GUI is required.

```sh
cmake -S src --preset dev
cd src
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

The build lands in `src/build/dev/`, the CLI at `src/build/dev/cli/context`. Other presets: `deterministic` (strict floating point), `sanitize` (ASan + UBSan) and `tsan` (both non-Windows).

## Use the CLI

<img src="docs/img/readme/terminal.svg" alt="context new my-game; context session new run.state --seed 7; context session step run.state --ticks 600 --json and context session hash run.state --json return an ok envelope with simTick 600 and the state hash" width="100%"/>

```sh
context new my-game                                   # a runnable project skeleton
context describe --json                               # the whole contract as JSON
context session new run.state --seed 7                # a headless deterministic session
context session step run.state --ticks 600 --json     # advance 600 fixed ticks
context session hash run.state --json                 # the canonical state hash
context doctor --target all                           # check the toolchain for every build target
```

With `--json`, every verb answers with one envelope: `{"ok": …, "data": …}`, or an `error` with a stable code. Runnable examples live in [`samples/`](samples/README.md), for instance the `platformer-2d` and `roll-3d` games; CI keeps every sample valid.

## Build and run the editor

Needs CMake ≥ 3.25 and Python 3; on Windows also Visual Studio with the C++ workload (the editor's browser shell prebuilt needs MSVC there, clang on Linux and macOS). The script finds the compiler environment by itself:

```sh
python tools/build_editor.py    # --no-gpu: CPU present path, no viewport scene
src/build/editor/editor/shell/Release/context_editor --project samples/platformer-2d
```

Without `--project` the editor opens its welcome screen. `--devtools` enables the Chromium DevTools, `--help` lists every option. The editor starts or attaches the project daemon itself.

## Repository layout

| Path | Contents |
|---|---|
| [`src/kernel/`](src/kernel) | The microkernel: the few stable interfaces everything plugs into |
| [`src/editor/`](src/editor) | The EditorKernel daemon, the contract registry, the editor shell and web UI |
| [`src/runtime/`](src/runtime) | The RuntimeKernel: sessions, determinism, V8 and WASM hosts |
| [`src/render/`](src/render) | The WebGPU renderer (native and web) |
| [`src/packages/`](src/packages) | First-party feature packages |
| [`src/cli/`](src/cli) | The `context` CLI |
| [`samples/`](samples/README.md) | Runnable sample projects, checked by CI |
| [`docs/`](docs/README.md) | Engineering notes on how the code implements the design |
| [`.taskflow/`](.taskflow/2026-07-01-context-engine/core) | Design authority: requirements, decisions, architecture, roadmap |

## License

Source-available under the **Context Engine EULA** ([LICENSE.md](LICENSE.md)). The current text is **draft v0.2 and has not been reviewed by counsel yet**. In short:

- You may use, modify and build the engine to develop games, and ship its runtime inside them.
- Royalty, per product and calendar year: none on the first **$200,000** of gross revenue, then **2% of the gross revenue above $200,000**.
- You may not redistribute the engine itself or use its source to build a competing engine or tool.
- It is **not open source**. The license file is the authority; this summary is not.

## Contributing

External pull requests cannot be merged until a CLA exists; see [CONTRIBUTING.md](.github/CONTRIBUTING.md). Issues, bug reports and design discussion are welcome. Security reports: [SECURITY.md](SECURITY.md).
