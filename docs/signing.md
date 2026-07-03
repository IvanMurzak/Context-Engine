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
| Verify-before-use gate | `tools/verify_artifact.py` | ✅ implemented + tested |
| Pinned **production** trust root | `tools/trust-root/allowed_signers` | ✅ committed, **intentionally empty until first release** |
| TEST-ONLY trust root + fixtures | `tools/tests/fixtures/` | ✅ committed (public key + sample signature only — **no private key**) |
| Fail-closed test coverage (R-QA-013) | `tools/tests/test_verify_artifact.py` | ✅ 16 tests: good sig passes; bad / missing / tampered / untrusted-key / wrong-namespace / wrong-identity / missing-trust-root / absent-ssh-keygen / verifier-timeout all fail closed |

The **production signing key is NOT minted yet** (see § Minting). Because no production
key is pinned, the default gate refuses every artifact — the correct fail-closed state
while nothing first-party is published.

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
(R-BUILD-004), fetched engine versions (R-VER-004), and cross-domain cache entries. **None
of those are first-party-published yet** — there are no engine releases, no toolchain
mirror, no export templates. So today the gate is the *ready mechanism*, wired in at the
points below as each acquisition path starts serving first-party signed artifacts:

- **R-VER-004 versioned fetch (next).** When the launcher/fetcher (second release) fetches
  a pinned engine version into `versions/<semver>/` (see `versioned-install.md`), it MUST
  call `verify_artifact.py` on the downloaded archive against its detached signature
  **before unpacking/executing** — verify-before-execute, fail closed.
- **L-42 toolchain fetch.** The `.github/actions/pinned-toolchain` action installs a pinned
  compiler. It fetches from upstream distributions today (apt.llvm.org) that carry their
  **own** signing, which the gate does not replace — our root signs **first-party**
  artifacts. When the engine mirrors/repackages a toolchain as a first-party artifact, that
  mirrored artifact is signed with the production key and verified through this gate.
- **Third-party CI downloads** (emsdk, wgpu-native, CEF, wasmtime/WAMR) are **out of scope**
  for this root: they are upstream third-party artifacts authenticated by their own
  publishers. Do **not** point this gate at them — it would fail closed on artifacts our
  root never signed.

Until a first-party signed artifact exists, the deliverable is the **gate + trust root +
fail-closed tests**, exactly as R-SEC-009's v1 staging intends.

## Minting the production key (first release — owner decision)

**Do this once, at the first release.** It produces the single publisher key R-SEC-009's v1
posture calls for (TUF/Sigstore-style signed metadata is the v2 upgrade behind the same
verify-before-use gate).

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

### Key custody — the owner's choice at first release

The private key must never live in the repo. Pick one custody model:

- **A. GitHub Actions repo/organization secret.** Store the private key as an encrypted
  secret (e.g. `RELEASE_SIGNING_KEY`); the release workflow writes it to a temp file, signs,
  and deletes it. *Pro:* fully automated releases. *Con:* the key material is reachable by
  any workflow with the secret in scope — tighten with environment scoping (below) and
  minimal `permissions:`.
- **B. Environment-protected secret (recommended).** Bind the secret to a protected
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
