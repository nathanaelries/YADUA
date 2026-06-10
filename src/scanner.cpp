// ============================================================================
// YADUA scanner implementation.
//
// Fast disk-space scanning for NTFS volumes. Instead of recursively walking
// directories with FindFirstFile/FindNextFile (one syscall round-trip per
// directory, random I/O all over the disk), we read the volume's Master File
// Table (MFT) directly:
//
//   1. Open the raw volume:           CreateFileW(L"\\\\.\\C:")
//   2. Ask NTFS where the MFT lives:  FSCTL_GET_NTFS_VOLUME_DATA
//   3. Read MFT record 0 ($MFT itself), decode its $DATA run list to learn
//      every on-disk extent of the MFT.
//   4. Stream the whole MFT with large sequential reads on the calling thread
//      while a pool of worker threads parses the records in parallel.
//   5. Parse each 1 KB FILE record: name + parent ref ($FILE_NAME attribute),
//      size ($DATA attribute), in-use / directory flags.
//   6. Reconstruct the directory tree from parent references, aggregate
//      folder sizes bottom-up, and build a size-sorted child index.
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
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "ntfs.h"
#include "scanner.h"

namespace yadua {
using namespace yadua::ntfs;

// ============================================================================
// Formatting helpers
// ============================================================================

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}

std::string HumanSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.2f %s", v, units[u]);
    return buf;
}

std::wstring ScanResult::Path(uint32_t index) const {
    std::vector<uint32_t> chain;
    uint32_t cur = index;
    for (int depth = 0; depth < 512 && cur != kRootRecord; ++depth) {
        chain.push_back(cur);
        const Node& n = Nodes[cur];
        if (n.Parent >= Nodes.size() || n.Parent == cur) break;
        cur = n.Parent;
    }
    std::wstring path = Drive;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        path += L'\\';
        path += Name(*it);
    }
    return path;
}

namespace {

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    return buf;
}

// ============================================================================
// Record parsing
// ============================================================================

// Prefer the long Win32 name over the POSIX namespace, and both over the
// 8.3 short name (a file usually carries both a Win32 and a DOS $FILE_NAME).
uint8_t NamespaceRank(uint8_t ns) {
    switch (ns) {
        case 1: case 3: return 0; // Win32 / Win32+DOS — best
        case 0:         return 1; // POSIX
        default:        return 2; // DOS 8.3 — last resort
    }
}

// Per-worker scratch state; merged into the result after the parallel phase.
struct ParseContext {
    std::vector<Node>* Nodes = nullptr;

    // Names found by this worker. Node.NameOffset temporarily indexes into
    // this arena; a post-parse pass rebases it into the merged arena.
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
bool ApplyFixups(uint8_t* rec, uint32_t recordSize) {
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

// Decode a run list. Each entry is a header byte (low nibble = #bytes of
// length field, high nibble = #bytes of LCN-delta field) followed by those
// variable-width little-endian integers. The LCN delta is SIGNED and relative
// to the previous run's LCN. A zero-width delta means a sparse run.
using Extent = MftExtent; // Lcn < 0 => sparse

std::vector<Extent> DecodeRunList(const uint8_t* p, const uint8_t* end,
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

// Parse one MFT record and fold its information into the nodes / the context.
void ParseRecord(uint8_t* rec, uint32_t recordSize, uint64_t recordIndex,
                 ParseContext& ctx) {
    std::vector<Node>& nodes = *ctx.Nodes;
    if (recordIndex >= nodes.size()) return;
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
        if (targetIndex >= nodes.size()) return;
        isExtension = true;
    }
    Node* node = isExtension ? nullptr : &nodes[targetIndex];
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
                    if (fn->FileAttributes & kFileAttrReparsePoint)
                        node->Flags |= kNodeReparse;
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
        } else if (attr->Type == kAttrData) {
            // Unnamed $DATA = the file's main contents; named $DATA attributes
            // are Alternate Data Streams, which also occupy space. Metafile
            // ADS are skipped entirely: $BadClus:$Bad logically spans the
            // whole volume (and is not reliably sparse-*flagged*), $Secure's
            // streams are filesystem metadata, not user data.
            bool isAds = attr->NameLength != 0;
            if (isAds && targetIndex < kFirstUserRecord) {
                // skip
            } else if (attr->NonResident) {
                auto* nr = reinterpret_cast<NonResidentAttribute*>(attr);
                // Only the header with LowestVcn == 0 carries the stream's
                // total sizes; continuation headers in extension records
                // repeat nothing and must not be double-counted.
                if (nr->LowestVcn == 0 && offset + sizeof(NonResidentAttribute) <= limit) {
                    // Sparse/compressed: AllocatedSize is just the reserved
                    // VCN span; actual disk usage is TotalAllocated (0x40).
                    uint64_t allocated = nr->AllocatedSize;
                    if ((attr->Flags & (kAttrFlagSparse | kAttrFlagCompressed)) &&
                        attr->Length >= kTotalAllocatedOffset + 8 &&
                        offset + kTotalAllocatedOffset + 8 <= limit)
                        memcpy(&allocated, rec + offset + kTotalAllocatedOffset, 8);
                    addAllocated += allocated;
                    if (!isAds || !(attr->Flags & kAttrFlagSparse))
                        addLogical += nr->RealSize;
                }
            } else {
                auto* res = reinterpret_cast<ResidentAttribute*>(attr);
                addLogical += res->ValueLength;
                // resident data occupies no clusters of its own => allocated 0
            }
        } else if (attr->Type == kAttrIndexAllocation && attr->NonResident) {
            // Directory index ($I30) clusters: real disk usage that Explorer
            // ignores but the volume's free-space accounting doesn't.
            auto* nr = reinterpret_cast<NonResidentAttribute*>(attr);
            if (nr->LowestVcn == 0 && offset + sizeof(NonResidentAttribute) <= limit)
                addAllocated += nr->AllocatedSize;
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

bool ReadAt(HANDLE volume, uint64_t offset, void* buffer, uint32_t bytes) {
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

// Streams the MFT (this thread does the I/O) while `workerCount` threads
// parse. Returns false on a read error.
bool StreamMft(HANDLE volume, const std::vector<Extent>& mftRuns,
               uint64_t mftBytes, uint32_t clusterSize, uint32_t recordSize,
               unsigned workerCount, std::vector<ParseContext>& contexts,
               ScanResult& result, ScanProgress* progress, std::wstring& error) {
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
        contexts[t].Nodes    = &result.Nodes;
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
                    error = Format(L"volume read failed at LCN %lld (error %lu)",
                                   run.Lcn, GetLastError());
                    freeQueue.Push(chunk);
                    readError = true;
                    break;
                }
                result.Stats.BytesRead += readLen;
                if (progress)
                    progress->BytesRead.store(result.Stats.BytesRead,
                                              std::memory_order_relaxed);
                size_t feed = (size_t)std::min<uint64_t>(remaining, readLen);
                dispatch(chunk, carryLen + feed);
                done += feed;
            }
        }
        streamed += runBytes;
    }

    readyQueue.Close();
    for (std::thread& w : workers) w.join();
    result.Stats.RecordsParsed = nextRecord;
    return !readError;
}

// Merge the per-worker results back into the result: apply deferred
// extension-record sizes and rebase name offsets into one shared arena.
void MergeContexts(std::vector<ParseContext>& contexts, ScanResult& result) {
    for (ParseContext& ctx : contexts)
        for (const ParseContext::Contribution& c : ctx.Deferred) {
            result.Nodes[c.Target].LogicalSize   += c.Logical;
            result.Nodes[c.Target].AllocatedSize += c.Allocated;
        }

    std::vector<size_t> arenaBase(contexts.size() + 1, 0);
    for (size_t t = 0; t < contexts.size(); ++t)
        arenaBase[t + 1] = arenaBase[t] + contexts[t].NameArena.size();

    result.NameArena.resize(arenaBase.back());
    for (size_t t = 0; t < contexts.size(); ++t)
        std::copy(contexts[t].NameArena.begin(), contexts[t].NameArena.end(),
                  result.NameArena.begin() + arenaBase[t]);

    for (Node& n : result.Nodes)
        if (n.Flags & kNodeNamed)
            n.NameOffset += static_cast<uint32_t>(arenaBase[n.ThreadId]);
}

// ============================================================================
// Orphan handling
// ============================================================================

bool IsValidParent(const std::vector<Node>& nodes, uint32_t parent,
                   uint16_t expectedSeq) {
    if (parent >= nodes.size()) return false;
    const Node& p = nodes[parent];
    // The sequence check rejects "stale" parents: if the directory was deleted
    // and its MFT slot reused for something else, the sequence won't match.
    return (p.Flags & kNodeInUse) && (p.Flags & kNodeIsDir) &&
           p.Sequence == expectedSeq;
}

// Reparent every node with a broken parent link (deleted/reused parent slot,
// corrupt reference, self-reference) under a synthetic "[orphaned]" directory
// at the volume root, so orphaned subtrees stay visible instead of silently
// dropping out of the totals. Also counts reparse points while it has the
// nodes in cache.
void CollectOrphans(ScanResult& r) {
    const size_t realCount = r.Nodes.size(); // bucket appended past this
    uint32_t bucket = UINT32_MAX;

    for (size_t i = 0; i < realCount; ++i) {
        Node& n = r.Nodes[i];
        if (!(n.Flags & kNodeInUse) || !(n.Flags & kNodeNamed)) continue;
        if (n.Flags & kNodeReparse) ++r.Stats.ReparseCount;
        if (i == kRootRecord) continue;
        if (n.Parent != i && IsValidParent(r.Nodes, n.Parent, n.ParentSeq))
            continue;

        if (bucket == UINT32_MAX) {
            Node b;
            b.Flags      = kNodeInUse | kNodeIsDir | kNodeNamed;
            b.Sequence   = 1;
            b.Parent     = (uint32_t)kRootRecord;
            b.ParentSeq  = r.Nodes[kRootRecord].Sequence;
            b.NameOffset = (uint32_t)r.NameArena.size();
            const wchar_t* name = L"[orphaned]";
            b.NameLength = 10;
            r.NameArena.insert(r.NameArena.end(), name, name + b.NameLength);
            r.Nodes.push_back(b);
            bucket = (uint32_t)(r.Nodes.size() - 1);
        }
        n.Parent    = bucket;
        n.ParentSeq = 1; // matches the bucket's Sequence
        ++r.Stats.OrphanCount;
    }
}

// For every file/dir, walk the parent chain to the root, adding its size and
// count into each ancestor. ~O(records * avg depth) — a few tens of millions
// of additions, negligible next to the disk read.
void Aggregate(ScanResult& r) {
    r.Totals.assign(r.Nodes.size(), DirTotals{});
    constexpr int kMaxDepth = 512; // cycle guard for corrupt parent chains
    for (size_t i = 0; i < r.Nodes.size(); ++i) {
        const Node& n = r.Nodes[i];
        if (!(n.Flags & kNodeInUse) || !(n.Flags & kNodeNamed)) continue;

        bool isDir = (n.Flags & kNodeIsDir) != 0;
        if (isDir) {
            // A directory's *own* bytes (its $I30 index clusters, rarely an
            // unnamed $DATA) belong in its cumulative total too; ancestors
            // pick them up through the walk below.
            r.Totals[i].LogicalSize   += n.LogicalSize;
            r.Totals[i].AllocatedSize += n.AllocatedSize;
        }
        if (i == kRootRecord) continue;
        if (isDir) ++r.DirCount; else ++r.FileCount;

        uint32_t cur = n.Parent;
        uint16_t seq = n.ParentSeq;
        for (int depth = 0; depth < kMaxDepth; ++depth) {
            if (!IsValidParent(r.Nodes, cur, seq)) break; // orphan — stop here
            DirTotals& t = r.Totals[cur];
            t.LogicalSize   += n.LogicalSize;
            t.AllocatedSize += n.AllocatedSize;
            if (isDir) ++t.DirCount; else ++t.FileCount;
            const Node& p = r.Nodes[cur];
            if (cur == p.Parent) break;                   // reached the root (5)
            seq = p.ParentSeq;
            cur = p.Parent;
        }
    }
}

void BuildChildIndex(ScanResult& r) {
    size_t n = r.Nodes.size();
    r.Children.Offset.assign(n + 1, 0);

    auto linked = [&](size_t i) {
        const Node& nd = r.Nodes[i];
        return (nd.Flags & kNodeInUse) && (nd.Flags & kNodeNamed) &&
               i != kRootRecord && IsValidParent(r.Nodes, nd.Parent, nd.ParentSeq);
    };

    for (size_t i = 0; i < n; ++i)
        if (linked(i)) ++r.Children.Offset[r.Nodes[i].Parent + 1];
    for (size_t i = 0; i < n; ++i)
        r.Children.Offset[i + 1] += r.Children.Offset[i];

    r.Children.List.resize(r.Children.Offset[n]);
    std::vector<uint32_t> cursor(r.Children.Offset.begin(),
                                 r.Children.Offset.end() - 1);
    for (size_t i = 0; i < n; ++i)
        if (linked(i)) r.Children.List[cursor[r.Nodes[i].Parent]++] = (uint32_t)i;

    for (size_t i = 0; i < n; ++i)
        std::sort(r.Children.List.begin() + r.Children.Offset[i],
                  r.Children.List.begin() + r.Children.Offset[i + 1],
                  [&](uint32_t a, uint32_t b) { return r.SizeOf(a) > r.SizeOf(b); });
}

// ============================================================================
// $ATTRIBUTE_LIST handling for a heavily fragmented $MFT
//
// When the $MFT's own run list grows past what fits in record 0, NTFS moves
// later portions of the $DATA attribute into extension records and leaves a
// $ATTRIBUTE_LIST in record 0 describing where each portion lives. The
// extension records themselves sit in the early part of the MFT, which the
// base run list already maps, so we can resolve and read them.
// ============================================================================

// Maps a byte offset inside the $MFT data stream to a disk offset using the
// (possibly partial) run list collected so far.
bool ResolveStreamOffset(const std::vector<Extent>& runs, uint32_t clusterSize,
                         uint64_t streamOffset, uint64_t& diskOffset) {
    uint64_t vcn = streamOffset / clusterSize;
    uint64_t rem = streamOffset % clusterSize;
    for (const Extent& r : runs)
        if (r.Lcn >= 0 && vcn >= r.Vcn && vcn < r.Vcn + r.Clusters) {
            diskOffset = (uint64_t)(r.Lcn + (vcn - r.Vcn)) * clusterSize + rem;
            return true;
        }
    return false;
}

// Reads one MFT record by number via the partial run map (cluster-aligned
// read). Fixups are applied unless the caller will run the record through
// ParseRecord, which applies them itself — applying fixups twice always
// fails, because the first pass replaces the sector tails with real data.
bool ReadMftRecord(HANDLE volume, const std::vector<Extent>& runs,
                   uint32_t clusterSize, uint32_t recordSize,
                   uint64_t recordNumber, std::vector<uint8_t>& out,
                   bool applyFixups = true) {
    uint64_t diskOffset = 0;
    if (!ResolveStreamOffset(runs, clusterSize, recordNumber * (uint64_t)recordSize,
                             diskOffset))
        return false;
    uint64_t alignedStart = diskOffset - diskOffset % clusterSize;
    uint32_t readLen = (uint32_t)(((diskOffset - alignedStart + recordSize +
                                    clusterSize - 1) / clusterSize) * clusterSize);
    std::vector<uint8_t> buf(readLen);
    if (!ReadAt(volume, alignedStart, buf.data(), readLen)) return false;
    out.assign(buf.begin() + (diskOffset - alignedStart),
               buf.begin() + (diskOffset - alignedStart) + recordSize);
    auto* hdr = reinterpret_cast<FileRecordHeader*>(out.data());
    if (hdr->Magic != kFileRecordMagic) return false;
    return !applyFixups || ApplyFixups(out.data(), recordSize);
}

// Extends `runs` with the $DATA portions held in extension records named by
// record 0's $ATTRIBUTE_LIST. Returns false (with `error`) if anything along
// the way is unreadable — the caller then reports the coverage shortfall.
bool ExtendRunsFromAttributeList(HANDLE volume, const uint8_t* rec0,
                                 uint32_t recordSize, uint32_t clusterSize,
                                 std::vector<Extent>& runs, std::wstring& error) {
    auto* hdr = reinterpret_cast<const FileRecordHeader*>(rec0);
    uint32_t limit  = std::min(hdr->UsedSize, recordSize);
    uint32_t offset = hdr->FirstAttributeOffset;

    // 1. Find $ATTRIBUTE_LIST and materialize its value bytes.
    std::vector<uint8_t> list;
    while (offset + sizeof(AttributeHeader) <= limit) {
        auto* attr = reinterpret_cast<const AttributeHeader*>(rec0 + offset);
        if (attr->Type == kAttrEnd || attr->Length == 0) break;
        if (attr->Type == kAttrAttributeList) {
            if (!attr->NonResident) {
                auto* res = reinterpret_cast<const ResidentAttribute*>(attr);
                if (res->ValueOffset + res->ValueLength <= attr->Length)
                    list.assign(rec0 + offset + res->ValueOffset,
                                rec0 + offset + res->ValueOffset + res->ValueLength);
            } else {
                // The list itself is non-resident: read its clusters.
                auto* nr = reinterpret_cast<const NonResidentAttribute*>(attr);
                auto listRuns = DecodeRunList(rec0 + offset + nr->RunListOffset,
                                              rec0 + offset + attr->Length,
                                              nr->LowestVcn);
                uint64_t want = nr->RealSize;
                for (const Extent& r : listRuns) {
                    if (r.Lcn < 0 || list.size() >= want) continue;
                    uint64_t bytes = std::min(r.Clusters * (uint64_t)clusterSize,
                                              want - list.size());
                    uint64_t readLen = (bytes + clusterSize - 1) / clusterSize *
                                       (uint64_t)clusterSize;
                    std::vector<uint8_t> chunk((size_t)readLen);
                    if (!ReadAt(volume, (uint64_t)r.Lcn * clusterSize,
                                chunk.data(), (uint32_t)readLen)) {
                        error = L"cannot read the non-resident $ATTRIBUTE_LIST";
                        return false;
                    }
                    list.insert(list.end(), chunk.begin(),
                                chunk.begin() + (size_t)bytes);
                }
            }
            break;
        }
        offset += attr->Length;
    }
    if (list.empty()) {
        error = L"$MFT run list is incomplete and record 0 has no readable "
                L"$ATTRIBUTE_LIST";
        return false;
    }

    // 2. Collect the extension records that hold $DATA portions.
    std::vector<uint64_t> extensionRecords;
    for (size_t pos = 0; pos + sizeof(AttributeListEntry) <= list.size(); ) {
        auto* entry = reinterpret_cast<const AttributeListEntry*>(list.data() + pos);
        if (entry->Length < sizeof(AttributeListEntry)) break;
        if (entry->Type == kAttrData && entry->NameLength == 0) {
            uint64_t recno = entry->FileRef & 0x0000FFFFFFFFFFFFull;
            if (recno != 0 && // record 0's own portion is already decoded
                std::find(extensionRecords.begin(), extensionRecords.end(),
                          recno) == extensionRecords.end())
                extensionRecords.push_back(recno);
        }
        pos += entry->Length;
    }

    // 3. Read each extension record and append its $DATA run-list portion.
    for (uint64_t recno : extensionRecords) {
        std::vector<uint8_t> rec;
        if (!ReadMftRecord(volume, runs, clusterSize, recordSize, recno, rec)) {
            error = Format(L"cannot read $MFT extension record %llu", recno);
            return false;
        }
        auto* ehdr = reinterpret_cast<FileRecordHeader*>(rec.data());
        uint32_t elimit  = std::min(ehdr->UsedSize, recordSize);
        uint32_t eoffset = ehdr->FirstAttributeOffset;
        while (eoffset + sizeof(AttributeHeader) <= elimit) {
            auto* attr = reinterpret_cast<AttributeHeader*>(rec.data() + eoffset);
            if (attr->Type == kAttrEnd || attr->Length == 0) break;
            if (attr->Type == kAttrData && attr->NameLength == 0 &&
                attr->NonResident) {
                auto* nr = reinterpret_cast<NonResidentAttribute*>(attr);
                auto more = DecodeRunList(rec.data() + eoffset + nr->RunListOffset,
                                          rec.data() + eoffset + attr->Length,
                                          nr->LowestVcn);
                runs.insert(runs.end(), more.begin(), more.end());
            }
            eoffset += attr->Length;
        }
    }
    std::sort(runs.begin(), runs.end(),
              [](const Extent& a, const Extent& b) { return a.Vcn < b.Vcn; });
    return true;
}

// ============================================================================
// Filesystem walk (shared by the fallback scan and per-folder rescans)
//
// Multi-threaded FindFirstFileExW enumeration rooted at `rootPath` (with the
// \\?\ long-path prefix), appending nodes under `rootNode`. Inaccessible
// directories are skipped; reparse-point directories are never descended into
// (junction cycles). Allocated sizes are approximated by cluster rounding.
// ============================================================================

void WalkTree(const std::wstring& rootPath, uint32_t rootNode, unsigned threads,
              ScanResult& out, ScanProgress* progress) {
    // Directory walks are latency-bound, so more threads than cores helps.
    if (threads == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        threads = hw ? std::min(hw * 2, 16u) : 4u;
    }
    const uint64_t clusterSize =
        out.Stats.ClusterSize ? out.Stats.ClusterSize : 4096;

    // Work queue of (directory path, its node index). `pending` counts
    // queued + in-flight directories; 0 means finished.
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::pair<std::wstring, uint32_t>> work;
    size_t pending = 1;
    work.emplace_back(rootPath, rootNode);

    struct Entry {
        std::wstring Name;
        uint64_t Size;
        bool IsDir;
        bool IsReparse;
    };

    auto worker = [&] {
        std::vector<Entry> entries;
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return !work.empty() || pending == 0; });
            if (work.empty()) return; // pending == 0: all done
            auto [dirPath, dirNode] = std::move(work.front());
            work.pop_front();
            lock.unlock();

            // Enumerate outside the lock — this is where the time goes.
            entries.clear();
            WIN32_FIND_DATAW fd;
            HANDLE find = FindFirstFileExW((dirPath + L"\\*").c_str(),
                                           FindExInfoBasic, &fd,
                                           FindExSearchNameMatch, nullptr,
                                           FIND_FIRST_EX_LARGE_FETCH);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.cFileName[0] == L'.' &&
                        (fd.cFileName[1] == L'\0' ||
                         (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                        continue;
                    entries.push_back(
                        {fd.cFileName,
                         ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                         (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                         (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
                } while (FindNextFileW(find, &fd));
                FindClose(find);
            }
            // else: access denied etc. — skip silently, like every walker must

            lock.lock();
            for (const Entry& e : entries) {
                uint32_t idx = (uint32_t)out.Nodes.size();
                Node n;
                n.Flags      = kNodeInUse | kNodeNamed;
                n.Sequence   = 1;
                n.Parent     = dirNode;
                n.ParentSeq  = out.Nodes[dirNode].Sequence;
                n.NameOffset = (uint32_t)out.NameArena.size();
                n.NameLength = (uint16_t)e.Name.size();
                if (e.IsReparse) n.Flags |= kNodeReparse;
                if (e.IsDir) {
                    n.Flags |= kNodeIsDir;
                } else {
                    n.LogicalSize   = e.Size;
                    n.AllocatedSize = (e.Size + clusterSize - 1)
                                      / clusterSize * clusterSize;
                }
                out.NameArena.insert(out.NameArena.end(),
                                     e.Name.begin(), e.Name.end());
                out.Nodes.push_back(n);
                // Never descend into reparse points: junction cycles would
                // loop forever and their targets are counted where they live.
                if (e.IsDir && !e.IsReparse) {
                    work.emplace_back(dirPath + L"\\" + e.Name, idx);
                    ++pending;
                }
            }
            --pending; // this directory is done
            if (progress)
                progress->BytesRead.fetch_add(entries.size(),
                                              std::memory_order_relaxed);
            bool finished = pending == 0 && work.empty();
            lock.unlock();
            if (finished) cv.notify_all();
            else cv.notify_one();
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
}

} // namespace

// ============================================================================
// ScanVolume — the public entry point
// ============================================================================

bool ScanVolume(const std::wstring& driveIn, unsigned threads, ScanResult& out,
                std::wstring& error, ScanProgress* progress) {
    auto t0 = std::chrono::steady_clock::now();
    out = ScanResult{};
    out.Drive = driveIn;
    if (out.Drive.size() == 1) out.Drive += L':';
    if (out.Drive.size() > 2 && out.Drive.back() == L'\\') out.Drive.pop_back();
    out.Stats.ScanUnixTime = (uint64_t)_time64(nullptr);

    if (threads == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        threads = hw > 3 ? std::min(hw - 2, 8u) : 1u; // leave cores for I/O + OS
    }
    threads = std::min(threads, 255u); // Node::ThreadId is a uint8_t
    out.Stats.Threads = threads;

    // ---- 1. Open the raw volume ------------------------------------------
    std::wstring volumePath = L"\\\\.\\" + out.Drive;
    HANDLE volume = CreateFileW(volumePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        error = Format(L"cannot open %ls (error %lu)%ls", volumePath.c_str(), err,
                       err == ERROR_ACCESS_DENIED
                           ? L" — raw volume access requires Administrator rights"
                           : L"");
        return false;
    }
    // Ensure the handle is closed on every return path below.
    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() { CloseHandle(h); }
    } closer{volume};

    // ---- 2. Where is the MFT? --------------------------------------------
    NTFS_VOLUME_DATA_BUFFER vol{};
    DWORD bytes = 0;
    if (!DeviceIoControl(volume, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                         &vol, sizeof(vol), &bytes, nullptr)) {
        error = Format(L"FSCTL_GET_NTFS_VOLUME_DATA failed (error %lu) — "
                       L"is %ls an NTFS volume?", GetLastError(), out.Drive.c_str());
        return false;
    }
    const uint32_t clusterSize = vol.BytesPerCluster;
    const uint32_t recordSize  = vol.BytesPerFileRecordSegment;
    const uint64_t mftBytes    = vol.MftValidDataLength.QuadPart;
    out.Stats.ClusterSize = clusterSize;
    out.Stats.RecordSize  = recordSize;
    out.Stats.MftBytes    = mftBytes;
    if (progress) {
        progress->TotalBytes.store(mftBytes, std::memory_order_relaxed);
        progress->Stage.store(ScanProgress::Reading, std::memory_order_relaxed);
    }

    // ---- 3. Read $MFT's own record (record 0) and decode its run list ----
    // FSCTL only tells us where the MFT *starts*; the MFT itself is usually
    // fragmented, and the authoritative extent map is the run list of its own
    // unnamed $DATA attribute, stored in record 0 at MftStartLcn.
    uint32_t firstReadSize =
        ((recordSize + clusterSize - 1) / clusterSize) * clusterSize;
    std::vector<uint8_t> rec0(firstReadSize);
    if (!ReadAt(volume, vol.MftStartLcn.QuadPart * (uint64_t)clusterSize,
                rec0.data(), firstReadSize)) {
        error = Format(L"cannot read MFT record 0 (error %lu)", GetLastError());
        return false;
    }
    auto* hdr0 = reinterpret_cast<FileRecordHeader*>(rec0.data());
    if (hdr0->Magic != kFileRecordMagic || !ApplyFixups(rec0.data(), recordSize)) {
        error = L"MFT record 0 is not a valid FILE record";
        return false;
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
        error = L"could not decode the $MFT run list "
                L"(possibly an $ATTRIBUTE_LIST overflow — not handled yet)";
        return false;
    }
    {
        auto mappedBytes = [&] {
            uint64_t clusters = 0;
            for (auto& r : mftRuns) clusters += r.Clusters;
            return clusters * clusterSize;
        };
        // A heavily fragmented $MFT spills later run-list portions into
        // extension records via $ATTRIBUTE_LIST; stitch them in.
        if (mappedBytes() < mftBytes &&
            !ExtendRunsFromAttributeList(volume, rec0.data(), recordSize,
                                         clusterSize, mftRuns, error))
            return false;
        if (mappedBytes() < mftBytes) {
            error = Format(L"$MFT run list covers only %llu of %llu bytes even "
                           L"after $ATTRIBUTE_LIST processing",
                           mappedBytes(), mftBytes);
            return false;
        }
    }

    // ---- 4 & 5. Stream the MFT and parse every record in parallel ---------
    out.MftMap = mftRuns; // retained so USN live updates can re-read records
    out.Nodes.assign(mftBytes / recordSize, Node{});
    std::vector<ParseContext> contexts;
    if (!StreamMft(volume, mftRuns, mftBytes, clusterSize, recordSize,
                   threads, contexts, out, progress, error))
        return false;
    MergeContexts(contexts, out);
    contexts.clear();

    auto t1 = std::chrono::steady_clock::now();
    out.Stats.StreamSeconds =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1e6;
    if (progress)
        progress->Stage.store(ScanProgress::Aggregating, std::memory_order_relaxed);

    // ---- 6. Reparent orphans, aggregate sizes, build the child index ------
    CollectOrphans(out);
    Aggregate(out);
    BuildChildIndex(out);

    auto t2 = std::chrono::steady_clock::now();
    out.Stats.TotalSeconds =
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count() / 1e6;
    if (progress)
        progress->Stage.store(ScanProgress::Done, std::memory_order_relaxed);
    return true;
}

// ============================================================================
// ScanVolumeFallback — multi-threaded FindFirstFileExW directory walk
//
// Used when raw volume access is impossible (no admin rights, non-NTFS
// filesystem). Builds the exact same ScanResult shape as the MFT path: the
// root sits at index kRootRecord (indices 0-4 are unused placeholders), all
// sequence numbers are 1, and Aggregate/BuildChildIndex run unchanged.
// ============================================================================

bool ScanVolumeFallback(const std::wstring& driveIn, unsigned threads,
                        ScanResult& out, std::wstring& error,
                        ScanProgress* progress) {
    auto t0 = std::chrono::steady_clock::now();
    out = ScanResult{};
    out.Drive = driveIn;
    if (out.Drive.size() == 1) out.Drive += L':';
    if (out.Drive.size() > 2 && out.Drive.back() == L'\\') out.Drive.pop_back();
    out.Stats.UsedFallback = true;
    out.Stats.ScanUnixTime = (uint64_t)_time64(nullptr);

    std::wstring rootDir = out.Drive + L"\\";
    if (GetFileAttributesW(rootDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = Format(L"cannot access %ls (error %lu)", rootDir.c_str(),
                       GetLastError());
        return false;
    }

    // Cluster size, for approximating "size on disk" (FindFirstFile reports
    // only logical sizes; compressed/sparse files will be overestimated).
    DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeC = 0, totalC = 0;
    GetDiskFreeSpaceW(rootDir.c_str(), &sectorsPerCluster, &bytesPerSector,
                      &freeC, &totalC);
    const uint64_t clusterSize =
        sectorsPerCluster && bytesPerSector
            ? (uint64_t)sectorsPerCluster * bytesPerSector : 4096;
    out.Stats.ClusterSize = (uint32_t)clusterSize;

    // Directory walks are latency-bound, so more threads than cores helps.
    if (threads == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        threads = hw ? std::min(hw * 2, 16u) : 4u;
    }
    out.Stats.Threads = threads;
    if (progress)
        progress->Stage.store(ScanProgress::Reading, std::memory_order_relaxed);

    // Root node at the index everything downstream expects.
    out.Nodes.resize(kRootRecord + 1);
    {
        Node& root = out.Nodes[kRootRecord];
        root.Flags      = kNodeInUse | kNodeIsDir | kNodeNamed;
        root.Sequence   = 1;
        root.Parent     = (uint32_t)kRootRecord;
        root.ParentSeq  = 1;
        root.NameOffset = 0;
        root.NameLength = 1;
        out.NameArena.push_back(L'.');
    }

    WalkTree(L"\\\\?\\" + out.Drive, (uint32_t)kRootRecord, threads, out,
             progress);

    out.Stats.RecordsParsed = out.Nodes.size() - (kRootRecord + 1);
    auto t1 = std::chrono::steady_clock::now();
    out.Stats.StreamSeconds =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1e6;
    if (progress)
        progress->Stage.store(ScanProgress::Aggregating, std::memory_order_relaxed);

    Aggregate(out);
    BuildChildIndex(out);

    auto t2 = std::chrono::steady_clock::now();
    out.Stats.TotalSeconds =
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count() / 1e6;
    if (progress)
        progress->Stage.store(ScanProgress::Done, std::memory_order_relaxed);
    return true;
}

bool ScanVolumeAuto(const std::wstring& drive, unsigned threads,
                    ScanResult& out, std::wstring& error,
                    ScanProgress* progress) {
    std::wstring mftError;
    if (ScanVolume(drive, threads, out, mftError, progress)) return true;

    if (progress) { // reset for the second attempt
        progress->BytesRead  = 0;
        progress->TotalBytes = 0;
        progress->Stage      = ScanProgress::Opening;
    }
    std::wstring walkError;
    if (ScanVolumeFallback(drive, threads, out, walkError, progress)) {
        out.FallbackReason = mftError;
        return true;
    }
    error = mftError + L"; directory-walk fallback also failed: " + walkError;
    return false;
}

// ============================================================================
// In-place updates: Reindex + RescanSubtree
// ============================================================================

void Reindex(ScanResult& r) {
    r.FileCount = 0;
    r.DirCount  = 0;
    Aggregate(r);       // re-assigns Totals from scratch
    BuildChildIndex(r);
}

bool RescanSubtree(ScanResult& r, uint32_t dir, std::wstring& error,
                   ScanProgress* progress) {
    if (dir >= r.Nodes.size() || !r.Exists(dir) || !r.IsDir(dir)) {
        error = L"not a scannable directory";
        return false;
    }
    if (r.Nodes[dir].Flags & kNodeReparse) {
        error = L"cannot rescan a reparse point (its contents live elsewhere)";
        return false;
    }
    std::wstring path = L"\\\\?\\" + r.Path(dir);
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        error = Format(L"cannot access %ls (error %lu)", r.Path(dir).c_str(),
                       GetLastError());
        return false;
    }

    // 1. Drop every current descendant (the dir node itself stays — it may be
    //    a real MFT record other nodes reference).
    std::vector<uint32_t> stack{dir};
    while (!stack.empty()) {
        uint32_t n = stack.back();
        stack.pop_back();
        for (uint32_t c = r.Children.Offset[n]; c < r.Children.Offset[n + 1]; ++c) {
            uint32_t child = r.Children.List[c];
            r.Nodes[child].Flags = 0;
            stack.push_back(child);
        }
    }
    // The walk can't see $I30 index sizes, so the dir's own allocated bytes
    // from an earlier MFT scan are kept as-is (they're still roughly right).

    // 2. Enumerate the subtree fresh; new nodes append past the old ones.
    if (progress) {
        progress->BytesRead  = 0;
        progress->TotalBytes = 0;
        progress->Stage      = ScanProgress::Reading;
    }
    if (!r.Stats.ClusterSize) r.Stats.ClusterSize = 4096;
    WalkTree(path, dir, 0, r, progress);

    // 3. Rebuild totals and the child index from the merged node set.
    if (progress)
        progress->Stage.store(ScanProgress::Aggregating, std::memory_order_relaxed);
    Reindex(r);
    if (progress)
        progress->Stage.store(ScanProgress::Done, std::memory_order_relaxed);
    return true;
}

// ============================================================================
// Snapshots
//
// Format ("YADUASNP", version 1, little-endian):
//   char[8] magic, u32 version, u64 scanUnixTime,
//   u32 driveChars, wchar drive[],
//   u64 nodeCount, then per node (parents always precede children):
//     u32 parentDense, u8 flags (dir|reparse), u16 nameChars,
//     u64 logicalSize, u64 allocatedSize, wchar name[]
// Dense index 0 is the root; the loader places it back at kRootRecord and
// recomputes totals/child index with Reindex, so a loaded snapshot behaves
// exactly like a live scan result.
// ============================================================================

namespace {

constexpr char     kSnapMagic[8]  = {'Y','A','D','U','A','S','N','P'};
constexpr uint32_t kSnapVersion   = 1;

#pragma pack(push, 1)
struct SnapNode {
    uint32_t Parent;     // dense index of the parent
    uint8_t  Flags;      // kNodeIsDir | kNodeReparse
    uint16_t NameChars;
    uint64_t Logical;
    uint64_t Allocated;
};
#pragma pack(pop)

} // namespace

bool SaveSnapshot(const ScanResult& r, const std::wstring& file,
                  std::wstring& error) {
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) {
        error = Format(L"cannot write %ls", file.c_str());
        return false;
    }
    setvbuf(f, nullptr, _IOFBF, 1 << 20);

    // BFS over the child index so every node's parent precedes it.
    std::vector<uint32_t> order;
    std::vector<uint32_t> dense(r.Nodes.size(), UINT32_MAX);
    order.reserve((size_t)(r.FileCount + r.DirCount + 1));
    order.push_back((uint32_t)kRootRecord);
    dense[kRootRecord] = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        uint32_t n = order[i];
        for (uint32_t c = r.Children.Offset[n]; c < r.Children.Offset[n + 1]; ++c) {
            uint32_t child = r.Children.List[c];
            if (!r.Exists(child)) continue;
            dense[child] = (uint32_t)order.size();
            order.push_back(child);
        }
    }

    fwrite(kSnapMagic, 1, 8, f);
    fwrite(&kSnapVersion, 4, 1, f);
    fwrite(&r.Stats.ScanUnixTime, 8, 1, f);
    uint32_t driveChars = (uint32_t)r.Drive.size();
    fwrite(&driveChars, 4, 1, f);
    fwrite(r.Drive.data(), sizeof(wchar_t), driveChars, f);
    uint64_t count = order.size();
    fwrite(&count, 8, 1, f);

    for (uint32_t n : order) {
        const Node& node = r.Nodes[n];
        SnapNode s{n == (uint32_t)kRootRecord ? 0u : dense[node.Parent],
                   (uint8_t)(node.Flags & (kNodeIsDir | kNodeReparse)),
                   node.NameLength, node.LogicalSize, node.AllocatedSize};
        fwrite(&s, sizeof(s), 1, f);
        fwrite(r.NameArena.data() + node.NameOffset, sizeof(wchar_t),
               node.NameLength, f);
    }

    bool ok = !ferror(f);
    fclose(f);
    if (!ok) error = Format(L"write error on %ls", file.c_str());
    return ok;
}

bool LoadSnapshot(const std::wstring& file, ScanResult& out,
                  std::wstring& error) {
    out = ScanResult{};
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) {
        error = Format(L"cannot open %ls", file.c_str());
        return false;
    }
    setvbuf(f, nullptr, _IOFBF, 1 << 20);
    auto fail = [&](const wchar_t* what) {
        fclose(f);
        error = Format(L"%ls is not a valid snapshot (%ls)", file.c_str(), what);
        return false;
    };

    char magic[8];
    uint32_t version = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, kSnapMagic, 8) != 0)
        return fail(L"bad magic");
    if (fread(&version, 4, 1, f) != 1 || version != kSnapVersion)
        return fail(L"unsupported version");
    if (fread(&out.Stats.ScanUnixTime, 8, 1, f) != 1) return fail(L"truncated");
    uint32_t driveChars = 0;
    if (fread(&driveChars, 4, 1, f) != 1 || driveChars > 64)
        return fail(L"bad drive");
    out.Drive.resize(driveChars);
    if (fread(out.Drive.data(), sizeof(wchar_t), driveChars, f) != driveChars)
        return fail(L"truncated");
    uint64_t count = 0;
    if (fread(&count, 8, 1, f) != 1 || count == 0 || count > 0xFFFFFFFFull)
        return fail(L"bad node count");

    // Dense index d maps to node index kRootRecord (d == 0) or kRootRecord + d.
    auto toIndex = [](uint32_t d) {
        return d == 0 ? (uint32_t)kRootRecord : (uint32_t)kRootRecord + d;
    };
    out.Nodes.assign((size_t)kRootRecord + count, Node{});
    for (uint64_t d = 0; d < count; ++d) {
        SnapNode s;
        if (fread(&s, sizeof(s), 1, f) != 1) return fail(L"truncated node");
        if (s.NameChars > 4096) return fail(L"bad name length");
        if (d > 0 && (s.Parent >= d)) return fail(L"bad parent order");

        Node& n = out.Nodes[toIndex((uint32_t)d)];
        n.Flags      = (uint8_t)(s.Flags | kNodeInUse | kNodeNamed);
        n.Sequence   = 1;
        n.Parent     = d == 0 ? (uint32_t)kRootRecord : toIndex(s.Parent);
        n.ParentSeq  = 1;
        n.LogicalSize   = s.Logical;
        n.AllocatedSize = s.Allocated;
        n.NameOffset = (uint32_t)out.NameArena.size();
        n.NameLength = s.NameChars;
        size_t old = out.NameArena.size();
        out.NameArena.resize(old + s.NameChars);
        if (s.NameChars &&
            fread(out.NameArena.data() + old, sizeof(wchar_t), s.NameChars, f)
                != s.NameChars)
            return fail(L"truncated name");
    }
    fclose(f);
    out.Nodes[kRootRecord].Flags |= kNodeIsDir; // the root is always a dir
    Reindex(out);
    return true;
}

// ============================================================================
// USN journal monitoring + incremental updates
// ============================================================================

bool UsnMonitor::Start(const std::wstring& drive, std::wstring& error) {
    Stop();
    std::wstring volumePath = L"\\\\.\\" + drive;
    HANDLE volume = CreateFileW(volumePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        error = Format(L"cannot open %ls for journal monitoring (error %lu)",
                       volumePath.c_str(), GetLastError());
        return false;
    }
    USN_JOURNAL_DATA_V0 journal{};
    DWORD bytes = 0;
    if (!DeviceIoControl(volume, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                         &journal, sizeof(journal), &bytes, nullptr)) {
        error = Format(L"volume %ls has no readable USN journal (error %lu)",
                       drive.c_str(), GetLastError());
        CloseHandle(volume);
        return false;
    }
    volume_    = volume;
    journalId_ = journal.UsnJournalID;
    nextUsn_   = journal.NextUsn; // only changes from this point onward
    stop_      = false;
    lost_      = false;
    pending_   = 0;
    dirty_.clear();
    thread_ = std::thread(&UsnMonitor::Loop, this);
    active_ = true;
    return true;
}

void UsnMonitor::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (volume_) {
        CloseHandle((HANDLE)volume_);
        volume_ = nullptr;
    }
    active_ = false;
}

std::vector<uint64_t> UsnMonitor::TakeDirty() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint64_t> out(dirty_.begin(), dirty_.end());
    dirty_.clear();
    pending_ = 0;
    return out;
}

void UsnMonitor::Loop() {
    std::vector<uint8_t> buffer(256 * 1024);
    while (!stop_) {
        READ_USN_JOURNAL_DATA_V0 request{};
        request.StartUsn          = nextUsn_;
        request.ReasonMask        = 0xFFFFFFFF;
        request.UsnJournalID      = journalId_;
        // BytesToWaitFor == 0: return immediately with whatever is available.
        DWORD bytes = 0;
        if (!DeviceIoControl((HANDLE)volume_, FSCTL_READ_USN_JOURNAL,
                             &request, sizeof(request), buffer.data(),
                             (DWORD)buffer.size(), &bytes, nullptr)) {
            // Journal wrapped past our cursor, was deleted, or the volume
            // went away: incremental tracking is no longer sound.
            lost_ = true;
            return;
        }
        if (bytes >= sizeof(USN)) {
            nextUsn_ = *reinterpret_cast<USN*>(buffer.data());
            size_t offset = sizeof(USN);
            std::lock_guard<std::mutex> lock(mutex_);
            while (offset + sizeof(USN_RECORD_V2) <= bytes) {
                auto* rec = reinterpret_cast<USN_RECORD_V2*>(buffer.data() + offset);
                if (rec->RecordLength < sizeof(USN_RECORD_V2) ||
                    offset + rec->RecordLength > bytes)
                    break;
                if (rec->MajorVersion == 2) {
                    dirty_.insert(rec->FileReferenceNumber);
                } else if (rec->MajorVersion == 3) {
                    // V3 uses 128-bit ids; NTFS only populates the low 64.
                    auto* v3 = reinterpret_cast<USN_RECORD_V3*>(rec);
                    uint64_t low;
                    memcpy(&low, v3->FileReferenceNumber.Identifier, 8);
                    dirty_.insert(low);
                }
                offset += rec->RecordLength;
            }
            pending_ = dirty_.size();
        }
        // Nothing more to read right now (or a tiny batch): poll politely.
        for (int i = 0; i < 10 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool ApplyMftUpdates(ScanResult& r, const std::vector<uint64_t>& fileRefs,
                     std::wstring& error, unsigned* unresolved) {
    if (unresolved) *unresolved = 0;
    if (r.MftMap.empty() || !r.Stats.RecordSize) {
        error = L"this result has no MFT map (fallback scan or snapshot)";
        return false;
    }
    if (fileRefs.empty()) return true;

    std::wstring volumePath = L"\\\\.\\" + r.Drive;
    HANDLE volume = CreateFileW(volumePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        error = Format(L"cannot open %ls (error %lu)", volumePath.c_str(),
                       GetLastError());
        return false;
    }

    ParseContext ctx;
    ctx.Nodes = &r.Nodes;
    std::vector<uint32_t> updated;
    updated.reserve(fileRefs.size());

    for (uint64_t ref : fileRefs) {
        uint64_t recno = ref & 0x0000FFFFFFFFFFFFull;
        if (recno >= 0xFFFFFFFFull) continue;
        // New files may land in records past the original scan; make room.
        if (recno >= r.Nodes.size()) {
            // Past the mapped MFT extents: ReadMftRecord below will fail and
            // count as unresolved, but in-range growth is fine.
            r.Nodes.resize(recno + 1);
        }
        r.Nodes[(size_t)recno] = Node{}; // deleted unless re-read succeeds

        std::vector<uint8_t> rec;
        if (!ReadMftRecord(volume, r.MftMap, r.Stats.ClusterSize,
                           r.Stats.RecordSize, recno, rec,
                           /*applyFixups=*/false)) { // ParseRecord fixes up
            if (unresolved) ++*unresolved;
            continue;
        }
        // ParseRecord applies fixups and checks in-use (a deleted file
        // simply leaves the node zeroed).
        ParseRecord(rec.data(), r.Stats.RecordSize, recno, ctx);
        if (r.Nodes[(size_t)recno].Flags & kNodeNamed)
            updated.push_back((uint32_t)recno);
    }
    CloseHandle(volume);

    // Names were appended to the context arena; merge and rebase.
    uint32_t base = (uint32_t)r.NameArena.size();
    r.NameArena.insert(r.NameArena.end(), ctx.NameArena.begin(),
                       ctx.NameArena.end());
    for (uint32_t recno : updated) r.Nodes[recno].NameOffset += base;
    // Extension-record contributions (large fragmented files).
    for (const ParseContext::Contribution& c : ctx.Deferred) {
        r.Nodes[c.Target].LogicalSize   += c.Logical;
        r.Nodes[c.Target].AllocatedSize += c.Allocated;
    }

    Reindex(r);
    r.Stats.ScanUnixTime = (uint64_t)_time64(nullptr);
    return true;
}

} // namespace yadua
