// ============================================================================
// YADUA auto-updater implementation. See updater.h for the trust model.
//
// Dependencies are all OS-native (no third-party crypto): WinHTTP for the
// TLS download, CNG/BCrypt for SHA-256 and ECDSA P-256 verification, the
// version API for the running build's version. TLS protects transport, but
// the signature verified against the embedded public key is the real trust
// anchor: even a fully compromised download channel cannot get past it.
// ============================================================================

#include "updater.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "update_pubkey.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shell32.lib")

namespace yadua {

namespace {

// The project's GitHub repo. GitHub's /releases/latest/download/<asset> path
// redirects to the newest non-prerelease release's asset, so the updater needs
// no API token and never has to parse the releases API — it fetches fixed
// asset names and lets the signed manifest name the installer.
constexpr wchar_t kLatestBase[] =
    L"https://github.com/nathanaelries/YADUA/releases/latest/download/";

// This build's architecture (compile-time = the arch of the running exe, which
// is what we must match, not the OS which could be emulating).
#if defined(_M_ARM64)
constexpr char kArch[] = "arm64";
#else
constexpr char kArch[] = "amd64";
#endif

inline bool NtOk(NTSTATUS s) { return s >= 0; }

std::wstring WFmt(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

// ---- CNG helpers ----------------------------------------------------------

// One-shot SHA-256. `out` must be 32 bytes.
bool Sha256(const void* data, size_t len, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    if (NtOk(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                         0)) &&
        NtOk(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0)) &&
        NtOk(BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0)) &&
        NtOk(BCryptFinishHash(h, out, 32, 0)))
        ok = true;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// Streaming SHA-256, for hashing a download without buffering it in memory.
struct Sha256Stream {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool Begin() {
        return NtOk(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                nullptr, 0)) &&
               NtOk(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0));
    }
    void Update(const void* p, size_t n) {
        if (h) BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0);
    }
    bool Finish(unsigned char out[32]) {
        return h && NtOk(BCryptFinishHash(h, out, 32, 0));
    }
    ~Sha256Stream() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
};

std::string ToHex(const unsigned char* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = d[p[i] >> 4];
        s[i * 2 + 1] = d[p[i] & 0xF];
    }
    return s;
}

bool EqualsIgnoreCaseAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

// Verify a raw (IEEE P1363, r||s, 64-byte) ECDSA P-256 signature over `hash`
// using the embedded public point (X, Y). Returns true only on a valid sig.
bool VerifyEcdsaP256(const unsigned char x[32], const unsigned char y[32],
                     const unsigned char hash[32], const unsigned char* sig,
                     size_t sigLen) {
    if (sigLen != 64) return false;

    // BCRYPT_ECCPUBLIC_BLOB: { ULONG Magic; ULONG cbKey; } X[32] Y[32].
    unsigned char blob[8 + 64];
    ULONG magic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC, cbKey = 32;
    memcpy(blob + 0, &magic, 4);
    memcpy(blob + 4, &cbKey, 4);
    memcpy(blob + 8, x, 32);
    memcpy(blob + 40, y, 32);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (NtOk(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM,
                                         nullptr, 0)) &&
        NtOk(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob,
                                 sizeof(blob), 0))) {
        NTSTATUS s = BCryptVerifySignature(key, nullptr, (PUCHAR)hash, 32,
                                           (PUCHAR)sig, (ULONG)sigLen, 0);
        ok = (s == 0); // STATUS_SUCCESS; anything else (incl. INVALID_SIGNATURE)
    }
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---- HTTP -----------------------------------------------------------------

struct HInet {
    HINTERNET h = nullptr;
    ~HInet() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

// GET `url` over TLS, following redirects (github.com -> the CDN host). Body
// is either accumulated into `body` (capped, for the small manifest/sig) or
// streamed to `file` while updating `hash`/`written`/`progress` (for the
// installer). Exactly one of `body` / `file` is used.
bool HttpFetch(const std::wstring& url, std::string* body, HANDLE file,
               Sha256Stream* hash, uint64_t* written, ScanProgress* progress,
               std::wstring& err) {
    constexpr size_t kBodyCap = 1u << 20; // 1 MiB ceiling for in-memory bodies

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[2048] = {0};
    uc.lpszHostName = host;      uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;       uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        err = L"malformed update URL";
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) { // never fetch updates over http
        err = L"refusing non-HTTPS update URL";
        return false;
    }

    HInet ses{WinHttpOpen(L"YADUA-Updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!ses) { err = L"WinHttpOpen failed"; return false; }

    // Require modern TLS.
    DWORD secure = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    secure |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &secure,
                     sizeof(secure));

    HInet con{WinHttpConnect(ses, host, uc.nPort, 0)};
    if (!con) { err = L"WinHttpConnect failed"; return false; }

    HInet req{WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE)};
    if (!req) { err = L"WinHttpOpenRequest failed"; return false; }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        err = WFmt(L"update server unreachable (%lu)", GetLastError());
        return false;
    }

    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (code != 200) { err = WFmt(L"update server returned HTTP %lu", code);
                       return false; }

    if (progress) {
        DWORD64 total = 0;
        DWORD tl = sizeof(total);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH |
                                         WINHTTP_QUERY_FLAG_NUMBER64,
                                WINHTTP_HEADER_NAME_BY_INDEX, &total, &tl,
                                WINHTTP_NO_HEADER_INDEX))
            progress->TotalBytes = total;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            err = L"download interrupted";
            return false;
        }
        if (avail == 0) break;
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &got)) {
            err = L"download read failed";
            return false;
        }
        if (got == 0) break;
        if (body) {
            if (body->size() + got > kBodyCap) { err = L"update metadata too large";
                                                 return false; }
            body->append(buf.data(), got);
        }
        if (file != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            if (!WriteFile(file, buf.data(), got, &wrote, nullptr) ||
                wrote != got) {
                err = L"failed writing update to disk";
                return false;
            }
        }
        if (hash) hash->Update(buf.data(), got);
        if (written) *written += got;
        if (progress)
            progress->BytesRead.fetch_add(got, std::memory_order_relaxed);
    }
    return true;
}

std::wstring LatestUrl(const std::wstring& asset) { return kLatestBase + asset; }

// ---- manifest parsing / version compare -----------------------------------

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// Parse up to three dotted numeric components; a "-prerelease" tail is ignored
// for ordering (GitHub's "latest" excludes prereleases, so the served
// manifest is always a full release anyway).
void ParseVer(const std::wstring& v, unsigned out[3]) {
    out[0] = out[1] = out[2] = 0;
    int part = 0;
    unsigned cur = 0;
    bool any = false;
    for (wchar_t c : v) {
        if (c >= L'0' && c <= L'9') {
            cur = cur * 10 + (unsigned)(c - L'0');
            any = true;
        } else if (c == L'.') {
            if (part < 3) out[part] = cur;
            ++part;
            cur = 0;
            if (part >= 3) break;
        } else {
            break; // '-', '+', space: end of the numeric core
        }
    }
    if (any && part < 3) out[part] = cur;
}

bool IsNewer(const std::wstring& candidate, const std::wstring& current) {
    unsigned a[3], b[3];
    ParseVer(candidate, a);
    ParseVer(current, b);
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false; // equal core version => not an upgrade (downgrade protection)
}

std::wstring WideOf(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---- staging directory ----------------------------------------------------

// Per-user staging dir under the (elevated) admin profile's LocalAppData —
// not writable by unprivileged users, which is what keeps the verify->launch
// step free of a tamper window.
bool StagingDir(std::wstring& out, std::wstring& err) {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { err = L"cannot locate LocalAppData"; return false; }
    std::wstring dir = std::wstring(base) + L"\\YADUA";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\updates";
    CreateDirectoryW(dir.c_str(), nullptr);
    out = dir;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

bool UpdaterConfigured() {
#ifdef YADUA_UPDATE_PUBKEY_PLACEHOLDER
    return false;
#else
    return true;
#endif
}

const char* UpdatePublicKeyFingerprint() {
    return YADUA_UPDATE_PUBKEY_FINGERPRINT;
}

std::wstring RunningVersion() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return L"0.0.0";
    DWORD ignored = 0;
    DWORD sz = GetFileVersionInfoSizeW(path, &ignored);
    if (!sz) return L"0.0.0";
    std::vector<unsigned char> buf(sz);
    if (!GetFileVersionInfoW(path, 0, sz, buf.data())) return L"0.0.0";
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &len) || !ffi)
        return L"0.0.0";
    return WFmt(L"%u.%u.%u", HIWORD(ffi->dwFileVersionMS),
                LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS));
}

bool CheckForUpdate(const std::wstring& currentVersion, UpdateInfo& out,
                    std::wstring& error) {
    if (!UpdaterConfigured()) {
        error = L"auto-update is not configured in this build (no signing key)";
        return false;
    }

    // Fetch the signed manifest and its detached signature.
    std::string manifest, sig;
    if (!HttpFetch(LatestUrl(L"latest.yupd"), &manifest, INVALID_HANDLE_VALUE,
                   nullptr, nullptr, nullptr, error))
        return false;
    if (!HttpFetch(LatestUrl(L"latest.yupd.sig"), &sig, INVALID_HANDLE_VALUE,
                   nullptr, nullptr, nullptr, error))
        return false;

    // Verify the signature over the EXACT manifest bytes before trusting any
    // field inside it.
    if (sig.size() != 64) { error = L"malformed update signature"; return false; }
    unsigned char digest[32];
    if (!Sha256(manifest.data(), manifest.size(), digest)) {
        error = L"hashing failed";
        return false;
    }
    if (!VerifyEcdsaP256(kYaduaUpdatePubKeyX, kYaduaUpdatePubKeyY, digest,
                         (const unsigned char*)sig.data(), sig.size())) {
        error = L"update signature is invalid - update rejected";
        return false;
    }

    // Parse the now-trusted manifest, line by line.
    UpdateInfo info;
    size_t pos = 0;
    while (pos < manifest.size()) {
        size_t nl = manifest.find('\n', pos);
        std::string line = manifest.substr(pos, nl == std::string::npos
                                                     ? std::string::npos
                                                     : nl - pos);
        pos = (nl == std::string::npos) ? manifest.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> t = Split(line);
        if (t.empty()) continue;
        if (t[0] == "version" && t.size() >= 2) {
            info.Version = WideOf(t[1]);
        } else if (t[0] == "notes" && t.size() >= 2) {
            info.NotesUrl = WideOf(t[1]);
        } else if (t[0] == "asset" && t.size() >= 6) {
            // asset <arch> <kind> <size> <sha256> <filename>
            if (t[1] == kArch && t[2] == "installer") {
                info.Size = _strtoui64(t[3].c_str(), nullptr, 10);
                info.Sha256 = t[4];
                for (char& c : info.Sha256)
                    if (c >= 'A' && c <= 'Z') c += 32;
                info.AssetName = WideOf(t[5]);
            }
        }
    }

    if (info.Version.empty()) { error = L"update manifest has no version";
                                return false; }
    if (info.AssetName.empty() || info.Sha256.size() != 64 || info.Size == 0) {
        error = L"update manifest has no installer for this architecture";
        return false;
    }

    info.Available = IsNewer(info.Version, currentVersion);
    out = info;
    return true;
}

bool DownloadUpdate(const UpdateInfo& info, std::wstring& installerPath,
                    std::wstring& error, ScanProgress* progress) {
    if (!UpdaterConfigured()) { error = L"auto-update is not configured";
                                return false; }
    if (info.AssetName.empty() || info.Sha256.size() != 64) {
        error = L"nothing to download";
        return false;
    }

    std::wstring dir;
    if (!StagingDir(dir, error)) return false;
    std::wstring path = dir + L"\\" + info.AssetName;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = L"cannot create the update staging file";
        return false;
    }

    Sha256Stream sha;
    if (!sha.Begin()) {
        CloseHandle(f);
        DeleteFileW(path.c_str());
        error = L"hashing init failed";
        return false;
    }

    uint64_t written = 0;
    bool okFetch = HttpFetch(LatestUrl(info.AssetName), nullptr, f, &sha,
                             &written, progress, error);
    CloseHandle(f);

    unsigned char digest[32];
    bool okHash = sha.Finish(digest);

    if (!okFetch) { DeleteFileW(path.c_str()); return false; }
    if (!okHash) { DeleteFileW(path.c_str()); error = L"hashing failed";
                   return false; }
    if (written != info.Size) {
        DeleteFileW(path.c_str());
        error = WFmt(L"update size mismatch (%llu vs expected %llu) - rejected",
                     (unsigned long long)written, (unsigned long long)info.Size);
        return false;
    }
    if (!EqualsIgnoreCaseAscii(ToHex(digest, 32), info.Sha256)) {
        DeleteFileW(path.c_str());
        error = L"update hash does not match the signed manifest - rejected";
        return false;
    }

    installerPath = path;
    return true;
}

bool LaunchInstaller(const std::wstring& installerPath, std::wstring& error) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC; // start the process before we exit
    sei.lpVerb = L"open";         // already elevated -> installer inherits admin
    sei.lpFile = installerPath.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        error = WFmt(L"could not launch the installer (%lu)", GetLastError());
        return false;
    }
    return true;
}

} // namespace yadua
