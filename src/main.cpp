// ============================================================================
// YADUA - Yet Another Disk Usage Analyzer
//
// Fast disk-space scanner for NTFS volumes. Instead of recursively walking
// directories with FindFirstFile/FindNextFile (one syscall round-trip per
// directory, random I/O all over the disk), we read the volume's Master File
// Table (MFT) directly:
//
//   1. Open the raw volume:           CreateFileW(L"\\\\.\\C:")
//   2. Ask NTFS where the MFT lives:  FSCTL_GET_NTFS_VOLUME_DATA
//   3. Read MFT record 0 ($MFT itself), decode its $DATA run list to learn
//      every on-disk extent of the MFT.
//   4. Stream the whole MFT with large sequential reads on a reader thread
//      while a pool of worker threads parses the records in parallel.
//   5. Parse each 1 KB FILE record: name + parent ref ($FILE_NAME attribute),
//      size ($DATA attribute), in-use / directory flags.
//   6. Reconstruct the directory tree from parent references and aggregate
//      folder sizes bottom-up.
//
// Concurrency model: each MFT record lives in exactly one chunk, and each
// chunk is parsed by exactly one worker, so writes to a record's own Node are
// exclusive. The two cross-thread cases are handled explicitly:
//   - extension records add sizes to their *base* record's node, which may be
//     owned by another worker -> contributions are queued per-thread and
//     applied single-threaded after the parse;
//   - names are appended to per-thread arenas and merged (offsets rebased)
//     after the parse.
//
// Requires Administrator privileges (raw volume access).
// Build: see build.ps1 (cl /O2 /std:c++20).
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// NTFS on-disk structures
//
// These are not in the Windows SDK (the MFT layout is technically
// undocumented but stable since NT 3.1 and described in great detail in the
// Linux-NTFS / ntfs-3g project docs). All fields are little-endian.
// ============================================================================
#pragma pack(push, 1)

// Every MFT record ("FILE record segment") starts with this header.
struct FileRecordHeader {
    uint32_t Magic;             // 'FILE' (0x454C4946), or 'BAAD' if corrupt
    uint16_t UsaOffset;         // offset of the Update Sequence Array
    uint16_t UsaCount;          // 1 (USN) + one entry per sector of the record
    uint64_t Lsn;               // $LogFile sequence number
    uint16_t SequenceNumber;    // bumped every time the record is reused
    uint16_t HardLinkCount;
    uint16_t FirstAttributeOffset;
    uint16_t Flags;             // 0x01 = in use, 0x02 = directory
    uint32_t UsedSize;          // bytes actually used within the record
    uint32_t AllocatedSize;     // total record size (== BytesPerFileRecordSegment)
    uint64_t BaseRecord;        // 0 for base records; file-reference of the base
                                // record for "extension" records (overflow)
    uint16_t NextAttributeId;
};

constexpr uint32_t kFileRecordMagic   = 0x454C4946; // 'FILE'
constexpr uint16_t kRecordInUse       = 0x0001;
constexpr uint16_t kRecordIsDirectory = 0x0002;

// Attribute type codes we care about.
constexpr uint32_t kAttrFileName = 0x30;
constexpr uint32_t kAttrData     = 0x80;
constexpr uint32_t kAttrEnd      = 0xFFFFFFFF;

// Common header at the start of every attribute inside a record.
struct AttributeHeader {
    uint32_t Type;
    uint32_t Length;            // total attribute length, 8-byte aligned
    uint8_t  NonResident;       // 0 = value stored inline, 1 = value on disk
    uint8_t  NameLength;        // attribute name length in WCHARs (e.g. ADS name)
    uint16_t NameOffset;
    uint16_t Flags;             // compressed / encrypted / sparse
    uint16_t AttributeId;
};

// Resident attribute: the value lives inside the MFT record itself.
struct ResidentAttribute {
    AttributeHeader Header;
    uint32_t ValueLength;
    uint16_t ValueOffset;
    uint8_t  IndexedFlag;
    uint8_t  Padding;
};

// Non-resident attribute: value lives in clusters on disk, located by a
// "run list" (a compact RLE encoding of (cluster, length) extents).
struct NonResidentAttribute {
    AttributeHeader Header;
    uint64_t LowestVcn;         // first Virtual Cluster Number covered by this
                                // record (>0 means continuation in an extension)
    uint64_t HighestVcn;
    uint16_t RunListOffset;
    uint16_t CompressionUnit;
    uint32_t Padding;
    uint64_t AllocatedSize;     // clusters reserved on disk, in bytes
    uint64_t RealSize;          // logical EOF
    uint64_t InitializedSize;
};

// Value of a $FILE_NAME attribute (always resident).
struct FileNameAttribute {
    uint64_t ParentRef;         // file reference of parent dir:
                                // low 48 bits = MFT record #, high 16 = sequence
    uint64_t CreationTime;
    uint64_t ModificationTime;
    uint64_t MftChangeTime;
    uint64_t AccessTime;
    uint64_t AllocatedSize;     // stale duplicates of $DATA sizes — do not trust,
    uint64_t RealSize;          //   NTFS only updates them lazily
    uint32_t FileAttributes;
    uint32_t ReparseValue;
    uint8_t  NameLength;        // in WCHARs
    uint8_t  NameSpace;         // 0=POSIX 1=Win32 2=DOS(8.3) 3=Win32&DOS
    // WCHAR Name[NameLength] follows
};

#pragma pack(pop)

// ============================================================================
// In-memory tree
// ============================================================================

// One entry per MFT record. Kept deliberately small because a big volume has
// millions of records; names live in a shared UTF-16 arena.
struct Node {
    uint64_t LogicalSize   = 0; // file size (EOF) of the unnamed $DATA stream
    uint64_t AllocatedSize = 0; // bytes of disk actually reserved
    uint32_t NameOffset    = 0; // into g_nameArena (after the rebase pass)
    uint32_t Parent        = 0; // MFT record number of parent directory
    uint16_t ParentSeq     = 0; // expected sequence number of the parent record
    uint16_t Sequence      = 0; // this record's own sequence number
    uint16_t NameLength    = 0; // in WCHARs
    uint8_t  Flags         = 0; // bit0 in-use, bit1 directory, bit2 has-name
    uint8_t  NameRank      = 0xFF; // namespace preference of the stored name
    uint8_t  ThreadId      = 0; // which worker's arena holds the name
};

constexpr uint8_t kNodeInUse  = 0x01;
constexpr uint8_t kNodeIsDir  = 0x02;
constexpr uint8_t kNodeNamed  = 0x04;

constexpr uint64_t kRootRecord = 5; // the root directory "." is always record 5

static std::vector<Node>    g_nodes;
static std::vector<wchar_t> g_nameArena;

// Prefer the long Win32 name over the POSIX namespace, and both over the
// 8.3 short name (a file usually carries both a Win32 and a DOS $FILE_NAME).
static uint8_t NamespaceRank(uint8_t ns) {
    switch (ns) {
        case 1: case 3: return 0; // Win32 / Win32+DOS — best
        case 0:         return 1; // POSIX
        default:        return 2; // DOS 8.3 — last resort
    }
}

// ============================================================================
// Record parsing
// ============================================================================

// Per-worker scratch state; merged into the globals after the parallel phase.
struct ParseContext {
    // Names found by this worker. Node.NameOffset temporarily indexes into
    // this arena; a post-parse pass rebases it into the merged g_nameArena.
    std::vector<wchar_t> NameArena;

    // Size contributions from extension records whose base node may be owned
    // by another worker. Applied single-threaded after the join.
    struct Contribution { uint32_t Target; uint64_t Logical; uint64_t Allocated; };
    std::vector<Contribution> Deferred;

    uint8_t ThreadId = 0;
};

// Multi-sector records are protected against torn writes: the last 2 bytes of
// every sector hold a copy of the Update Sequence Number and the real bytes
// are stashed in the Update Sequence Array. We must swap them back before
// reading anything past the first sector. Returns false on a torn/corrupt record.
static bool ApplyFixups(uint8_t* rec, uint32_t recordSize) {
    auto* hdr = reinterpret_cast<FileRecordHeader*>(rec);
    if (hdr->UsaCount < 2) return false;
    uint32_t stride = recordSize / (hdr->UsaCount - 1); // bytes per protected sector
    if (hdr->UsaOffset + hdr->UsaCount * 2u > recordSize) return false;

    const uint16_t* usa = reinterpret_cast<uint16_t*>(rec + hdr->UsaOffset);
    uint16_t usn = usa[0];
    for (uint32_t i = 1; i < hdr->UsaCount; ++i) {
        uint8_t* tail = rec + i * stride - 2;
        if (*reinterpret_cast<uint16_t*>(tail) != usn) return false; // torn write
        memcpy(tail, &usa[i], 2);
    }
    return true;
}

// Decode one run list entry pair. Each entry is a header byte (low nibble =
// #bytes of length field, high nibble = #bytes of LCN-delta field) followed by
// those variable-width little-endian integers. The LCN delta is SIGNED and
// relative to the previous run's LCN. A zero-width delta means a sparse run.
struct Extent { uint64_t Vcn; int64_t Lcn; uint64_t Clusters; }; // Lcn < 0 => sparse

static std::vector<Extent> DecodeRunList(const uint8_t* p, const uint8_t* end,
                                         uint64_t startVcn) {
    std::vector<Extent> runs;
    int64_t  lcn = 0;
    uint64_t vcn = startVcn;
    while (p < end && *p != 0) {
        int lenBytes = *p & 0x0F;
        int offBytes = (*p >> 4) & 0x0F;
        ++p;
        if (lenBytes == 0 || p + lenBytes + offBytes > end) break;

        uint64_t length = 0;
        memcpy(&length, p, lenBytes);
        p += lenBytes;

        if (offBytes == 0) {
            runs.push_back({vcn, -1, length});       // sparse (no disk clusters)
        } else {
            int64_t delta = 0;
            memcpy(&delta, p, offBytes);
            // sign-extend the variable-width delta
            if (p[offBytes - 1] & 0x80)
                delta |= ~0LL << (offBytes * 8);
            p += offBytes;
            lcn += delta;
            runs.push_back({vcn, lcn, length});
        }
        vcn += length;
    }
    return runs;
}

// Parse one MFT record and fold its information into g_nodes / the context.
static void ParseRecord(uint8_t* rec, uint32_t recordSize, uint64_t recordIndex,
                        ParseContext& ctx) {
    if (recordIndex >= g_nodes.size()) return;
    auto* hdr = reinterpret_cast<FileRecordHeader*>(rec);
    if (hdr->Magic != kFileRecordMagic) return;       // unused/corrupt slot
    if (!(hdr->Flags & kRecordInUse)) return;         // deleted file
    if (!ApplyFixups(rec, recordSize)) return;

    // Attributes of very large / very fragmented files overflow into
    // "extension" records whose BaseRecord points at the owner. Their $DATA
    // headers belong to the base file, so their sizes are routed to the base
    // node — via the deferred queue, because another worker may own it.
    uint64_t targetIndex = recordIndex;
    bool isExtension = false;
    if (hdr->BaseRecord != 0) {
        targetIndex = hdr->BaseRecord & 0x0000FFFFFFFFFFFFull;
        if (targetIndex >= g_nodes.size()) return;
        isExtension = true;
    }
    Node* node = isExtension ? nullptr : &g_nodes[targetIndex];
    if (node) {
        node->Flags   |= kNodeInUse;
        node->Sequence = hdr->SequenceNumber;
        if (hdr->Flags & kRecordIsDirectory) node->Flags |= kNodeIsDir;
    }

    uint64_t addLogical = 0, addAllocated = 0;

    // Walk the attribute list. Every offset comes from disk, so bounds-check
    // everything — a single bad record must not crash the scan.
    uint32_t limit  = std::min(hdr->UsedSize, recordSize);
    uint32_t offset = hdr->FirstAttributeOffset;
    while (offset + sizeof(AttributeHeader) <= limit) {
        auto* attr = reinterpret_cast<AttributeHeader*>(rec + offset);
        if (attr->Type == kAttrEnd) break;
        if (attr->Length < sizeof(AttributeHeader) || offset + attr->Length > limit)
            break;

        if (attr->Type == kAttrFileName && !attr->NonResident && node) {
            auto* res = reinterpret_cast<ResidentAttribute*>(attr);
            if (res->ValueOffset + sizeof(FileNameAttribute) <= attr->Length) {
                auto* fn = reinterpret_cast<FileNameAttribute*>(
                    rec + offset + res->ValueOffset);
                uint32_t nameBytes = fn->NameLength * sizeof(wchar_t);
                uint8_t  rank      = NamespaceRank(fn->NameSpace);
                // Keep the best-ranked name. A file with several Win32 names
                // is a hard link; we keep the first one we see (counted once).
                if (rank < node->NameRank &&
                    res->ValueOffset + sizeof(FileNameAttribute) + nameBytes
                        <= attr->Length) {
                    node->NameRank   = rank;
                    node->Parent     = static_cast<uint32_t>(
                        fn->ParentRef & 0x0000FFFFFFFFFFFFull);
                    node->ParentSeq  = static_cast<uint16_t>(fn->ParentRef >> 48);
                    node->NameOffset = static_cast<uint32_t>(ctx.NameArena.size());
                    node->NameLength = fn->NameLength;
                    node->ThreadId   = ctx.ThreadId;
                    node->Flags     |= kNodeNamed;
                    const wchar_t* name = reinterpret_cast<wchar_t*>(
                        rec + offset + res->ValueOffset + sizeof(FileNameAttribute));
                    ctx.NameArena.insert(ctx.NameArena.end(),
                                         name, name + fn->NameLength);
                }
            }
        } else if (attr->Type == kAttrData && attr->NameLength == 0) {
            // Unnamed $DATA = the file's main contents. Named $DATA attributes
            // are Alternate Data Streams — skipped on purpose (this also dodges
            // $BadClus:$Bad, a sparse ADS the size of the whole volume!).
            if (attr->NonResident) {
                auto* nr = reinterpret_cast<NonResidentAttribute*>(attr);
                // Only the header with LowestVcn == 0 carries the stream's
                // total sizes; continuation headers in extension records
                // repeat nothing and must not be double-counted.
                if (nr->LowestVcn == 0 && offset + sizeof(NonResidentAttribute) <= limit) {
                    addLogical   += nr->RealSize;
                    addAllocated += nr->AllocatedSize;
                }
            } else {
                auto* res = reinterpret_cast<ResidentAttribute*>(attr);
                addLogical += res->ValueLength;
                // resident data occupies no clusters of its own => allocated 0
            }
        }

        offset += attr->Length;
    }

    if (node) {
        node->LogicalSize   += addLogical;
        node->AllocatedSize += addAllocated;
    } else if (addLogical || addAllocated) {
        ctx.Deferred.push_back({static_cast<uint32_t>(targetIndex),
                                addLogical, addAllocated});
    }
}

// ============================================================================
// MFT streaming (reader thread + worker pool)
// ============================================================================

static bool ReadAt(HANDLE volume, uint64_t offset, void* buffer, uint32_t bytes) {
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    return ReadFile(volume, buffer, bytes, &read, &ov) && read == bytes;
}

// A chunk of the $MFT data stream, always holding whole FILE records.
struct Chunk {
    std::vector<uint8_t> Data;
    size_t   Bytes       = 0;   // valid bytes (multiple of the record size)
    uint64_t FirstRecord = 0;   // stream-wide index of the first record
};

template <typename T>
class BlockingQueue {
public:
    void Push(T* item) {
        { std::lock_guard<std::mutex> lock(mutex_); items_.push_back(item); }
        cv_.notify_one();
    }
    // Blocks until an item is available; returns false once the queue is
    // closed and drained.
    bool Pop(T*& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !items_.empty() || closed_; });
        if (items_.empty()) return false;
        out = items_.front();
        items_.pop_front();
        return true;
    }
    void Close() {
        { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; }
        cv_.notify_all();
    }
private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::deque<T*>          items_;
    bool                    closed_ = false;
};

struct ScanStats {
    uint64_t BytesRead     = 0;
    uint64_t RecordsParsed = 0;
};

// Streams the MFT (this thread does the I/O) while `workers` threads parse.
// Returns false on a read error.
static bool StreamMft(HANDLE volume, const std::vector<Extent>& mftRuns,
                      uint64_t mftBytes, uint32_t clusterSize, uint32_t recordSize,
                      unsigned workerCount, std::vector<ParseContext>& contexts,
                      ScanStats& stats) {
    constexpr size_t kChunkSize = 8 * 1024 * 1024;

    BlockingQueue<Chunk> readyQueue;  // filled chunks awaiting a parser
    BlockingQueue<Chunk> freeQueue;   // empty chunks awaiting the reader

    // Enough chunks that the reader can stay ahead of the parsers.
    std::vector<Chunk> pool(workerCount * 2 + 2);
    for (Chunk& c : pool) {
        c.Data.resize(kChunkSize + recordSize + clusterSize);
        freeQueue.Push(&c);
    }

    contexts.resize(workerCount);
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned t = 0; t < workerCount; ++t) {
        contexts[t].ThreadId = static_cast<uint8_t>(t);
        contexts[t].NameArena.reserve(4u * 1024 * 1024);
        workers.emplace_back([&, t] {
            ParseContext& ctx = contexts[t];
            Chunk* chunk = nullptr;
            while (readyQueue.Pop(chunk)) {
                for (size_t off = 0; off < chunk->Bytes; off += recordSize)
                    ParseRecord(chunk->Data.data() + off, recordSize,
                                chunk->FirstRecord + off / recordSize, ctx);
                freeQueue.Push(chunk);
            }
        });
    }

    // FILE records can straddle chunk (or even extent) boundaries when the
    // cluster size is smaller than the record size; `carry` holds the partial
    // record between iterations and is prepended to the next chunk.
    std::vector<uint8_t> carry;
    carry.reserve(recordSize);
    uint64_t nextRecord = 0;
    bool readError = false;

    // Hand `validBytes` of stream data (carry included) to the parsers; the
    // sub-record tail goes back into `carry`.
    auto dispatch = [&](Chunk* chunk, size_t validBytes) {
        size_t completeBytes = validBytes / recordSize * recordSize;
        carry.assign(chunk->Data.data() + completeBytes,
                     chunk->Data.data() + validBytes);
        if (completeBytes == 0) { freeQueue.Push(chunk); return; }
        chunk->Bytes       = completeBytes;
        chunk->FirstRecord = nextRecord;
        nextRecord += completeBytes / recordSize;
        readyQueue.Push(chunk);
    };

    uint64_t streamed = 0; // bytes of the $MFT data stream consumed so far
    for (const Extent& run : mftRuns) {
        if (readError || streamed >= mftBytes) break;
        uint64_t runBytes =
            std::min(run.Clusters * (uint64_t)clusterSize, mftBytes - streamed);

        for (uint64_t done = 0; done < runBytes; ) {
            Chunk* chunk = nullptr;
            freeQueue.Pop(chunk);
            size_t carryLen = carry.size();
            if (carryLen) memcpy(chunk->Data.data(), carry.data(), carryLen);

            uint64_t remaining = runBytes - done;
            if (run.Lcn < 0) {
                // Sparse run: no clusters on disk, the stream reads as zeros.
                // Feed zeros to keep record numbering aligned (zero magic =>
                // records are skipped by the parser).
                size_t n = (size_t)std::min<uint64_t>(kChunkSize, remaining);
                memset(chunk->Data.data() + carryLen, 0, n);
                dispatch(chunk, carryLen + n);
                done += n;
            } else {
                // Reads must be cluster-aligned on disk; round the request up
                // and feed only the bytes that are real stream data.
                size_t maxRead = (chunk->Data.size() - carryLen)
                                 / clusterSize * clusterSize;
                maxRead = std::min(maxRead, kChunkSize);
                uint64_t wanted = (remaining + clusterSize - 1)
                                  / clusterSize * (uint64_t)clusterSize;
                uint32_t readLen = (uint32_t)std::min<uint64_t>(maxRead, wanted);
                if (!ReadAt(volume, (uint64_t)run.Lcn * clusterSize + done,
                            chunk->Data.data() + carryLen, readLen)) {
                    fprintf(stderr, "error: volume read failed at LCN %lld (%lu)\n",
                            run.Lcn, GetLastError());
                    freeQueue.Push(chunk);
                    readError = true;
                    break;
                }
                stats.BytesRead += readLen;
                size_t feed = (size_t)std::min<uint64_t>(remaining, readLen);
                dispatch(chunk, carryLen + feed);
                done += feed;
            }
        }
        streamed += runBytes;
    }

    readyQueue.Close();
    for (std::thread& w : workers) w.join();
    stats.RecordsParsed = nextRecord;
    return !readError;
}

// Merge the per-worker results back into the globals: apply deferred
// extension-record sizes and rebase name offsets into one shared arena.
static void MergeContexts(std::vector<ParseContext>& contexts) {
    for (ParseContext& ctx : contexts)
        for (const ParseContext::Contribution& c : ctx.Deferred) {
            g_nodes[c.Target].LogicalSize   += c.Logical;
            g_nodes[c.Target].AllocatedSize += c.Allocated;
        }

    std::vector<size_t> arenaBase(contexts.size() + 1, 0);
    for (size_t t = 0; t < contexts.size(); ++t)
        arenaBase[t + 1] = arenaBase[t] + contexts[t].NameArena.size();

    g_nameArena.resize(arenaBase.back());
    for (size_t t = 0; t < contexts.size(); ++t)
        std::copy(contexts[t].NameArena.begin(), contexts[t].NameArena.end(),
                  g_nameArena.begin() + arenaBase[t]);

    for (Node& n : g_nodes)
        if (n.Flags & kNodeNamed)
            n.NameOffset += static_cast<uint32_t>(arenaBase[n.ThreadId]);
}

// ============================================================================
// Aggregation
// ============================================================================

struct DirTotals {
    uint64_t LogicalSize   = 0;
    uint64_t AllocatedSize = 0;
    uint64_t FileCount     = 0;
    uint64_t DirCount      = 0;
};

static bool IsValidParent(uint32_t parent, uint16_t expectedSeq) {
    if (parent >= g_nodes.size()) return false;
    const Node& p = g_nodes[parent];
    // The sequence check rejects "stale" parents: if the directory was deleted
    // and its MFT slot reused for something else, the sequence won't match.
    return (p.Flags & kNodeInUse) && (p.Flags & kNodeIsDir) &&
           p.Sequence == expectedSeq;
}

// For every file/dir, walk the parent chain to the root, adding its size and
// count into each ancestor. ~O(records * avg depth) — a few tens of millions
// of additions, negligible next to the disk read.
static void Aggregate(std::vector<DirTotals>& totals) {
    constexpr int kMaxDepth = 512; // cycle guard for corrupt parent chains
    for (size_t i = 0; i < g_nodes.size(); ++i) {
        const Node& n = g_nodes[i];
        if (!(n.Flags & kNodeInUse) || !(n.Flags & kNodeNamed)) continue;
        if (i == kRootRecord) continue;

        bool isDir = (n.Flags & kNodeIsDir) != 0;
        uint32_t cur = n.Parent;
        uint16_t seq = n.ParentSeq;
        for (int depth = 0; depth < kMaxDepth; ++depth) {
            if (!IsValidParent(cur, seq)) break;     // orphan — stop attributing
            DirTotals& t = totals[cur];
            t.LogicalSize   += n.LogicalSize;
            t.AllocatedSize += n.AllocatedSize;
            if (isDir) ++t.DirCount; else ++t.FileCount;
            const Node& p = g_nodes[cur];
            if (cur == p.Parent) break;              // reached the root (5)
            seq = p.ParentSeq;
            cur = p.Parent;
        }
    }
}

// Child adjacency in CSR form (offsets into one flat child array), used by the
// full-tree exports. Built from validated parent links only, so a DFS from the
// root cannot cycle. Children come out sorted by size, largest first.
struct ChildIndex {
    std::vector<uint32_t> Offset; // size = nodes + 1
    std::vector<uint32_t> List;
};

static ChildIndex BuildChildIndex(const std::vector<DirTotals>& totals) {
    ChildIndex ci;
    size_t n = g_nodes.size();
    ci.Offset.assign(n + 1, 0);

    auto linked = [&](size_t i) {
        const Node& nd = g_nodes[i];
        return (nd.Flags & kNodeInUse) && (nd.Flags & kNodeNamed) &&
               i != kRootRecord && IsValidParent(nd.Parent, nd.ParentSeq);
    };

    for (size_t i = 0; i < n; ++i)
        if (linked(i)) ++ci.Offset[g_nodes[i].Parent + 1];
    for (size_t i = 0; i < n; ++i)
        ci.Offset[i + 1] += ci.Offset[i];

    ci.List.resize(ci.Offset[n]);
    std::vector<uint32_t> cursor(ci.Offset.begin(), ci.Offset.end() - 1);
    for (size_t i = 0; i < n; ++i)
        if (linked(i)) ci.List[cursor[g_nodes[i].Parent]++] = (uint32_t)i;

    auto sizeOf = [&](uint32_t i) {
        return (g_nodes[i].Flags & kNodeIsDir) ? totals[i].LogicalSize
                                               : g_nodes[i].LogicalSize;
    };
    for (size_t i = 0; i < n; ++i)
        std::sort(ci.List.begin() + ci.Offset[i], ci.List.begin() + ci.Offset[i + 1],
                  [&](uint32_t a, uint32_t b) { return sizeOf(a) > sizeOf(b); });
    return ci;
}

// ============================================================================
// Formatting helpers
// ============================================================================

static std::wstring NodeName(uint32_t index) {
    const Node& n = g_nodes[index];
    return std::wstring(g_nameArena.data() + n.NameOffset, n.NameLength);
}

static std::wstring BuildPath(uint32_t index, const std::wstring& volumeLabel) {
    std::vector<uint32_t> chain;
    uint32_t cur = index;
    for (int depth = 0; depth < 512 && cur != kRootRecord; ++depth) {
        chain.push_back(cur);
        const Node& n = g_nodes[cur];
        if (n.Parent >= g_nodes.size() || n.Parent == cur) break;
        cur = n.Parent;
    }
    std::wstring path = volumeLabel; // e.g. L"C:"
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        path += L'\\';
        path += NodeName(*it);
    }
    return path;
}

static std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        out.data(), len, nullptr, nullptr);
    return out;
}

static std::string HumanSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.2f %s", v, units[u]);
    return buf;
}

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

static void ExportCsv(FILE* f, const std::wstring& drive, const TopLists& top,
                      const std::vector<DirTotals>& totals,
                      const ChildIndex* tree /* null => top-N only */) {
    fputs("\xEF\xBB\xBF", f); // UTF-8 BOM so Excel decodes correctly
    fputs("type,path,size_bytes,allocated_bytes,files,folders\n", f);

    auto folderRow = [&](const std::string& path, uint32_t idx) {
        fprintf(f, "folder,%s,%llu,%llu,%llu,%llu\n", CsvEscape(path).c_str(),
                totals[idx].LogicalSize, totals[idx].AllocatedSize,
                totals[idx].FileCount, totals[idx].DirCount);
    };
    auto fileRow = [&](const std::string& path, uint32_t idx) {
        fprintf(f, "file,%s,%llu,%llu,,\n", CsvEscape(path).c_str(),
                g_nodes[idx].LogicalSize, g_nodes[idx].AllocatedSize);
    };

    if (!tree) {
        for (size_t i = 0; i < top.DirCount; ++i)
            folderRow(Utf8(BuildPath(top.Dirs[i], drive)), top.Dirs[i]);
        for (size_t i = 0; i < top.FileCount; ++i)
            fileRow(Utf8(BuildPath(top.Files[i], drive)), top.Files[i]);
        return;
    }

    // Full tree: DFS preorder, reusing one path string instead of rebuilding
    // the path for each of potentially millions of rows.
    std::wstring path = drive;
    std::function<void(uint32_t)> walk = [&](uint32_t idx) {
        if (g_nodes[idx].Flags & kNodeIsDir) {
            folderRow(Utf8(path), idx);
            for (uint32_t c = tree->Offset[idx]; c < tree->Offset[idx + 1]; ++c) {
                uint32_t child = tree->List[c];
                size_t len = path.size();
                path += L'\\';
                path += NodeName(child);
                walk(child);
                path.resize(len);
            }
        } else {
            fileRow(Utf8(path), idx);
        }
    };
    walk(kRootRecord);
}

static void ExportJson(FILE* f, const std::wstring& drive, const TopLists& top,
                       const std::vector<DirTotals>& totals,
                       uint64_t fileCount, uint64_t dirCount,
                       const ChildIndex* tree /* null => no full tree */) {
    const DirTotals& root = totals[kRootRecord];
    fputs("{\n", f);
    fputs("  \"volume\": ", f);
    JsonWriteString(f, Utf8(drive));
    fprintf(f, ",\n  \"files\": %llu,\n  \"folders\": %llu,\n"
               "  \"total_size\": %llu,\n  \"allocated_size\": %llu,\n",
            fileCount, dirCount, root.LogicalSize, root.AllocatedSize);

    fputs("  \"top_folders\": [", f);
    for (size_t i = 0; i < top.DirCount; ++i) {
        uint32_t idx = top.Dirs[i];
        fputs(i ? ",\n    " : "\n    ", f);
        fputs("{\"path\": ", f);
        JsonWriteString(f, Utf8(BuildPath(idx, drive)));
        fprintf(f, ", \"size\": %llu, \"allocated\": %llu, "
                   "\"files\": %llu, \"folders\": %llu}",
                totals[idx].LogicalSize, totals[idx].AllocatedSize,
                totals[idx].FileCount, totals[idx].DirCount);
    }
    fputs("\n  ],\n  \"top_files\": [", f);
    for (size_t i = 0; i < top.FileCount; ++i) {
        uint32_t idx = top.Files[i];
        fputs(i ? ",\n    " : "\n    ", f);
        fputs("{\"path\": ", f);
        JsonWriteString(f, Utf8(BuildPath(idx, drive)));
        fprintf(f, ", \"size\": %llu, \"allocated\": %llu}",
                g_nodes[idx].LogicalSize, g_nodes[idx].AllocatedSize);
    }
    fputs("\n  ]", f);

    if (tree) {
        fputs(",\n  \"tree\": ", f);
        std::function<void(uint32_t, bool)> emit = [&](uint32_t idx, bool isRoot) {
            fputs("{\"name\": ", f);
            JsonWriteString(f, isRoot ? Utf8(drive) : Utf8(NodeName(idx)));
            if (g_nodes[idx].Flags & kNodeIsDir) {
                fprintf(f, ", \"size\": %llu, \"allocated\": %llu, "
                           "\"files\": %llu, \"folders\": %llu, \"children\": [",
                        totals[idx].LogicalSize, totals[idx].AllocatedSize,
                        totals[idx].FileCount, totals[idx].DirCount);
                for (uint32_t c = tree->Offset[idx]; c < tree->Offset[idx + 1]; ++c) {
                    if (c != tree->Offset[idx]) fputc(',', f);
                    emit(tree->List[c], false);
                }
                fputs("]}", f);
            } else {
                fprintf(f, ", \"size\": %llu, \"allocated\": %llu}",
                        g_nodes[idx].LogicalSize, g_nodes[idx].AllocatedSize);
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
        else if (!arg.empty() && arg[0] != L'-') {
            drive = arg;
            if (drive.size() == 1) drive += L':';   // allow "C"
            if (drive.size() > 2 && drive.back() == L'\\') drive.pop_back();
        }
    }
    if (threads == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        threads = hw > 3 ? std::min(hw - 2, 8u) : 1u; // leave cores for I/O + OS
    }
    threads = std::min(threads, 255u); // ThreadId is a uint8_t

    auto t0 = std::chrono::steady_clock::now();

    // ---- 1. Open the raw volume ------------------------------------------
    std::wstring volumePath = L"\\\\.\\" + drive;
    HANDLE volume = CreateFileW(volumePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        fprintf(stderr, "error: cannot open %ls (Win32 error %lu)\n",
                volumePath.c_str(), err);
        if (err == ERROR_ACCESS_DENIED)
            fprintf(stderr, "Raw volume access requires Administrator rights — "
                            "re-run from an elevated terminal.\n");
        return 1;
    }

    // ---- 2. Where is the MFT? --------------------------------------------
    NTFS_VOLUME_DATA_BUFFER vol{};
    DWORD bytes = 0;
    if (!DeviceIoControl(volume, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                         &vol, sizeof(vol), &bytes, nullptr)) {
        fprintf(stderr, "error: FSCTL_GET_NTFS_VOLUME_DATA failed (%lu) — "
                        "is %ls an NTFS volume?\n", GetLastError(), drive.c_str());
        return 1;
    }
    const uint32_t clusterSize = vol.BytesPerCluster;
    const uint32_t recordSize  = vol.BytesPerFileRecordSegment;
    const uint64_t mftBytes    = vol.MftValidDataLength.QuadPart;
    const uint64_t recordCount = mftBytes / recordSize;

    printf("Volume %ls: cluster %u B, MFT record %u B, MFT size %s "
           "(%llu records), %u parser threads\n",
           drive.c_str(), clusterSize, recordSize,
           HumanSize(mftBytes).c_str(), recordCount, threads);

    // ---- 3. Read $MFT's own record (record 0) and decode its run list ----
    // FSCTL only tells us where the MFT *starts*; the MFT itself is usually
    // fragmented, and the authoritative extent map is the run list of its own
    // unnamed $DATA attribute, stored in record 0 at MftStartLcn.
    uint32_t firstReadSize =
        ((recordSize + clusterSize - 1) / clusterSize) * clusterSize;
    std::vector<uint8_t> rec0(firstReadSize);
    if (!ReadAt(volume, vol.MftStartLcn.QuadPart * (uint64_t)clusterSize,
                rec0.data(), firstReadSize)) {
        fprintf(stderr, "error: cannot read MFT record 0 (%lu)\n", GetLastError());
        return 1;
    }
    auto* hdr0 = reinterpret_cast<FileRecordHeader*>(rec0.data());
    if (hdr0->Magic != kFileRecordMagic || !ApplyFixups(rec0.data(), recordSize)) {
        fprintf(stderr, "error: MFT record 0 is not a valid FILE record\n");
        return 1;
    }

    std::vector<Extent> mftRuns;
    {
        uint32_t limit  = std::min(hdr0->UsedSize, recordSize);
        uint32_t offset = hdr0->FirstAttributeOffset;
        while (offset + sizeof(AttributeHeader) <= limit) {
            auto* attr = reinterpret_cast<AttributeHeader*>(rec0.data() + offset);
            if (attr->Type == kAttrEnd || attr->Length == 0) break;
            if (attr->Type == kAttrData && attr->NameLength == 0 && attr->NonResident) {
                auto* nr = reinterpret_cast<NonResidentAttribute*>(attr);
                const uint8_t* rl  = rec0.data() + offset + nr->RunListOffset;
                const uint8_t* end = rec0.data() + offset + attr->Length;
                mftRuns = DecodeRunList(rl, end, nr->LowestVcn);
                break;
            }
            offset += attr->Length;
        }
    }
    if (mftRuns.empty()) {
        fprintf(stderr, "error: could not decode $MFT run list "
                        "(possibly an $ATTRIBUTE_LIST overflow — not handled yet)\n");
        return 1;
    }
    {
        uint64_t mapped = 0;
        for (auto& r : mftRuns) mapped += r.Clusters;
        if (mapped * clusterSize < mftBytes) {
            fprintf(stderr, "error: $MFT run list covers %s but valid data is %s — "
                            "run list continues in an extension record (not handled yet)\n",
                    HumanSize(mapped * clusterSize).c_str(), HumanSize(mftBytes).c_str());
            return 1;
        }
    }

    // ---- 4 & 5. Stream the MFT and parse every record in parallel ---------
    g_nodes.assign(recordCount, Node{});

    std::vector<ParseContext> contexts;
    ScanStats stats;
    bool ok = StreamMft(volume, mftRuns, mftBytes, clusterSize, recordSize,
                        threads, contexts, stats);
    CloseHandle(volume);
    if (!ok) return 1;
    MergeContexts(contexts);
    contexts.clear(); // free per-thread arenas
    contexts.shrink_to_fit();

    auto t1 = std::chrono::steady_clock::now();
    double scanSec =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1e6;

    // ---- 6. Aggregate folder sizes bottom-up -------------------------------
    std::vector<DirTotals> totals(g_nodes.size());
    Aggregate(totals);

    auto t2 = std::chrono::steady_clock::now();
    double totalSec =
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count() / 1e6;

    // ---- Reporting ----------------------------------------------------------
    uint64_t fileCount = 0, dirCount = 0;
    TopLists top;
    top.Files.reserve(g_nodes.size());
    for (uint32_t i = 0; i < g_nodes.size(); ++i) {
        const Node& n = g_nodes[i];
        if (!(n.Flags & kNodeInUse) || !(n.Flags & kNodeNamed)) continue;
        if (n.Flags & kNodeIsDir) { ++dirCount; top.Dirs.push_back(i); }
        else                      { ++fileCount; top.Files.push_back(i); }
    }

    auto byFileSize = [](uint32_t a, uint32_t b) {
        return g_nodes[a].LogicalSize > g_nodes[b].LogicalSize;
    };
    auto byDirSize = [&totals](uint32_t a, uint32_t b) {
        return totals[a].LogicalSize > totals[b].LogicalSize;
    };
    top.FileCount = std::min<size_t>(topN, top.Files.size());
    top.DirCount  = std::min<size_t>(topN, top.Dirs.size());
    std::partial_sort(top.Files.begin(), top.Files.begin() + top.FileCount,
                      top.Files.end(), byFileSize);
    std::partial_sort(top.Dirs.begin(), top.Dirs.begin() + top.DirCount,
                      top.Dirs.end(), byDirSize);

    const DirTotals& root = totals[kRootRecord];
    printf("\nScanned %llu records in %.2f s (MFT streamed in %.2f s, %s @ %.0f MB/s)\n",
           stats.RecordsParsed, totalSec, scanSec,
           HumanSize(stats.BytesRead).c_str(),
           stats.BytesRead / scanSec / (1024.0 * 1024.0));
    printf("Files: %llu   Folders: %llu   Total size: %s   On disk: %s\n",
           fileCount, dirCount,
           HumanSize(root.LogicalSize).c_str(),
           HumanSize(root.AllocatedSize).c_str());

    printf("\n=== Top %zu folders by size ===\n", top.DirCount);
    for (size_t i = 0; i < top.DirCount; ++i) {
        uint32_t idx = top.Dirs[i];
        printf("%12s  %9llu files  %s\n",
               HumanSize(totals[idx].LogicalSize).c_str(),
               totals[idx].FileCount,
               Utf8(BuildPath(idx, drive)).c_str());
    }

    printf("\n=== Top %zu files by size ===\n", top.FileCount);
    for (size_t i = 0; i < top.FileCount; ++i) {
        uint32_t idx = top.Files[i];
        printf("%12s  %s\n",
               HumanSize(g_nodes[idx].LogicalSize).c_str(),
               Utf8(BuildPath(idx, drive)).c_str());
    }

    // ---- Export -------------------------------------------------------------
    ChildIndex childIndex;
    if (exportAll && (!csvPath.empty() || !jsonPath.empty()))
        childIndex = BuildChildIndex(totals);
    const ChildIndex* tree = exportAll ? &childIndex : nullptr;

    auto openOut = [](const std::wstring& path) {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) setvbuf(f, nullptr, _IOFBF, 1 << 20); // big buffer: millions of rows
        else fprintf(stderr, "error: cannot write %ls\n", path.c_str());
        return f;
    };
    if (!csvPath.empty()) {
        if (FILE* f = openOut(csvPath)) {
            ExportCsv(f, drive, top, totals, tree);
            fclose(f);
            printf("\nCSV written to %ls\n", csvPath.c_str());
        } else return 1;
    }
    if (!jsonPath.empty()) {
        if (FILE* f = openOut(jsonPath)) {
            ExportJson(f, drive, top, totals, fileCount, dirCount, tree);
            fclose(f);
            printf("JSON written to %ls\n", jsonPath.c_str());
        } else return 1;
    }

    return 0;
}
