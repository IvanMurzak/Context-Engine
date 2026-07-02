# src/editor/

**EditorKernel** (formerly "EditorCore") — the headless, file-authoritative project language
server: a live derived index over project files where every authored mutation is a file write,
and GUI, CLI, and AI agents are equal clients over one RPC/event surface. Runs with no GPU and
no display. Nothing lands here until the M1 skeleton. Governed by the Context Engine design
records: **ARCHITECTURE.md** and **REQUIREMENTS.md** (R-HEAD-*, R-CLI-*, R-FILE-*; lock L-19).
