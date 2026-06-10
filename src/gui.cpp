// ============================================================================
// YADUA GUI frontend — Dear ImGui (Win32 + DirectX 11) tree view.
//
// The scan runs on a background thread (see scanner.h); the UI polls a
// ScanProgress while it runs and takes ownership of the ScanResult when done.
// The tree view renders straight out of the scanner's CSR child index, which
// is already sorted by size.
//
// The linker manifest requests Administrator elevation (raw volume access),
// so double-clicking the exe shows a UAC prompt.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>

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

// ============================================================================
// Application state
// ============================================================================

struct App {
    std::vector<std::wstring> Drives;
    std::vector<std::string>  DriveLabels; // UTF-8 copies for ImGui
    int  DriveIndex = 0;
    bool AutoScan   = false;               // --autoscan: start scanning at launch

    std::unique_ptr<yadua::ScanResult> Result;   // owned by the UI thread
    std::unique_ptr<yadua::ScanResult> Pending;  // being filled by the scan thread
    yadua::ScanProgress Progress;
    std::thread         ScanThread;
    std::atomic<bool>   ScanDone{false};
    bool                Scanning = false;
    std::wstring        PendingError; // written by scan thread before ScanDone
    std::wstring        Error;

    // Name filter: Visible[i] != 0 <=> node i matches or has a matching
    // descendant. Empty vector means "no filter, everything visible".
    char                 Filter[256] = {};
    std::vector<uint8_t> Visible;
};

static void ListNtfsDrives(App& app) {
    DWORD mask = GetLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        if (!(mask & (1u << (letter - L'A')))) continue;
        std::wstring root{letter, L':', L'\\'};
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        wchar_t fs[MAX_PATH] = {};
        if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr,
                                   nullptr, fs, MAX_PATH)) continue;
        if (wcscmp(fs, L"NTFS") != 0) continue;
        app.Drives.push_back(std::wstring{letter, L':'});
        app.DriveLabels.push_back(yadua::Utf8(app.Drives.back()));
    }
}

static void StartScan(App& app) {
    if (app.Scanning || app.Drives.empty()) return;
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
        if (!yadua::ScanVolume(drive, 0, *app.Pending, err, &app.Progress))
            app.PendingError = err;
        app.ScanDone = true; // release: PendingError/Pending written before this
    });
}

// Recompute the visibility map for the current filter: mark every node whose
// name contains the (case-insensitive) needle, then mark all its ancestors so
// the tree path down to each match stays visible.
static void RecomputeFilter(App& app) {
    const yadua::ScanResult* r = app.Result.get();
    if (!r || app.Filter[0] == '\0') { app.Visible.clear(); return; }

    std::wstring needle = yadua::Wide(app.Filter);
    for (wchar_t& c : needle) c = towlower(c);

    app.Visible.assign(r->Nodes.size(), 0);
    std::wstring name;
    for (uint32_t i = 0; i < r->Nodes.size(); ++i) {
        if (!r->Exists(i) || app.Visible[i]) continue;
        name = r->Name(i);
        for (wchar_t& c : name) c = towlower(c);
        if (name.find(needle) == std::wstring::npos) continue;

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

// ============================================================================
// Tree view
// ============================================================================

static void NodeContextMenu(const yadua::ScanResult& r, uint32_t idx) {
    if (!ImGui::BeginPopupContextItem()) return;
    if (ImGui::MenuItem("Open in Explorer")) {
        std::wstring args = L"/select,\"" + r.Path(idx) + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
                      nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::MenuItem("Copy path"))
        ImGui::SetClipboardText(yadua::Utf8(r.Path(idx)).c_str());
    ImGui::EndPopup();
}

static void DrawTree(App& app, const yadua::ScanResult& r, uint32_t idx,
                     uint64_t parentSize, bool isRoot) {
    // Very large directories: render the biggest entries (children are sorted
    // by size already) and summarize the rest instead of emitting 100k rows.
    constexpr uint32_t kMaxChildrenShown = 2000;

    const bool     filtered  = !app.Visible.empty();
    const bool     isDir     = r.IsDir(idx);
    const uint64_t size      = r.SizeOf(idx);
    const uint32_t childBegin = r.Children.Offset[idx];
    const uint32_t childEnd   = r.Children.Offset[idx + 1];
    const bool     leaf       = !isDir || childBegin == childEnd;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (isRoot) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    else if (filtered && isDir) ImGui::SetNextItemOpen(true); // expand to matches

    std::string label = isRoot ? yadua::Utf8(r.Drive) : yadua::Utf8(r.Name(idx));
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)idx, flags, "%s", label.c_str());
    NodeContextMenu(r, idx);

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
        uint32_t shown = 0, hidden = 0;
        uint64_t hiddenBytes = 0;
        for (uint32_t c = childBegin; c < childEnd; ++c) {
            uint32_t child = r.Children.List[c];
            if (filtered && !app.Visible[child]) continue;
            if (shown >= kMaxChildrenShown) {
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
            ImGui::TextDisabled("(... %u smaller items, %s)", hidden,
                                yadua::HumanSize(hiddenBytes).c_str());
        }
        ImGui::TreePop();
    }
}

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
    ImGui::BeginDisabled(app.Scanning);
    if (ImGui::Button(app.Result ? "Rescan" : "Scan")) StartScan(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (ImGui::InputTextWithHint("##filter", "filter by name...",
                                 app.Filter, sizeof(app.Filter)))
        RecomputeFilter(app);

    if (app.Result) {
        const yadua::ScanStats& s = app.Result->Stats;
        ImGui::SameLine();
        ImGui::TextDisabled("| %llu files, %llu folders, %s  (scanned in %.2f s)",
                            app.Result->FileCount, app.Result->DirCount,
                            yadua::HumanSize(
                                app.Result->Totals[yadua::kRootRecord].LogicalSize)
                                .c_str(),
                            s.TotalSeconds);
    }
    ImGui::Separator();

    // ---- Body ---------------------------------------------------------------
    if (app.Scanning) {
        int stage = app.Progress.Stage.load(std::memory_order_relaxed);
        uint64_t read  = app.Progress.BytesRead.load(std::memory_order_relaxed);
        uint64_t total = app.Progress.TotalBytes.load(std::memory_order_relaxed);
        const char* what = stage <= yadua::ScanProgress::Reading
                               ? "Reading MFT..." : "Building tree...";
        ImGui::NewLine();
        ImGui::TextUnformatted(what);
        float frac = total ? (float)((double)read / (double)total) : 0.0f;
        if (stage >= yadua::ScanProgress::Aggregating) frac = 1.0f;
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%s / %s",
                 yadua::HumanSize(read).c_str(), yadua::HumanSize(total).c_str());
        ImGui::ProgressBar(frac, ImVec2(ImGui::GetFontSize() * 25.0f, 0.0f), overlay);
    } else if (!app.Error.empty()) {
        ImGui::NewLine();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Scan failed: %s",
                           yadua::Utf8(app.Error).c_str());
    } else if (app.Result) {
        if (ImGui::BeginTable("tree", 5,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            float ch = ImGui::GetFontSize();
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ch * 6);
            ImGui::TableSetupColumn("% of parent",
                                    ImGuiTableColumnFlags_WidthFixed, ch * 8);
            ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthFixed, ch * 5);
            ImGui::TableSetupColumn("Folders",
                                    ImGuiTableColumnFlags_WidthFixed, ch * 5);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            DrawTree(app, *app.Result, (uint32_t)yadua::kRootRecord, 0, true);
            ImGui::EndTable();
        }
    } else {
        ImGui::NewLine();
        ImGui::TextDisabled("Pick a drive and hit Scan.");
    }

    ImGui::End();
}

// ============================================================================
// Win32 + DirectX 11 plumbing (standard Dear ImGui example skeleton)
// ============================================================================

static ID3D11Device*           g_d3dDevice        = nullptr;
static ID3D11DeviceContext*    g_d3dContext       = nullptr;
static IDXGISwapChain*         g_swapChain        = nullptr;
static ID3D11RenderTargetView* g_renderTarget     = nullptr;
static UINT                    g_resizeWidth      = 0;
static UINT                    g_resizeHeight     = 0;

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
    sd.BufferCount        = 2;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hwnd;
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

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

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance,
                      nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr,
                      L"YADUA", nullptr};
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
    ListNtfsDrives(app);
    app.AutoScan = cmdLine && wcsstr(cmdLine, L"--autoscan") != nullptr;

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
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
