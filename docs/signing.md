# Artifact signing & the trust root (R-SEC-009 / L-58)

**The trust anchor.** R-SEC-009 requires a **pinned cryptographic root of trust** and
**per-artifact detached signatures**, with **mandatory verify-before-use** for every
executable or trust-bearing artifact — engine binaries, the pinned native toolchain
(L-42), per-platform export templates (R-BUILD-004), fetched engine versions (R-VER-004),
and cross-trust-domain cache entries. Verification **fails closed**: an artifact that does
not verify against the pinned root is **refused, not used with a warning**. This page is
how this repository implements that anchor and how the owner mints the production key at
the first release.

## What ships today

| Piece | Path | State |
| --- | --- | --- |
| Verify-before-use gate (Python) | `tools/verify_artifact.py` | ✅ implemented + tested |
| Verify-before-use gate (C++ mirror) | `src/common/verify_signature.{h,cpp}` | ✅ the `ssh-keygen -Y verify` seam the C++ build uses (no Python needed) |
| Pinned **production** trust root | `tools/trust-root/allowed_signers` | ✅ committed, **one production key PINNED** (a08) — `context-engine-release`, `SHA256:4f8ZHq0vI6mKLm8MvKYE8PPslZu5IuA8ZUxtWuZ434U` |
| R-BUILD-004 export-template / toolchain verify | `src/cli/build_command.cpp` | ✅ `context build --runtime-sig / --toolchain-sig` verify-before-use → `build.template_unverified` / `build.toolchain_fetch_failed` |
| R-VER-004 versioned-fetch verify seam | `tools/versioned_fetch.py` | ✅ verify-before-execute seam for a fetched `versions/<semver>/` (the fetcher itself is second-release) |
| Signing release workflow (custody model B) | `.github/workflows/release-sign.yml` | ✅ environment-protected `release` job (required reviewer) signs + verifies via `ssh-keygen -Y sign` |
| TEST-ONLY trust root + fixtures | `tools/tests/fixtures/` | ✅ committed (public key + sample signature only — **no private key**) |
| Fail-closed test coverage (R-QA-013) | `tools/tests/test_verify_artifact.py`, `test_versioned_fetch.py`, `src/common/tests/test_verify_signature.cpp`, `src/cli/tests/test_build_command.cpp` | ✅ good sig passes; bad / missing / tampered / untrusted-key / wrong-namespace / wrong-identity / missing-trust-root / absent-ssh-keygen / verifier-timeout all fail closed — across the Python gate, the versioned-fetch seam, the C++ gate, and the build wiring |

The **production signing key is minted and PINNED** (task a08). The single publisher key
`context-engine-release` is pinned in `tools/trust-root/allowed_signers` (public half only); its
private half is held ONLY as the environment-protected `RELEASE_SIGNING_KEY` secret (custody model
B — see § Key custody). Because the production root pins exactly this one key, the default gate
**accepts only signatures by that key** (under principal `context-engine-release` / namespace
`context-engine-artifact`) and **refuses everything else** — the correct fail-closed posture.

## Mechanism — OpenSSH Ed25519 signatures

- **Algorithm: Ed25519 ONLY.** A single standard algorithm, no agility, no downgrade
  surface. Ed25519 is a public standard-crypto algorithm, so (business/legal, 2026-07-02)
  its use requires **no EAR export notification** and costs nothing.
- **Tooling: `ssh-keygen -Y` (OpenSSH).** The gate is a thin wrapper over `ssh-keygen -Y
  verify`. Rationale over a Python crypto library: **zero third-party Python dependencies**
  — `ssh-keygen` ships with OpenSSH and is present on every CI runner (ubuntu/macos/windows)
  and dev machine, so nothing is added to the `python-tests` job's `pip install` line and no
  native crypto wheel is vendored. It also delegates the actual crypto to a widely-audited
  implementation rather than to bespoke code.
- **Trust root: an OpenSSH `allowed_signers` file** (see *ALLOWED SIGNERS* in
  `ssh-keygen(1)`), committed in-repo — the pinned, **non-TOFU** root. Each line names a
  principal, the namespaces it may sign in, its key type, and the public key.
- **Signature: a detached armored blob** (`-----BEGIN SSH SIGNATURE-----`) produced by
  `ssh-keygen -Y sign -n <namespace>`. The **namespace** (`context-engine-artifact`)
  domain-separates Context Engine artifact signatures from any other use of the same key —
  a signature minted for another context cannot be replayed here.

### Using the gate

```bash
python3 tools/verify_artifact.py \
    --artifact  path/to/artifact \
    --signature path/to/artifact.sig \
    [--trust-root tools/trust-root/allowed_signers] \  # default
    [--identity  context-engine-release]            \  # default
    [--namespace context-engine-artifact]              # default
```

Exit codes (**any non-zero == REFUSED, do not use the artifact**):

- `0` — verified; the artifact is authentic under the pinned trust root.
- `1` — verification FAILED (bad / missing / tampered / untrusted-key / wrong-namespace
  signature) — the fail-closed refusal.
- `2` — configuration/usage error (trust root missing/unreadable, artifact missing, or
  `ssh-keygen` not found) — still a refusal; the gate could not run.

There is also a Python API — `verify_artifact.verify_artifact(...) -> VerifyResult` — for
callers that want the structured result rather than a process exit code.

## Where the gate plugs in

R-SEC-009's subjects are engine binaries, the pinned toolchain (L-42), export templates
(R-BUILD-004), fetched engine versions (R-VER-004), and cross-domain cache entries. No
first-party signed artifact is *published* yet (there are no engine releases), so the seams
below are the **ready mechanism**, wired in and **opt-in**: each activates the moment a
detached signature is supplied on its path, and fails closed once its artifacts are signed.

- **R-BUILD-004 export template + engine-fetched toolchain (wired, task a08).** `context build`
  verifies-before-use the per-platform export template (the shipped `--runtime` host binary) and
  an engine-fetched/mirrored toolchain artifact against the pinned trust root, via the C++ gate
  `context::common::verify_signature` (`src/common/verify_signature.cpp` — the `ssh-keygen -Y
  verify` mirror of this Python gate, so a headless build needs no Python). Supply `--runtime-sig`
  / `--toolchain-sig` (+ `--trust-root`); a template/toolchain that does not verify refuses the
  build **fail-closed** with `build.template_unverified` / `build.toolchain_fetch_failed`
  (registered in the one contract catalog, so CLI ≡ RPC ≡ MCP). Verification is opt-in until export
  templates are first-party-signed, so an unsigned local/CI build is unchanged.
- **R-VER-004 versioned fetch (verify seam wired; fetcher is second-release — task a08).** The
  verify-before-execute seam is `tools/versioned_fetch.py`: the future launcher/fetcher fetches a
  pinned engine version into `versions/<semver>/` (see `versioned-install.md`) and calls
  `versioned_fetch.require_verified_before_execute(archive, signature)` **before
  unpacking/executing** — fail closed. It is a thin wrapper over `verify_artifact.py` (same trust
  root / principal / namespace).
- **L-42 toolchain fetch (upstream).** The `.github/actions/pinned-toolchain` action installs a
  pinned compiler from upstream distributions (apt.llvm.org) that carry their **own** signing,
  which this gate does not replace — our root signs **first-party** artifacts. When the engine
  mirrors/repackages a toolchain as a first-party artifact, that mirrored artifact is signed with
  the production key and verified through the export/toolchain seam above.
- **Third-party CI downloads** (emsdk, wgpu-native, CEF, V8, wasmtime/WAMR) are **out of scope**
  for this root: they are upstream third-party artifacts authenticated by their own publishers. Do
  **not** point this gate at them — it would fail closed on artifacts our root never signed.

The signing side is `.github/workflows/release-sign.yml` (custody model B): an environment-protected
`release` job signs each first-party artifact with `ssh-keygen -Y sign -n context-engine-artifact`
and publishes the `.sig` beside it, for these seams to verify against the pinned root.

## Minting the production key (DONE — task a08)

**Status: minted + pinned.** The single publisher key R-SEC-009's v1 posture calls for is minted;
its PUBLIC half is pinned in `tools/trust-root/allowed_signers` (principal `context-engine-release`,
`SHA256:4f8ZHq0vI6mKLm8MvKYE8PPslZu5IuA8ZUxtWuZ434U`) and its PRIVATE half is held as the
environment-protected `RELEASE_SIGNING_KEY` secret (custody model B, below). (TUF/Sigstore-style
signed metadata is the v2 upgrade behind the same verify-before-use gate.) The steps below record
how the key was produced and how each release artifact is signed against it.

1. **Generate the keypair** (Ed25519), offline, on a trusted machine:
   ```bash
   ssh-keygen -t ed25519 -C "context-engine-release <YYYY-MM-DD>" -f context-engine-release
   # -> context-engine-release  (PRIVATE — never commit, never leave this machine unwrapped)
   #    context-engine-release.pub (public)
   ```
2. **Pin the public half** in `tools/trust-root/allowed_signers` — append exactly one line:
   ```
   context-engine-release namespaces="context-engine-artifact" ssh-ed25519 AAAA...<pubkey> context-engine-release <YYYY-MM-DD>
   ```
   Commit that change. This is a **signed, versioned operation** (a normal reviewed commit);
   key *rotation* later follows the same append/replace-then-commit discipline.
3. **Sign each release artifact** at publish time and attach the `.sig` alongside it:
   ```bash
   ssh-keygen -Y sign -f <private-key> -n context-engine-artifact <artifact>
   # -> <artifact>.sig  (publish next to the artifact)
   ```
4. **Consumers verify before use** with `verify_artifact.py` (defaults already target the
   production trust root, the `context-engine-release` principal, and the
   `context-engine-artifact` namespace).

### Key custody — model B is ACTIVE (task a08)

The private key must never live in the repo. The chosen model is **B** — the private key is the
`RELEASE_SIGNING_KEY` secret bound to the protected `release` GitHub Environment (required
reviewers), and `.github/workflows/release-sign.yml` writes it to a private temp file, signs, and
deletes it (never echoing it). The alternatives are recorded for completeness:

- **A. GitHub Actions repo/organization secret.** Store the private key as an encrypted
  secret (e.g. `RELEASE_SIGNING_KEY`); the release workflow writes it to a temp file, signs,
  and deletes it. *Pro:* fully automated releases. *Con:* the key material is reachable by
  any workflow with the secret in scope — tighten with environment scoping (below) and
  minimal `permissions:`.
- **B. Environment-protected secret (CHOSEN — active).** Bind the secret to a protected
  **GitHub Environment** (e.g. `release`) that requires **required reviewers** and/or a
  branch/tag filter, so signing only runs after a human approves the deployment. *Pro:*
  human-in-the-loop gate on every use of the key; smallest blast radius. *Con:* a manual
  approval step per release (usually desired for signing).
- **C. Offline / hardware-backed signing.** Keep the key on a hardware token or an offline
  machine; sign release artifacts manually and upload the `.sig` files. *Pro:* the private
  key never touches CI. *Con:* fully manual; best once release cadence is low-frequency and
  high-stakes.

**Recommendation:** start with **B** (environment-protected secret with required reviewers)
— it keeps releases automatable while making every use of the signing key a human-approved
action, matching the org's human-approval posture for high-trust operations. Revisit **C**
if/when the threat model warrants an air-gapped key.

## Rotation & revocation (forward note)

- **Rotation** is an append/replace in `allowed_signers` + a reviewed commit (a signed,
  versioned operation per R-SEC-009). Keep the retired key's line until every artifact it
  signed is out of support, then remove it.
- **Revocation** in v1 is removal of the compromised key's line (fail-closed: anything it
  signed stops verifying). Signed-metadata revocation (thresholds, transparency) arrives with
  the v2 TUF/Sigstore upgrade behind the same gate.
