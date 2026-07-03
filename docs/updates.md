# Auto-update: design, trust model, and operations

YADUA can check for, verify, and install its own updates. Because the GUI runs
**elevated** (raw NTFS volume access), a subverted updater would be
SYSTEM-level code execution on every user's machine, so the design is
deliberately conservative: it is **consent-gated** (it never installs without
the user clicking Update) and it **fails closed** (any error — network,
signature, hash, version — means no update, never "install anyway").

## How it works

1. **Check.** On launch (opt-out) or from *About → Check now*, YADUA fetches
   two small files from the project's GitHub **latest** release over TLS:
   - `latest.yupd` — a text manifest: the version, release-notes URL, and one
     line per installer asset (`arch`, `kind`, `size`, `sha256`, `filename`).
   - `latest.yupd.sig` — a detached 64-byte ECDSA P-256 signature over the
     exact bytes of `latest.yupd`.

   It uses the fixed URL `…/releases/latest/download/<asset>`, which GitHub
   redirects to the newest **non-prerelease** release — so there is no API
   token and no releases-API JSON to parse.

2. **Verify the manifest.** YADUA computes SHA-256 of the downloaded manifest
   bytes and verifies the signature against the **public key compiled into the
   binary** (`src/update_pubkey.h`) using Windows CNG/BCrypt. Nothing inside the
   manifest is trusted until this passes.

3. **Downgrade protection.** The manifest version is parsed and compared to the
   running build's version (from its `VERSIONINFO`). Anything not **strictly
   newer** is ignored, so an attacker cannot roll a user back to an older,
   signed-but-vulnerable release.

4. **Consent.** If (and only if) a strictly-newer, signature-verified release
   exists, the toolbar shows *Update available* with **Update now / Release
   notes / Skip this version**. Nothing is downloaded until the user acts.

5. **Download + verify the installer.** The named installer is streamed to a
   per-user staging directory under the elevated profile's `%LOCALAPPDATA%`
   (`…\YADUA\updates\`), hashing as it writes. Its **size and SHA-256 are
   checked against the signed manifest**; a mismatch deletes the file and
   aborts. The staging dir is not writable by unprivileged users, which closes
   the time-of-check/time-of-use gap before launch.

6. **Install.** The verified installer (the existing Inno Setup installer) is
   launched and YADUA exits so files can be replaced. Reusing the installer
   avoids hand-rolling replacement of a running elevated executable.

## Why a signature and not just SHA256SUMS

`SHA256SUMS.txt` is an **integrity** check served from the same channel as the
binaries: anyone who can tamper with that channel can rewrite both. The update
signature is an **authenticity** check anchored in a key the attacker does not
have. TLS protects transport, but the signature is the real trust anchor —
even a fully compromised download channel or GitHub asset upload cannot get
past verification, because the private key never touches the download path.

## Threats and controls

| Threat | Control |
|---|---|
| MITM / DNS hijack serving fake metadata or binary | HTTPS **and** signature; the pinned key — not TLS — is the trust anchor |
| Compromised GitHub account uploads a malicious asset | Detached signature verified against the embedded public key; private key is not on the upload path |
| Rollback to an old, signed, vulnerable release | Strict newer-version requirement (downgrade protection) |
| Metadata swap (old binary, new version number) | The **manifest** is signed (version + per-asset hash), not just the binary |
| Tampered file between verify and launch (TOCTOU) | Staged in an admin-profile dir; size+hash verified on the exact bytes on disk |
| Compromised **build** pipeline | The signature would faithfully sign malware, so build integrity matters too: pin Actions to commit SHAs, build-provenance attestation (SLSA), least-privilege tokens |
| Any failure | Fail closed — no update |

Complementary, on the roadmap: **Authenticode** code signing (stops SmartScreen
/ AV false positives). Authenticode is *not* a substitute for the pinned-key
check — it trusts the whole CA ecosystem rather than your specific key.

## Key custody (the most important part)

- **Generate the keypair offline**, once, with `tools/gen-signing-key.ps1`.
  Only the **public** half is committed (`src/update_pubkey.h`) and embedded in
  the binary. The **private** half must never be committed or placed anywhere a
  build/release could read it beyond the one signing step.
- Store the private key as the GitHub Actions secret **`YADUA_SIGNING_KEY`**.
  For stronger custody, keep it **offline / in a hardware token** and sign
  releases from a machine the repo cannot reach; a CI secret is convenient but
  reachable by anyone who can alter the release workflow.
- Restrict who can push tags / edit `.github/workflows`, require 2FA, and
  consider a protected `release` environment with required reviewers gating the
  secret.
- **Rotation:** regenerating the key invalidates every prior signature, and
  users on old builds embed the *old* public key, so they will not accept
  updates signed by the new key — they must reinstall once. Only rotate on
  suspected compromise. There is intentionally no in-band "trust a new key"
  message: that would let a stolen key authorize its own replacement.

## Enabling it (one-time setup)

Until a real key is embedded, `src/update_pubkey.h` holds an all-zero
**placeholder**; `UpdaterConfigured()` returns false and the whole feature is
disabled (the About box says so). To turn it on:

1. `pwsh tools/gen-signing-key.ps1` (offline). This writes the private key to
   `.keys/yadua-signing-key.pem` (git-ignored) and rewrites
   `src/update_pubkey.h` with the real public key.
2. In the GitHub repo: **Settings → Secrets and variables → Actions →** new
   secret `YADUA_SIGNING_KEY` = the full contents of the `.pem`.
3. Paste the printed fingerprint into `README.md` so users can cross-check it
   (About shows the same value).
4. Commit `src/update_pubkey.h` (never the `.pem`) and cut a release. The
   release workflow signs `latest.yupd` automatically.
5. Vault or delete the local `.pem` once the secret is set.

## Privacy

The launch check is a request to GitHub and reveals the client IP/timing. It is
opt-out in *About → Check for updates on launch* (persisted in
`%LOCALAPPDATA%\YADUA\settings.txt`). Dev builds (version `0.0.0`) never check.
