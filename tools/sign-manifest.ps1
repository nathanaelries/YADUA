# ============================================================================
# Builds and signs the update manifest for a release. Invoked by the release
# workflow with the private key in $env:YADUA_SIGNING_KEY (a PKCS#8 PEM).
#
# Writes into -DistDir:
#   latest.yupd      text manifest: version + one line per installer asset
#                    (arch, kind, size, sha256, filename)
#   latest.yupd.sig  raw 64-byte ECDSA P-256 (IEEE P1363 r||s) signature over
#                    the exact bytes of latest.yupd
#
# The client (src/updater.cpp) verifies the signature over the downloaded
# manifest bytes against the embedded public key BEFORE trusting any field,
# then checks each installer's size + sha256 against this manifest.
#
# If YADUA_SIGNING_KEY is not set, the step is skipped (no update files) so
# releases still publish before the key is configured. Requires PowerShell 7+.
# ============================================================================
param(
    [Parameter(Mandatory)][string]$Version,   # e.g. 1.2.3
    [Parameter(Mandatory)][string]$DistDir,
    [string]$Repo = 'nathanaelries/YADUA'
)
$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw "Run this in PowerShell 7+ (pwsh)."
}

$pem = $env:YADUA_SIGNING_KEY
if ([string]::IsNullOrWhiteSpace($pem)) {
    Write-Warning "YADUA_SIGNING_KEY is not set - skipping update manifest signing."
    Write-Warning "Releases will not carry auto-update metadata until the secret exists."
    return
}

# --- build the manifest ----------------------------------------------------
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('yadua-update 1')
$lines.Add("version $Version")
$lines.Add("notes https://github.com/$Repo/releases/tag/v$Version")

$installers = Get-ChildItem $DistDir -Filter 'YADUA-setup-*.exe' | Sort-Object Name
if ($installers.Count -eq 0) { throw "No installer assets found in $DistDir." }
foreach ($f in $installers) {
    $arch = if ($f.Name -match '-(amd64|arm64)\.exe$') { $Matches[1] } else { 'unknown' }
    $sha  = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()
    $lines.Add("asset $arch installer $($f.Length) $sha $($f.Name)")
}

# LF newlines, UTF-8 without BOM - the client verifies these exact bytes.
$text  = ($lines -join "`n") + "`n"
$bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
$manifestPath = Join-Path $DistDir 'latest.yupd'
[System.IO.File]::WriteAllBytes($manifestPath, $bytes)

# --- sign ------------------------------------------------------------------
$ec = [System.Security.Cryptography.ECDsa]::Create()
$ec.ImportFromPem($pem)
# 2-arg SignData returns IEEE P1363 (raw r||s), which is what BCrypt expects.
$sig = $ec.SignData($bytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256)
if ($sig.Length -ne 64) {
    throw "Unexpected signature length $($sig.Length); expected 64 (P-256 r||s)."
}

# Self-check: verify with the public half before publishing.
if (-not $ec.VerifyData($bytes, $sig,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256)) {
    throw "Self-verification of the signature failed - refusing to publish."
}

[System.IO.File]::WriteAllBytes((Join-Path $DistDir 'latest.yupd.sig'), $sig)

Write-Host "Signed update manifest for $Version :"
Write-Host $text
