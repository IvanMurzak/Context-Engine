# Context Game Engine — End User License Agreement

> ## ⚠️ DRAFT v0.1 — NOT YET COUNSEL-REVIEWED — effective placeholder until reviewed
>
> This document is an AI-drafted working draft prepared to satisfy the M0 gate ("a
> counsel-reviewed LICENSE exists in the repo before the first public push" —
> `DESIGN-DECISIONS.md` L-57, `ROADMAP.md` §1 M0). **It is not legal advice, has not been
> reviewed by a licensed attorney, and must not be treated as final.** It is written to be
> internally consistent with the owner-ruled business terms recorded in `DESIGN-DECISIONS.md`
> (L-57) and `README.md` ("v1 at a glance" → Business model row), so that counsel has a concrete,
> complete starting point rather than a blank page. See **"Notes for counsel"** at the end for the
> specific points that need professional review before this ships as the repo's real `LICENSE`.
>
> **Licensor (placeholder — flagged below):** Ivan Murzak (sole owner) [entity TBD — counsel]
> **Effective Date:** [DATE OF FIRST PUBLIC PUSH — TBD]
> **Engine repository:** [GITHUB ORG/REPO URL — TBD; naming still open per L-57/README "Open items"]
> **Scope note:** This EULA governs **only** the Context Game Engine source repository
> (EditorKernel, RuntimeKernel, the CLI, schemas, sample content, and associated tooling and
> documentation published in that repository — collectively, the **"Engine"**). It does **not**
> apply to the company's separate MCP plugin repositories (Unity-MCP, Godot-MCP, Unreal-MCP and
> their extensions), which remain licensed independently under the Apache License 2.0, nor to the
> ai-game.dev SaaS platform, which is governed by its own Terms of Service.

---

## PART I — Context Game Engine EULA

### 1. Definitions

- **"Engine"** — the Context Game Engine source code, object code, build tooling, CLI, schemas,
  sample/template content, and documentation made available by Licensor in the repository
  identified above, together with any update, patch, or new **Engine Version** Licensor makes
  available under this or a later version of this EULA.
- **"Engine Version"** — a numbered/tagged release of the Engine (see §10, Version-Locked Terms).
- **"You" / "Licensee"** — the individual or legal entity exercising rights under this EULA.
- **"Product"** — a discrete game or interactive application You develop using the Engine,
  identified by its own distinct name, title, or SKU as released or distributed to end users. A
  sequel, spin-off, or unrelated title is a separate Product; a patch, DLC, expansion, or season
  content released under the same title as an existing Product is **not** a separate Product for
  royalty purposes (its revenue is added to that Product's Gross Revenue).
- **"Gross Revenue"** — for a given Product in a given Measurement Year, the sum of all revenue
  actually received by You (or Your affiliates) that is directly attributable to that Product,
  including sale/purchase price, in-app purchases, microtransactions, downloadable content,
  season passes or subscriptions sold within the Product, and advertising or sponsorship revenue
  earned within the Product — **less**: (a) amounts actually refunded or charged back to end
  users; (b) sales tax, VAT, or similar transaction taxes collected on behalf of a taxing
  authority; and (c) standard distribution/platform fees actually charged by a third-party
  storefront or platform (e.g., a console, mobile, or PC storefront's standard revenue share) on
  that revenue. Gross Revenue does **not** include revenue from goods or services not built with
  the Engine (e.g., unrelated merchandise), revenue of a Product not built with the Engine even if
  co-distributed, or investment/grant funding not tied to end-user transactions. *(⚠️ See Notes
  for counsel — the netting choices in this definition are a business/legal judgment call, not a
  settled default.)*
- **"Measurement Year"** — each successive 12-month period beginning January 1 and ending
  December 31, measured per Product, aggregating Gross Revenue across all platforms, regions, and
  currencies in which that Product is distributed. *(⚠️ proration for a Product's first partial
  year is undecided — see Notes for counsel.)*
- **"Royalty Threshold"** — USD $200,000 of Gross Revenue for a given Product in a given
  Measurement Year. The threshold applies **per Product, per Measurement Year**, and resets at
  the start of each new Measurement Year — it does not accumulate across years or across
  different Products.
- **"Qualifying Subscription"** — an active, paid subscription to the ai-game.dev platform (Pro
  tier or higher; not the Free tier) held by You or Your development team and associated with the
  Product's development, as further described in §6.
- **"Competing Engine Product"** — a general-purpose game engine, game-engine middleware, or
  game-development authoring/runtime platform that You offer or make available (commercially or
  otherwise) to any third party outside Your own affiliated development team, and that is created,
  in whole or in material part, using the Engine's source code, architecture, or non-public
  technical materials. For the avoidance of doubt, this definition restricts *use of the Engine's
  source* to build such a product — it does not restrict any individual's personal skills,
  general engine-development knowledge, or independent work on an unrelated engine project that
  does not use or derive from the Engine's source. *(⚠️ flagged for enforceability review — see
  Notes for counsel.)*
- **"Contribution"** — any source code, documentation, or other material You submit to Licensor
  for inclusion in the Engine (see §9 and the CLA in Part II).

### 2. License Grant

Subject to Your compliance with this EULA, Licensor grants You a worldwide, royalty-bearing
(per §5), non-exclusive, non-transferable (except per §15) license to:

1. **Use, read, and modify** the Engine source code, for the purpose of developing Products;
2. **Build and compile** the Engine and Your modifications, for Your own development and for
   Products;
3. **Embed and distribute** the RuntimeKernel and other Engine runtime components, in object or
   source form, **as part of a shipped Product**, on any platform, and whether the Product is
   distributed commercially or free of charge; and
4. **Distribute Your Product** built with the Engine to end users worldwide, by any distribution
   method.

This grant does not require a subscription to ai-game.dev, does not require any AI usage, and
does not require registration, activation, or any network call to Licensor's servers (§7
reporting is self-directed; see §8 of `DESIGN-DECISIONS.md` L-57's "contractual-only
enforcement" principle).

### 3. Restrictions

You may **not**, without Licensor's prior separate written agreement:

1. **Redistribute the Engine itself**, in source or compiled form, standalone or as a
   development tool/SDK, to any third party — the only permitted redistribution of Engine
   runtime components is embedded inside a shipped Product per §2(3);
2. **Use the Engine's source, architecture, or non-public technical materials to create,
   contribute to, market, or distribute a Competing Engine Product** (as defined in §1);
3. **Sublicense** the Engine source code or grant any third party rights in it beyond what this
   EULA grants You;
4. **Remove, obscure, or alter** any copyright, license, or attribution notice included in the
   Engine source; or
5. **Represent** that You are Licensor, that Your Competing Engine Product (if any, in breach of
   §3(2)) is endorsed by Licensor, or otherwise misuse Licensor's name or the "Context" /
   "Context Game Engine" / "Context Engine" marks beyond truthfully stating a Product was "made
   with Context Game Engine."

For clarity, §3(2) restricts use of the Engine's *source* to build a competing product — it does
**not** restrict You from selling adjacent, Engine-compatible goods or services (asset packs,
plugins distributed as separate packages, paid tutorials, paid support, consulting), which remain
permitted and are not "commercial exploitation of the Engine itself."

### 4. Reservation of Rights; Ownership

Licensor retains all right, title, and interest in and to the Engine, including all intellectual
property rights, subject only to the license expressly granted in §2. No rights are granted by
implication, estoppel, or otherwise. You retain all right, title, and interest in Your Product
and in any original code You write, subject to Licensor's rights in the Engine code embedded or
incorporated within it, and subject to the CLA (Part II) for any Contribution You submit back to
the Engine itself.

### 5. Royalty

1. For each Product, in each Measurement Year in which that Product's Gross Revenue **exceeds**
   the Royalty Threshold, You owe Licensor a royalty equal to **2% of the portion of Gross
   Revenue that exceeds the Royalty Threshold** (a marginal royalty — the first $200,000 of Gross
   Revenue for that Product in that Measurement Year is royalty-free).
2. *Worked example (illustrative only, not a warranty of tax or accounting treatment):* if a
   Product's Gross Revenue in a Measurement Year is $350,000, the royalty due is
   2% × ($350,000 − $200,000) = 2% × $150,000 = **$3,000**.
3. Below the Royalty Threshold, use of the Engine is entirely free — no subscription, no
   registration, and no AI usage of any kind is required, consistent with `README.md`'s "free
   under $200k/year" commitment and `REQUIREMENTS.md` R-HUX-009.
4. Royalty is calculated and owed **per Product**; revenue from one Product is never aggregated
   with another Product to reach the threshold.
5. Payment is due in USD within 45 days after the end of the Measurement Year in which the
   threshold was exceeded, together with the royalty statement described in §7. *(⚠️ currency
   conversion methodology for non-USD revenue is unspecified — flagged for counsel/finance.)*

### 6. Royalty Waiver — ai-game.dev Subscription

The royalty in §5 is **waived** for a Product's Measurement Year if, as of the date the royalty
statement for that Measurement Year is due under §7, You hold a **Qualifying Subscription**
associated with that Product's development team. Holding a Qualifying Subscription does not
require that the Product's AI usage occur on ai-game.dev, or that any particular volume of usage
occurred — the waiver's nexus is **subscription status**, not proof of AI-assisted development.
*(⚠️ This is a deliberately simple, literal nexus test per the owner's instruction; it is also the
single highest-risk definitional gap in this draft — see Notes for counsel. A more precise test
— e.g., subscription must be active for the entire Measurement Year, or must be held by every
team member, or must be a specific tier — is a business decision Licensor has not yet made
final.)*

### 7. Reporting, Records, and Audit

1. **Self-reporting.** You must monitor Your own Product revenue. If a Product's Gross Revenue in
   a Measurement Year exceeds the Royalty Threshold and no Qualifying Subscription waiver applies
   under §6, You must, within 45 days after that Measurement Year ends, submit to Licensor a
   royalty statement showing Gross Revenue, the deductions applied, and the royalty calculation,
   together with payment.
2. **Records.** You must keep records sufficient to verify Gross Revenue for each Product for at
   least three years after the end of the relevant Measurement Year.
3. **Audit.** Licensor (or an independent accountant engaged by Licensor under confidentiality)
   may, on at least 30 days' prior written notice and no more than once per calendar year per
   Product, inspect Your relevant books and records during normal business hours, solely to
   verify Gross Revenue and royalty calculations. If an audit reveals an underpayment of more
   than 5% for the audited period, You bear the reasonable cost of that audit in addition to the
   shortfall and any royalty otherwise owed.
4. **No telemetry.** Consistent with L-57's contractual-only enforcement principle, the Engine
   itself (EditorKernel, RuntimeKernel, or a shipped Product build) contains no license-server
   calls, no subscription checks, no phone-home, and no revenue telemetry. Reporting is entirely
   self-directed and contractual.

### 8. Attribution and Notices

You must retain existing copyright and license headers in unmodified Engine source files You
redistribute (e.g., embedded in a Product's shipped assets, if source is included). No in-Product
attribution (e.g., a splash screen or credits line) is required by this EULA. *(Licensor may
choose to request voluntary "Made with Context Game Engine" attribution as a marketing matter —
that is a business, not legal, decision and is left open here.)*

### 9. Contributions

If You submit a Contribution to the Engine, it is accepted and incorporated **only** under the
Contributor License Agreement (CLA) in Part II of this document. A Contribution submitted without
a signed/accepted CLA on file will not be merged. A Developer Certificate of Origin (DCO)
sign-off alone is **not** sufficient (L-57) — the CLA requires copyright assignment or an
exclusive, unrestricted grant, because Licensor's full, unencumbered ownership of the Engine's
copyright is what keeps this EULA (including future relicensing and the royalty terms)
enforceable now that the source is public.

### 10. Version-Locked Terms

The version of this EULA published alongside a given Engine Version governs Your use of **that**
Engine Version and any Product built with it, even if Licensor later publishes a different EULA
for a subsequent Engine Version. Licensor may change these terms for future Engine Versions;
such changes are never retroactive to a Product built solely on an earlier, unchanged Engine
Version. If You upgrade a Product to a newer Engine Version, the EULA published with the Engine
Version You then adopt governs Your use going forward. *(⚠️ the precise mechanics for a Product
that upgrades mid-Measurement-Year are not fully specified — see Notes for counsel.)*

### 11. Term and Termination

1. This EULA is effective from the date You first exercise any right under §2 and continues
   until terminated.
2. Licensor may terminate this EULA if You materially breach it and fail to cure that breach
   within **30 days** of written notice describing the breach.
3. Upon termination, Your license under §2 ends and You must stop further development, building,
   and distribution of Products under the Engine and stop distributing the Engine itself.
4. **Termination does not affect end users.** End users who lawfully acquired a copy of a
   Product before termination may continue to use that copy; termination is not intended to
   revoke rights already granted to a Product's end users, only Your further rights to develop,
   build, or distribute using the Engine going forward.
5. Sections 4 (Ownership), 5 and 7 (accrued Royalty/reporting obligations for revenue earned
   before termination), 12, 13, 14, and 15 survive termination.

### 12. Disclaimer of Warranty

THE ENGINE IS PROVIDED **"AS IS,"** WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
WITHOUT LIMITATION THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
AND NON-INFRINGEMENT. LICENSOR HAS NO OBLIGATION TO PROVIDE SUPPORT, MAINTENANCE, UPDATES, OR
BUG FIXES. *(⚠️ some jurisdictions do not allow the exclusion of certain implied warranties for
consumer transactions — a savings clause may be needed; see Notes for counsel.)*

### 13. Limitation of Liability

TO THE MAXIMUM EXTENT PERMITTED BY LAW, LICENSOR WILL NOT BE LIABLE FOR ANY INDIRECT,
INCIDENTAL, SPECIAL, CONSEQUENTIAL, OR PUNITIVE DAMAGES, OR ANY LOSS OF PROFITS, REVENUE, DATA,
OR GOODWILL, ARISING OUT OF OR RELATED TO THIS EULA OR THE ENGINE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGES. LICENSOR'S TOTAL AGGREGATE LIABILITY UNDER THIS EULA WILL NOT EXCEED
THE GREATER OF (A) USD $100, OR (B) THE ROYALTIES YOU ACTUALLY PAID LICENSOR UNDER THIS EULA FOR
THE PRODUCT GIVING RISE TO THE CLAIM IN THE 12 MONTHS BEFORE THE CLAIM AROSE.

### 14. Governing Law and Disputes

This EULA is governed by the laws of **[JURISDICTION — counsel to determine]**, without regard to
conflict-of-laws principles. *(⚠️ Placeholder pending counsel — the ai-game.dev SaaS Terms of
Service currently use Washington State law per `project-context.md`; whether the Engine EULA
should match, for consistency and cost, or use a different jurisdiction, is a decision for
counsel together with the Licensor-entity question below.)* Any dispute not resolved informally
within 30 days may be brought exclusively in the courts of [VENUE — counsel to determine], and
each party consents to that venue's jurisdiction.

### 15. General

1. **Severability.** If any provision of this EULA is held unenforceable, the remaining
   provisions remain in full force, and the unenforceable provision will be reformed to the
   minimum extent necessary to make it enforceable while preserving its intent.
2. **Entire agreement.** This EULA (together with the CLA, for Contributors) is the entire
   agreement between You and Licensor regarding the Engine, superseding any prior discussions.
3. **Assignment.** You may not assign this EULA without Licensor's written consent, except in
   connection with a bona fide sale of substantially all assets relating to a Product, provided
   the assignee agrees in writing to be bound by this EULA.
4. **No waiver.** Licensor's failure to enforce any provision is not a waiver of that or any
   other provision.
5. **No partnership.** Nothing in this EULA creates an employment, agency, partnership, or joint
   venture relationship between You and Licensor.
6. **Notices.** Notices to Licensor must be sent to [LEGAL CONTACT EMAIL/ADDRESS — TBD].

---
