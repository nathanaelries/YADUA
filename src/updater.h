// ============================================================================
// YADUA auto-updater (consent-gated).
//
// Trust model: releases carry a small text manifest (latest.yupd) plus a
// detached ECDSA P-256 signature over it (latest.yupd.sig). The matching
// PUBLIC key is compiled into the binary (src/update_pubkey.h); the private
// key never touches the download path. The updater fetches the manifest and
// signature from the project's GitHub "latest" release, verifies the
// signature against the embedded key, refuses anything that is not strictly
// newer (downgrade protection), then downloads the named installer and checks
// its size and SHA-256 against the *signed* manifest before doing anything
// with it. Every failure is fatal (fail closed): a bad signature, hash, or
// version means no update, never "install anyway".
//
// Nothing here downloads or installs on its own. CheckForUpdate only reports;
// the GUI asks the user before DownloadUpdate / LaunchInstaller run.
//
// See docs/updates.md for the full threat model, key custody, and the release
// signing process.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "scanner.h" // ScanProgress (reused for download progress)

namespace yadua {

struct UpdateInfo {
    bool         Available = false; // a strictly-newer, signature-verified release
    std::wstring Version;           // manifest version, e.g. L"1.2.3"
    std::wstring AssetName;         // installer asset for this build's arch
    std::wstring NotesUrl;          // release notes page
    std::string  Sha256;            // lowercase hex of the installer (from manifest)
    uint64_t     Size = 0;          // installer size in bytes (from manifest)
};

// False in builds with no real signing key embedded (the placeholder key in
// src/update_pubkey.h): the whole feature is disabled and CheckForUpdate
// reports "not configured". The GUI hides its update controls in that case.
bool UpdaterConfigured();

// SHA-256 fingerprint (lowercase hex) of the embedded public key, for display
// in the About box so a user can cross-check it against the README.
const char* UpdatePublicKeyFingerprint();

// The running executable's own version ("X.Y.Z") from its VERSIONINFO, or
// L"0.0.0" for an unstamped local build (used to suppress dev-build checks).
std::wstring RunningVersion();

// Fetches + verifies the signed manifest, compares against `currentVersion`,
// and fills `out`. Returns false and sets `error` on ANY failure (network,
// signature, parse, no matching asset). A clean check with nothing newer
// returns true with out.Available == false.
bool CheckForUpdate(const std::wstring& currentVersion, UpdateInfo& out,
                    std::wstring& error);

// Downloads the installer named in `info` into a per-user (admin-profile)
// staging directory, verifying size and SHA-256 against the signed manifest
// as it streams to disk. On success `installerPath` is the verified file; on
// any mismatch the partial file is deleted and false is returned. `progress`
// (optional) reports BytesRead / TotalBytes.
bool DownloadUpdate(const UpdateInfo& info, std::wstring& installerPath,
                    std::wstring& error, ScanProgress* progress = nullptr);

// Launches the already-verified installer (elevated, inheriting our token).
// The caller should quit the app right after so files can be replaced.
bool LaunchInstaller(const std::wstring& installerPath, std::wstring& error);

} // namespace yadua
