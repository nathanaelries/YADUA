// ============================================================================
// YADUA scanner library — fast NTFS volume scan via direct MFT reads.
//
// Usage:
//   yadua::ScanResult result;
//   std::wstring error;
//   if (yadua::ScanVolume(L"C:", 0, result, error)) { ... }
//
// See scanner.cpp for the full description of the scanning strategy and
// ntfs.h for the on-disk structures.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace yadua {

// One entry per MFT record. Kept deliberately small because a big volume has
// millions of records; names live in ScanResult::NameArena.
struct Node {
    uint64_t LogicalSize   = 0; // file size (EOF) of the unnamed $DATA stream
    uint64_t AllocatedSize = 0; // bytes of disk actually reserved
    uint64_t ModifiedTime  = 0; // $STANDARD_INFORMATION last-write FILETIME (0=unknown)
    uint32_t NameOffset    = 0; // into NameArena
    uint32_t Parent        = 0; // MFT record number of parent directory
    uint16_t ParentSeq     = 0; // expected sequence number of the parent record
    uint16_t Sequence      = 0; // this record's own sequence number
    uint16_t NameLength    = 0; // in WCHARs
    uint8_t  Flags         = 0; // kNode* bits below
    uint8_t  NameRank      = 0xFF; // namespace preference of the stored name
    uint8_t  ThreadId      = 0; // which worker's arena held the name (internal)
};

constexpr uint8_t kNodeInUse   = 0x01;
constexpr uint8_t kNodeIsDir   = 0x02;
constexpr uint8_t kNodeNamed   = 0x04;
constexpr uint8_t kNodeReparse = 0x08; // junction / symlink / placeholder

constexpr uint64_t kRootRecord = 5; // the root directory "." is always record 5

// Cumulative (whole-subtree) totals; only meaningful for directories.
struct DirTotals {
    uint64_t LogicalSize   = 0;
    uint64_t AllocatedSize = 0;
    uint64_t FileCount     = 0;
    uint64_t DirCount      = 0;
};

// Child adjacency in CSR form (Offset[i]..Offset[i+1] index into List), built
// from validated parent links only, so a DFS from the root cannot cycle.
// Children are sorted by size, largest first.
struct ChildIndex {
    std::vector<uint32_t> Offset; // size = Nodes.size() + 1
    std::vector<uint32_t> List;
};

// One extent of the $MFT data stream on disk (Lcn < 0 = sparse). The map is
// kept on the ScanResult so live updates can re-read individual records.
struct MftExtent {
    uint64_t Vcn;
    int64_t  Lcn;
    uint64_t Clusters;
};

struct ScanStats {
    uint64_t BytesRead     = 0;
    uint64_t RecordsParsed = 0;
    uint64_t ReparseCount  = 0; // junctions, symlinks, cloud placeholders
    uint64_t OrphanCount   = 0; // nodes reparented under the [orphaned] bucket
    uint32_t ClusterSize   = 0;
    uint32_t RecordSize    = 0;
    uint64_t MftBytes      = 0;
    double   StreamSeconds = 0; // MFT read+parse (or directory walk)
    double   TotalSeconds  = 0; // including aggregation and indexing
    uint64_t ScanUnixTime  = 0; // when the scan ran (for snapshot diffs)
    unsigned Threads       = 0;
    bool     UsedFallback  = false; // directory walk instead of raw MFT
};

// Live progress for UIs; safe to poll from another thread.
struct ScanProgress {
    std::atomic<uint64_t> BytesRead{0};
    std::atomic<uint64_t> TotalBytes{0};
    enum Phase : int { Opening = 0, Reading = 1, Aggregating = 2, Done = 3 };
    std::atomic<int> Stage{Opening};
};

struct ScanResult {
    std::wstring           Drive;     // e.g. L"C:"
    std::vector<Node>      Nodes;     // indexed by MFT record number
    std::vector<wchar_t>   NameArena;
    std::vector<DirTotals> Totals;    // indexed like Nodes
    ChildIndex             Children;
    ScanStats              Stats;
    std::vector<MftExtent> MftMap;         // raw-MFT scans only (else empty)
    std::wstring           FallbackReason; // why the MFT path was unavailable
    uint64_t               FileCount = 0;
    uint64_t               DirCount  = 0;

    // Display toggle: when true, SizeOf reports bytes-on-disk (allocated)
    // instead of logical size, so the tree, treemap, and summaries all switch
    // together (WizTree-style "size on disk"). Set by the GUI; the CLI leaves
    // it false and reads the raw fields directly.
    bool                   DisplayAllocated = false;

    bool Exists(uint32_t i) const {
        return (Nodes[i].Flags & kNodeInUse) && (Nodes[i].Flags & kNodeNamed);
    }
    bool IsDir(uint32_t i) const { return (Nodes[i].Flags & kNodeIsDir) != 0; }
    // A directory's display size is its cumulative subtree size.
    uint64_t SizeOf(uint32_t i) const {
        if (DisplayAllocated)
            return IsDir(i) ? Totals[i].AllocatedSize : Nodes[i].AllocatedSize;
        return IsDir(i) ? Totals[i].LogicalSize : Nodes[i].LogicalSize;
    }
    std::wstring Name(uint32_t i) const {
        const Node& n = Nodes[i];
        return std::wstring(NameArena.data() + n.NameOffset, n.NameLength);
    }
    std::wstring Path(uint32_t i) const; // full path, rooted at Drive
};

// Scans an NTFS volume ("C:", "D:", ...) via raw MFT reads and fills `out`
// (nodes, cumulative totals, sorted child index). `threads` = 0 picks a
// sensible default. Returns false and sets `error` on failure (no admin
// rights, not NTFS, ...). `progress` may be null.
bool ScanVolume(const std::wstring& drive, unsigned threads, ScanResult& out,
                std::wstring& error, ScanProgress* progress = nullptr);

// Slower fallback: a multi-threaded FindFirstFileExW directory walk. Works
// on any filesystem and without Administrator rights (inaccessible
// directories are skipped). Allocated sizes are approximated by rounding up
// to whole clusters. Progress reports entries found via BytesRead with
// TotalBytes left at 0 (the total is unknown up front).
bool ScanVolumeFallback(const std::wstring& drive, unsigned threads,
                        ScanResult& out, std::wstring& error,
                        ScanProgress* progress = nullptr);

// Tries the raw-MFT scan first and silently falls back to the directory
// walk; Stats.UsedFallback / FallbackReason say what happened.
bool ScanVolumeAuto(const std::wstring& drive, unsigned threads,
                    ScanResult& out, std::wstring& error,
                    ScanProgress* progress = nullptr);

// Recomputes FileCount/DirCount, cumulative totals, and the child index from
// Nodes — call after in-place modifications (deletion, subtree rescan).
void Reindex(ScanResult& r);

// Re-enumerates one directory's subtree with a filesystem walk and splices
// the fresh nodes into the result, then runs Reindex. The rescanned part's
// allocated sizes become cluster-rounded estimates (the walk can't see the
// MFT), and hard links inside it count once per link. Not allowed on
// reparse-point directories.
bool RescanSubtree(ScanResult& r, uint32_t dir, std::wstring& error,
                   ScanProgress* progress = nullptr);

// Snapshots: a compact binary dump of the tree (names, parents, sizes,
// dir/reparse flags) for later diffing. LoadSnapshot rebuilds a full
// ScanResult (totals and child index included), so a loaded snapshot can be
// browsed or diffed like a live scan.
bool SaveSnapshot(const ScanResult& r, const std::wstring& file,
                  std::wstring& error);
bool LoadSnapshot(const std::wstring& file, ScanResult& out,
                  std::wstring& error);

// Watches the NTFS USN change journal on a background thread, accumulating
// the file-reference numbers of records that changed since the scan. Pair
// with ApplyMftUpdates to fold the changes into a ScanResult without a
// rescan. Requires the same elevated access as the raw-MFT scan.
class UsnMonitor {
public:
    ~UsnMonitor() { Stop(); }
    bool Start(const std::wstring& drive, std::wstring& error);
    void Stop();
    bool Active() const { return active_.load(std::memory_order_relaxed); }
    // The journal wrapped or became unavailable; a full rescan is needed.
    bool Lost() const { return lost_.load(std::memory_order_relaxed); }
    uint64_t Pending() const { return pending_.load(std::memory_order_relaxed); }
    std::vector<uint64_t> TakeDirty(); // drains the dirty set

private:
    void Loop();

    void*                        volume_ = nullptr; // HANDLE
    std::thread                  thread_;
    std::mutex                   mutex_;
    std::unordered_set<uint64_t> dirty_;
    std::atomic<bool>            stop_{false}, active_{false}, lost_{false};
    std::atomic<uint64_t>        pending_{0};
    uint64_t                     journalId_ = 0;
    int64_t                      nextUsn_   = 0;
};

// Re-reads the given MFT records (file references from the USN journal) and
// updates the tree in place: changed sizes, new files, deletions, renames.
// `unresolved` counts records that could not be located (e.g. the MFT grew
// past the mapped extents — a rescan picks those up). Runs Reindex.
bool ApplyMftUpdates(ScanResult& r, const std::vector<uint64_t>& fileRefs,
                     std::wstring& error, unsigned* unresolved = nullptr);

// Formatting helpers shared by the frontends.
std::string  Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);
std::string  HumanSize(uint64_t bytes);

} // namespace yadua
