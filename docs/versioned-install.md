# Versioned install layout (R-VER-004)

**The layout is the day-one contract.** From the very first release, Context Engine packaging
installs every engine version to:

```
<root>/versions/<semver>/
```

and **no version ever installs over another**. Flat installs (an engine unpacked directly into
`<root>/`) are **forbidden** — a first release that installs flat would foreclose side-by-side
engine versions forever, because every subsequent tool, launcher, and project pin would have to
special-case the legacy flat layout.

## Why this exists before any release machinery

R-VER-004 (MUST) requires multiple engine versions to install **side-by-side**: real teams run
many projects on many engine versions concurrently, and a project's engine pin (R-VER-003)
selects which daemon/CLI serves it. The **resolver/fetcher/launcher machinery arrives with the
second release** — when two versions first coexist and there is something to resolve — but the
layout convention it will resolve against must exist from release one. That is this page.

## Rules

1. Install root: `<root>/versions/<semver>/` — e.g. `versions/0.1.0/`, `versions/0.2.0-rc.1/`.
2. `<semver>` is the full semantic version of the engine build, exactly as tagged.
3. Nothing writes outside its own `versions/<semver>/` subtree at install time; versions are
   immutable once installed.
4. Uninstalling a version removes only its own subtree.
5. On-demand fetch of a pinned engine version is **signed + verified against the trust root
   (R-SEC-009), verify-before-execute, fail closed** — the pin is only meaningful if the
   fetched artifact is authenticated before it ever runs.

Any packaging script, installer, CI artifact layout, or archive produced by this repository
MUST follow this convention from the first release.

## Implementation status (M0 skeleton)

The install/package skeleton that stakes out this layout is wired into `src/CMakeLists.txt`:

- `cmake --install <build> --prefix <root>` stages the payload into
  `<root>/versions/<semver>/` (`bin/` for executables, plus a `context-version.txt` marker a
  resolver enumerates by listing `versions/*/`). The `<semver>` is `PROJECT_VERSION`.
- `cpack -G ZIP` produces an archive carrying the **same** internal
  `versions/<semver>/` layout (it consumes the same `install()` rules).
- **CI proof**: the `versioned-install-layout` ctest (`cmake/versioned-install-check.cmake`)
  stages a real install and asserts it landed under `versions/<semver>/` and **not flat**,
  failing the build on any violation. It runs under `ctest --preset dev` on all three OS legs
  of the CI `build` matrix.

The resolver/fetcher/launcher machinery (second release) will resolve the R-VER-003 project
pin against this layout, and R-SEC-009 requires every fetched version be **verify-before-use /
fail closed** (`tools/verify_artifact.py`; see `docs/signing.md`) before it is ever executed.
