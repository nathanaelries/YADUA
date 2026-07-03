// ============================================================================
// Squarified treemap view (WinDirStat-style) rendered with Dear ImGui.
//
// The layout (Bruls et al. "Squarified Treemaps") is rebuilt only when the
// canvas size, the zoom root, or the scan result changes; rendering the cached
// rects is cheap. Rect count is bounded by the pixel area (items smaller than
// a few pixels are lumped into a gray "smaller items" rect), so even a volume
// with millions of files stays at a few tens of thousands of rects.
// ============================================================================
#pragma once

#include <functional>
#include <vector>

#include "imgui.h"
#include "scanner.h"

// Color derived from a file's extension (FNV hash -> hue), so same-type
// files match across the treemap and the tree view's name tinting.
ImU32 ExtensionColor(const yadua::ScanResult& r, uint32_t node,
                     float sat, float val);

class TreemapView {
public:
    // WinDirStat-style cushion look: per-rect gradient from a lit top-left
    // corner to a shaded bottom-right. Toggled from the treemap toolbar.
    bool Cushion = true;

    // Marks the cached layout stale (new scan, deletion, ...).
    void Invalidate() { dirty_ = true; }

    void Reset() {
        root_ = (uint32_t)yadua::kRootRecord;
        selected_ = UINT32_MAX;
        dirty_ = true;
    }

    void SetRoot(uint32_t node) { root_ = node; dirty_ = true; }
    uint32_t Root() const { return root_; }
    uint32_t Selected() const { return selected_; }

    // Volume capacity, so the whole-disk view can reserve free-space and
    // "unaccounted" blocks alongside the scanned tree (0 = don't show them,
    // e.g. a folder scan). Only used at the volume root (not when zoomed in).
    void SetVolumeInfo(uint64_t total, uint64_t free) {
        if (total != total_ || free != free_) dirty_ = true;
        total_ = total;
        free_  = free;
    }

    // Node clicked this frame (UINT32_MAX if none); clears on read so the
    // host can sync its own selection without fighting the tree view's.
    uint32_t ConsumeClicked() {
        uint32_t c = clicked_;
        clicked_ = UINT32_MAX;
        return c;
    }

    // Draws toolbar + canvas into the current window's remaining space.
    // `contextMenu` is invoked inside a right-click popup with the hit node.
    void Draw(const yadua::ScanResult& r,
              const std::function<void(uint32_t)>& contextMenu);

private:
    enum Special : uint8_t { NotSpecial = 0, FreeSpace = 1, Unaccounted = 2 };
    struct Item {
        ImVec2   Min, Max;
        uint32_t Node;
        uint32_t RestCount = 0; // >0: "N smaller items" lump inside dir `Node`
        uint64_t Bytes     = 0; // for Special blocks (free / unaccounted)
        uint8_t  Depth     = 0;
        uint8_t  Kind      = NotSpecial;
        bool     IsDir     = false;
    };

    void Build(const yadua::ScanResult& r, ImVec2 origin, ImVec2 size);
    void LayoutChildren(const yadua::ScanResult& r, uint32_t dir,
                        float x0, float y0, float x1, float y1, int depth);

    std::vector<Item> items_;
    uint32_t root_      = (uint32_t)yadua::kRootRecord;
    uint32_t selected_  = UINT32_MAX;
    uint32_t clicked_   = UINT32_MAX;
    uint32_t popupNode_ = UINT32_MAX;
    uint64_t total_     = 0; // volume capacity (bytes), 0 = whole-disk view off
    uint64_t free_      = 0;
    ImVec2   builtPos_{0, 0};
    ImVec2   builtSize_{0, 0};
    bool     dirty_ = true;
};
