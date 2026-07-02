# Contributing to Context Game Engine

Thank you for your interest — and please read this before opening a pull request.

## External contributions are currently blocked

Honest status: **pull requests from external contributors cannot be merged yet.** Context is
source-available under a proprietary EULA (see [LICENSE.md](../LICENSE.md)), and keeping that
license enforceable requires that the project owns the full copyright of every line in the
repository. That means every external contribution must be covered by a **Contributor License
Agreement (CLA) with copyright assignment** — a Developer Certificate of Origin (DCO) sign-off
is **insufficient**. The CLA text and its signing flow do not exist yet (see
[CLA.md](CLA.md)); until they do, external PRs will be closed with a pointer to this document.

Issues, bug reports, and design discussion are welcome in the meantime.

## When the CLA flow exists

Once the CLA is in place, contributions will additionally need to pass the standing CI rails:

- **Build + test** on Linux, Windows, and macOS (`dev` preset), plus the ASan/UBSan `sanitize`
  job.
- **Warnings-as-errors** — the baseline is on by default (`CONTEXT_WARNINGS_AS_ERRORS=ON`).
- **Formatting** — `src/.clang-format` (LLVM-based, 100 columns) and the `src/.clang-tidy` starter
  set (copies in `spikes/` and `bench/` cover those sibling trees).
- **Dependency-license gate** — `tools/check_licenses.py` is deny-by-default: any new
  dependency whose license is unknown or not on `tools/license-allowlist.json` fails CI.

## Tests are part of the feature (R-QA-013)

Test delivery is **PR-granular** (engine-design requirement R-QA-013, owner-ruled
2026-07-02): a behavior change merges only together with the tests that pin it down —
happy path, edge cases, **and** failure paths — never in a later "hardening pass".
Concretely:

- **Engine/tool code (C++/Python/TS)** ships with a test suite in the same PR
  (Python tooling: pytest under `tools/tests/` and `bench/tests/`, run per-PR by the
  `python-tests` CI job; C++ targets: ctest registrations).
- **Spikes** are exempt from full suites but must carry a **runnable self-check
  registered with ctest** under the `spikes` preset (e.g. `ecs-spike-zerocopy-runs`,
  `context-parse-bench-canon-check`, `context-spike-jsengine-quickjs-smoke`).
- A PR that changes behavior with no accompanying tests is incomplete by definition.

## Commit style

Conventional commits (`feat:`, `fix:`, `build:`, `ci:`, `docs:`, `chore:` …).
