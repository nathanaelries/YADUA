// ============================================================================
// YADUA GUI frontend — Dear ImGui (Win32 + DirectX 11).
//
// Views (tabs): a size-sorted tree table (sortable columns, name filter), a
// flat "Files" list of the largest files anywhere on the volume, a "File
// Types" summary grouping space by extension, and a WinDirStat-style
// squarified treemap (src/treemap.*). All four share the one filter box.
//
// The scan runs on a background thread (see scanner.h); the UI polls a
// ScanProgress while it runs and takes ownership of the ScanResult when done.
// Deletions go to the Recycle Bin (SHFileOperationW + FOF_ALLOWUNDO) on a
// background thread, then the in-memory tree is updated without a rescan.
//
// The linker manifest requests Administrator elevation (raw volume access),
// so double-clicking the exe shows a UAC prompt.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "scanner.h"
#include "treemap.h"
#include "updater.h"

// ============================================================================
// Application state
// ============================================================================

enum TreeColumn : ImGuiID {
    ColName = 0, ColSize, ColPercent, ColFiles, ColFolders, ColModified
};

// A FILETIME (100 ns since 1601 UTC) as a local "YYYY-MM-DD HH:MM" string;
// "-" when unknown (0) or unconvertible.
static std::string FormatTime(uint64_t filetime) {
    if (filetime == 0) return "-";
    FILETIME utc, local;
    utc.dwLowDateTime  = (DWORD)(filetime & 0xFFFFFFFFull);
    utc.dwHighDateTime = (DWORD)(filetime >> 32);
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&utc, &local) ||
        !FileTimeToSystemTime(&local, &st))
        return "-";
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute);
    return buf;
}

struct App {
    std::vector<std::wstring> Drives;
    std::vector<std::string>  DriveLabels; // UTF-8 copies for ImGui
    int  DriveIndex  = 0;
    bool AutoScan    = false;              // --autoscan: start scanning at launch
    bool OpenTreemap = false;              // --view treemap: select treemap tab

    std::unique_ptr<yadua::ScanResult> Result;   // owned by the UI thread
    std::unique_ptr<yadua::ScanResult> Pending;  // being filled by the scan thread
    yadua::ScanProgress Progress;
    std::thread         ScanThread;
    std::atomic<bool>   ScanDone{false};
    bool                Scanning = false;
    std::wstring        PendingError; // written by scan thread before ScanDone
    std::wstring        Error;

    // Filter: Visible[i] != 0 <=> node i matches or has a matching
    // descendant. Empty vector means "no filter, everything visible".
    char                 Filter[256] = {};
    std::vector<uint8_t> Visible;

    // Cross-view navigation: selection is shared, and either view can ask the
    // other to show a node (treemap "Reveal in tree" / tree "Show in treemap").
    uint32_t             SelectedNode = UINT32_MAX;
    uint32_t             RevealNode   = UINT32_MAX; // tree scrolls here, 1 frame
    std::vector<uint8_t> RevealOpen;                // ancestors to force open
    bool                 SwitchToTree     = false;
    bool                 SwitchToTreemap  = false;
    bool                 SwitchToFileTypes = false; // View menu -> File Types tab
    bool                 FocusFilter      = false;  // Search/Ctrl+F -> filter box
    bool                 ShowAllocated    = false;  // show size on disk (persisted)

    // Tree sorting. The scanner's child index is size-descending (canonical);
    // any other order lives in SortedChildren (parallel to Children.List).
    std::vector<uint32_t> SortedChildren;
    bool                  UseSorted = false;

    // Flat "Files" view (WizTree-style largest-files list, ignoring folder
    // structure) and "File Types" summary (space grouped by extension). Both
    // are derived from the current filter and cached until it changes;
    // RecomputeFilter marks them dirty. Rebuilt lazily when their tab draws.
    std::vector<uint32_t> FileList;                 // file node indices, sorted
    uint64_t              FileListBytes = 0;        // total size of FileList
    bool                  FileListDirty = true;
    ImGuiID               FileSortCol   = ColSize;  // active sort column
    bool                  FileSortAsc   = false;
    struct TypeAgg { std::wstring Ext; uint64_t Bytes = 0; uint64_t Count = 0; };
    std::vector<TypeAgg>  TypeList;                 // by extension, size-desc
    bool                  TypeListDirty = true;
    bool                  SwitchToFiles = false;    // File Types row -> Files tab

    TreemapView Treemap;
    // Treemap panel docked under the tree (the Treemap tab still offers the
    // full-screen view); height is user-draggable via a splitter.
    bool  ShowMapPanel   = true;
    float MapPanelHeight = 0; // 0 = pick a default on first layout

    // Recycle-bin deletion (one at a time, on a background thread because the
    // shell can take a while on big folders).
    uint32_t          ConfirmDelete = UINT32_MAX; // node awaiting confirmation
    bool              Deleting      = false;
    std::atomic<bool> DeleteDone{false};
    std::thread       DeleteThread;
    uint32_t          DeleteNode    = UINT32_MAX;
    std::wstring      DeletePath;
    int               DeleteResult  = 0;    // SHFileOperation return code
    bool              DeleteAborted = false;
    std::string       Status;               // last action feedback, UTF-8

    // Per-folder rescan. The worker mutates Result in place, so the UI must
    // not read Result while RescanBusy is set (DrawUi gates on it).
    bool                Rescanning = false;
    std::atomic<bool>   RescanDone{false};
    std::thread         RescanThread;
    std::wstring        RescanPath;  // for the progress display
    std::wstring        RescanError; // written by the worker before RescanDone
    yadua::ScanProgress RescanProgress;

    // USN live updates: the monitor counts filesystem changes since the scan;
    // Apply folds them in by re-reading just the affected MFT records (same
    // UI gating as a rescan while Result mutates).
    yadua::UsnMonitor Monitor;
    bool              Applying = false;
    std::atomic<bool> ApplyDone{false};
    std::thread       ApplyThread;
    std::wstring      ApplyError;
    unsigned          ApplyUnresolved = 0;
    size_t            ApplyCount      = 0;

    // ---- Auto-update (consent-gated) --------------------------------------
    // A background thread checks the signed manifest on launch (if enabled)
    // and reports; nothing downloads or installs until the user clicks Update.
    bool                UpdCheckOnLaunch = true;  // persisted setting
    std::wstring        UpdSkipVersion;           // "skip this version", persisted
    bool                UpdCheckStarted  = false; // launch check fired once
    bool                UpdChecking      = false;
    bool                UpdCheckedThisRun = false; // show result after manual check
    std::atomic<bool>   UpdCheckDone{false};
    std::thread         UpdCheckThread;
    yadua::UpdateInfo   UpdInfo;                  // written by the check thread
    std::wstring        UpdCheckError;            // empty = ok
    bool                ShowAbout        = false;

    bool                UpdDownloading   = false;
    std::atomic<bool>   UpdDownloadDone{false};
    std::thread         UpdDownloadThread;
    yadua::ScanProgress UpdProgress;
    std::wstring        UpdInstallerPath;         // verified installer, when ready
    std::wstring        UpdDownloadError;
};

// All fixed drives: NTFS gets the fast MFT scan, anything else (or a
// non-elevated run) automatically uses the directory-walk fallback.
static void ListFixedDrives(App& app) {
    DWORD mask = GetLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        if (!(mask & (1u << (letter - L'A')))) continue;
        std::wstring root{letter, L':', L'\\'};
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        app.Drives.push_back(std::wstring{letter, L':'});
        app.DriveLabels.push_back(yadua::Utf8(app.Drives.back()));
    }
}

static void StartScan(App& app) {
    if (app.Scanning || app.Deleting || app.Rescanning || app.Applying ||
        app.Drives.empty())
        return;
    app.Monitor.Stop(); // a fresh scan resets the change-tracking baseline
    if (app.ScanThread.joinable()) app.ScanThread.join();
    app.Pending = std::make_unique<yadua::ScanResult>();
    app.Progress.BytesRead  = 0;
    app.Progress.TotalBytes = 0;
    app.Progress.Stage      = yadua::ScanProgress::Opening;
    app.ScanDone  = false;
    app.Scanning  = true;
    app.Error.clear();
    std::wstring drive = app.Drives[app.DriveIndex];
    app.ScanThread = std::thread([&app, drive] {
        std::wstring err;
        if (!yadua::ScanVolumeAuto(drive, 0, *app.Pending, err, &app.Progress))
            app.PendingError = err;
        app.ScanDone = true; // release: PendingError/Pending written before this
    });
}

// ============================================================================
// Filtering & sorting
// ============================================================================

// Filter language: space-separated terms, all must match (AND).
//   foo       name contains "foo" (case-insensitive)
//   *.iso     extension is "iso" (ext:iso also works)
//   >100mb    size greater than (files: own size, folders: subtree size)
//   <1.5gb    size less than    (units: b, kb, mb, gb, tb; bare = bytes)
struct FilterTerm {
    enum Kind { Substr, Ext, SizeGT, SizeLT } What = Substr;
    std::wstring Text;   // lowercased needle / extension
    uint64_t     Bytes = 0;
};

static bool ParseSizeToken(const std::wstring& t, uint64_t& out) {
    size_t i = 0;
    while (i < t.size() && (iswdigit(t[i]) || t[i] == L'.')) ++i;
    if (i == 0) return false;
    double v = _wtof(t.substr(0, i).c_str());
    std::wstring unit = t.substr(i);
    double mult;
    if (unit.empty() || unit == L"b") mult = 1;
    else if (unit == L"k" || unit == L"kb") mult = 1024.0;
    else if (unit == L"m" || unit == L"mb") mult = 1048576.0;
    else if (unit == L"g" || unit == L"gb") mult = 1073741824.0;
    else if (unit == L"t" || unit == L"tb") mult = 1099511627776.0;
    else return false;
    out = (uint64_t)(v * mult);
    return true;
}

static std::vector<FilterTerm> ParseFilter(const char* utf8) {
    std::wstring raw = yadua::Wide(utf8);
    for (wchar_t& c : raw) c = towlower(c);
    std::vector<FilterTerm> terms;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t end = raw.find(L' ', pos);
        if (end == std::wstring::npos) end = raw.size();
        std::wstring tok = raw.substr(pos, end - pos);
        pos = end + 1;
        if (tok.empty()) continue;

        FilterTerm term;
        if ((tok[0] == L'>' || tok[0] == L'<') &&
            ParseSizeToken(tok.substr(1), term.Bytes)) {
            term.What = tok[0] == L'>' ? FilterTerm::SizeGT : FilterTerm::SizeLT;
        } else if (tok.rfind(L"ext:", 0) == 0 && tok.size() > 4) {
            term.What = FilterTerm::Ext;
            term.Text = tok.substr(4);
        } else if (tok.rfind(L"*.", 0) == 0 && tok.size() > 2) {
            term.What = FilterTerm::Ext;
            term.Text = tok.substr(2);
        } else {
            term.What = FilterTerm::Substr;
            term.Text = tok;
        }
        terms.push_back(std::move(term));
    }
    return terms;
}

// Recompute the visibility map for the current filter: mark every node that
// matches all terms, then mark all its ancestors so the tree path down to
// each match stays visible.
static void RecomputeFilter(App& app) {
    app.FileListDirty = true; // the flat Files / File Types views derive from it
    app.TypeListDirty = true;
    const yadua::ScanResult* r = app.Result.get();
    std::vector<FilterTerm> terms = r ? ParseFilter(app.Filter)
                                      : std::vector<FilterTerm>{};
    if (terms.empty()) { app.Visible.clear(); return; }

    app.Visible.assign(r->Nodes.size(), 0);
    std::wstring name;
    for (uint32_t i = 0; i < r->Nodes.size(); ++i) {
        if (!r->Exists(i)) continue;
        name = r->Name(i);
        for (wchar_t& c : name) c = towlower(c);

        bool match = true;
        for (const FilterTerm& t : terms) {
            switch (t.What) {
                case FilterTerm::Substr:
                    match = name.find(t.Text) != std::wstring::npos;
                    break;
                case FilterTerm::Ext:
                    match = name.size() > t.Text.size() &&
                            name[name.size() - t.Text.size() - 1] == L'.' &&
                            name.compare(name.size() - t.Text.size(),
                                         t.Text.size(), t.Text) == 0;
                    break;
                case FilterTerm::SizeGT: match = r->SizeOf(i) > t.Bytes; break;
                case FilterTerm::SizeLT: match = r->SizeOf(i) < t.Bytes; break;
            }
            if (!match) break;
        }
        if (!match) continue;

        app.Visible[i] = 1;
        uint32_t cur = r->Nodes[i].Parent;
        for (int depth = 0; depth < 512; ++depth) {  // mark the ancestor chain
            if (cur >= r->Nodes.size() || app.Visible[cur]) break;
            app.Visible[cur] = 1;
            if (r->Nodes[cur].Parent == cur) break;
            cur = r->Nodes[cur].Parent;
        }
    }
}

// Allocation-free case-insensitive name compare straight out of the arena
// (a comparator that builds std::wstrings would dominate the sort time).
static int CompareNames(const yadua::ScanResult& r, uint32_t a, uint32_t b) {
    const wchar_t* pa = r.NameArena.data() + r.Nodes[a].NameOffset;
    const wchar_t* pb = r.NameArena.data() + r.Nodes[b].NameOffset;
    uint16_t la = r.Nodes[a].NameLength, lb = r.Nodes[b].NameLength;
    for (uint16_t i = 0; i < la && i < lb; ++i) {
        wchar_t ca = towlower(pa[i]), cb = towlower(pb[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return la == lb ? 0 : (la < lb ? -1 : 1);
}

// Re-sort every directory's child range by the clicked column. The canonical
// order (size descending) needs no copy; anything else sorts a parallel list.
static void ApplySort(App& app, const ImGuiTableColumnSortSpecs& spec) {
    const yadua::ScanResult& r = *app.Result;
    bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;

    if (spec.ColumnUserID == ColSize && !asc) { app.UseSorted = false; return; }

    app.SortedChildren = r.Children.List;
    auto key = [&](uint32_t n) -> uint64_t {
        switch (spec.ColumnUserID) {
            case ColFiles:    return r.IsDir(n) ? r.Totals[n].FileCount : 0;
            case ColFolders:  return r.IsDir(n) ? r.Totals[n].DirCount : 0;
            case ColModified: return r.Nodes[n].ModifiedTime;
            default:          return r.SizeOf(n); // ColSize / ColPercent
        }
    };
    for (size_t d = 0; d + 1 < r.Children.Offset.size(); ++d) {
        auto begin = app.SortedChildren.begin() + r.Children.Offset[d];
        auto end   = app.SortedChildren.begin() + r.Children.Offset[d + 1];
        if (spec.ColumnUserID == ColName)
            std::sort(begin, end, [&](uint32_t a, uint32_t b) {
                int c = CompareNames(r, a, b);
                return asc ? c < 0 : c > 0;
            });
        else
            std::sort(begin, end, [&](uint32_t a, uint32_t b) {
                return asc ? key(a) < key(b) : key(a) > key(b);
            });
    }
    app.UseSorted = true;
}

// ============================================================================
// Deletion (Recycle Bin)
// ============================================================================

static void StartApply(App& app) {
    if (app.Applying || app.Scanning || app.Deleting || app.Rescanning ||
        !app.Result)
        return;
    if (app.ApplyThread.joinable()) app.ApplyThread.join();
    app.ApplyError.clear();
    app.ApplyDone = false;
    app.Applying  = true;
    app.ApplyThread = std::thread([&app] {
        std::vector<uint64_t> dirty = app.Monitor.TakeDirty();
        app.ApplyCount = dirty.size();
        std::wstring err;
        if (!yadua::ApplyMftUpdates(*app.Result, dirty, err,
                                    &app.ApplyUnresolved))
            app.ApplyError = err;
        app.ApplyDone = true;
    });
}

static void StartRescan(App& app, uint32_t node) {
    if (app.Rescanning || app.Scanning || app.Deleting || app.Applying ||
        !app.Result)
        return;
    if (app.RescanThread.joinable()) app.RescanThread.join();
    app.RescanPath  = app.Result->Path(node);
    app.RescanError.clear();
    app.RescanDone  = false;
    app.Rescanning  = true;
    app.RescanThread = std::thread([&app, node] {
        std::wstring err;
        if (!yadua::RescanSubtree(*app.Result, node, err, &app.RescanProgress))
            app.RescanError = err;
        app.RescanDone = true;
    });
}

static void StartDelete(App& app, uint32_t node) {
    if (app.Deleting || app.Scanning || app.Rescanning || app.Applying ||
        !app.Result)
        return;
    if (app.DeleteThread.joinable()) app.DeleteThread.join();
    app.DeleteNode   = node;
    app.DeletePath   = app.Result->Path(node);
    app.DeleteDone   = false;
    app.Deleting     = true;
    std::wstring path = app.DeletePath;
    app.DeleteThread = std::thread([&app, path] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring from = path;
        from.push_back(L'\0'); // SHFileOperation wants double-null termination
        SHFILEOPSTRUCTW op{};
        op.wFunc  = FO_DELETE;
        op.pFrom  = from.c_str();
        op.fFlags = FOF_ALLOWUNDO; // Recycle Bin, shell shows progress UI
        app.DeleteResult  = SHFileOperationW(&op);
        app.DeleteAborted = op.fAnyOperationsAborted;
        CoUninitialize();
        app.DeleteDone = true;
    });
}

// After a successful recycle: subtract the subtree from every ancestor's
// totals and flag all its nodes as gone, so tree/treemap/filter skip them
// without a rescan.
static void RemoveSubtree(yadua::ScanResult& r, uint32_t idx) {
    uint64_t logical, allocated, files, dirs;
    if (r.IsDir(idx)) {
        logical   = r.Totals[idx].LogicalSize;
        allocated = r.Totals[idx].AllocatedSize;
        files     = r.Totals[idx].FileCount;
        dirs      = r.Totals[idx].DirCount + 1; // + the folder itself
    } else {
        logical   = r.Nodes[idx].LogicalSize;
        allocated = r.Nodes[idx].AllocatedSize;
        files     = 1;
        dirs      = 0;
    }
    r.FileCount -= files;
    r.DirCount  -= dirs;

    uint32_t cur = r.Nodes[idx].Parent;
    for (int depth = 0; depth < 512; ++depth) {
        if (cur >= r.Nodes.size() || !r.IsDir(cur)) break;
        yadua::DirTotals& t = r.Totals[cur];
        t.LogicalSize   -= logical;
        t.AllocatedSize -= allocated;
        t.FileCount     -= files;
        t.DirCount      -= dirs;
        if (r.Nodes[cur].Parent == cur) break; // root
        cur = r.Nodes[cur].Parent;
    }

    // Flag the whole subtree as gone (iterative DFS over the child index).
    std::vector<uint32_t> stack{idx};
    while (!stack.empty()) {
        uint32_t n = stack.back();
        stack.pop_back();
        r.Nodes[n].Flags = 0;
        for (uint32_t c = r.Children.Offset[n]; c < r.Children.Offset[n + 1]; ++c)
            stack.push_back(r.Children.List[c]);
    }
}

static void FinishDelete(App& app) {
    app.DeleteThread.join();
    app.Deleting = false;
    if (app.DeleteResult == 0 && !app.DeleteAborted) {
        yadua::ScanResult& r = *app.Result;
        std::string what = yadua::Utf8(app.DeletePath);
        app.Status = "Recycled " + what + " (" +
                     yadua::HumanSize(r.SizeOf(app.DeleteNode)) + ")";
        RemoveSubtree(r, app.DeleteNode);
        if (!r.Exists(app.Treemap.Root())) app.Treemap.Reset();
        app.Treemap.Invalidate();
        RecomputeFilter(app);
    } else if (app.DeleteAborted || app.DeleteResult == 1223 /*ERROR_CANCELLED*/) {
        app.Status = "Delete cancelled";
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Delete failed (shell error 0x%X)",
                 (unsigned)app.DeleteResult);
        app.Status = buf;
    }
}

// ============================================================================
// Settings + auto-update (consent-gated)
// ============================================================================

// A tiny key=value file under %LOCALAPPDATA%\YADUA. Only two settings live
// here today (the launch update-check opt-out and a skipped version), so the
// format is deliberately trivial.
static std::wstring SettingsPath() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring dir = std::wstring(base) + L"\\YADUA";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.txt";
}

static void LoadSettings(App& app) {
    std::wstring p = SettingsPath();
    if (p.empty()) return;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq), val = s.substr(eq + 1);
        if (key == "checkOnLaunch")       app.UpdCheckOnLaunch = (val != "0");
        else if (key == "skip")           app.UpdSkipVersion = yadua::Wide(val);
        else if (key == "showAllocated")  app.ShowAllocated = (val != "0");
    }
    fclose(f);
}

static void SaveSettings(App& app) {
    std::wstring p = SettingsPath();
    if (p.empty()) return;
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return;
    fprintf(f, "checkOnLaunch=%d\n", app.UpdCheckOnLaunch ? 1 : 0);
    fprintf(f, "skip=%s\n", yadua::Utf8(app.UpdSkipVersion).c_str());
    fprintf(f, "showAllocated=%d\n", app.ShowAllocated ? 1 : 0);
    fclose(f);
}

static void StartUpdateCheck(App& app, bool manual) {
    if (app.UpdChecking || app.UpdDownloading || !yadua::UpdaterConfigured())
        return;
    if (app.UpdCheckThread.joinable()) app.UpdCheckThread.join();
    app.UpdChecking = true;
    app.UpdCheckDone = false;
    app.UpdCheckError.clear();
    if (manual) app.UpdCheckedThisRun = true;
    std::wstring cur = yadua::RunningVersion();
    app.UpdCheckThread = std::thread([&app, cur] {
        yadua::UpdateInfo info;
        std::wstring err;
        if (!yadua::CheckForUpdate(cur, info, err)) app.UpdCheckError = err;
        else app.UpdInfo = info;
        app.UpdCheckDone = true; // release: UpdInfo/UpdCheckError written before
    });
}

static void StartUpdateDownload(App& app) {
    if (app.UpdDownloading || app.UpdChecking || !app.UpdInfo.Available) return;
    if (app.UpdDownloadThread.joinable()) app.UpdDownloadThread.join();
    app.UpdDownloading = true;
    app.UpdDownloadDone = false;
    app.UpdDownloadError.clear();
    app.UpdInstallerPath.clear();
    app.UpdProgress.BytesRead  = 0;
    app.UpdProgress.TotalBytes = 0;
    yadua::UpdateInfo info = app.UpdInfo;
    app.UpdDownloadThread = std::thread([&app, info] {
        std::wstring path, err;
        if (!yadua::DownloadUpdate(info, path, err, &app.UpdProgress))
            app.UpdDownloadError = err;
        else app.UpdInstallerPath = path;
        app.UpdDownloadDone = true;
    });
}

// True when there is a verified, strictly-newer release the user hasn't
// chosen to skip. Gated on !UpdChecking so we never read UpdInfo while the
// check thread is mid-write (the main loop clears UpdChecking only after the
// join, which synchronizes that write).
static bool UpdateBannerVisible(const App& app) {
    return yadua::UpdaterConfigured() && !app.UpdChecking &&
           app.UpdInfo.Available && app.UpdInfo.Version != app.UpdSkipVersion;
}

// ============================================================================
// Shared context menu (tree rows + treemap rects)
// ============================================================================

static void NodeMenuItems(App& app, const yadua::ScanResult& r, uint32_t idx,
                          bool fromTreemap) {
    if (fromTreemap) {
        if (ImGui::MenuItem("Reveal in tree")) {
            app.SelectedNode = idx;
            app.RevealNode   = idx;
            app.RevealOpen.assign(r.Nodes.size(), 0);
            uint32_t cur = r.Nodes[idx].Parent;
            for (int depth = 0; depth < 512; ++depth) {
                if (cur >= r.Nodes.size()) break;
                app.RevealOpen[cur] = 1;
                if (r.Nodes[cur].Parent == cur) break;
                cur = r.Nodes[cur].Parent;
            }
            app.SwitchToTree = true;
        }
    } else {
        if (ImGui::MenuItem("Show in treemap")) {
            app.SelectedNode = idx;
            uint32_t target = r.IsDir(idx) ? idx : r.Nodes[idx].Parent;
            if (target < r.Nodes.size() && r.Exists(target) && r.IsDir(target))
                app.Treemap.SetRoot(target);
            // With the docked panel the zoom is already visible below the
            // tree; only switch tabs when the panel is hidden.
            if (!app.ShowMapPanel) app.SwitchToTreemap = true;
        }
    }
    if (r.IsDir(idx) && !(r.Nodes[idx].Flags & yadua::kNodeReparse)) {
        ImGui::BeginDisabled(app.Scanning || app.Deleting || app.Rescanning);
        if (ImGui::MenuItem("Rescan folder")) StartRescan(app, idx);
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Open in Explorer")) {
        std::wstring args = L"/select,\"" + r.Path(idx) + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
                      nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::MenuItem("Copy path"))
        ImGui::SetClipboardText(yadua::Utf8(r.Path(idx)).c_str());
    if (ImGui::MenuItem("Properties")) {
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask  = SEE_MASK_INVOKEIDLIST;
        sei.lpVerb = L"properties";
        std::wstring path = r.Path(idx);
        sei.lpFile = path.c_str();
        sei.nShow  = SW_SHOW;
        ShellExecuteExW(&sei);
    }
    ImGui::Separator();
    ImGui::BeginDisabled(idx == (uint32_t)yadua::kRootRecord || app.Deleting);
    if (ImGui::MenuItem("Delete (Recycle Bin)..."))
        app.ConfirmDelete = idx;
    ImGui::EndDisabled();
}

// ============================================================================
// Tree view
// ============================================================================

static void DrawTree(App& app, const yadua::ScanResult& r, uint32_t idx,
                     uint64_t parentSize, bool isRoot) {
    // Very large directories: render the biggest entries and summarize the
    // rest instead of emitting 100k rows.
    constexpr uint32_t kMaxChildrenShown = 2000;

    const bool     filtered   = !app.Visible.empty();
    const bool     isDir      = r.IsDir(idx);
    const uint64_t size       = r.SizeOf(idx);
    const uint32_t childBegin = r.Children.Offset[idx];
    const uint32_t childEnd   = r.Children.Offset[idx + 1];
    const bool     leaf       = !isDir || childBegin == childEnd;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (idx == app.SelectedNode) flags |= ImGuiTreeNodeFlags_Selected;
    if (isRoot) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    else if (filtered && isDir) ImGui::SetNextItemOpen(true); // expand to matches
    if (!app.RevealOpen.empty() && isDir && app.RevealOpen[idx])
        ImGui::SetNextItemOpen(true);                         // expand to reveal

    std::string label = isRoot ? yadua::Utf8(r.Drive) : yadua::Utf8(r.Name(idx));
    if (r.Nodes[idx].Flags & yadua::kNodeReparse)
        label += "  [link]"; // junction / symlink / cloud placeholder
    // Tint file names by extension with the same hue the treemap uses, so
    // the two views read consistently.
    if (!isDir)
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ExtensionColor(r, idx, 0.40f, 0.95f));
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)idx, flags, "%s", label.c_str());
    if (!isDir) ImGui::PopStyleColor();
    if (idx == app.RevealNode) ImGui::SetScrollHereY(0.35f);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) app.SelectedNode = idx;
    if (ImGui::BeginPopupContextItem()) {
        NodeMenuItems(app, r, idx, false);
        ImGui::EndPopup();
    }

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(yadua::HumanSize(size).c_str());

    ImGui::TableNextColumn();
    float frac = parentSize ? (float)((double)size / (double)parentSize) : 1.0f;
    char overlay[16];
    snprintf(overlay, sizeof(overlay), "%.1f%%", frac * 100.0);
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.0f), overlay);

    ImGui::TableNextColumn();
    if (isDir) ImGui::Text("%llu", r.Totals[idx].FileCount);

    ImGui::TableNextColumn();
    if (isDir) ImGui::Text("%llu", r.Totals[idx].DirCount);

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(FormatTime(r.Nodes[idx].ModifiedTime).c_str());

    if (open && !leaf) {
        const uint32_t* list = app.UseSorted ? app.SortedChildren.data()
                                             : r.Children.List.data();
        uint32_t shown = 0, hidden = 0;
        uint64_t hiddenBytes = 0;
        for (uint32_t c = childBegin; c < childEnd; ++c) {
            uint32_t child = list[c];
            bool revealing = child == app.RevealNode ||
                             (!app.RevealOpen.empty() && app.RevealOpen[child]);
            if (!r.Exists(child)) continue;            // recycled this session
            if (filtered && !app.Visible[child] && !revealing) continue;
            if (shown >= kMaxChildrenShown && !revealing) {
                ++hidden;
                hiddenBytes += r.SizeOf(child);
                continue;
            }
            DrawTree(app, r, child, size, false);
            ++shown;
        }
        if (hidden) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("(... %u more items, %s)", hidden,
                                yadua::HumanSize(hiddenBytes).c_str());
        }
        ImGui::TreePop();
    }
}

static void DrawTreeTable(App& app, const yadua::ScanResult& r) {
    if (!ImGui::BeginTable("tree", 6,
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_Reorderable | ImGuiTableFlags_Sortable))
        return;
    float ch = ImGui::GetFontSize();
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0, ColName);
    ImGui::TableSetupColumn("Size",
                            ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_DefaultSort |
                            ImGuiTableColumnFlags_PreferSortDescending,
                            ch * 6, ColSize);
    ImGui::TableSetupColumn("% of parent",
                            ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_NoSort, ch * 8, ColPercent);
    ImGui::TableSetupColumn("Files",
                            ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_PreferSortDescending,
                            ch * 5, ColFiles);
    ImGui::TableSetupColumn("Folders",
                            ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_PreferSortDescending,
                            ch * 5, ColFolders);
    ImGui::TableSetupColumn("Modified",
                            ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_PreferSortDescending,
                            ch * 9, ColModified);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
        specs && specs->SpecsDirty) {
        if (specs->SpecsCount > 0) ApplySort(app, specs->Specs[0]);
        else app.UseSorted = false; // tri-state: back to canonical order
        specs->SpecsDirty = false;
    }

    DrawTree(app, r, (uint32_t)yadua::kRootRecord, 0, true);
    ImGui::EndTable();

    // Reveal is a one-shot: ancestors were forced open and the row scrolled
    // into view this frame; the tree keeps the open state on its own now.
    if (app.RevealNode != UINT32_MAX) {
        app.RevealNode = UINT32_MAX;
        app.RevealOpen.clear();
    }
}

// Tree tab: the tree table on top and (optionally) a docked treemap panel
// below it, separated by a draggable horizontal splitter.
static void DrawTreeTab(App& app, const yadua::ScanResult& r) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitter = 6.0f;
    const float minTree = 120.0f, minMap = 100.0f;

    if (!app.ShowMapPanel || avail.y < minTree + minMap + splitter) {
        DrawTreeTable(app, r);
        return;
    }

    if (app.MapPanelHeight <= 0) app.MapPanelHeight = avail.y * 0.38f;
    app.MapPanelHeight = std::min(std::max(app.MapPanelHeight, minMap),
                                  avail.y - minTree - splitter);

    ImGui::BeginChild("##treepane",
                      ImVec2(0, avail.y - app.MapPanelHeight - splitter));
    DrawTreeTable(app, r);
    ImGui::EndChild();

    // Splitter: dragging up grows the map panel.
    ImGui::InvisibleButton("##split", ImVec2(-FLT_MIN, splitter));
    if (ImGui::IsItemActive())
        app.MapPanelHeight -= ImGui::GetIO().MouseDelta.y;
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImVec2 smin = ImGui::GetItemRectMin(), smax = ImGui::GetItemRectMax();
    float midY = (smin.y + smax.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(smin.x, midY), ImVec2(smax.x, midY),
        ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive
                           : ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered
                                                    : ImGuiCol_Separator),
        2.0f);

    ImGui::BeginChild("##mappane");
    app.Treemap.Draw(r, [&](uint32_t node) {
        NodeMenuItems(app, *app.Result, node, true);
    });
    uint32_t clicked = app.Treemap.ConsumeClicked();
    if (clicked != UINT32_MAX) app.SelectedNode = clicked;
    ImGui::EndChild();
}

// ============================================================================
// Files view (flat largest-files list) + File Types (extension summary)
//
// Both ignore the folder hierarchy: the Files tab is "where are the big files
// on this volume, wherever they live", the File Types tab is "which kinds of
// files eat the space". They honor the same filter box as the tree.
// ============================================================================

// Lowercased extension (chars after the last dot), or empty for "no type".
// Mirrors the treemap's rule so a file lands in the same bucket/hue it is
// colored with elsewhere: a dot must exist and the tail be at most 8 chars.
static std::wstring FileExt(const yadua::ScanResult& r, uint32_t idx) {
    std::wstring name = r.Name(idx);
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || name.size() - dot > 9)
        return std::wstring();
    std::wstring ext = name.substr(dot + 1);
    for (wchar_t& c : ext) c = towlower(c);
    return ext;
}

// FNV-1a hue over a bare (already-lowercased) extension, matching the FNV the
// treemap/tree use so a given type reads the same color across every view.
static ImU32 ExtHueColor(const std::wstring& ext, float sat, float val) {
    uint32_t h = 2166136261u;
    if (!ext.empty() && ext.size() <= 8)
        for (wchar_t c : ext) h = (h ^ (uint32_t)c) * 16777619u;
    float hue = (float)(h % 360u) / 360.0f;
    return ImColor(ImColor::HSV(hue, sat, val));
}

static void SortFileList(App& app) {
    const yadua::ScanResult& r = *app.Result;
    bool asc = app.FileSortAsc;
    if (app.FileSortCol == ColName)
        std::sort(app.FileList.begin(), app.FileList.end(),
                  [&](uint32_t a, uint32_t b) {
                      int c = CompareNames(r, a, b);
                      return asc ? c < 0 : c > 0;
                  });
    else if (app.FileSortCol == ColModified)
        std::sort(app.FileList.begin(), app.FileList.end(),
                  [&](uint32_t a, uint32_t b) {
                      uint64_t ma = r.Nodes[a].ModifiedTime;
                      uint64_t mb = r.Nodes[b].ModifiedTime;
                      if (ma != mb) return asc ? ma < mb : ma > mb;
                      return CompareNames(r, a, b) < 0;
                  });
    else // ColSize (also the % column, which ranks by size)
        std::sort(app.FileList.begin(), app.FileList.end(),
                  [&](uint32_t a, uint32_t b) {
                      uint64_t sa = r.SizeOf(a);
                      uint64_t sb = r.SizeOf(b);
                      if (sa != sb) return asc ? sa < sb : sa > sb;
                      return CompareNames(r, a, b) < 0; // stable-ish tiebreak
                  });
}

// Collect every file (directories excluded) that passes the current filter.
static void RebuildFileList(App& app) {
    const yadua::ScanResult& r = *app.Result;
    const bool filtered = !app.Visible.empty();
    app.FileList.clear();
    app.FileListBytes = 0;
    for (uint32_t i = 0; i < r.Nodes.size(); ++i) {
        if (!r.Exists(i) || r.IsDir(i)) continue;
        if (filtered && !app.Visible[i]) continue;
        app.FileList.push_back(i);
        app.FileListBytes += r.SizeOf(i);
    }
    SortFileList(app);
    app.FileListDirty = false;
}

static void DrawFilesTab(App& app, const yadua::ScanResult& r) {
    if (app.FileListDirty) RebuildFileList(app);
    uint64_t volTotal = r.SizeOf(yadua::kRootRecord);

    ImGui::TextDisabled("%zu files, %s total%s%s", app.FileList.size(),
                        yadua::HumanSize(app.FileListBytes).c_str(),
                        r.DisplayAllocated ? " on disk" : "",
                        app.Visible.empty() ? "" : "  (filtered)");

    if (!ImGui::BeginTable("files", 5,
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_Reorderable | ImGuiTableFlags_Sortable))
        return;
    float ch = ImGui::GetFontSize();
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0, ColName);
    ImGui::TableSetupColumn("Size",
                            ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_DefaultSort |
                            ImGuiTableColumnFlags_PreferSortDescending,
                            ch * 6, ColSize);
    ImGui::TableSetupColumn("% of volume",
                            ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_NoSort, ch * 8, ColPercent);
    ImGui::TableSetupColumn("Modified",
                            ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_PreferSortDescending,
                            ch * 9, ColModified);
    ImGui::TableSetupColumn("Folder",
                            ImGuiTableColumnFlags_WidthStretch |
                            ImGuiTableColumnFlags_NoSort, 0, ColFolders);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
        specs && specs->SpecsDirty) {
        if (specs->SpecsCount > 0) {
            app.FileSortCol = specs->Specs[0].ColumnUserID;
            app.FileSortAsc =
                specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            SortFileList(app);
        }
        specs->SpecsDirty = false;
    }

    // Millions of files are possible, so only the visible rows are laid out.
    ImGuiListClipper clipper;
    clipper.Begin((int)app.FileList.size());
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            uint32_t idx = app.FileList[row];
            ImGui::TableNextRow();
            ImGui::PushID((int)idx);

            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ExtensionColor(r, idx, 0.40f, 0.95f));
            if (ImGui::Selectable(yadua::Utf8(r.Name(idx)).c_str(),
                                  idx == app.SelectedNode,
                                  ImGuiSelectableFlags_SpanAllColumns))
                app.SelectedNode = idx;
            ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem()) {
                app.SelectedNode = idx;
                NodeMenuItems(app, r, idx, false);
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(yadua::HumanSize(r.SizeOf(idx)).c_str());

            ImGui::TableSetColumnIndex(2);
            float frac = volTotal
                ? (float)((double)r.SizeOf(idx) / (double)volTotal)
                : 0.0f;
            char overlay[16];
            snprintf(overlay, sizeof(overlay), "%.2f%%", frac * 100.0);
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.0f), overlay);

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(FormatTime(r.Nodes[idx].ModifiedTime).c_str());

            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(
                yadua::Utf8(r.Path(r.Nodes[idx].Parent)).c_str());

            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

// Aggregate every filtered file by extension into a size-descending table.
static void RebuildTypeList(App& app) {
    const yadua::ScanResult& r = *app.Result;
    const bool filtered = !app.Visible.empty();
    std::unordered_map<std::wstring, App::TypeAgg> agg;
    for (uint32_t i = 0; i < r.Nodes.size(); ++i) {
        if (!r.Exists(i) || r.IsDir(i)) continue;
        if (filtered && !app.Visible[i]) continue;
        std::wstring ext = FileExt(r, i);
        App::TypeAgg& a = agg[ext];
        a.Ext    = ext;
        a.Bytes += r.SizeOf(i);
        a.Count += 1;
    }
    app.TypeList.clear();
    app.TypeList.reserve(agg.size());
    for (auto& kv : agg) app.TypeList.push_back(std::move(kv.second));
    std::sort(app.TypeList.begin(), app.TypeList.end(),
              [](const App::TypeAgg& a, const App::TypeAgg& b) {
                  return a.Bytes > b.Bytes;
              });
    app.TypeListDirty = false;
}

static void DrawTypesTab(App& app, const yadua::ScanResult& r) {
    if (app.TypeListDirty) RebuildTypeList(app);
    uint64_t volTotal = r.SizeOf(yadua::kRootRecord);

    ImGui::TextDisabled("%zu file types%s  -  click a row to filter to it",
                        app.TypeList.size(),
                        app.Visible.empty() ? "" : "  (filtered)");

    if (!ImGui::BeginTable("types", 4,
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
        return;
    float ch = ImGui::GetFontSize();
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ch * 6);
    ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthFixed, ch * 6);
    ImGui::TableSetupColumn("% of volume",
                            ImGuiTableColumnFlags_WidthFixed, ch * 8);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin((int)app.TypeList.size());
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const App::TypeAgg& t = app.TypeList[row];
            ImGui::TableNextRow();
            ImGui::PushID(row);

            ImGui::TableSetColumnIndex(0);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float lh = ImGui::GetTextLineHeight();
            float sq = lh * 0.72f;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(p.x, p.y + (lh - sq) * 0.5f),
                ImVec2(p.x + sq, p.y + (lh - sq) * 0.5f + sq),
                ExtHueColor(t.Ext, 0.55f, 0.80f), 2.0f);
            ImGui::SetCursorScreenPos(
                ImVec2(p.x + sq + ImGui::GetStyle().ItemInnerSpacing.x, p.y));
            std::string label = t.Ext.empty() ? std::string("(no extension)")
                                              : "." + yadua::Utf8(t.Ext);
            if (ImGui::Selectable(label.c_str()) && !t.Ext.empty()) {
                snprintf(app.Filter, sizeof(app.Filter), "*.%s",
                         yadua::Utf8(t.Ext).c_str());
                RecomputeFilter(app);
                app.SwitchToFiles = true;
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(yadua::HumanSize(t.Bytes).c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu", (unsigned long long)t.Count);

            ImGui::TableSetColumnIndex(3);
            float frac = volTotal ? (float)((double)t.Bytes / (double)volTotal)
                                  : 0.0f;
            char overlay[16];
            snprintf(overlay, sizeof(overlay), "%.2f%%", frac * 100.0);
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.0f), overlay);

            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

// ============================================================================
// Menu bar
// ============================================================================

// The current selection, if it still exists (deletion can invalidate it).
static bool HasSelection(const App& app, uint32_t& sel) {
    sel = app.SelectedNode;
    const yadua::ScanResult* r = app.Result.get();
    return r && sel != UINT32_MAX && sel < r->Nodes.size() && r->Exists(sel);
}

// Push the logical/allocated ("size on disk") display mode into the shared
// result so the tree, treemap, and both summaries switch together, and mark
// the cached views stale.
static void ApplySizeMode(App& app) {
    if (app.Result) app.Result->DisplayAllocated = app.ShowAllocated;
    app.Treemap.Invalidate();
    app.FileListDirty = true;
    app.TypeListDirty = true;
}

static void DrawMenuBar(App& app) {
    yadua::ScanResult* r = app.Result.get();
    const bool busy = app.Scanning || app.Deleting || app.Rescanning ||
                      app.Applying;
    uint32_t sel;
    const bool hasSel = HasSelection(app, sel);

    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem(r ? "Rescan" : "Scan", "Ctrl+R", false,
                            !app.Drives.empty() && !busy))
            StartScan(app);
        if (ImGui::BeginMenu("Scan drive", !app.Drives.empty() && !busy)) {
            for (int i = 0; i < (int)app.Drives.size(); ++i)
                if (ImGui::MenuItem(app.DriveLabels[i].c_str(), nullptr,
                                    i == app.DriveIndex)) {
                    app.DriveIndex = i;
                    StartScan(app);
                }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) PostQuitMessage(0);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Copy path", "Ctrl+C", false, hasSel))
            ImGui::SetClipboardText(yadua::Utf8(r->Path(sel)).c_str());
        if (ImGui::MenuItem("Open in Explorer", nullptr, false, hasSel)) {
            std::wstring args = L"/select,\"" + r->Path(sel) + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
                          nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete to Recycle Bin", "Del", false,
                            hasSel && sel != (uint32_t)yadua::kRootRecord &&
                                !app.Deleting))
            app.ConfirmDelete = sel;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Size on disk (allocated)", nullptr,
                            app.ShowAllocated)) {
            app.ShowAllocated = !app.ShowAllocated;
            ApplySizeMode(app);
            SaveSettings(app);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Show bytes actually reserved on disk (allocated,\n"
                              "cluster-rounded) instead of logical file size.");
        ImGui::Separator();
        ImGui::MenuItem("Treemap panel", nullptr, &app.ShowMapPanel);
        ImGui::MenuItem("Treemap cushion shading", nullptr, &app.Treemap.Cushion);
        ImGui::Separator();
        ImGui::BeginDisabled(!r);
        if (ImGui::MenuItem("Tree"))       app.SwitchToTree = true;
        if (ImGui::MenuItem("Files"))      app.SwitchToFiles = true;
        if (ImGui::MenuItem("File Types")) app.SwitchToFileTypes = true;
        if (ImGui::MenuItem("Treemap"))    app.SwitchToTreemap = true;
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Search")) {
        if (ImGui::MenuItem("Find...", "Ctrl+F", false, r != nullptr))
            app.FocusFilter = true;
        if (ImGui::MenuItem("Clear filter", "Esc", false, app.Filter[0] != 0)) {
            app.Filter[0] = '\0';
            RecomputeFilter(app);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        const bool live = app.Monitor.Active() && !app.Monitor.Lost();
        const uint64_t pending = live ? app.Monitor.Pending() : 0;
        char applyLbl[64];
        snprintf(applyLbl, sizeof(applyLbl), "Apply %llu filesystem change%s",
                 (unsigned long long)pending, pending == 1 ? "" : "s");
        if (ImGui::MenuItem(applyLbl, nullptr, false, pending > 0 && !busy))
            StartApply(app);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Fold filesystem changes seen since the scan into "
                              "the tree (via the NTFS change journal), without "
                              "a full rescan.");
        const bool canRescan = hasSel && r->IsDir(sel) &&
                               !(r->Nodes[sel].Flags & yadua::kNodeReparse) &&
                               !busy;
        if (ImGui::MenuItem("Rescan selected folder", nullptr, false, canRescan))
            StartRescan(app, sel);
        ImGui::Separator();
        if (ImGui::MenuItem("Check for updates...", nullptr, false,
                            yadua::UpdaterConfigured() && !app.UpdChecking &&
                                !app.UpdDownloading)) {
            StartUpdateCheck(app, true);
            app.ShowAbout = true; // surface the result
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About YADUA")) app.ShowAbout = true;
        if (ImGui::MenuItem("Project on GitHub"))
            ShellExecuteW(nullptr, L"open",
                          L"https://github.com/nathanaelries/YADUA", nullptr,
                          nullptr, SW_SHOWNORMAL);
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

// Global keyboard shortcuts mirrored from the menus.
static void HandleShortcuts(App& app) {
    const bool busy = app.Scanning || app.Deleting || app.Rescanning ||
                      app.Applying;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R) &&
        !app.Drives.empty() && !busy)
        StartScan(app);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
        app.FocusFilter = true;
    // Text-editing keys only act on nodes when no input box has focus.
    if (!ImGui::GetIO().WantTextInput) {
        uint32_t sel;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && HasSelection(app, sel) &&
            sel != (uint32_t)yadua::kRootRecord && !app.Deleting)
            app.ConfirmDelete = sel;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && app.Filter[0]) {
            app.Filter[0] = '\0';
            RecomputeFilter(app);
        }
    }
}

// ============================================================================
// Top-level UI
// ============================================================================

static void DrawUi(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawMenuBar(app);
    HandleShortcuts(app);

    // ---- Toolbar -----------------------------------------------------------
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    if (ImGui::BeginCombo("##drive",
            app.Drives.empty() ? "?" : app.DriveLabels[app.DriveIndex].c_str())) {
        for (int i = 0; i < (int)app.Drives.size(); ++i)
            if (ImGui::Selectable(app.DriveLabels[i].c_str(), i == app.DriveIndex))
                app.DriveIndex = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(app.Scanning || app.Deleting);
    if (ImGui::Button(app.Result ? "Rescan" : "Scan")) StartScan(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (app.FocusFilter) { ImGui::SetKeyboardFocusHere(); app.FocusFilter = false; }
    if (ImGui::InputTextWithHint("##filter", "filter: name  *.ext  >100mb",
                                 app.Filter, sizeof(app.Filter)))
        RecomputeFilter(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Space-separated terms (all must match):\n"
                          "  setup        name contains \"setup\"\n"
                          "  *.iso        extension filter (or ext:iso)\n"
                          "  >100mb <2gb  size filter (b/kb/mb/gb/tb)");

    if (app.Result && !app.Rescanning) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %llu files, %llu folders, %s%s  (scanned in %.2f s%s)",
                            app.Result->FileCount, app.Result->DirCount,
                            yadua::HumanSize(
                                app.Result->SizeOf(yadua::kRootRecord)).c_str(),
                            app.Result->DisplayAllocated ? " on disk" : "",
                            app.Result->Stats.TotalSeconds,
                            app.Result->Stats.UsedFallback
                                ? ", directory walk" : "");
        if (app.Result->Stats.UsedFallback &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Raw MFT access was unavailable (%s), so this "
                              "scan used the slower directory walk.",
                              yadua::Utf8(app.Result->FallbackReason).c_str());
    }
    // Live change tracking: a passive indicator here; the action lives in
    // Tools > Apply filesystem changes.
    if (app.Result && !app.Rescanning && !app.Applying && app.Monitor.Active()) {
        ImGui::SameLine();
        if (app.Monitor.Lost()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                               "| live tracking lost - rescan");
        } else {
            uint64_t pending = app.Monitor.Pending();
            ImGui::TextDisabled("| %llu change%s since scan", pending,
                                pending == 1 ? "" : "s");
            if (pending && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("Tools > Apply filesystem changes folds these "
                                  "into the tree without a full rescan.");
        }
    }
    if (!app.Status.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "| %s",
                           app.Status.c_str());
    }
    ImGui::Separator();

    // ---- Update banner ------------------------------------------------------
    if (UpdateBannerVisible(app) && !app.UpdDownloading) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.85f, 0.60f, 1.0f));
        ImGui::Text("Update available: YADUA %s",
                    yadua::Utf8(app.UpdInfo.Version).c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Update now")) StartUpdateDownload(app);
        if (!app.UpdInfo.NotesUrl.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Release notes"))
                ShellExecuteW(nullptr, L"open", app.UpdInfo.NotesUrl.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Skip this version")) {
            app.UpdSkipVersion = app.UpdInfo.Version;
            SaveSettings(app);
        }
        ImGui::Separator();
    }

    // ---- Body ---------------------------------------------------------------
    if (app.Applying) {
        // ApplyMftUpdates is mutating Result on a worker thread.
        ImGui::NewLine();
        ImGui::TextUnformatted("Applying filesystem changes...");
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(),
                           ImVec2(ImGui::GetFontSize() * 25.0f, 0.0f));
    } else if (app.Rescanning) {
        // The worker is mutating Result in place: render nothing that reads it.
        uint64_t found = app.RescanProgress.BytesRead.load(std::memory_order_relaxed);
        int stage = app.RescanProgress.Stage.load(std::memory_order_relaxed);
        ImGui::NewLine();
        ImGui::Text("Rescanning %s ...", yadua::Utf8(app.RescanPath).c_str());
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%llu entries", found);
        ImGui::ProgressBar(stage >= yadua::ScanProgress::Aggregating
                               ? 1.0f : -1.0f * (float)ImGui::GetTime(),
                           ImVec2(ImGui::GetFontSize() * 25.0f, 0.0f), overlay);
    } else if (app.Scanning) {
        int stage = app.Progress.Stage.load(std::memory_order_relaxed);
        uint64_t read  = app.Progress.BytesRead.load(std::memory_order_relaxed);
        uint64_t total = app.Progress.TotalBytes.load(std::memory_order_relaxed);
        bool walking = total == 0 && stage <= yadua::ScanProgress::Reading;
        const char* what = stage >= yadua::ScanProgress::Aggregating
                               ? "Building tree..."
                               : walking ? "Scanning directories..."
                                         : "Reading MFT...";
        ImGui::NewLine();
        ImGui::TextUnformatted(what);
        char overlay[64];
        float frac;
        if (stage >= yadua::ScanProgress::Aggregating) {
            frac = 1.0f;
            overlay[0] = '\0';
        } else if (walking) { // total unknown: indeterminate bar + entry count
            frac = -1.0f * (float)ImGui::GetTime();
            snprintf(overlay, sizeof(overlay), "%llu entries", read);
        } else {
            frac = (float)((double)read / (double)total);
            snprintf(overlay, sizeof(overlay), "%s / %s",
                     yadua::HumanSize(read).c_str(), yadua::HumanSize(total).c_str());
        }
        ImGui::ProgressBar(frac, ImVec2(ImGui::GetFontSize() * 25.0f, 0.0f),
                           overlay[0] ? overlay : nullptr);
    } else if (!app.Error.empty()) {
        ImGui::NewLine();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Scan failed: %s",
                           yadua::Utf8(app.Error).c_str());
    } else if (app.Result) {
        if (ImGui::BeginTabBar("views")) {
            // Keep requesting SetSelected until the target tab actually shows
            // (a request consumed on the tab bar's very first frame can get
            // lost before the bar settles), then clear the flag.
            if (ImGui::BeginTabItem("Tree", nullptr,
                                    app.SwitchToTree
                                        ? ImGuiTabItemFlags_SetSelected : 0)) {
                app.SwitchToTree = false;
                DrawTreeTab(app, *app.Result);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Files", nullptr,
                                    app.SwitchToFiles
                                        ? ImGuiTabItemFlags_SetSelected : 0)) {
                app.SwitchToFiles = false;
                DrawFilesTab(app, *app.Result);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("File Types", nullptr,
                                    app.SwitchToFileTypes
                                        ? ImGuiTabItemFlags_SetSelected : 0)) {
                app.SwitchToFileTypes = false;
                DrawTypesTab(app, *app.Result);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Treemap", nullptr,
                                    app.OpenTreemap || app.SwitchToTreemap
                                        ? ImGuiTabItemFlags_SetSelected : 0)) {
                app.OpenTreemap = app.SwitchToTreemap = false;
                app.Treemap.Draw(*app.Result, [&](uint32_t node) {
                    NodeMenuItems(app, *app.Result, node, true);
                });
                uint32_t clicked = app.Treemap.ConsumeClicked();
                if (clicked != UINT32_MAX) app.SelectedNode = clicked;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else {
        ImGui::NewLine();
        ImGui::TextDisabled("Pick a drive and hit Scan.");
    }

    // ---- Delete confirmation + progress modals ------------------------------
    if (app.ConfirmDelete != UINT32_MAX && !ImGui::IsPopupOpen("Delete?"))
        ImGui::OpenPopup("Delete?");
    if (ImGui::BeginPopupModal("Delete?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        uint32_t idx = app.ConfirmDelete;
        const yadua::ScanResult& r = *app.Result;
        ImGui::Text("Move to Recycle Bin?");
        ImGui::Separator();
        ImGui::TextUnformatted(yadua::Utf8(r.Path(idx)).c_str());
        ImGui::TextDisabled("%s%s", yadua::HumanSize(r.SizeOf(idx)).c_str(),
                            r.IsDir(idx) ? " (entire folder)" : "");
        ImGui::Separator();
        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            StartDelete(app, idx);
            app.ConfirmDelete = UINT32_MAX;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            app.ConfirmDelete = UINT32_MAX;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (app.Deleting && !ImGui::IsPopupOpen("Recycling"))
        ImGui::OpenPopup("Recycling");
    if (ImGui::BeginPopupModal("Recycling", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Moving to Recycle Bin...");
        ImGui::TextUnformatted(yadua::Utf8(app.DeletePath).c_str());
        if (app.Deleting && app.DeleteDone) {
            FinishDelete(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- About / updates ----------------------------------------------------
    if (app.ShowAbout && !ImGui::IsPopupOpen("About YADUA"))
        ImGui::OpenPopup("About YADUA");
    if (ImGui::BeginPopupModal("About YADUA", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("YADUA - Yet Another Disk Usage Analyzer");
        ImGui::Text("Version %s", yadua::Utf8(yadua::RunningVersion()).c_str());
        ImGui::Separator();
        if (yadua::UpdaterConfigured()) {
            ImGui::TextDisabled("Update signing key (SHA-256):");
            ImGui::TextWrapped("%s", yadua::UpdatePublicKeyFingerprint());
            ImGui::Spacing();
            if (ImGui::Checkbox("Check for updates on launch",
                                &app.UpdCheckOnLaunch))
                SaveSettings(app);
            ImGui::BeginDisabled(app.UpdChecking || app.UpdDownloading);
            if (ImGui::Button("Check now")) StartUpdateCheck(app, true);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (app.UpdChecking) {
                ImGui::TextDisabled("checking...");
            } else if (!app.UpdCheckError.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "check failed");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s",
                                      yadua::Utf8(app.UpdCheckError).c_str());
            } else if (app.UpdInfo.Available) {
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f),
                                   "v%s available",
                                   yadua::Utf8(app.UpdInfo.Version).c_str());
            } else if (app.UpdCheckedThisRun) {
                ImGui::TextDisabled("up to date");
            }
            if (UpdateBannerVisible(app)) {
                ImGui::Spacing();
                if (ImGui::Button("Download & install update"))
                    StartUpdateDownload(app);
            }
        } else {
            ImGui::TextDisabled("Auto-update is not configured in this build");
            ImGui::TextDisabled("(no signing key embedded).");
        }
        ImGui::Separator();
        if (ImGui::Button("Close")) {
            app.ShowAbout = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (app.UpdDownloading && !ImGui::IsPopupOpen("Updating"))
        ImGui::OpenPopup("Updating");
    if (ImGui::BeginPopupModal("Updating", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Downloading update %s ...",
                    yadua::Utf8(app.UpdInfo.Version).c_str());
        uint64_t rd = app.UpdProgress.BytesRead.load(std::memory_order_relaxed);
        uint64_t tt = app.UpdProgress.TotalBytes.load(std::memory_order_relaxed);
        char overlay[64];
        float frac;
        if (tt) {
            frac = (float)((double)rd / (double)tt);
            snprintf(overlay, sizeof(overlay), "%s / %s",
                     yadua::HumanSize(rd).c_str(), yadua::HumanSize(tt).c_str());
        } else {
            frac = -1.0f * (float)ImGui::GetTime();
            snprintf(overlay, sizeof(overlay), "%s", yadua::HumanSize(rd).c_str());
        }
        ImGui::ProgressBar(frac, ImVec2(ImGui::GetFontSize() * 25.0f, 0.0f),
                           overlay);
        ImGui::TextDisabled("The verified installer will launch and YADUA will "
                            "close to finish updating.");
        if (!app.UpdDownloading) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}

// ============================================================================
// Win32 + DirectX 11 plumbing (standard Dear ImGui example skeleton)
// ============================================================================

static ID3D11Device*           g_d3dDevice    = nullptr;
static ID3D11DeviceContext*    g_d3dContext   = nullptr;
static IDXGISwapChain*         g_swapChain    = nullptr;
static ID3D11RenderTargetView* g_renderTarget = nullptr;
static UINT                    g_resizeWidth  = 0;
static UINT                    g_resizeHeight = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
    backBuffer->Release();
}

static void CleanupRenderTarget() {
    if (g_renderTarget) { g_renderTarget->Release(); g_renderTarget = nullptr; }
}

static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount      = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow     = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed         = TRUE;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapChain, &g_d3dDevice, &level, &g_d3dContext);
    if (hr == DXGI_ERROR_UNSUPPORTED) // e.g. RDP / VMs without GPU
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapChain, &g_d3dDevice, &level,
            &g_d3dContext);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapChain)  { g_swapChain->Release();  g_swapChain = nullptr; }
    if (g_d3dContext) { g_d3dContext->Release(); g_d3dContext = nullptr; }
    if (g_d3dDevice)  { g_d3dDevice->Release();  g_d3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM,
                                                             LPARAM);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                g_resizeWidth  = LOWORD(lp);
                g_resizeHeight = HIWORD(lp);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0; // disable ALT menu
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // shell verbs (Properties)

    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(1)); // assets/yadua.rc
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance,
                      icon, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr,
                      L"YADUA", icon};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName,
                              L"YADUA - Yet Another Disk Usage Analyzer",
                              WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
                              nullptr, nullptr, instance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11.", L"YADUA",
                    MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini clutter next to the exe

    float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf",
                                                17.0f * scale);
    if (!font) io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext);

    App app;
    ListFixedDrives(app);
    LoadSettings(app);
    app.AutoScan    = cmdLine && wcsstr(cmdLine, L"--autoscan") != nullptr;
    app.OpenTreemap = cmdLine && wcsstr(cmdLine, L"--view treemap") != nullptr;

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resizeWidth) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight,
                                       DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateRenderTarget();
        }

        if (app.AutoScan) { app.AutoScan = false; StartScan(app); }

        // One-shot update check on launch (opt-out, and never for unstamped
        // local builds whose version reads 0.0.0).
        if (!app.UpdCheckStarted) {
            app.UpdCheckStarted = true;
            if (app.UpdCheckOnLaunch && yadua::UpdaterConfigured() &&
                yadua::RunningVersion() != L"0.0.0")
                StartUpdateCheck(app, false);
        }
        if (app.UpdChecking && app.UpdCheckDone) {
            app.UpdCheckThread.join();
            app.UpdChecking = false; // UI reads UpdInfo / UpdCheckError
        }
        if (app.UpdDownloading && app.UpdDownloadDone) {
            app.UpdDownloadThread.join();
            app.UpdDownloading = false;
            if (app.UpdDownloadError.empty() && !app.UpdInstallerPath.empty()) {
                std::wstring err;
                if (yadua::LaunchInstaller(app.UpdInstallerPath, err))
                    done = true; // quit so the installer can replace files
                else
                    app.Status = "Update launch failed: " + yadua::Utf8(err);
            } else {
                app.Status = "Update failed: " + yadua::Utf8(app.UpdDownloadError);
            }
        }

        if (app.Applying && app.ApplyDone) {
            app.ApplyThread.join();
            app.Applying = false;
            if (app.ApplyError.empty()) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Applied %zu changes%s",
                         app.ApplyCount,
                         app.ApplyUnresolved ? " (some unresolved - rescan "
                                               "for full accuracy)" : "");
                app.Status = buf;
                if (!app.Result->Exists(app.Treemap.Root())) app.Treemap.Reset();
                app.Treemap.Invalidate();
                app.UseSorted = false;
                app.SortedChildren.clear();
                RecomputeFilter(app);
            } else {
                app.Status = "Apply failed: " + yadua::Utf8(app.ApplyError);
            }
        }
        if (app.Rescanning && app.RescanDone) {
            app.RescanThread.join();
            app.Rescanning = false;
            if (app.RescanError.empty()) {
                app.Status = "Rescanned " + yadua::Utf8(app.RescanPath);
                if (!app.Result->Exists(app.Treemap.Root())) app.Treemap.Reset();
                app.Treemap.Invalidate();
                app.UseSorted = false; // stale: node set changed
                app.SortedChildren.clear();
                RecomputeFilter(app);
            } else {
                app.Status = "Rescan failed: " + yadua::Utf8(app.RescanError);
            }
        }
        if (app.Scanning && app.ScanDone) {
            app.ScanThread.join();
            app.Scanning = false;
            if (app.PendingError.empty()) {
                app.Result = std::move(app.Pending);
                app.Result->DisplayAllocated = app.ShowAllocated; // keep the toggle
                app.UseSorted = false;
                app.Treemap.Reset();
                app.Status.clear();
                RecomputeFilter(app);
                // Raw-MFT scans can track live changes via the USN journal.
                if (!app.Result->MftMap.empty()) {
                    std::wstring monErr;
                    app.Monitor.Start(app.Result->Drive, monErr); // best effort
                }
            } else {
                app.Error = std::move(app.PendingError);
                app.PendingError.clear();
            }
        }

        if (done) break; // e.g. update installer just launched

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi(app);
        ImGui::Render();

        const float clear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
        g_d3dContext->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_d3dContext->ClearRenderTargetView(g_renderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0); // vsync
    }

    app.Monitor.Stop();
    if (app.ScanThread.joinable()) app.ScanThread.join();
    if (app.DeleteThread.joinable()) app.DeleteThread.join();
    if (app.RescanThread.joinable()) app.RescanThread.join();
    if (app.ApplyThread.joinable()) app.ApplyThread.join();
    if (app.UpdCheckThread.joinable()) app.UpdCheckThread.join();
    if (app.UpdDownloadThread.joinable()) app.UpdDownloadThread.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
