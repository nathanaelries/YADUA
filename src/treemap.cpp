// ============================================================================
// Squarified treemap implementation. See treemap.h for the overview.
// ============================================================================

#include "treemap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <string>

using yadua::ScanResult;

namespace {

constexpr float    kMinSide   = 3.0f;   // lump items smaller than this
constexpr int      kMaxDepth  = 24;
constexpr size_t   kMaxItems  = 200000; // hard cap, keeps frame cost bounded

// FNV-1a over the lowercased extension.
uint32_t ExtensionHash(const ScanResult& r, uint32_t node) {
    std::wstring name = r.Name(node);
    size_t dot = name.find_last_of(L'.');
    uint32_t h = 2166136261u;
    if (dot != std::wstring::npos && name.size() - dot <= 9)
        for (size_t i = dot + 1; i < name.size(); ++i)
            h = (h ^ (uint32_t)towlower(name[i])) * 16777619u;
    return h;
}

// Treemap fill: extension hue, vivid enough that neighbouring types read as
// distinct colors (not washed-out grey), with per-hash saturation + brightness
// jitter so two files that happen to share a hue still differ in shade.
ImU32 FileColor(const ScanResult& r, uint32_t node) {
    uint32_t h = ExtensionHash(r, node);
    float hue = (float)(h % 360u) / 360.0f;
    float sat = 0.70f + (float)((h >> 17) % 20u) / 100.0f; // 0.70 - 0.89
    float val = 0.75f + (float)((h >> 9) % 22u) / 100.0f;  // 0.75 - 0.96
    return ImColor(ImColor::HSV(hue, sat, val));
}

// A muted, per-directory tint for a "N smaller items" lump, so the treemap's
// unresolvable-detail areas read as subdued color instead of one flat grey.
ImU32 RestColor(uint32_t dirNode) {
    uint32_t h = dirNode * 2654435761u; // Knuth multiplicative hash
    float hue = (float)(h % 360u) / 360.0f;
    return ImColor(ImColor::HSV(hue, 0.38f, 0.45f));
}

// Directory fill kept close to the app background so the gaps between colored
// children read as clean separators, not a grey grid.
constexpr ImU32 kDirFill    = IM_COL32(22, 23, 28, 255);
constexpr ImU32 kDirBorder  = IM_COL32(64, 66, 78, 255);
// A faint pure-white hairline between rects so adjacent similar hues stay
// visually separable (the "which is which" problem).
constexpr ImU32 kFileBorder = IM_COL32(255, 255, 255, 55);

// Worst aspect ratio of a row of areas laid along a side of length `side`.
// Lower is better (1.0 = all squares). From the squarified-treemap paper.
double WorstRatio(double rowSum, double rowMin, double rowMax, double side) {
    double s2 = rowSum * rowSum, w2 = side * side;
    return std::max((w2 * rowMax) / s2, s2 / (w2 * rowMin));
}

} // namespace

ImU32 ExtensionColor(const ScanResult& r, uint32_t node, float sat, float val) {
    float hue = (float)(ExtensionHash(r, node) % 360u) / 360.0f;
    return ImColor(ImColor::HSV(hue, sat, val));
}

// Lay out the children of `dir` (canonical child order is size-descending,
// exactly what squarify wants) into the rectangle (x0,y0)-(x1,y1).
void TreemapView::LayoutChildren(const ScanResult& r, uint32_t dir,
                                 float x0, float y0, float x1, float y1,
                                 int depth) {
    if (depth > kMaxDepth || items_.size() > kMaxItems) return;
    if (x1 - x0 < kMinSide || y1 - y0 < kMinSide) return;

    uint32_t begin = r.Children.Offset[dir], end = r.Children.Offset[dir + 1];
    double total = 0;
    for (uint32_t c = begin; c < end; ++c) {
        uint32_t child = r.Children.List[c];
        if (r.Exists(child)) total += (double)r.SizeOf(child);
    }
    if (total <= 0) return;

    double scale = ((double)(x1 - x0) * (double)(y1 - y0)) / total; // px² per byte

    auto lumpRest = [&](uint32_t from, float rx0, float ry0, float rx1, float ry1) {
        uint32_t count = 0;
        for (uint32_t c = from; c < end; ++c)
            if (r.Exists(r.Children.List[c])) ++count;
        if (count && rx1 - rx0 >= 1.0f && ry1 - ry0 >= 1.0f)
            items_.push_back({{rx0, ry0}, {rx1, ry1}, dir, count, 0,
                              (uint8_t)depth, NotSpecial, false});
    };

    uint32_t c = begin;
    while (c < end) {
        // Skip deleted entries and the zero-size tail (children are sorted
        // descending, so the first zero ends the layout).
        while (c < end && !r.Exists(r.Children.List[c])) ++c;
        if (c == end) break;
        if (r.SizeOf(r.Children.List[c]) == 0) break;

        float w = x1 - x0, hgt = y1 - y0;
        if (w < 1.0f || hgt < 1.0f) { lumpRest(c, x0, y0, x1, y1); break; }
        bool   horizontalRow = w < hgt;     // rows go along the shorter side
        double side = horizontalRow ? w : hgt;

        // Greedily grow the row while the worst aspect ratio keeps improving.
        double rowSum = 0, rowMin = 0, rowMax = 0, best = 1e300;
        uint32_t rowEnd = c;
        for (uint32_t j = c; j < end; ++j) {
            uint32_t child = r.Children.List[j];
            if (!r.Exists(child)) { if (rowEnd == j) ++rowEnd; continue; }
            double a = (double)r.SizeOf(child) * scale;
            if (a <= 0) break;
            double nSum = rowSum + a;
            double nMin = rowSum == 0 ? a : std::min(rowMin, a);
            double nMax = rowSum == 0 ? a : std::max(rowMax, a);
            double worst = WorstRatio(nSum, nMin, nMax, side);
            if (rowSum != 0 && worst > best) break;
            rowSum = nSum; rowMin = nMin; rowMax = nMax; best = worst;
            rowEnd = j + 1;
        }
        if (rowSum <= 0) break;

        double thickness = rowSum / side; // row extent along the longer axis
        if (thickness < kMinSide) {       // remaining items are all tiny
            lumpRest(c, x0, y0, x1, y1);
            break;
        }

        // Emit the row's rects, splitting `side` proportionally to size.
        double along = 0;
        for (uint32_t j = c; j < rowEnd; ++j) {
            uint32_t child = r.Children.List[j];
            if (!r.Exists(child)) continue;
            double a = (double)r.SizeOf(child) * scale;
            double extent = a / thickness;
            float cx0, cy0, cx1, cy1;
            if (horizontalRow) {
                cx0 = x0 + (float)along;        cx1 = x0 + (float)(along + extent);
                cy0 = y0;                       cy1 = y0 + (float)thickness;
            } else {
                cx0 = x0;                       cx1 = x0 + (float)thickness;
                cy0 = y0 + (float)along;        cy1 = y0 + (float)(along + extent);
            }
            along += extent;
            if (cx1 - cx0 < 0.5f || cy1 - cy0 < 0.5f) continue; // sub-pixel
            bool isDir = r.IsDir(child);
            items_.push_back({{cx0, cy0}, {cx1, cy1}, child, 0, 0,
                              (uint8_t)depth, NotSpecial, isDir});
            if (isDir && cx1 - cx0 >= 2 * kMinSide && cy1 - cy0 >= 2 * kMinSide)
                LayoutChildren(r, child, cx0 + 1, cy0 + 1, cx1 - 1, cy1 - 1,
                               depth + 1);
        }

        // Shrink the remaining free rectangle and continue with the rest.
        if (horizontalRow) y0 += (float)thickness;
        else               x0 += (float)thickness;
        c = rowEnd;
    }
}

void TreemapView::Build(const ScanResult& r, ImVec2 origin, ImVec2 size) {
    items_.clear();
    items_.reserve(65536);
    if (root_ >= r.Nodes.size() || !r.Exists(root_) || !r.IsDir(root_))
        root_ = (uint32_t)yadua::kRootRecord;

    // The tree fills the whole canvas, unless we're showing the whole disk (at
    // the volume root, with capacity known): then split the canvas into
    // [ scanned tree | unaccounted | free ] proportional to bytes, and lay the
    // tree into just the "scanned" band.
    ImVec2 treeOrigin = origin, treeSize = size;
    bool wholeDisk = root_ == (uint32_t)yadua::kRootRecord && total_ > 0;
    if (wholeDisk) {
        double used  = (double)r.SizeOf(root_);
        double freeB = (double)free_;
        double other = (double)total_ - freeB - used;
        if (other < 0) other = 0;
        double denom = used + freeB + other;
        if (denom <= 0) denom = 1;

        bool  horiz  = size.x >= size.y;
        float extent = horiz ? size.x : size.y;
        float usedExt = (float)(extent * used / denom);
        treeSize = horiz ? ImVec2(usedExt, size.y) : ImVec2(size.x, usedExt);

        float pos = horiz ? origin.x + usedExt : origin.y + usedExt;
        auto addBlock = [&](double bytes, uint8_t kind) {
            float ext = (float)(extent * bytes / denom);
            if (ext < 1.0f) return;
            ImVec2 mn = horiz ? ImVec2(pos, origin.y) : ImVec2(origin.x, pos);
            ImVec2 mx = horiz ? ImVec2(pos + ext, origin.y + size.y)
                              : ImVec2(origin.x + size.x, pos + ext);
            items_.push_back({mn, mx, 0, 0, (uint64_t)bytes, 0, kind, false});
            pos += ext;
        };
        addBlock(other, Unaccounted);
        addBlock(freeB, FreeSpace);
    }

    items_.push_back({treeOrigin,
                      {treeOrigin.x + treeSize.x, treeOrigin.y + treeSize.y},
                      root_, 0, 0, 0, NotSpecial, true});
    LayoutChildren(r, root_, treeOrigin.x + 1, treeOrigin.y + 1,
                   treeOrigin.x + treeSize.x - 1, treeOrigin.y + treeSize.y - 1,
                   1);
    builtPos_  = origin;
    builtSize_ = size;
    dirty_     = false;
}

void TreemapView::Draw(const ScanResult& r,
                       const std::function<void(uint32_t)>& contextMenu) {
    // ---- Toolbar: breadcrumb + zoom controls --------------------------------
    ImGui::BeginDisabled(root_ == (uint32_t)yadua::kRootRecord);
    if (ImGui::Button("Up")) {
        uint32_t parent = r.Nodes[root_].Parent;
        SetRoot(parent < r.Nodes.size() && r.Exists(parent) && r.IsDir(parent)
                    ? parent : (uint32_t)yadua::kRootRecord);
    }
    ImGui::SameLine();
    if (ImGui::Button("Top")) Reset();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Cushion", &Cushion);
    ImGui::SameLine();
    ImGui::TextUnformatted(yadua::Utf8(r.Path(root_)).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(hover: details, double-click: zoom, right-click: actions)");

    // ---- Canvas -------------------------------------------------------------
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size   = ImGui::GetContentRegionAvail();
    if (size.x < 50 || size.y < 50) return;
    if (dirty_ || size.x != builtSize_.x || size.y != builtSize_.y ||
        origin.x != builtPos_.x || origin.y != builtPos_.y)
        Build(r, origin, size);

    ImGui::InvisibleButton("##treemap", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight);
    bool hoveredCanvas = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    // Topmost item under the mouse = last emitted one containing it (children
    // are emitted after their parents).
    int hover = -1;
    if (hoveredCanvas)
        for (int i = (int)items_.size() - 1; i >= 0; --i) {
            const Item& it = items_[i];
            if (mouse.x >= it.Min.x && mouse.x < it.Max.x &&
                mouse.y >= it.Min.y && mouse.y < it.Max.y) { hover = i; break; }
        }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    for (const Item& it : items_) {
        if (it.Kind != NotSpecial) {
            // Whole-disk blocks: free space (green) and unaccounted (neutral).
            ImU32 fill = it.Kind == FreeSpace ? IM_COL32(46, 78, 58, 255)
                                              : IM_COL32(56, 52, 46, 255);
            dl->AddRectFilled(it.Min, it.Max, fill);
            dl->AddRect(it.Min, it.Max, kFileBorder);
            const char* lbl = it.Kind == FreeSpace ? "free" : "other";
            ImVec2 ts = ImGui::CalcTextSize(lbl);
            if (it.Max.x - it.Min.x > ts.x + 6 && it.Max.y - it.Min.y > ts.y + 6)
                dl->AddText({(it.Min.x + it.Max.x) * 0.5f - ts.x * 0.5f,
                             (it.Min.y + it.Max.y) * 0.5f - ts.y * 0.5f},
                            IM_COL32(210, 220, 214, 220), lbl);
        } else if (it.RestCount) {
            dl->AddRectFilled(it.Min, it.Max, RestColor(it.Node));
            if (it.Max.x - it.Min.x > 4 && it.Max.y - it.Min.y > 4)
                dl->AddRect(it.Min, it.Max, kFileBorder);
        } else if (it.IsDir) {
            dl->AddRectFilled(it.Min, it.Max, kDirFill);
            dl->AddRect(it.Min, it.Max, kDirBorder);
        } else {
            ImU32 fill = FileColor(r, it.Node);
            if (Cushion && it.Max.x - it.Min.x > 3 && it.Max.y - it.Min.y > 3) {
                // Pillow-style gradient: lit top-left, shaded bottom-right.
                ImVec4 f = ImGui::ColorConvertU32ToFloat4(fill);
                auto shade = [&](float m) {
                    return ImGui::ColorConvertFloat4ToU32(
                        ImVec4(std::min(f.x * m, 1.0f), std::min(f.y * m, 1.0f),
                               std::min(f.z * m, 1.0f), 1.0f));
                };
                dl->AddRectFilledMultiColor(it.Min, it.Max, shade(1.35f), fill,
                                            shade(0.55f), fill);
            } else {
                dl->AddRectFilled(it.Min, it.Max, fill);
            }
            if (it.Max.x - it.Min.x > 4 && it.Max.y - it.Min.y > 4)
                dl->AddRect(it.Min, it.Max, kFileBorder);
        }
    }
    if (hover >= 0) {
        const Item& it = items_[hover];
        dl->AddRect(it.Min, it.Max, IM_COL32(255, 255, 255, 230), 0, 0, 2.0f);
    }
    dl->PopClipRect();

    // ---- Interaction --------------------------------------------------------
    if (hover >= 0 && items_[hover].Kind != NotSpecial) {
        // Free / unaccounted block: informational tooltip only (no node).
        const Item& it = items_[hover];
        if (ImGui::BeginTooltip()) {
            ImGui::TextUnformatted(it.Kind == FreeSpace
                ? "Free space"
                : "Unaccounted (system/overhead not in the scan)");
            ImGui::TextDisabled("%s", yadua::HumanSize(it.Bytes).c_str());
            ImGui::EndTooltip();
        }
    } else if (hover >= 0) {
        const Item& it = items_[hover];
        if (ImGui::BeginTooltip()) {
            if (it.RestCount) {
                ImGui::Text("%u smaller items in %s", it.RestCount,
                            yadua::Utf8(r.Path(it.Node)).c_str());
            } else {
                ImGui::TextUnformatted(yadua::Utf8(r.Path(it.Node)).c_str());
                ImGui::TextDisabled("%s", yadua::HumanSize(r.SizeOf(it.Node)).c_str());
                if (it.IsDir)
                    ImGui::TextDisabled("%llu files, %llu folders",
                                        r.Totals[it.Node].FileCount,
                                        r.Totals[it.Node].DirCount);
                if (r.Nodes[it.Node].Flags & yadua::kNodeReparse)
                    ImGui::TextDisabled("reparse point (junction/symlink/placeholder)");
            }
            ImGui::EndTooltip();
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selected_ = it.Node;
            clicked_  = it.Node;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // Zoom: into the dir itself, or into a file's parent directory.
            uint32_t target = (it.IsDir || it.RestCount)
                                  ? it.Node : r.Nodes[it.Node].Parent;
            if (target < r.Nodes.size() && r.Exists(target) &&
                r.IsDir(target) && target != root_)
                SetRoot(target);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            popupNode_ = it.Node;
            ImGui::OpenPopup("##treemap_ctx");
        }
    }
    if (ImGui::BeginPopup("##treemap_ctx")) {
        if (popupNode_ < r.Nodes.size() && r.Exists(popupNode_))
            contextMenu(popupNode_);
        else
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
