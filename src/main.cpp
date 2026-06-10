// ============================================================================
// YADUA - Yet Another Disk Usage Analyzer (proof of concept)
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
//   4. Stream the whole MFT with large sequential reads (~4 MB chunks).
//   5. Parse each 1 KB FILE record: name + parent ref ($FILE_NAME attribute),
//      size ($DATA attribute), in-use / directory flags.
//   6. Rebuild the tree in memory by following parent references and
//      aggregate folder sizes bottom-up.
//
// The MFT for a volume with ~2M files is ~2 GB at most (usually far less),
// and we read it sequentially — so the whole scan is bounded by sequential
// disk throughput, not by per-file syscalls. This is the same trick WizTree
// and ntfs-3g's tooling use.
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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

// One entry per MFT record. Kept deliberately small (40 bytes) because a big
// volume has millions of records; names live in a shared UTF-16 arena.
struct Node {
    uint64_t LogicalSize  = 0;  // file size (EOF) of the unnamed $DATA stream
    uint64_t AllocatedSize = 0; // bytes of disk actually reserved
    uint32_t NameOffset   = 0;  // into g_nameArena
    uint32_t Parent       = 0;  // MFT record number of parent directory
    uint16_t ParentSeq    = 0;  // expected sequence number of the parent record
    uint16_t Sequence     = 0;  // this record's own sequence number
    uint16_t NameLength   = 0;  // in WCHARs
    uint8_t  Flags        = 0;  // bit0 in-use, bit1 directory, bit2 has-name
    uint8_t  NameRank     = 0xFF; // namespace preference of the stored name
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

// Parse one (fixup-applied) MFT record and fold its information into g_nodes.
static void ParseRecord(uint8_t* rec, uint32_t recordSize, uint64_t recordIndex) {
    auto* hdr = reinterpret_cast<FileRecordHeader*>(rec);
    if (hdr->Magic != kFileRecordMagic) return;       // unused/corrupt slot
    if (!(hdr->Flags & kRecordInUse)) return;         // deleted file
    if (!ApplyFixups(rec, recordSize)) return;

    // Attributes of very large / very fragmented files overflow into
    // "extension" records whose BaseRecord points at the owner. Their $DATA
    // headers belong to the base file, so fold sizes into the base node.
    uint64_t targetIndex = recordIndex;
    bool isExtension = false;
    if (hdr->BaseRecord != 0) {
        targetIndex = hdr->BaseRecord & 0x0000FFFFFFFFFFFFull;
        if (targetIndex >= g_nodes.size()) return;
        isExtension = true;
    }
    Node& node = g_nodes[targetIndex];
    if (!isExtension) {
        node.Flags   |= kNodeInUse;
        node.Sequence = hdr->SequenceNumber;
        if (hdr->Flags & kRecordIsDirectory) node.Flags |= kNodeIsDir;
    }

    // Walk the attribute list. Every offset comes from disk, so bounds-check
    // everything — a single bad record must not crash the scan.
    uint32_t limit  = std::min(hdr->UsedSize, recordSize);
    uint32_t offset = hdr->FirstAttributeOffset;
    while (offset + sizeof(AttributeHeader) <= limit) {
        auto* attr = reinterpret_cast<AttributeHeader*>(rec + offset);
        if (attr->Type == kAttrEnd) break;
        if (attr->Length < sizeof(AttributeHeader) || offset + attr->Length > limit)
            break;

        if (attr->Type == kAttrFileName && !attr->NonResident && !isExtension) {
            auto* res = reinterpret_cast<ResidentAttribute*>(attr);
            if (res->ValueOffset + sizeof(FileNameAttribute) <= attr->Length) {
                auto* fn = reinterpret_cast<FileNameAttribute*>(
                    rec + offset + res->ValueOffset);
                uint32_t nameBytes = fn->NameLength * sizeof(wchar_t);
                uint8_t  rank      = NamespaceRank(fn->NameSpace);
                // Keep the best-ranked name. A file with several Win32 names
                // is a hard link; we keep the first one we see (counted once).
                if (rank < node.NameRank &&
                    res->ValueOffset + sizeof(FileNameAttribute) + nameBytes
                        <= attr->Length) {
                    node.NameRank   = rank;
                    node.Parent     = static_cast<uint32_t>(
                        fn->ParentRef & 0x0000FFFFFFFFFFFFull);
                    node.ParentSeq  = static_cast<uint16_t>(fn->ParentRef >> 48);
                    node.NameOffset = static_cast<uint32_t>(g_nameArena.size());
                    node.NameLength = fn->NameLength;
                    node.Flags     |= kNodeNamed;
                    const wchar_t* name = reinterpret_cast<wchar_t*>(
                        rec + offset + res->ValueOffset + sizeof(FileNameAttribute));
                    g_nameArena.insert(g_nameArena.end(), name, name + fn->NameLength);
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
                    node.LogicalSize   += nr->RealSize;
                    node.AllocatedSize += nr->AllocatedSize;
                }
            } else {
                auto* res = reinterpret_cast<ResidentAttribute*>(attr);
                node.LogicalSize += res->ValueLength;
                // resident data occupies no clusters of its own => allocated 0
            }
        }

        offset += attr->Length;
    }
}

// ============================================================================
// MFT streaming
// ============================================================================

static bool ReadAt(HANDLE volume, uint64_t offset, void* buffer, uint32_t bytes) {
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    return ReadFile(volume, buffer, bytes, &read, &ov) && read == bytes;
}

// Feeds raw MFT bytes (in VCN order) through a reassembly buffer so that a
// FILE record split across two non-contiguous extents (possible when cluster
// size < record size, e.g. 512-byte clusters) is still parsed correctly.
class RecordStream {
public:
    RecordStream(uint32_t recordSize, uint64_t maxRecords)
        : recordSize_(recordSize), maxRecords_(maxRecords) {
        carry_.reserve(recordSize);
    }

    void Feed(uint8_t* data, size_t bytes) {
        size_t pos = 0;
        if (!carry_.empty()) {              // finish a record split across reads
            size_t need = std::min(recordSize_ - carry_.size(), bytes);
            carry_.insert(carry_.end(), data, data + need);
            pos = need;
            if (carry_.size() == recordSize_) {
                Emit(carry_.data());
                carry_.clear();
            }
        }
        while (pos + recordSize_ <= bytes) {
            Emit(data + pos);
            pos += recordSize_;
        }
        if (pos < bytes)
            carry_.assign(data + pos, data + bytes);
    }

    uint64_t RecordsParsed() const { return recordIndex_; }

private:
    void Emit(uint8_t* rec) {
        if (recordIndex_ < maxRecords_)
            ParseRecord(rec, recordSize_, recordIndex_);
        ++recordIndex_;
    }

    uint32_t recordSize_;
    uint64_t maxRecords_;
    uint64_t recordIndex_ = 0;
    std::vector<uint8_t> carry_;
};

// ============================================================================
// Aggregation & reporting
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
// main
// ============================================================================

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::wstring drive = L"C:";
    int topN = 50;
    std::wstring csvPath;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--top" && i + 1 < argc)      topN = _wtoi(argv[++i]);
        else if (arg == L"--csv" && i + 1 < argc) csvPath = argv[++i];
        else if (arg == L"--help" || arg == L"-h") {
            printf("Usage: yadua.exe [drive] [--top N] [--csv out.csv]\n"
                   "  drive   volume to scan, e.g. C: (default C:)\n"
                   "  --top   how many entries per list (default 50)\n"
                   "  --csv   also export the lists to a CSV file\n"
                   "Must be run from an elevated (Administrator) prompt.\n");
            return 0;
        }
        else if (!arg.empty() && arg[0] != L'-') {
            drive = arg;
            if (drive.size() == 1) drive += L':';   // allow "C"
            if (drive.size() > 2 && drive.back() == L'\\') drive.pop_back();
        }
    }

    auto t0 = std::chrono::steady_clock::now();

    // ---- 1. Open the raw volume ------------------------------------------
    std::wstring volumePath = L"\\\\.\\" + drive;
    HANDLE volume = CreateFileW(volumePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
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

    printf("Volume %ls: cluster %u B, MFT record %u B, MFT size %s (%llu records)\n",
           drive.c_str(), clusterSize, recordSize,
           HumanSize(mftBytes).c_str(), recordCount);

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
                // Note: if the MFT is fragmented into hundreds of extents the
                // run list itself can overflow into an extension record via
                // $ATTRIBUTE_LIST. Rare in practice; detect and bail loudly.
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

    // ---- 4 & 5. Stream the MFT and parse every record ---------------------
    g_nodes.assign(recordCount, Node{});
    g_nameArena.reserve(recordCount * 12);  // ~12 chars per name on average

    constexpr uint32_t kChunk = 4 * 1024 * 1024;
    std::vector<uint8_t> chunk(kChunk);
    std::vector<uint8_t> zeros; // lazily allocated, only if sparse runs exist
    RecordStream stream(recordSize, recordCount);

    uint64_t bytesRead = 0;
    uint64_t streamed  = 0;     // bytes of the $MFT data stream fed so far
    for (const Extent& run : mftRuns) {
        uint64_t runBytes = run.Clusters * (uint64_t)clusterSize;
        if (streamed >= mftBytes) break;
        runBytes = std::min(runBytes, mftBytes - streamed);

        if (run.Lcn < 0) {
            // Sparse run: no clusters on disk, the stream reads as zeros.
            // Feed zeros to keep record numbering aligned (zero magic => skipped).
            if (zeros.empty()) zeros.assign(kChunk, 0);
            for (uint64_t done = 0; done < runBytes; ) {
                uint32_t n = (uint32_t)std::min<uint64_t>(kChunk, runBytes - done);
                stream.Feed(zeros.data(), n);
                done += n;
            }
        } else {
            uint64_t diskOffset = (uint64_t)run.Lcn * clusterSize;
            for (uint64_t done = 0; done < runBytes; ) {
                // Reads must stay sector-aligned: round the tail read up to a
                // cluster, but feed only the valid bytes to the parser.
                uint64_t remaining = runBytes - done;
                uint32_t feed = (uint32_t)std::min<uint64_t>(kChunk, remaining);
                uint32_t read = ((feed + clusterSize - 1) / clusterSize) * clusterSize;
                if (!ReadAt(volume, diskOffset + done, chunk.data(), read)) {
                    fprintf(stderr, "error: volume read failed at offset %llu (%lu)\n",
                            diskOffset + done, GetLastError());
                    return 1;
                }
                stream.Feed(chunk.data(), feed);
                bytesRead += read;
                done += feed;
            }
        }
        streamed += runBytes;
    }
    CloseHandle(volume);

    auto t1 = std::chrono::steady_clock::now();
    double readSec =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1e6;

    // ---- 6. Aggregate folder sizes bottom-up -------------------------------
    std::vector<DirTotals> totals(g_nodes.size());
    Aggregate(totals);

    auto t2 = std::chrono::steady_clock::now();
    double totalSec =
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count() / 1e6;

    // ---- Reporting ----------------------------------------------------------
    uint64_t fileCount = 0, dirCount = 0;
    std::vector<uint32_t> files, dirs;
    files.reserve(g_nodes.size());
    for (uint32_t i = 0; i < g_nodes.size(); ++i) {
        const Node& n = g_nodes[i];
        if (!(n.Flags & kNodeInUse) || !(n.Flags & kNodeNamed)) continue;
        if (n.Flags & kNodeIsDir) { ++dirCount; dirs.push_back(i); }
        else                      { ++fileCount; files.push_back(i); }
    }

    auto byFileSize = [](uint32_t a, uint32_t b) {
        return g_nodes[a].LogicalSize > g_nodes[b].LogicalSize;
    };
    auto byDirSize = [&totals](uint32_t a, uint32_t b) {
        return totals[a].LogicalSize > totals[b].LogicalSize;
    };
    size_t nf = std::min<size_t>(topN, files.size());
    size_t nd = std::min<size_t>(topN, dirs.size());
    std::partial_sort(files.begin(), files.begin() + nf, files.end(), byFileSize);
    std::partial_sort(dirs.begin(),  dirs.begin() + nd,  dirs.end(),  byDirSize);

    const DirTotals& root = totals[kRootRecord];
    printf("\nScanned %llu records in %.2f s (MFT read %.2f s, %s @ %.0f MB/s)\n",
           stream.RecordsParsed(), totalSec, readSec,
           HumanSize(bytesRead).c_str(), bytesRead / readSec / (1024.0 * 1024.0));
    printf("Files: %llu   Folders: %llu   Total size: %s   On disk: %s\n",
           fileCount, dirCount,
           HumanSize(root.LogicalSize).c_str(),
           HumanSize(root.AllocatedSize).c_str());

    printf("\n=== Top %zu folders by size ===\n", nd);
    for (size_t i = 0; i < nd; ++i) {
        uint32_t idx = dirs[i];
        printf("%12s  %9llu files  %s\n",
               HumanSize(totals[idx].LogicalSize).c_str(),
               totals[idx].FileCount,
               Utf8(BuildPath(idx, drive)).c_str());
    }

    printf("\n=== Top %zu files by size ===\n", nf);
    for (size_t i = 0; i < nf; ++i) {
        uint32_t idx = files[i];
        printf("%12s  %s\n",
               HumanSize(g_nodes[idx].LogicalSize).c_str(),
               Utf8(BuildPath(idx, drive)).c_str());
    }

    // ---- Optional CSV export ------------------------------------------------
    if (!csvPath.empty()) {
        FILE* f = _wfopen(csvPath.c_str(), L"wb");
        if (!f) {
            fprintf(stderr, "error: cannot write %ls\n", csvPath.c_str());
            return 1;
        }
        fputs("\xEF\xBB\xBF", f); // UTF-8 BOM so Excel decodes correctly
        fputs("type,path,size_bytes,allocated_bytes,files,folders\n", f);
        auto csvEscape = [](std::string s) {
            if (s.find_first_of(",\"") == std::string::npos) return s;
            std::string out = "\"";
            for (char c : s) { if (c == '"') out += '"'; out += c; }
            return out + "\"";
        };
        for (size_t i = 0; i < nd; ++i) {
            uint32_t idx = dirs[i];
            fprintf(f, "folder,%s,%llu,%llu,%llu,%llu\n",
                    csvEscape(Utf8(BuildPath(idx, drive))).c_str(),
                    totals[idx].LogicalSize, totals[idx].AllocatedSize,
                    totals[idx].FileCount, totals[idx].DirCount);
        }
        for (size_t i = 0; i < nf; ++i) {
            uint32_t idx = files[i];
            fprintf(f, "file,%s,%llu,%llu,,\n",
                    csvEscape(Utf8(BuildPath(idx, drive))).c_str(),
                    g_nodes[idx].LogicalSize, g_nodes[idx].AllocatedSize);
        }
        fclose(f);
        printf("\nCSV written to %ls\n", csvPath.c_str());
    }

    return 0;
}
