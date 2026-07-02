# cmake/

Shared CMake modules for the Context Engine build. Currently: `ContextWarnings.cmake` (the
`context_warnings` interface target — the warnings-as-errors baseline every target links).
Toolchain pinning per L-42 (from-source vcpkg + per-target pinned Clang toolchain manifest)
lands here as M0 progresses. Governed by the Context Engine design records:
**DESIGN-DECISIONS.md locks L-38 (sanitizer-gated CI) and L-42** and **ROADMAP.md §1 M0**.
