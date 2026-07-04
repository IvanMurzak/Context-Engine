# Context Game Engine — End User License Agreement

> ## ⚠️ DRAFT v0.2 — NOT YET COUNSEL-REVIEWED — effective placeholder until reviewed
>
> This document is an AI-drafted working draft published so the repository carries a concrete,
> complete license from day one. **It is not legal advice, has not been reviewed by a licensed
> attorney, and must not be treated as final.** It is written to be internally consistent with
> the project's owner-ruled business terms (design decision L-57, as amended by owner rulings
> dated 2026-07-03). Professional counsel review is a standing gate before this draft banner is
> removed.
>
> **v0.2 (2026-07-03)** folds in the business department's legal review (round 1) and two owner
> rulings dated 2026-07-03: **(1)** the royalty base is **gross receipts** — storefront/platform
> fees are **not** deducted; and **(2)** this EULA is **fully decoupled from ai-game.dev** — the
> former subscription-based royalty waiver is **removed**, and no subscription to any product or
> service affects this license in any way (§6). See the Version history at the end.
>
> **Licensor:** Ivan Murzak, an individual (the Engine's sole owner). Licensor intends to form a
> legal entity and to republish this EULA under that entity with a future Engine Version — see
> §10 (Version-Locked Terms) and §15(7) (Licensor succession); such a republication changes the
> identity of the Licensor, never the terms locked to earlier Engine Versions.
> **Effective Date:** [DATE OF FIRST PUBLIC PUSH — TBD]
> **Engine repository:** https://github.com/IvanMurzak/Context-Engine
> **Scope note:** This EULA governs **only** the Context Game Engine source repository
> (EditorKernel, RuntimeKernel, the CLI, schemas, sample content, and associated tooling and
> documentation published in that repository — collectively, the **"Engine"**). It does **not**
> apply to the company's separate MCP plugin repositories (Unity-MCP, Godot-MCP, Unreal-MCP and
> their extensions), which remain licensed independently under the Apache License 2.0, nor to
> any separate product or service of Licensor, each of which is governed by its own terms.
> **This EULA has no connection to any subscription or AI service** — using the Engine never
> requires one, and holding one never changes these terms (§6).

---

## PART I — Context Game Engine EULA

### 1. Definitions

- **"Engine"** — the Context Game Engine source code, object code, build tooling, CLI, schemas,
  sample/template content, and documentation made available by Licensor in the repository
  identified above, together with any update, patch, or new **Engine Version** Licensor makes
  available under this or a later version of this EULA.
- **"Engine Version"** — a numbered/tagged release of the Engine (see §10, Version-Locked Terms).
- **"You" / "Licensee"** — the individual or legal entity exercising rights under this EULA.
- **"Affiliate"** — any entity that controls, is controlled by, or is under common control with
  You, where "control" means direct or indirect ownership of more than 50% of the voting
  interests or the power to direct the entity's management.
- **"Your affiliated development team"** — Your employees and the individual contractors
  engaged by You (or by Your Affiliate) under confidentiality obligations to work on Your
  Products; it does not include any other company, studio, publisher, or organization that is
  not You or Your Affiliate.
- **"Product"** — a discrete game or interactive application You develop using the Engine,
  identified by its own distinct name, title, or SKU as released or distributed to end users.
  For royalty purposes:
  - a sequel, spin-off, or unrelated title is a **separate** Product;
  - a patch, update, DLC, expansion, season content, or in-Product subscription offering
    released under the same title is part of the **same** Product (its revenue is added to that
    Product's Gross Revenue);
  - ports, platform-specific versions, regional editions, and content-tier editions (e.g.,
    standard/deluxe) of the same title are the **same** Product, aggregated across all
    platforms and regions;
  - a free demo, prologue, or trial of a title is part of that title's Product;
  - an early-access or beta release and its subsequent full release under the same title are
    the **same** Product; and
  - a remake or remaster sold to end users as a new purchase is a **separate** Product only if
    it involves substantial new development; a re-release with no substantial new development
    is part of the original title's Product (its revenue aggregates there).
- **"Gross Revenue"** — for a given Product in a given Measurement Year, the sum of all revenue
  attributable to that Product worldwide, measured on a **gross-receipts basis**:
  - for revenue arising from end-user transactions: the amounts paid by or on behalf of end
    users for the Product (the end-user transaction price), **before and without deduction of**
    any storefront, platform, or distribution fee, commission, or revenue share (e.g., a
    console, mobile, or PC storefront's cut), payment-processing fees, or any other cost or
    expense of any kind, whether or not such amounts are withheld by an intermediary before
    remittance to You; and
  - for Product revenue not arising from individual end-user transactions (e.g., a fixed
    platform, catalog, or subscription-service fee paid for including or distributing the
    Product, a porting or exclusivity fee for the Product, or advertising or sponsorship
    revenue earned within the Product): the amounts payable to You (or Your affiliates) under
    the relevant arrangement.

  Gross Revenue includes, without limitation, sale/purchase price, in-app purchases,
  microtransactions, downloadable content, and season passes or subscriptions sold within the
  Product. Gross Revenue **excludes only**: (a) amounts actually refunded or charged back to
  end users; and (b) sales tax, VAT, or similar transaction taxes collected on behalf of a
  taxing authority — amounts never retained by You, not cost deductions. Gross Revenue does not
  include revenue from goods or services not built with the Engine (e.g., unrelated
  merchandise), revenue of a separate product not built with the Engine even if co-distributed,
  or investment or grant funding not tied to end-user transactions. *(⚠️ currency-conversion
  methodology and first-partial-year proration are open counsel/finance items.)*
  - **Bundles.** If a Product is sold together with other products or services for a single
    combined price, the portion of the bundle price attributable to the Product is determined
    by a commercially reasonable, consistently applied allocation — by default, pro-rata by the
    bundled items' individual bona fide standalone list prices at the time of sale; where a
    bundled Product has no bona fide standalone list price, or its standalone price is set
    below fair market value, a good-faith fair-value allocation applies — and only that
    portion is Gross Revenue of that Product. Where several bundled items are Products built
    with the Engine, each Product's Royalty Threshold and royalty are computed on its own
    allocated share.
  - **Related-party and below-market sales.** Revenue from a sale or license of the Product to
    or through an Affiliate or other related party (including wholesale or key sales to a
    related reseller) on terms below fair market value is included at the arm's-length fair
    market value of the transaction, not at the discounted price.
  - **Shared virtual currency.** Where a virtual currency, wallet, or similar cross-title
    credit is usable in more than one title, its purchase revenue is attributed to a Product
    according to in-Product consumption of that currency; where consumption cannot reasonably
    be traced, according to purchases initiated within the Product.
- **"Measurement Year"** — each successive 12-month period beginning January 1 and ending
  December 31, measured per Product, aggregating Gross Revenue across all platforms, regions, and
  currencies in which that Product is distributed. *(⚠️ proration for a Product's first partial
  year is undecided — flagged for counsel.)*
- **"Royalty Threshold"** — USD $200,000 of Gross Revenue for a given Product in a given
  Measurement Year. The threshold applies **per Product, per Measurement Year**, and resets at
  the start of each new Measurement Year — it does not accumulate across years or across
  different Products.
- **"Competing Engine Product"** — any product or service **made available to any third party**
  outside You and Your affiliated development team (commercially or free of charge, in any form
  — including distributed software, an SDK or other development tool, middleware, or a hosted
  or managed service) that:
  - (a) **is, embeds, or exposes the Engine or a modified version of it** for any purpose other
    than running a shipped Product — including offering the Engine (or any substantial part of
    it) to third parties as a game engine, authoring tool, development or simulation platform,
    middleware, or as a hosted or managed service whose value derives, in whole or in material
    part, from the Engine's functionality; or
  - (b) **is derived from, incorporates, or was created using the Engine's source code** (or a
    modification of it) and provides third parties with the same or substantially similar
    functionality as the Engine or a substantial component of it (e.g., its editor kernel,
    runtime, build pipeline, or asset/derivation system).

  For purposes of (b), "created using the Engine's source code" means created by copying from,
  adapting, or making a derivative work of the Engine's source code — not by independently
  developing a product with the general skills, ideas, methods, or knowledge a person gains
  from studying the Engine, including knowledge of its architecture or design.

  For the avoidance of doubt, **none of the following is a Competing Engine Product**:
  - a shipped Product (a game or interactive application under the §2 grant), including its
    embedded Engine runtime components;
  - a plugin, package, extension, editor tool, or asset pack **for** the Engine that requires a
    licensed copy of the Engine to function and does not itself provide the Engine's
    functionality apart from the Engine;
  - generic third-party compute, CI/CD, build, or hosting infrastructure that runs a licensed
    copy of the Engine solely on Your behalf and under Your instructions to develop, build, or
    host Your own Product, and does not offer the Engine's functionality to its own customers
    as a distinct product or as a substitute for the Engine;
  - a modding or user-generated-content SDK, tool, or editor extension for a Product that
    requires an end user's copy of that shipped Product to function;
  - paid tutorials, training, support, consulting, or contract development for Engine users;
  - internal tooling used solely by You and Your affiliated development team and not made
    available to third parties; and
  - an independently developed engine, tool, or middleware that does not use, incorporate, or
    derive from the Engine's source code.

  This definition — both limbs (a) and (b) and the list above — restricts what You do with the
  Engine's **source code and derivatives of it**; it does not restrict any individual's
  personal skills or general engine-development knowledge. *(⚠️ flagged for enforceability
  review by counsel.)*
- **"Contribution"** — any source code, documentation, or other material You submit to Licensor
  for inclusion in the Engine (see §9 and the CLA skeleton in
  [`.github/CLA.md`](.github/CLA.md)).

### 2. License Grant

Subject to Your compliance with this EULA, Licensor grants You a worldwide, royalty-bearing
(per §5), non-exclusive, non-transferable (except per §15(3)) license to:

1. **Use, read, and modify** the Engine source code, for the purpose of developing Products;
2. **Build and compile** the Engine and Your modifications, for Your own development and for
   Products;
3. **Embed and distribute** the RuntimeKernel and other Engine runtime components, in object or
   source form, **as part of a shipped Product**, on any platform, and whether the Product is
   distributed commercially or free of charge; and
4. **Distribute Your Product** built with the Engine to end users worldwide, by any distribution
   method.

This grant does not require a subscription to any product or service (Licensor's or anyone
else's), does not require any AI usage, and does not require registration, activation, or any
network call to Licensor's servers — royalty reporting under §7 is entirely self-directed and
contractual (§7(4)).

**Acceptance.** This EULA operates as a condition on the copyright license to the Engine. You
accept it by exercising any right granted in this §2 — for example, cloning or downloading the
Engine's source for development use, building it, modifying it, embedding it in a Product, or
distributing a Product built with it. If You do not agree to this EULA, do not exercise those
rights: no other permission is granted, and use of the Engine outside this EULA's terms
infringes Licensor's copyright rather than merely breaching a contract. Merely viewing the
public repository, reading its code or documentation, or performing acts the hosting platform's
own terms independently allow (e.g., viewing and forking within GitHub under GitHub's Terms of
Service) does not constitute acceptance and imposes none of this EULA's obligations. The
license GitHub's Terms of Service grant to other users is limited to viewing and reproducing
the repository within GitHub and grants no right to clone or download the Engine for
development use, or to build, run, modify, embed, or distribute it — including on a local copy
of a GitHub fork; those acts are licensed only under this EULA and constitute acceptance. By
accepting, You additionally represent that You enter into this EULA for purposes relating to
Your trade, business, craft, or profession (including commercial indie or solo development),
and not as a consumer, to the maximum extent that characterization is permitted under
applicable law.

For the avoidance of doubt, the royalty in §5 is both (i) a **condition** of the copyright
license granted in this §2 with respect to any Product whose Gross Revenue is measured under
§5, and (ii) an independent **contractual payment obligation** undertaken by exercising any
right under this §2 to develop, build, or distribute such a Product. Licensor may rely on
either or both characterizations; the unavailability of one in a given jurisdiction does not
affect the other.

### 3. Restrictions

You may **not**, without Licensor's prior separate written agreement:

1. **Redistribute the Engine itself**, in source or compiled form, standalone or as a
   development tool/SDK, to any third party — the only permitted redistribution of Engine
   runtime components is embedded inside a shipped Product per §2(3);
2. **Use the Engine, its source code, or any modification or derivative of it to create,
   operate, market, or distribute a Competing Engine Product** (as defined in §1), including
   offering the Engine's functionality to third parties as a hosted or managed service;
3. **Sublicense** the Engine source code or grant any third party rights in it beyond what this
   EULA grants You;
4. **Remove, obscure, or alter** any copyright, license, or attribution notice included in the
   Engine source; or
5. **Represent** that You are Licensor, that Your Competing Engine Product (if any, in breach of
   §3(2)) is endorsed by Licensor, or otherwise misuse Licensor's name or the "Context" /
   "Context Game Engine" / "Context Engine" marks beyond truthfully stating a Product was "made
   with Context Game Engine."

For clarity, §3(2) restricts use of the Engine's *source code and derivatives of it* to build a
competing product — it does **not** restrict You from selling adjacent, Engine-compatible goods
or services (asset packs, plugins distributed as separate packages that require the Engine, paid
tutorials, paid support, consulting), which remain permitted and are not "commercial
exploitation of the Engine itself."

The restrictions in this §3 operate as conditions on the scope of the copyright license granted
in §2 — a field-of-use limitation on the use of Licensor's copyrighted work — not as a covenant
in restraint of any person's trade or profession. If any part of this §3 (or of the §1
"Competing Engine Product" definition) is held unenforceable in a particular jurisdiction, it is
to be reformed per §15(1) to the maximum scope enforceable in that jurisdiction, and any
resulting severance applies only in that jurisdiction, leaving the provision in full force
everywhere else.

### 4. Reservation of Rights; Ownership

Licensor retains all right, title, and interest in and to the Engine, including all intellectual
property rights, subject only to the license expressly granted in §2. No rights are granted by
implication, estoppel, or otherwise. You retain all right, title, and interest in Your Product
and in any original code You write, subject to Licensor's rights in the Engine code embedded or
incorporated within it, and subject to the CLA (see §9) for any Contribution You submit back to
the Engine itself.

### 5. Royalty

1. For each Product, in each Measurement Year in which that Product's Gross Revenue **exceeds**
   the Royalty Threshold, You owe Licensor a royalty equal to **2% of the portion of Gross
   Revenue that exceeds the Royalty Threshold** (a marginal royalty — the first $200,000 of Gross
   Revenue for that Product in that Measurement Year is royalty-free).
2. *Worked examples (illustrative only, not a warranty of tax or accounting treatment):*
   - A Product's Gross Revenue in a Measurement Year is $350,000. The royalty due is
     2% × ($350,000 − $200,000) = 2% × $150,000 = **$3,000**.
   - End users paid a storefront $350,000 for the Product in the Measurement Year; the
     storefront withheld its 30% revenue share ($105,000) and remitted $245,000 to You. Gross
     Revenue is still **$350,000** — the storefront's share is not deducted (§1, "Gross
     Revenue") — and the royalty due is the same **$3,000**.
   - A Product's Gross Revenue in a Measurement Year is $180,000. That is below the Royalty
     Threshold: **no royalty is due** for that Product for that year and no statement is
     required (§7).
3. Below the Royalty Threshold, use of the Engine is entirely free — the full Engine, with no
   subscription of any kind, no registration, and no AI usage required.
4. Royalty is calculated and owed **per Product**; revenue from one Product is never aggregated
   with another Product to reach the threshold.
5. Payment is due in USD within 45 days after the end of the Measurement Year in which the
   threshold was exceeded, together with the royalty statement described in §7. *(⚠️ currency
   conversion methodology for non-USD revenue is unspecified — flagged for counsel/finance.)*

### 6. The Royalty Is Unconditional — No Subscription or Service Nexus

The royalty in §5 is a fixed term of this EULA. It is **not waived, reduced, deferred, or
otherwise affected** by any subscription to, purchase of, or usage of any product or service of
Licensor or of any third party — including any AI or cloud service. This EULA and the Engine
are **fully independent of any such service**: no subscription is ever required to use the
Engine (§2), holding one never changes the royalty, and Your choice of AI tooling — any
provider's, Your own, or none at all — is irrelevant to this EULA. Any change to the royalty's
amount, threshold, or conditions can be made only prospectively, for a future Engine Version,
per §10. A separate written agreement under §3 permits only the act it describes and does not
affect the royalty unless it says so expressly and is signed by Licensor; nothing in this EULA
prevents Licensor and You from agreeing to different royalty terms in a separate signed
agreement that expressly modifies §5.

### 7. Reporting, Records, and Audit

1. **Self-reporting.** You must monitor Your own Product revenue. If a Product's Gross Revenue in
   a Measurement Year exceeds the Royalty Threshold, You must, within 45 days after that
   Measurement Year ends, submit to Licensor a royalty statement showing Gross Revenue, the
   exclusions applied, and the royalty calculation, together with payment.
2. **Records.** You must keep records sufficient to verify Gross Revenue for each Product for at
   least three years after the end of the relevant Measurement Year.
3. **Audit.** Licensor (or an independent accountant engaged by Licensor under confidentiality)
   may, on at least 30 days' prior written notice and no more than once per calendar year per
   Product, inspect Your relevant books and records during normal business hours, solely to
   verify Gross Revenue and royalty calculations. If an audit reveals an underpayment of more
   than 5% for the audited period, You bear the reasonable cost of that audit in addition to the
   shortfall and any royalty otherwise owed. Licensor will keep confidential, and use solely to
   verify compliance with this EULA, any non-public information obtained through an audit; an
   engaged accountant reports to Licensor only the compliance conclusions and any shortfall
   amount, not Your underlying records, except as reasonably necessary to pursue an identified
   shortfall.
4. **No telemetry.** Consistent with the Engine's contractual-only enforcement principle, the
   Engine itself (EditorKernel, RuntimeKernel, or a shipped Product build) contains no
   license-server calls, no subscription or entitlement checks, no phone-home, and no revenue
   telemetry. Reporting is entirely self-directed and contractual.

### 8. Attribution and Notices

You must retain existing copyright and license headers in unmodified Engine source files You
redistribute (e.g., embedded in a Product's shipped assets, if source is included). No in-Product
attribution (e.g., a splash screen or credits line) is required by this EULA. *(Licensor may
choose to request voluntary "Made with Context Game Engine" attribution as a marketing matter —
that is a business, not legal, decision and is left open here.)*

### 9. Contributions

1. **External contributions are not yet accepted.** Until Licensor announces that a Contributor
   License Agreement (CLA) signing flow is in place (see
   [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md)), pull requests from external
   contributors will be closed without merging, and no rights in a submission made anyway are
   granted to or acquired by Licensor. Issues, bug reports, and design discussion are welcome.
2. **When contributions open**, a Contribution will be accepted and incorporated **only** under
   a CLA providing **copyright assignment** to Licensor (or, where assignment is not legally
   available in the Contributor's jurisdiction, an exclusive, irrevocable, unrestricted,
   sublicensable license), per the CLA skeleton in [`.github/CLA.md`](.github/CLA.md). A
   Developer Certificate of Origin (DCO) sign-off alone is **not** sufficient. Licensor's full,
   unencumbered ownership of the Engine's copyright is what keeps this EULA — including the
   royalty terms and any future relicensing — enforceable now that the source is public.

### 10. Version-Locked Terms

The version of this EULA published alongside a given Engine Version governs Your use of **that**
Engine Version and any Product built with it, even if Licensor later publishes a different EULA
for a subsequent Engine Version. Licensor may change these terms for future Engine Versions;
such changes are never retroactive to a Product built solely on an earlier, unchanged Engine
Version. If You upgrade a Product to a newer Engine Version, the EULA published with the Engine
Version You then adopt governs Your use going forward. A future Engine Version's EULA may also
be published by a successor Licensor under §15(7); such a succession changes the identity of the
Licensor, never the terms locked to earlier Engine Versions. *(⚠️ the precise mechanics for a
Product that upgrades mid-Measurement-Year are not fully specified — flagged for counsel.)*

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
   before termination), 12, 13, 14, 15, and 16 survive termination.
6. For clarity, if You continue to develop, build, or distribute a Product after termination
   in breach of §11(3), You remain liable for the royalty under §5 on all Gross Revenue of
   that Product arising after termination (calculated as if the royalty terms remained in
   force for that purpose only), in addition to any other remedy available to Licensor for the
   breach, including for copyright infringement.

### 12. Disclaimer of Warranty

THE ENGINE IS PROVIDED **"AS IS,"** WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
WITHOUT LIMITATION THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
AND NON-INFRINGEMENT. LICENSOR HAS NO OBLIGATION TO PROVIDE SUPPORT, MAINTENANCE, UPDATES, OR
BUG FIXES. *(⚠️ some jurisdictions do not allow the exclusion of certain implied warranties for
consumer transactions — a savings clause may be needed; flagged for counsel.)*

### 13. Limitation of Liability

TO THE MAXIMUM EXTENT PERMITTED BY LAW, LICENSOR WILL NOT BE LIABLE FOR ANY INDIRECT,
INCIDENTAL, SPECIAL, CONSEQUENTIAL, OR PUNITIVE DAMAGES, OR ANY LOSS OF PROFITS, REVENUE, DATA,
OR GOODWILL, ARISING OUT OF OR RELATED TO THIS EULA OR THE ENGINE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGES. LICENSOR'S TOTAL AGGREGATE LIABILITY UNDER THIS EULA WILL NOT EXCEED
THE GREATER OF (A) USD $100, OR (B) THE ROYALTIES YOU ACTUALLY PAID LICENSOR UNDER THIS EULA FOR
THE PRODUCT GIVING RISE TO THE CLAIM IN THE 12 MONTHS BEFORE THE CLAIM AROSE.

### 14. Governing Law and Disputes

This EULA is governed by the laws of **[JURISDICTION — counsel to determine]**, without regard to
conflict-of-laws principles. *(⚠️ Placeholder pending counsel — Washington State is the expected
choice, consistent with the Licensor's location; the final call belongs to counsel together with
the entity-formation decision.)* Any dispute not resolved informally within 30 days may be
brought exclusively in the courts of [VENUE — counsel to determine], and each party consents to
that venue's jurisdiction.

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
7. **Licensor succession.** Licensor may assign this EULA, and/or transfer the Engine and its
   copyrights, without Your consent, to a legal entity that assumes Licensor's rights and
   obligations under it **in full, without reducing any guarantee, right, or remedy this EULA
   gives You** (including in connection with entity formation, corporate reorganization, or a
   sale of the Engine business); upon that assignment the successor becomes the "Licensor"
   under this EULA, bound by all of the original Licensor's obligations under it. Licensor
   will identify the successor in the EULA published with a subsequent Engine Version (§10);
   the succession does not change the terms that govern any earlier Engine Version.

### 16. If You Are a Consumer

If applicable law characterizes You as a consumer in connection with this EULA (a natural
person acting wholly or mainly outside a trade, business, craft, or profession), then,
notwithstanding anything else in this EULA: (a) nothing in §12 or §13 excludes or limits any
liability, warranty, right, or remedy that applicable law does not permit to be excluded or
limited in a contract with a consumer (including liability for death, personal injury, or
fraud, and any non-waivable statutory right); (b) §12 and §13 apply to the fullest extent, and
only to the extent, permitted by applicable consumer-protection law; and (c) this §16 prevails
over any conflicting provision solely as applied to You in that capacity, and does not
otherwise narrow this EULA as applied to a Licensee acting in a trade, business, or
professional capacity.

---

## Version history

- **v0.2.1 (2026-07-04)** — Business legal round-2 clause-level markup folded: skill/knowledge
  carve-out defining "created using the Engine's source code" (§1); two safe-harbor additions
  (third-party compute/CI infrastructure running a licensed Engine; Product-dependent
  modding/UGC SDKs); field-of-use reformation + per-jurisdiction severance hook (§3);
  related-party arm's-length FMV rule, shared-virtual-currency attribution rule, and a bundle
  fair-value floor (§1 "Gross Revenue", with a new "Affiliate" definition); remaster
  threshold-reset guard (§1 "Product"); GitHub fork→local-clone seam sentence and an EU/UK
  business-capacity representation (§2 Acceptance); §6↔§3 separate-agreement reconciler. Same
  day, independent legal-compliance review folded: new **§16 consumer savings clause**;
  royalty dual characterization in §2 (license condition AND independent contractual
  obligation); "Your affiliated development team" defined; §7(3) audit confidentiality duty on
  Licensor; §11(6) post-termination royalty accrual; §15(7) succession qualified "in full,
  without reducing any guarantee"; survival list extended to §16; CI/build-infrastructure safe
  harbor enriched; CLA.md banner aligned with the interim contribution freeze.
- **v0.2 (2026-07-03)** — Owner rulings encoded: royalty base is **gross receipts** (no
  storefront/platform-fee netting); the **subscription royalty waiver is REMOVED** and the EULA
  fully decoupled from ai-game.dev (former §6 replaced by an unconditionality clause — no
  subscription or service nexus). Licensor-succession mechanics added (§15(7), §10) for the
  planned entity cutover; the contributions section aligned with the interim
  external-contribution freeze and pointed at the in-repo CLA skeleton. Polish: "Competing
  Engine Product" tightened (operative restriction narrowed to source code and derivatives; a
  hosted/managed-service vector; an explicit safe-harbor list); Product
  edition/demo/early-access/remaster rules and a bundle-allocation rule; an express acceptance
  clause (§2); storefront-cut and below-threshold worked examples (§5).
- **v0.1 (2026-07-02)** — initial AI-drafted working draft from the owner-ruled business terms.
