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
struct Extent { uint64_t Vcn; int64_t Lcn; uint64_t Clusters; }; // Lcn < 0 => sparse

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
// Aggregation
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

// For every file/dir, walk the parent chain to the root, adding its size and
// count into each ancestor. ~O(records * avg depth) — a few tens of millions
// of additions, negligible next to the disk read.
void Aggregate(ScanResult& r) {
    r.Totals.assign(r.Nodes.size(), DirTotals{});
    constexpr int kMaxDepth = 512; // cycle guard for corrupt parent chains
    for (size_t i = 0; i < r.Nodes.size(); ++i) {
        const Node& n = r.Nodes[i];
        if (!(n.Flags & kNodeInUse) || !(n.Flags & kNodeNamed)) continue;
        if (i == kRootRecord) continue;

        bool isDir = (n.Flags & kNodeIsDir) != 0;
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
        uint64_t mapped = 0;
        for (auto& r : mftRuns) mapped += r.Clusters;
        if (mapped * clusterSize < mftBytes) {
            error = L"the $MFT run list continues in an extension record "
                    L"($ATTRIBUTE_LIST overflow — not handled yet)";
            return false;
        }
    }

    // ---- 4 & 5. Stream the MFT and parse every record in parallel ---------
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

    // ---- 6. Aggregate folder sizes and build the child index --------------
    Aggregate(out);
    BuildChildIndex(out);

    auto t2 = std::chrono::steady_clock::now();
    out.Stats.TotalSeconds =
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count() / 1e6;
    if (progress)
        progress->Stage.store(ScanProgress::Done, std::memory_order_relaxed);
    return true;
}

} // namespace yadua
