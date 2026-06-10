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
#include <string>
#include <vector>

namespace yadua {

// One entry per MFT record. Kept deliberately small because a big volume has
// millions of records; names live in ScanResult::NameArena.
struct Node {
    uint64_t LogicalSize   = 0; // file size (EOF) of the unnamed $DATA stream
    uint64_t AllocatedSize = 0; // bytes of disk actually reserved
    uint32_t NameOffset    = 0; // into NameArena
    uint32_t Parent        = 0; // MFT record number of parent directory
    uint16_t ParentSeq     = 0; // expected sequence number of the parent record
    uint16_t Sequence      = 0; // this record's own sequence number
    uint16_t NameLength    = 0; // in WCHARs
    uint8_t  Flags         = 0; // kNode* bits below
    uint8_t  NameRank      = 0xFF; // namespace preference of the stored name
    uint8_t  ThreadId      = 0; // which worker's arena held the name (internal)
};

constexpr uint8_t kNodeInUse = 0x01;
constexpr uint8_t kNodeIsDir = 0x02;
constexpr uint8_t kNodeNamed = 0x04;

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

struct ScanStats {
    uint64_t BytesRead     = 0;
    uint64_t RecordsParsed = 0;
    uint32_t ClusterSize   = 0;
    uint32_t RecordSize    = 0;
    uint64_t MftBytes      = 0;
    double   StreamSeconds = 0; // MFT read+parse
    double   TotalSeconds  = 0; // including aggregation and indexing
    unsigned Threads       = 0;
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
    uint64_t               FileCount = 0;
    uint64_t               DirCount  = 0;

    bool Exists(uint32_t i) const {
        return (Nodes[i].Flags & kNodeInUse) && (Nodes[i].Flags & kNodeNamed);
    }
    bool IsDir(uint32_t i) const { return (Nodes[i].Flags & kNodeIsDir) != 0; }
    // A directory's display size is its cumulative subtree size.
    uint64_t SizeOf(uint32_t i) const {
        return IsDir(i) ? Totals[i].LogicalSize : Nodes[i].LogicalSize;
    }
    std::wstring Name(uint32_t i) const {
        const Node& n = Nodes[i];
        return std::wstring(NameArena.data() + n.NameOffset, n.NameLength);
    }
    std::wstring Path(uint32_t i) const; // full path, rooted at Drive
};

// Scans an NTFS volume ("C:", "D:", ...) and fills `out` (nodes, cumulative
// totals, sorted child index). `threads` = 0 picks a sensible default.
// Returns false and sets `error` on failure. `progress` may be null.
bool ScanVolume(const std::wstring& drive, unsigned threads, ScanResult& out,
                std::wstring& error, ScanProgress* progress = nullptr);

// Formatting helpers shared by the frontends.
std::string  Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);
std::string  HumanSize(uint64_t bytes);

} // namespace yadua
