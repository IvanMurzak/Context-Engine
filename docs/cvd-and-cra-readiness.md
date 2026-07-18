# Coordinated vulnerability disclosure (CVD) + EU CRA reporting readiness

This engineering doc records the maintainer-side **coordinated-disclosure workflow** behind the
reporter-facing [`SECURITY.md`](../SECURITY.md), and the **EU Cyber Resilience Act (CRA)
reporting-readiness note** the R6 design review added (owner design records: ROADMAP §1-M8.5 CVD
bullet, ROADMAP §7 CRA item; pairs with the R-SEC-009 / O-7 supply-chain posture already in place).
The engine's trust-boundary map — what is and is not a boundary — is
[`security-boundaries-v1.md`](security-boundaries-v1.md).

## The CVD workflow (maintainer side)

1. **Intake.** Reports arrive via GitHub private vulnerability reporting (the primary channel in
   `SECURITY.md`) or the `[SECURITY]` email fallback. An email report is moved into a **draft GitHub
   security advisory** immediately, so every report has one private, durable workspace.
2. **Acknowledge.** Within the `SECURITY.md` SLO (3 business days).
3. **Triage.** Reproduce; classify against the documented trust boundaries
   (`security-boundaries-v1.md` — is a *designed* boundary violated, or is the finding within the
   accepted same-user threat model?); assess severity (CVSS 3.1 as a guide, not a gate); determine
   affected versions. **Determine whether the vulnerability is being actively exploited** — that
   determination starts the CRA reporting clock below once the reporting obligations apply.
4. **Fix.** Develop the fix in the advisory's private workspace when public visibility would put
   users at risk, otherwise on a normal branch. Tests ship in the same PR as the fix (R-QA-013) —
   ideally a regression test in the red-team suites
   (`src/tests/integration/test_redteam_boundaries.cpp` / `test_redteam_tamper_e2e.cpp`).
5. **Release.** Land on `main`. While the engine is pre-alpha with no versioned releases, landing on
   `main` IS the release; once versioned releases exist (M8 onward), backport to the supported lines
   per the `SECURITY.md` support table and cut patched releases.
6. **Disclose.** Publish the GitHub security advisory: affected versions, impact, the fix commit,
   workarounds if any, and **credit to the reporter** (unless anonymity is requested). Request a CVE
   through GitHub's CVE numbering when the finding warrants one.
7. **Record.** The published advisory history is the durable record of handled vulnerabilities; it
   doubles as the evidence trail for the CRA reporting readiness below.

Timeline defaults are the ones in `SECURITY.md`: 90-day coordinated disclosure, accelerated when a
vulnerability is actively exploited (where the CRA clock below is the binding floor once
applicable).

## EU CRA reporting readiness

> ⚠️ **DRAFT — pending legal confirmation.** The CRA applicability analysis for Context Engine was
> routed to the business/legal department via fleet envelope **`20260715-165440-5cd64a`**; the
> Technical Director reconciles that reply into this note (it is referenced here verbatim for that
> reconciliation). Until then, this section records reporting-readiness **facts from the regulation
> text** and our internal readiness mapping — it makes **no confirmed legal claim** about the
> engine's classification or the exact scope of its obligations.

**What.** Regulation (EU) 2024/2847 (the Cyber Resilience Act) imposes security and reporting
obligations on manufacturers of "products with digital elements" placed on the EU market. Its
**reporting obligations (Article 14) apply from 2026-09-11**; the main obligations apply from
2027-12-11.

**Who reports.** The manufacturer. For this repository that is the licensor named in
[`LICENSE.md`](../LICENSE.md) — currently **Ivan Murzak, an individual**; if the licensor role
succeeds to a legal entity (EULA §15(7)), the reporting role moves with it. Internally, the
maintainer who triages a report (workflow step 3) is the person who starts and runs the reporting
clock.

**What triggers a report.** Two distinct triggers, each with its own track:

- an **actively exploited vulnerability** in the product, and
- a **severe incident** having an impact on the security of the product.

**Where.** Reports go to the **ENISA single reporting platform** (the CRA's one-stop notification
endpoint, operated by ENISA), which routes to the designated CSIRT coordinator and ENISA. Until the
platform's operational details are confirmed for our case, the readiness assumption is: submit via
the single reporting platform; keep the advisory (workflow step 7) as the supporting record.

**The clock — actively exploited vulnerability (the 24h/72h/14d track):**

| Deadline | Obligation |
|---|---|
| **24 hours** from awareness | early warning |
| **72 hours** from awareness | vulnerability notification (general information, severity/impact, any corrective or mitigating measures taken or available) |
| **14 days** after a corrective or mitigating measure is available | final report |

**The clock — severe incident:** 24-hour early warning, 72-hour incident notification, final report
within **1 month** after the incident notification.

**Internal readiness mapping (who does what, today):**

- **Intake + awareness**: the CVD intake channels above are the awareness funnel; triage (step 3)
  explicitly asks "actively exploited?" — a *yes* starts the 24h clock.
- **Reporter of record**: the maintainer/licensor (above). No delegation is currently in place.
- **Evidence**: the draft/published GitHub security advisory carries the technical facts a
  notification needs (affected versions, impact, measures); the final report reuses it.
- **Users to notify**: impacted users are informed via the published advisory and, where relevant,
  release notes on the supported lines (`SECURITY.md` support table).

**Open items for TD reconciliation (with the `20260715-165440-5cd64a` reply):**

- Confirm CRA applicability and product classification for a royalty-bearing, source-available
  engine (the FOSS carve-out analysis) — the load-bearing legal conclusion this note deliberately
  does not assert.
- Confirm the reporting entity's identity/registration details as they should appear in
  notifications (individual vs. future legal entity).
- Align this note's wording (and `SECURITY.md`'s safe harbor) with counsel-confirmed text, then
  remove the draft banner above.
