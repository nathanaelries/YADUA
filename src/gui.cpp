// ============================================================================
// YADUA GUI frontend — Dear ImGui (Win32 + DirectX 11).
//
// Views: a size-sorted tree table (sortable columns, name filter) and a
// WinDirStat-style squarified treemap (src/treemap.*), in tabs.
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
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "scanner.h"
#include "treemap.h"

// ============================================================================
// Application state
// ============================================================================

enum TreeColumn : ImGuiID {
    ColName = 0, ColSize, ColPercent, ColFiles, ColFolders
};

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
    bool                 SwitchToTree    = false;
    bool                 SwitchToTreemap = false;

    // Tree sorting. The scanner's child index is size-descending (canonical);
    // any other order lives in SortedChildren (parallel to Children.List).
    std::vector<uint32_t> SortedChildren;
    bool                  UseSorted = false;

    TreemapView Treemap;

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
    if (app.Scanning || app.Deleting || app.Drives.empty()) return;
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
            case ColFiles:   return r.IsDir(n) ? r.Totals[n].FileCount : 0;
            case ColFolders: return r.IsDir(n) ? r.Totals[n].DirCount : 0;
            default:         return r.SizeOf(n); // ColSize / ColPercent
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

static void StartDelete(App& app, uint32_t node) {
    if (app.Deleting || app.Scanning || !app.Result) return;
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
            app.SwitchToTreemap = true;
        }
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
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)idx, flags, "%s", label.c_str());
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

static void DrawTreeTab(App& app, const yadua::ScanResult& r) {
    if (!ImGui::BeginTable("tree", 5,
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_Sortable))
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

// ============================================================================
// Top-level UI
// ============================================================================

static void DrawUi(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

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
    if (ImGui::InputTextWithHint("##filter", "filter: name  *.ext  >100mb",
                                 app.Filter, sizeof(app.Filter)))
        RecomputeFilter(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Space-separated terms (all must match):\n"
                          "  setup        name contains \"setup\"\n"
                          "  *.iso        extension filter (or ext:iso)\n"
                          "  >100mb <2gb  size filter (b/kb/mb/gb/tb)");

    if (app.Result) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %llu files, %llu folders, %s  (scanned in %.2f s%s)",
                            app.Result->FileCount, app.Result->DirCount,
                            yadua::HumanSize(
                                app.Result->Totals[yadua::kRootRecord].LogicalSize)
                                .c_str(),
                            app.Result->Stats.TotalSeconds,
                            app.Result->Stats.UsedFallback
                                ? ", directory walk" : "");
        if (app.Result->Stats.UsedFallback &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Raw MFT access was unavailable (%s), so this "
                              "scan used the slower directory walk.",
                              yadua::Utf8(app.Result->FallbackReason).c_str());
    }
    if (!app.Status.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "| %s",
                           app.Status.c_str());
    }
    ImGui::Separator();

    // ---- Body ---------------------------------------------------------------
    if (app.Scanning) {
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
            ImGuiTabItemFlags treeFlags = 0;
            if (app.SwitchToTree) { treeFlags = ImGuiTabItemFlags_SetSelected;
                                    app.SwitchToTree = false; }
            if (ImGui::BeginTabItem("Tree", nullptr, treeFlags)) {
                DrawTreeTab(app, *app.Result);
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags tmFlags = 0;
            if (app.OpenTreemap || app.SwitchToTreemap) {
                tmFlags = ImGuiTabItemFlags_SetSelected;
                app.OpenTreemap = app.SwitchToTreemap = false;
            }
            if (ImGui::BeginTabItem("Treemap", nullptr, tmFlags)) {
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
                              L"YADUA — Yet Another Disk Usage Analyzer",
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
        if (app.Scanning && app.ScanDone) {
            app.ScanThread.join();
            app.Scanning = false;
            if (app.PendingError.empty()) {
                app.Result = std::move(app.Pending);
                app.UseSorted = false;
                app.Treemap.Reset();
                app.Status.clear();
                RecomputeFilter(app);
            } else {
                app.Error = std::move(app.PendingError);
                app.PendingError.clear();
            }
        }

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

    if (app.ScanThread.joinable()) app.ScanThread.join();
    if (app.DeleteThread.joinable()) app.DeleteThread.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
