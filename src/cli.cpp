// ============================================================================
// YADUA console frontend: scan a volume, print the top-N largest folders and
// files, optionally export to CSV/JSON (top-N lists or the entire tree).
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "scanner.h"

using namespace yadua;

// ============================================================================
// Export
// ============================================================================

static std::string CsvEscape(std::string s) {
    if (s.find_first_of(",\"") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    return out + "\"";
}

// A FILETIME (100 ns since 1601 UTC) as a local "YYYY-MM-DD HH:MM:SS" string;
// empty when unknown (0), matching what the GUI's Modified column shows.
static std::string IsoTime(uint64_t filetime) {
    if (filetime == 0) return std::string();
    FILETIME utc, local;
    utc.dwLowDateTime  = (DWORD)(filetime & 0xFFFFFFFFull);
    utc.dwHighDateTime = (DWORD)(filetime >> 32);
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&utc, &local) ||
        !FileTimeToSystemTime(&local, &st))
        return std::string();
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

static void JsonWriteString(FILE* f, const std::string& s) {
    fputc('"', f);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;   // Windows paths!
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else fputc(c, f);
        }
    }
    fputc('"', f);
}

struct TopLists {
    std::vector<uint32_t> Dirs;   // first DirCount entries sorted by size desc
    std::vector<uint32_t> Files;  // first FileCount entries sorted by size desc
    size_t DirCount  = 0;
    size_t FileCount = 0;
};

// Win32 file attributes as compact letters (R/H/S/A/C/E).
static std::string AttrString(uint32_t a) {
    std::string s;
    if (a & 0x00000001u) s += 'R';
    if (a & 0x00000002u) s += 'H';
    if (a & 0x00000004u) s += 'S';
    if (a & 0x00000020u) s += 'A';
    if (a & 0x00000800u) s += 'C';
    if (a & 0x00004000u) s += 'E';
    return s;
}

static void ExportCsv(FILE* f, const ScanResult& r, const TopLists& top,
                      bool fullTree) {
    fputs("\xEF\xBB\xBF", f); // UTF-8 BOM so Excel decodes correctly
    fputs("type,path,size_bytes,allocated_bytes,files,folders,modified,"
          "attributes\n", f);

    auto folderRow = [&](const std::string& path, uint32_t idx) {
        fprintf(f, "folder,%s,%llu,%llu,%llu,%llu,%s,%s\n", CsvEscape(path).c_str(),
                r.Totals[idx].LogicalSize, r.Totals[idx].AllocatedSize,
                r.Totals[idx].FileCount, r.Totals[idx].DirCount,
                IsoTime(r.Nodes[idx].ModifiedTime).c_str(),
                AttrString(r.Nodes[idx].Attributes).c_str());
    };
    auto fileRow = [&](const std::string& path, uint32_t idx) {
        fprintf(f, "file,%s,%llu,%llu,,,%s,%s\n", CsvEscape(path).c_str(),
                r.Nodes[idx].LogicalSize, r.Nodes[idx].AllocatedSize,
                IsoTime(r.Nodes[idx].ModifiedTime).c_str(),
                AttrString(r.Nodes[idx].Attributes).c_str());
    };

    if (!fullTree) {
        for (size_t i = 0; i < top.DirCount; ++i)
            folderRow(Utf8(r.Path(top.Dirs[i])), top.Dirs[i]);
        for (size_t i = 0; i < top.FileCount; ++i)
            fileRow(Utf8(r.Path(top.Files[i])), top.Files[i]);
        return;
    }

    // Full tree: DFS preorder, reusing one path string instead of rebuilding
    // the path for each of potentially millions of rows.
    std::wstring path = r.Drive;
    std::function<void(uint32_t)> walk = [&](uint32_t idx) {
        if (r.IsDir(idx)) {
            folderRow(Utf8(path), idx);
            for (uint32_t c = r.Children.Offset[idx];
                 c < r.Children.Offset[idx + 1]; ++c) {
                uint32_t child = r.Children.List[c];
                size_t len = path.size();
                path += L'\\';
                path += r.Name(child);
                walk(child);
                path.resize(len);
            }
        } else {
            fileRow(Utf8(path), idx);
        }
    };
    walk(kRootRecord);
}

static void ExportJson(FILE* f, const ScanResult& r, const TopLists& top,
                       bool fullTree) {
    const DirTotals& root = r.Totals[kRootRecord];
    fputs("{\n", f);
    fputs("  \"volume\": ", f);
    JsonWriteString(f, Utf8(r.Drive));
    fprintf(f, ",\n  \"files\": %llu,\n  \"folders\": %llu,\n"
               "  \"total_size\": %llu,\n  \"allocated_size\": %llu,\n",
            r.FileCount, r.DirCount, root.LogicalSize, root.AllocatedSize);

    fputs("  \"top_folders\": [", f);
    for (size_t i = 0; i < top.DirCount; ++i) {
        uint32_t idx = top.Dirs[i];
        fputs(i ? ",\n    " : "\n    ", f);
        fputs("{\"path\": ", f);
        JsonWriteString(f, Utf8(r.Path(idx)));
        fprintf(f, ", \"size\": %llu, \"allocated\": %llu, "
                   "\"files\": %llu, \"folders\": %llu, \"modified\": \"%s\"}",
                r.Totals[idx].LogicalSize, r.Totals[idx].AllocatedSize,
                r.Totals[idx].FileCount, r.Totals[idx].DirCount,
                IsoTime(r.Nodes[idx].ModifiedTime).c_str());
    }
    fputs("\n  ],\n  \"top_files\": [", f);
    for (size_t i = 0; i < top.FileCount; ++i) {
        uint32_t idx = top.Files[i];
        fputs(i ? ",\n    " : "\n    ", f);
        fputs("{\"path\": ", f);
        JsonWriteString(f, Utf8(r.Path(idx)));
        fprintf(f, ", \"size\": %llu, \"allocated\": %llu, \"modified\": \"%s\"}",
                r.Nodes[idx].LogicalSize, r.Nodes[idx].AllocatedSize,
                IsoTime(r.Nodes[idx].ModifiedTime).c_str());
    }
    fputs("\n  ]", f);

    if (fullTree) {
        fputs(",\n  \"tree\": ", f);
        std::function<void(uint32_t, bool)> emit = [&](uint32_t idx, bool isRoot) {
            fputs("{\"name\": ", f);
            JsonWriteString(f, isRoot ? Utf8(r.Drive) : Utf8(r.Name(idx)));
            if (r.IsDir(idx)) {
                fprintf(f, ", \"size\": %llu, \"allocated\": %llu, "
                           "\"files\": %llu, \"folders\": %llu, "
                           "\"modified\": \"%s\", \"children\": [",
                        r.Totals[idx].LogicalSize, r.Totals[idx].AllocatedSize,
                        r.Totals[idx].FileCount, r.Totals[idx].DirCount,
                        IsoTime(r.Nodes[idx].ModifiedTime).c_str());
                for (uint32_t c = r.Children.Offset[idx];
                     c < r.Children.Offset[idx + 1]; ++c) {
                    if (c != r.Children.Offset[idx]) fputc(',', f);
                    emit(r.Children.List[c], false);
                }
                fputs("]}", f);
            } else {
                fprintf(f, ", \"size\": %llu, \"allocated\": %llu, "
                           "\"modified\": \"%s\"}",
                        r.Nodes[idx].LogicalSize, r.Nodes[idx].AllocatedSize,
                        IsoTime(r.Nodes[idx].ModifiedTime).c_str());
            }
        };
        emit(kRootRecord, true);
    }
    fputs("\n}\n", f);
}

// ============================================================================
// Scan with a live progress line (only when stderr is an interactive console;
// redirected output stays clean).
// ============================================================================

static bool ScanWithProgress(const std::wstring& drive, unsigned threads,
                             ScanResult& r, std::wstring& error,
                             bool forceWalk = false) {
    ScanProgress progress;
    std::atomic<bool> finished{false};
    std::thread ticker;
    if (_isatty(_fileno(stderr))) {
        ticker = std::thread([&] {
            while (!finished) {
                uint64_t total = progress.TotalBytes.load(std::memory_order_relaxed);
                uint64_t read  = progress.BytesRead.load(std::memory_order_relaxed);
                int stage = progress.Stage.load(std::memory_order_relaxed);
                if (stage >= ScanProgress::Aggregating)
                    fprintf(stderr, "\rBuilding tree...                         ");
                else if (total)
                    fprintf(stderr, "\rReading MFT... %5.1f%% (%s / %s)   ",
                            100.0 * (double)read / (double)total,
                            HumanSize(read).c_str(), HumanSize(total).c_str());
                else if (read) // directory-walk fallback: total is unknown
                    fprintf(stderr, "\rScanning... %llu entries found   ", read);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            fprintf(stderr, "\r%*s\r", 60, ""); // wipe the progress line
        });
    }
    bool ok = forceWalk
                  ? ScanVolumeFallback(drive, threads, r, error, &progress)
                  : ScanVolumeAuto(drive, threads, r, error, &progress);
    finished = true;
    if (ticker.joinable()) ticker.join();
    return ok;
}

// Inserts a drive suffix before the extension when exporting several volumes
// in one run: results.csv -> results_C.csv
static std::wstring SuffixedPath(const std::wstring& path, const ScanResult& r,
                                 bool multi) {
    if (!multi) return path;
    std::wstring suffix = L"_";
    suffix += r.Drive[0];
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot < path.find_last_of(L"\\/") + 1)
        return path + suffix;
    return path.substr(0, dot) + suffix + path.substr(dot);
}

// ============================================================================
// Snapshot diff: recursive tree comparison matching children by name.
// ============================================================================

struct DiffChange {
    std::wstring Path;
    int64_t      Delta = 0;       // signed byte change (cumulative for dirs)
    uint64_t     OldSize = 0, NewSize = 0;
    bool         IsDir = false;
    enum Kind { Changed, Added, Removed } What = Changed;
};

struct DiffTally {
    uint64_t FilesAdded = 0, FilesRemoved = 0, DirsAdded = 0, DirsRemoved = 0;
};

static std::wstring LowerName(const ScanResult& r, uint32_t n) {
    std::wstring s = r.Name(n);
    for (wchar_t& c : s) c = towlower(c);
    return s;
}

static void RecordWholeSubtree(const ScanResult& r, uint32_t n,
                               const std::wstring& path, bool added,
                               std::vector<DiffChange>& changes, DiffTally& t) {
    DiffChange c;
    c.Path  = path;
    c.IsDir = r.IsDir(n);
    c.What  = added ? DiffChange::Added : DiffChange::Removed;
    uint64_t size = r.SizeOf(n);
    c.Delta = added ? (int64_t)size : -(int64_t)size;
    (added ? c.NewSize : c.OldSize) = size;
    if (c.IsDir) {
        uint64_t files = r.Totals[n].FileCount, dirs = r.Totals[n].DirCount + 1;
        if (added) { t.FilesAdded += files; t.DirsAdded += dirs; }
        else       { t.FilesRemoved += files; t.DirsRemoved += dirs; }
    } else {
        if (added) ++t.FilesAdded; else ++t.FilesRemoved;
    }
    if (c.Delta != 0 || !c.IsDir) changes.push_back(std::move(c));
}

static void DiffDirs(const ScanResult& a, uint32_t da,
                     const ScanResult& b, uint32_t db, std::wstring& path,
                     std::vector<DiffChange>& changes, DiffTally& tally,
                     int depth) {
    if (depth > 512) return;
    std::unordered_map<std::wstring, uint32_t> bKids;
    for (uint32_t c = b.Children.Offset[db]; c < b.Children.Offset[db + 1]; ++c) {
        uint32_t child = b.Children.List[c];
        if (b.Exists(child)) bKids.emplace(LowerName(b, child), child);
    }

    for (uint32_t c = a.Children.Offset[da]; c < a.Children.Offset[da + 1]; ++c) {
        uint32_t ca = a.Children.List[c];
        if (!a.Exists(ca)) continue;
        size_t len = path.size();
        path += L'\\';
        path += a.Name(ca);

        auto it = bKids.find(LowerName(a, ca));
        if (it == bKids.end()) {
            RecordWholeSubtree(a, ca, path, false, changes, tally);
        } else {
            uint32_t cb = it->second;
            bKids.erase(it);
            if (a.IsDir(ca) != b.IsDir(cb)) { // type flipped: remove + add
                RecordWholeSubtree(a, ca, path, false, changes, tally);
                RecordWholeSubtree(b, cb, path, true, changes, tally);
            } else if (a.IsDir(ca)) {
                int64_t delta = (int64_t)b.Totals[cb].LogicalSize -
                                (int64_t)a.Totals[ca].LogicalSize;
                if (delta != 0)
                    changes.push_back({path, delta, a.Totals[ca].LogicalSize,
                                       b.Totals[cb].LogicalSize, true,
                                       DiffChange::Changed});
                DiffDirs(a, ca, b, cb, path, changes, tally, depth + 1);
            } else if (a.Nodes[ca].LogicalSize != b.Nodes[cb].LogicalSize) {
                changes.push_back({path,
                                   (int64_t)b.Nodes[cb].LogicalSize -
                                       (int64_t)a.Nodes[ca].LogicalSize,
                                   a.Nodes[ca].LogicalSize,
                                   b.Nodes[cb].LogicalSize, false,
                                   DiffChange::Changed});
            }
        }
        path.resize(len);
    }

    // Anything left in b had no counterpart in a: newly added.
    for (const auto& [name, cb] : bKids) {
        size_t len = path.size();
        path += L'\\';
        path += b.Name(cb);
        RecordWholeSubtree(b, cb, path, true, changes, tally);
        path.resize(len);
    }
}

static std::string SignedSize(int64_t d) {
    std::string mag = HumanSize((uint64_t)(d < 0 ? -d : d));
    return (d < 0 ? "-" : "+") + mag;
}

static std::string FormatUnixTime(uint64_t t) {
    if (!t) return "unknown time";
    __time64_t tt = (__time64_t)t;
    tm local{};
    _localtime64_s(&local, &tt);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
    return buf;
}

static int RunDiff(const std::wstring& oldSpec, const std::wstring& newSpec,
                   int topN, unsigned threads) {
    ScanResult a, b;
    std::wstring err;
    if (!LoadSnapshot(oldSpec, a, err)) {
        fprintf(stderr, "error: %s\n", Utf8(err).c_str());
        return 1;
    }
    // The "new" side can be a snapshot file or a drive to scan live.
    bool live = newSpec.size() <= 3 && newSpec.size() >= 2 && newSpec[1] == L':';
    if (live ? !ScanWithProgress(newSpec, threads, b, err)
             : !LoadSnapshot(newSpec, b, err)) {
        fprintf(stderr, "error: %s\n", Utf8(err).c_str());
        return 1;
    }
    if (_wcsicmp(a.Drive.c_str(), b.Drive.c_str()) != 0)
        printf("note: comparing different volumes (%s vs %s)\n\n",
               Utf8(a.Drive).c_str(), Utf8(b.Drive).c_str());

    std::vector<DiffChange> changes;
    DiffTally tally;
    std::wstring path = b.Drive;
    DiffDirs(a, (uint32_t)kRootRecord, b, (uint32_t)kRootRecord, path, changes,
             tally, 0);

    const DirTotals& ra = a.Totals[kRootRecord];
    const DirTotals& rb = b.Totals[kRootRecord];
    printf("=== %s: %s -> %s ===\n", Utf8(b.Drive).c_str(),
           FormatUnixTime(a.Stats.ScanUnixTime).c_str(),
           FormatUnixTime(b.Stats.ScanUnixTime).c_str());
    printf("Total size: %s -> %s (%s)\n", HumanSize(ra.LogicalSize).c_str(),
           HumanSize(rb.LogicalSize).c_str(),
           SignedSize((int64_t)rb.LogicalSize - (int64_t)ra.LogicalSize).c_str());
    printf("Files: %llu -> %llu (+%llu added, -%llu removed)\n",
           a.FileCount, b.FileCount, tally.FilesAdded, tally.FilesRemoved);
    printf("Folders: %llu -> %llu (+%llu added, -%llu removed)\n",
           a.DirCount, b.DirCount, tally.DirsAdded, tally.DirsRemoved);

    size_t n = std::min<size_t>(topN, changes.size());
    std::partial_sort(changes.begin(), changes.begin() + n, changes.end(),
                      [](const DiffChange& x, const DiffChange& y) {
                          return (x.Delta < 0 ? -x.Delta : x.Delta) >
                                 (y.Delta < 0 ? -y.Delta : y.Delta);
                      });
    printf("\n=== Top %zu changes ===\n", n);
    for (size_t i = 0; i < n; ++i) {
        const DiffChange& c = changes[i];
        const char* tag = c.What == DiffChange::Added     ? "added  "
                          : c.What == DiffChange::Removed ? "removed"
                          : c.IsDir                       ? "dir    "
                                                          : "file   ";
        printf("%12s  %s  %s", SignedSize(c.Delta).c_str(), tag,
               Utf8(c.Path).c_str());
        if (c.What == DiffChange::Changed && !c.IsDir)
            printf("  (%s -> %s)", HumanSize(c.OldSize).c_str(),
                   HumanSize(c.NewSize).c_str());
        printf("\n");
    }
    if (changes.empty()) printf("(no changes)\n");
    return 0;
}

// Resolve "C:\foo\bar" to its node index by descending the child index.
static uint32_t FindNodeByPath(const ScanResult& r, const std::wstring& path) {
    uint32_t cur = (uint32_t)kRootRecord;
    size_t pos = path.find(L'\\');
    while (pos != std::wstring::npos && cur != UINT32_MAX) {
        size_t end = path.find(L'\\', pos + 1);
        std::wstring part = path.substr(pos + 1, (end == std::wstring::npos
                                                      ? path.size() : end) - pos - 1);
        pos = end;
        if (part.empty()) continue;
        uint32_t next = UINT32_MAX;
        for (uint32_t c = r.Children.Offset[cur]; c < r.Children.Offset[cur + 1]; ++c) {
            uint32_t child = r.Children.List[c];
            if (r.Exists(child) && _wcsicmp(r.Name(child).c_str(), part.c_str()) == 0) {
                next = child;
                break;
            }
        }
        cur = next;
    }
    return cur;
}

// Hidden self-test for RescanSubtree: scan the drive, splice-rescan the given
// folder, and report subtree + volume totals before and after.
static int DebugRescan(const std::wstring& path, unsigned threads) {
    ScanResult r;
    std::wstring error;
    if (!ScanWithProgress(path.substr(0, 2), threads, r, error)) {
        fprintf(stderr, "error: %s\n", Utf8(error).c_str());
        return 1;
    }
    auto report = [&](const char* when) {
        uint32_t node = FindNodeByPath(r, path);
        if (node == UINT32_MAX) {
            fprintf(stderr, "%s: path not found in scan\n", when);
            return false;
        }
        printf("%s: subtree size=%llu alloc=%llu files=%llu dirs=%llu | "
               "volume size=%llu files=%llu folders=%llu\n",
               when, r.Totals[node].LogicalSize, r.Totals[node].AllocatedSize,
               r.Totals[node].FileCount, r.Totals[node].DirCount,
               r.Totals[kRootRecord].LogicalSize, r.FileCount, r.DirCount);
        return true;
    };
    if (!report("before")) return 1;
    uint32_t node = FindNodeByPath(r, path);
    std::wstring err;
    if (!RescanSubtree(r, node, err)) {
        fprintf(stderr, "rescan failed: %s\n", Utf8(err).c_str());
        return 1;
    }
    if (!report("after ")) return 1;
    return 0;
}

// Hidden self-test for the USN live-update pipeline: scan, create a file,
// apply the journal deltas, verify the node appears; delete it, apply again,
// verify it disappears.
static int DebugUsn(unsigned threads) {
    ScanResult r;
    std::wstring err;
    wchar_t tempDir[MAX_PATH], longDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    // GetTempPathW often returns 8.3 short names (NATHAN~1); the tree stores
    // long Win32 names, so expand before looking the file up by path.
    if (!GetLongPathNameW(tempDir, longDir, MAX_PATH)) wcscpy_s(longDir, tempDir);
    std::wstring testFile = std::wstring(longDir) + L"yadua_usn_test.bin";
    std::wstring drive = testFile.substr(0, 2);

    if (!ScanWithProgress(drive, threads, r, err)) {
        fprintf(stderr, "error: %s\n", Utf8(err).c_str());
        return 1;
    }
    if (r.Stats.UsedFallback) {
        fprintf(stderr, "debug-usn requires the raw-MFT scan (run elevated)\n");
        return 1;
    }

    UsnMonitor monitor;
    if (!monitor.Start(r.Drive, err)) {
        fprintf(stderr, "monitor: %s\n", Utf8(err).c_str());
        return 1;
    }
    printf("monitor started on %s\n", Utf8(r.Drive).c_str());

    // Force file + volume metadata to disk so the raw MFT read sees it.
    auto flushVolume = [&] {
        HANDLE v = CreateFileW((L"\\\\.\\" + r.Drive).c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (v != INVALID_HANDLE_VALUE) { FlushFileBuffers(v); CloseHandle(v); }
    };
    auto applyAndFind = [&](const char* phase) -> int64_t {
        flushVolume();
        Sleep(3000);
        std::vector<uint64_t> dirty = monitor.TakeDirty();
        unsigned unresolved = 0;
        std::wstring applyErr;
        if (!ApplyMftUpdates(r, dirty, applyErr, &unresolved)) {
            fprintf(stderr, "apply: %s\n", Utf8(applyErr).c_str());
            return -2;
        }
        uint32_t node = FindNodeByPath(r, testFile);
        std::string state =
            node == UINT32_MAX
                ? std::string("ABSENT")
                : "present, " + HumanSize(r.Nodes[node].LogicalSize);
        printf("%s: %zu dirty records applied (%u unresolved), file %s\n",
               phase, dirty.size(), unresolved, state.c_str());
        return node == UINT32_MAX ? -1 : (int64_t)r.Nodes[node].LogicalSize;
    };

    FILE* f = _wfopen(testFile.c_str(), L"wb");
    if (!f) { fprintf(stderr, "cannot create test file\n"); return 1; }
    std::vector<char> block(1 << 20, 'y');
    for (int i = 0; i < 8; ++i) fwrite(block.data(), 1, block.size(), f);
    fclose(f);

    int64_t size = applyAndFind("after create");
    bool createOk = size == 8 * (1 << 20);

    DeleteFileW(testFile.c_str());
    int64_t gone = applyAndFind("after delete");
    bool deleteOk = gone == -1;

    monitor.Stop();
    printf("create tracked: %s   delete tracked: %s\n",
           createOk ? "OK" : "FAIL", deleteOk ? "OK" : "FAIL");
    return createOk && deleteOk ? 0 : 1;
}

// ============================================================================
// main
// ============================================================================

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::wstring> drives;
    int topN = 50;
    bool exportAll = false, forceWalk = false;
    std::wstring csvPath, jsonPath, snapshotPath, diffOld, diffNew;
    unsigned threads = 0;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--top" && i + 1 < argc)           topN = _wtoi(argv[++i]);
        else if (arg == L"--csv" && i + 1 < argc)      csvPath = argv[++i];
        else if (arg == L"--json" && i + 1 < argc)     jsonPath = argv[++i];
        else if (arg == L"--snapshot" && i + 1 < argc) snapshotPath = argv[++i];
        else if (arg == L"--diff" && i + 2 < argc) {
            diffOld = argv[++i];
            diffNew = argv[++i];
        }
        else if (arg == L"--threads" && i + 1 < argc)  threads = _wtoi(argv[++i]);
        else if (arg == L"--all")                      exportAll = true;
        else if (arg == L"--walk")                     forceWalk = true;
        else if (arg == L"--debug-rescan" && i + 1 < argc) // hidden self-test
            return DebugRescan(argv[++i], threads);
        else if (arg == L"--debug-usn")                    // hidden self-test
            return DebugUsn(threads);
        else if (arg == L"--help" || arg == L"-h") {
            printf("Usage: yadua.exe [drives...] [options]\n"
                   "  drives           volumes to scan, e.g. C: D: (default C:)\n"
                   "  --top N          how many entries per list (default 50)\n"
                   "  --csv FILE       export to CSV (multiple drives: FILE_C.csv...)\n"
                   "  --json FILE      export to JSON\n"
                   "  --all            export the entire tree, not just the top-N\n"
                   "                   lists (CSV: one row per file/folder; JSON:\n"
                   "                   adds a nested \"tree\" object)\n"
                   "  --snapshot FILE  save a snapshot (.ysnap) for later diffing\n"
                   "  --diff OLD NEW   compare snapshots; NEW may be a drive to\n"
                   "                   scan live (e.g. --diff before.ysnap C:)\n"
                   "  --walk           force the directory-walk scanner\n"
                   "  --threads N      parser threads (default: auto)\n"
                   "Run from an elevated prompt for the fast raw-MFT scan.\n");
            return 0;
        }
        else if (!arg.empty() && arg[0] != L'-') drives.push_back(arg);
    }
    if (!diffOld.empty()) return RunDiff(diffOld, diffNew, topN, threads);
    if (drives.empty()) drives.push_back(L"C:");
    const bool multi = drives.size() > 1;

    int failures = 0;
    for (size_t d = 0; d < drives.size(); ++d) {
    if (d) printf("\n%s\n\n", std::string(72, '=').c_str());

    ScanResult r;
    std::wstring error;
    // Note: never printf("%ls") — the C-locale wide conversion silently
    // truncates output at the first non-ASCII character. Console output is
    // UTF-8 (SetConsoleOutputCP), so always go through Utf8().
    if (!ScanWithProgress(drives[d], threads, r, error, forceWalk)) {
        fprintf(stderr, "error: %s\n", Utf8(error).c_str());
        ++failures;
        continue;
    }

    if (r.Stats.UsedFallback) {
        printf("Volume %s: directory-walk fallback, %u threads\n"
               "  (raw MFT access unavailable: %s)\n",
               Utf8(r.Drive).c_str(), r.Stats.Threads,
               Utf8(r.FallbackReason).c_str());
        printf("Scanned %llu entries in %.2f s\n",
               r.Stats.RecordsParsed, r.Stats.TotalSeconds);
    } else {
        printf("Volume %s: cluster %u B, MFT record %u B, MFT size %s "
               "(%llu records), %u parser threads\n",
               Utf8(r.Drive).c_str(), r.Stats.ClusterSize, r.Stats.RecordSize,
               HumanSize(r.Stats.MftBytes).c_str(), r.Stats.RecordsParsed,
               r.Stats.Threads);
        printf("Scanned %llu records in %.2f s (MFT streamed in %.2f s, %s @ %.0f MB/s)\n",
               r.Stats.RecordsParsed, r.Stats.TotalSeconds, r.Stats.StreamSeconds,
               HumanSize(r.Stats.BytesRead).c_str(),
               r.Stats.BytesRead / r.Stats.StreamSeconds / (1024.0 * 1024.0));
    }

    const DirTotals& root = r.Totals[kRootRecord];
    printf("Files: %llu   Folders: %llu   Total size: %s   On disk: %s\n",
           r.FileCount, r.DirCount,
           HumanSize(root.LogicalSize).c_str(),
           HumanSize(root.AllocatedSize).c_str());
    if (r.Stats.ReparseCount || r.Stats.OrphanCount)
        printf("Reparse points (junctions/links): %llu   Orphaned entries: %llu%s\n",
               r.Stats.ReparseCount, r.Stats.OrphanCount,
               r.Stats.OrphanCount ? "  (see [orphaned] at the root)" : "");

    // ---- Top-N lists --------------------------------------------------------
    TopLists top;
    top.Files.reserve(r.Nodes.size());
    for (uint32_t i = 0; i < r.Nodes.size(); ++i) {
        if (!r.Exists(i)) continue;
        if (r.IsDir(i)) top.Dirs.push_back(i);
        else            top.Files.push_back(i);
    }
    top.FileCount = std::min<size_t>(topN, top.Files.size());
    top.DirCount  = std::min<size_t>(topN, top.Dirs.size());
    auto bySize = [&r](uint32_t a, uint32_t b) { return r.SizeOf(a) > r.SizeOf(b); };
    std::partial_sort(top.Files.begin(), top.Files.begin() + top.FileCount,
                      top.Files.end(), bySize);
    std::partial_sort(top.Dirs.begin(), top.Dirs.begin() + top.DirCount,
                      top.Dirs.end(), bySize);

    printf("\n=== Top %zu folders by size ===\n", top.DirCount);
    for (size_t i = 0; i < top.DirCount; ++i) {
        uint32_t idx = top.Dirs[i];
        printf("%12s  %9llu files  %s\n",
               HumanSize(r.Totals[idx].LogicalSize).c_str(),
               r.Totals[idx].FileCount, Utf8(r.Path(idx)).c_str());
    }
    printf("\n=== Top %zu files by size ===\n", top.FileCount);
    for (size_t i = 0; i < top.FileCount; ++i) {
        uint32_t idx = top.Files[i];
        printf("%12s  %s\n", HumanSize(r.Nodes[idx].LogicalSize).c_str(),
               Utf8(r.Path(idx)).c_str());
    }

    // ---- Export -------------------------------------------------------------
    auto openOut = [](const std::wstring& path) {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) setvbuf(f, nullptr, _IOFBF, 1 << 20); // big buffer: millions of rows
        else fprintf(stderr, "error: cannot write %s\n", Utf8(path).c_str());
        return f;
    };
    if (!csvPath.empty()) {
        std::wstring path = SuffixedPath(csvPath, r, multi);
        if (FILE* f = openOut(path)) {
            ExportCsv(f, r, top, exportAll);
            fclose(f);
            printf("\nCSV written to %s\n", Utf8(path).c_str());
        } else return 1;
    }
    if (!jsonPath.empty()) {
        std::wstring path = SuffixedPath(jsonPath, r, multi);
        if (FILE* f = openOut(path)) {
            ExportJson(f, r, top, exportAll);
            fclose(f);
            printf("JSON written to %s\n", Utf8(path).c_str());
        } else return 1;
    }
    if (!snapshotPath.empty()) {
        std::wstring path = SuffixedPath(snapshotPath, r, multi);
        std::wstring snapErr;
        if (SaveSnapshot(r, path, snapErr))
            printf("Snapshot written to %s\n", Utf8(path).c_str());
        else {
            fprintf(stderr, "error: %s\n", Utf8(snapErr).c_str());
            return 1;
        }
    }

    } // for each drive

    return failures ? 1 : 0;
}
