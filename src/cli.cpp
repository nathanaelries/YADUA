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
#include <functional>
#include <string>
#include <thread>
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

static void ExportCsv(FILE* f, const ScanResult& r, const TopLists& top,
                      bool fullTree) {
    fputs("\xEF\xBB\xBF", f); // UTF-8 BOM so Excel decodes correctly
    fputs("type,path,size_bytes,allocated_bytes,files,folders\n", f);

    auto folderRow = [&](const std::string& path, uint32_t idx) {
        fprintf(f, "folder,%s,%llu,%llu,%llu,%llu\n", CsvEscape(path).c_str(),
                r.Totals[idx].LogicalSize, r.Totals[idx].AllocatedSize,
                r.Totals[idx].FileCount, r.Totals[idx].DirCount);
    };
    auto fileRow = [&](const std::string& path, uint32_t idx) {
        fprintf(f, "file,%s,%llu,%llu,,\n", CsvEscape(path).c_str(),
                r.Nodes[idx].LogicalSize, r.Nodes[idx].AllocatedSize);
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
                   "\"files\": %llu, \"folders\": %llu}",
                r.Totals[idx].LogicalSize, r.Totals[idx].AllocatedSize,
                r.Totals[idx].FileCount, r.Totals[idx].DirCount);
    }
    fputs("\n  ],\n  \"top_files\": [", f);
    for (size_t i = 0; i < top.FileCount; ++i) {
        uint32_t idx = top.Files[i];
        fputs(i ? ",\n    " : "\n    ", f);
        fputs("{\"path\": ", f);
        JsonWriteString(f, Utf8(r.Path(idx)));
        fprintf(f, ", \"size\": %llu, \"allocated\": %llu}",
                r.Nodes[idx].LogicalSize, r.Nodes[idx].AllocatedSize);
    }
    fputs("\n  ]", f);

    if (fullTree) {
        fputs(",\n  \"tree\": ", f);
        std::function<void(uint32_t, bool)> emit = [&](uint32_t idx, bool isRoot) {
            fputs("{\"name\": ", f);
            JsonWriteString(f, isRoot ? Utf8(r.Drive) : Utf8(r.Name(idx)));
            if (r.IsDir(idx)) {
                fprintf(f, ", \"size\": %llu, \"allocated\": %llu, "
                           "\"files\": %llu, \"folders\": %llu, \"children\": [",
                        r.Totals[idx].LogicalSize, r.Totals[idx].AllocatedSize,
                        r.Totals[idx].FileCount, r.Totals[idx].DirCount);
                for (uint32_t c = r.Children.Offset[idx];
                     c < r.Children.Offset[idx + 1]; ++c) {
                    if (c != r.Children.Offset[idx]) fputc(',', f);
                    emit(r.Children.List[c], false);
                }
                fputs("]}", f);
            } else {
                fprintf(f, ", \"size\": %llu, \"allocated\": %llu}",
                        r.Nodes[idx].LogicalSize, r.Nodes[idx].AllocatedSize);
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
                             ScanResult& r, std::wstring& error) {
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
    bool ok = ScanVolumeAuto(drive, threads, r, error, &progress);
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
// main
// ============================================================================

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::wstring> drives;
    int topN = 50;
    bool exportAll = false;
    std::wstring csvPath, jsonPath;
    unsigned threads = 0;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--top" && i + 1 < argc)          topN = _wtoi(argv[++i]);
        else if (arg == L"--csv" && i + 1 < argc)     csvPath = argv[++i];
        else if (arg == L"--json" && i + 1 < argc)    jsonPath = argv[++i];
        else if (arg == L"--threads" && i + 1 < argc) threads = _wtoi(argv[++i]);
        else if (arg == L"--all")                     exportAll = true;
        else if (arg == L"--help" || arg == L"-h") {
            printf("Usage: yadua.exe [drives...] [options]\n"
                   "  drives       volumes to scan, e.g. C: D: (default C:)\n"
                   "  --top N      how many entries per list (default 50)\n"
                   "  --csv FILE   export to CSV (multiple drives: FILE_C.csv...)\n"
                   "  --json FILE  export to JSON\n"
                   "  --all        export the entire tree, not just the top-N\n"
                   "               lists (CSV: one row per file/folder; JSON:\n"
                   "               adds a nested \"tree\" object)\n"
                   "  --threads N  parser threads (default: auto)\n"
                   "Must be run from an elevated (Administrator) prompt.\n");
            return 0;
        }
        else if (!arg.empty() && arg[0] != L'-') drives.push_back(arg);
    }
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
    if (!ScanWithProgress(drives[d], threads, r, error)) {
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

    } // for each drive

    return failures ? 1 : 0;
}
