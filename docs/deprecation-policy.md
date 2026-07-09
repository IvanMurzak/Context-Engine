# Contract deprecation policy (R-CLI-010)

The public contract — the CLI verb surface, the RPC methods, the built-in MCP tools, the core/verb
flags, and the advertised capabilities — is **versioned** and evolves under a **written deprecation
lifecycle** so scripts and agents never break silently across engine updates. This document is the
normative home of that policy; the machinery it describes is generated from the single registry
(R-CLI-009) and self-described by `context describe --json` (R-CLI-013).

## Staged activation — inert until the M3 freeze

The contract ships **explicitly UNSTABLE at `protocolMajor = 0`** through M1/M3-entry. While
`protocolMajor` is `0` the contract **MAY break without a deprecation cycle**, and `context describe`
says so loudly (`contract.protocol.note` + `contract.deprecationPolicy.note`) so no client mistakes
the pre-freeze surface for a frozen one.

The deprecation lifecycle **activates at the M3 freeze**, when `protocolMajor` bumps to `1` (a
separate, owner-gated task — *not* this one; this task lands the machinery with `protocolMajor`
staying `0`). From that point the rules below are binding, and
`contract.deprecationPolicy.active` reads `true`.

## The lifecycle

A verb, RPC method, MCP tool, flag, or capability that is being retired is:

1. **Marked `deprecated: true`** in the registry, carrying a **`removedIn`** version — the protocol
   version it is scheduled to be removed in. Both fields surface in `context describe --json` (see
   *Introspection surface* below).
2. **Retained for at least `N` minor versions** after it is first marked deprecated, giving clients a
   bounded migration window. **`N = 2`** (surfaced as
   `contract.deprecationPolicy.minMinorsBeforeRemoval`). Two minors is a deliberate balance: long
   enough that an agent or script pinned a minor or two behind still resolves the entry, short enough
   that the surface does not accrete dead grammar indefinitely.
3. **Removed** no earlier than the `removedIn` version, and only once the ≥ `N`-minor window has
   elapsed.

### Method-id stability guarantee

**Stable ids never change across the lifecycle.** A verb's `rpcMethod` and `mcpTool` (the R-CLI-004
method-ids), and a flag's name (its grammar slot), are **fixed from registration through removal**. A
client that stored an id keeps resolving it for the entire compatibility window — deprecation changes
the *metadata* on an entry, never its identity. `describe` surfaces this as
`contract.deprecationPolicy.methodIdStability = "stable-across-lifecycle"`.

### Compatibility window (negotiation)

Version compatibility is **negotiated, not equality-checked** (R-CLI-010, superseding the original
version-equality predicate). The attach handshake exchanges `{ protocolMajor, capabilities[],
minClientProtocol }`; an in-window client **degrades to the negotiated capability subset**, and a
client **outside the window hard-fails** through the R-CLI-008 error envelope (never a silent
degrade).

**v1 behavior is hard-fail-on-mismatch.** With only one released protocol version there is nothing to
negotiate with, so v1 carries the wire fields but does not yet degrade. The
negotiation/degradation behavior **activates at the second released protocol version**, in lockstep
with the deprecation lifecycle. This trims v1 implementation, not the contract shape.

## Introspection surface (`context describe --json`)

The policy and the per-entry metadata are both machine-readable:

- **Per-entry metadata.** Every verb (`contract.verbs[]`, `contract.rpcMethods[]`,
  `contract.mcpTools[]`) and every flag (`contract.coreFlags[]` and each verb's `flags[]`) always
  carries a boolean **`deprecated`**. When `deprecated` is `true` the entry additionally carries
  **`removedIn`** (the removal version). A non-deprecated entry omits `removedIn`.
- **Policy section.** `contract.deprecationPolicy` documents the rules:
  - `requirement` — `"R-CLI-010"`.
  - `active` — `false` while `protocolMajor = 0`; `true` once the freeze activates the lifecycle.
  - `minMinorsBeforeRemoval` — the `N`-minor retention window (currently `2`).
  - `methodIdStability` — `"stable-across-lifecycle"`.
  - `perEntryFields` — `["deprecated", "removedIn"]`, the per-entry field names above.
  - `appliesTo` — `["verb", "rpcMethod", "mcpTool", "flag", "capability"]`.
  - `compatibilityWindow` — the negotiation intent (hard-fail outside; behavior activates at the 2nd
    released protocol version).
  - `note` — the pre-freeze "may break without a cycle" caveat.

## No live deprecations at the freeze

There are **no real deprecated entries** at the freeze — every registered verb and flag has
`deprecated: false`. The machinery is inert until the first genuine deprecation flows through the same
registry projection. The conformance test (`contract-test_deprecation`) locks both facts: no real
surface is deprecated, and an *example* (test-only) deprecated entry correctly advertises
`deprecated` / `removedIn` while keeping its stable method-id.
