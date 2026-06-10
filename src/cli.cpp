// ============================================================================
// YADUA console frontend: scan a volume, print the top-N largest folders and
// files, optionally export to CSV/JSON (top-N lists or the entire tree).
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
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
// main
// ============================================================================

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::wstring drive = L"C:";
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
            printf("Usage: yadua.exe [drive] [options]\n"
                   "  drive        volume to scan, e.g. C: (default C:)\n"
                   "  --top N      how many entries per list (default 50)\n"
                   "  --csv FILE   export to CSV\n"
                   "  --json FILE  export to JSON\n"
                   "  --all        export the entire tree, not just the top-N\n"
                   "               lists (CSV: one row per file/folder; JSON:\n"
                   "               adds a nested \"tree\" object)\n"
                   "  --threads N  parser threads (default: auto)\n"
                   "Must be run from an elevated (Administrator) prompt.\n");
            return 0;
        }
        else if (!arg.empty() && arg[0] != L'-') drive = arg;
    }

    ScanResult r;
    std::wstring error;
    if (!ScanVolume(drive, threads, r, error)) {
        fprintf(stderr, "error: %ls\n", error.c_str());
        return 1;
    }

    printf("Volume %ls: cluster %u B, MFT record %u B, MFT size %s "
           "(%llu records), %u parser threads\n",
           r.Drive.c_str(), r.Stats.ClusterSize, r.Stats.RecordSize,
           HumanSize(r.Stats.MftBytes).c_str(), r.Stats.RecordsParsed,
           r.Stats.Threads);
    printf("Scanned %llu records in %.2f s (MFT streamed in %.2f s, %s @ %.0f MB/s)\n",
           r.Stats.RecordsParsed, r.Stats.TotalSeconds, r.Stats.StreamSeconds,
           HumanSize(r.Stats.BytesRead).c_str(),
           r.Stats.BytesRead / r.Stats.StreamSeconds / (1024.0 * 1024.0));

    const DirTotals& root = r.Totals[kRootRecord];
    printf("Files: %llu   Folders: %llu   Total size: %s   On disk: %s\n",
           r.FileCount, r.DirCount,
           HumanSize(root.LogicalSize).c_str(),
           HumanSize(root.AllocatedSize).c_str());

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
        else fprintf(stderr, "error: cannot write %ls\n", path.c_str());
        return f;
    };
    if (!csvPath.empty()) {
        if (FILE* f = openOut(csvPath)) {
            ExportCsv(f, r, top, exportAll);
            fclose(f);
            printf("\nCSV written to %ls\n", csvPath.c_str());
        } else return 1;
    }
    if (!jsonPath.empty()) {
        if (FILE* f = openOut(jsonPath)) {
            ExportJson(f, r, top, exportAll);
            fclose(f);
            printf("JSON written to %ls\n", jsonPath.c_str());
        } else return 1;
    }

    return 0;
}
