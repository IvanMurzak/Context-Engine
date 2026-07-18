# Security Policy

Context Game Engine follows a **coordinated vulnerability disclosure (CVD)** process. This page is
the reporter-facing policy: how to reach us privately, what to expect, and the safe harbor we extend
to good-faith research. The maintainer-side workflow and the EU Cyber Resilience Act (CRA)
reporting-readiness note live in [`docs/cvd-and-cra-readiness.md`](docs/cvd-and-cra-readiness.md);
the engine's trust-boundary map is [`docs/security-boundaries-v1.md`](docs/security-boundaries-v1.md).

## Supported versions

The engine is **pre-alpha** and has **no versioned releases yet** — `main` is the only supported
line, and security fixes land there.

| Version | Supported |
|---|---|
| `main` (pre-alpha; no releases yet) | ✅ |

Once versioned releases exist (the M8 build pipeline onward), this table will enumerate the
supported release lines; until then "latest `main`" is the supported version.

## Reporting a vulnerability

**Do not open a public issue, discussion, or pull request for a suspected vulnerability.**

- **Primary (private) channel — GitHub private vulnerability reporting:**
  <https://github.com/IvanMurzak/Context-Engine/security/advisories/new>.
  If the report form is unavailable (the repository setting may still be in the process of being
  enabled), use the fallback below.
- **Fallback:** email `support@ai-game.dev` with the subject prefix `[SECURITY]`. Please do not
  include full exploit details in the initial email; we will move the exchange into a private
  GitHub security advisory.

A useful report includes: the affected component or path (e.g. `src/editor/bridge/`,
`src/runtime/js/`), the commit or version you tested, reproduction steps or a minimal proof of
concept, and your assessment of the impact. Reports about the engine's *designed* trust posture
(untrusted same-user code — see `docs/security-boundaries-v1.md`) are welcome too; please say which
documented boundary you believe is violated.

## Response targets (SLOs)

This is a solo-maintainer, pre-alpha project; these are the targets we commit to and expect to meet:

| Stage | Target |
|---|---|
| Acknowledgement of your report | within **3 business days** |
| Triage + initial severity assessment | within **7 calendar days** of acknowledgement |
| Fix, or a documented mitigation plan, for a confirmed vulnerability | within **90 days** of triage (faster when the vulnerability is being actively exploited) |
| Coordinated public disclosure | default **90 days** from the report, or earlier/later by mutual agreement (e.g. when a fix needs more time) |

When a report is declined (not a vulnerability, out of scope, works as designed), we say so
explicitly with the reasoning, and you are free to disclose after the default window.

## Scope

- **In scope:** everything in this repository — EditorKernel, RuntimeKernel, the `context` CLI,
  schemas, build tooling (`tools/`, `bench/`), and the documented trust boundaries
  (`docs/security-boundaries-v1.md`).
- **Out of scope:** the separate MCP plugin repositories (Unity-MCP, Godot-MCP, Unreal-MCP — their
  own repos/policies), the `ai-game.dev` online services (a separate product), and vulnerabilities
  purely in third-party dependencies (report upstream — but do tell us as well so we can pin/bump;
  the dependency surface is gated by `tools/license-allowlist.json` and the vendored-prebuilt
  pins).

## Safe harbor

We support good-faith security research on Context Engine:

- We will **not initiate legal action** (including under the Context Engine EULA, `LICENSE.md`) or
  report you to law enforcement for security research that is conducted in good faith, complies
  with this policy, and stays within the scope above.
- Good faith means: test against **your own installations/builds**; do not access, destroy, or
  exfiltrate data that is not yours (a minimal proof of concept is fine); do not degrade
  infrastructure you do not own; comply with applicable law; and give us the coordinated-disclosure
  window above before publishing.
- This safe harbor authorizes **security research and disclosure only** — it does not extend the
  EULA's grant otherwise (e.g. it is not a license to redistribute the engine), and it cannot
  authorize testing against third-party systems or services.

If you are unsure whether something is covered, ask first via the private channel — we would rather
answer a question than lose a report.
