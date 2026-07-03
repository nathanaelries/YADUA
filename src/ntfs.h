// ============================================================================
// NTFS on-disk structures
//
// These are not in the Windows SDK (the MFT layout is technically
// undocumented but stable since NT 3.1 and described in great detail in the
// Linux-NTFS / ntfs-3g project docs). All fields are little-endian.
// ============================================================================
#pragma once
#include <cstdint>

namespace yadua::ntfs {

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
constexpr uint32_t kAttrStandardInfo    = 0x10;
constexpr uint32_t kAttrAttributeList   = 0x20;
constexpr uint32_t kAttrFileName        = 0x30;
constexpr uint32_t kAttrData            = 0x80;
constexpr uint32_t kAttrIndexAllocation = 0xA0; // directory index ($I30) clusters
constexpr uint32_t kAttrEnd             = 0xFFFFFFFF;

// AttributeHeader::Flags bits.
constexpr uint16_t kAttrFlagCompressed = 0x0001;
constexpr uint16_t kAttrFlagSparse     = 0x8000;

// Records 0-15 are the classic NTFS metafiles ($MFT, $BadClus, $Secure, ...).
constexpr uint64_t kFirstUserRecord = 16;

// For sparse/compressed non-resident attributes, NonResidentAttribute ends
// with an extra uint64 at offset 0x40 (TotalAllocated): the clusters actually
// backed by disk. The regular AllocatedSize field then only describes the
// reserved VCN span (e.g. the whole volume for $BadClus:$Bad).
constexpr uint32_t kTotalAllocatedOffset = 0x40;

// FILE_ATTRIBUTE_REPARSE_POINT as mirrored into $FILE_NAME's FileAttributes.
constexpr uint32_t kFileAttrReparsePoint = 0x00000400;

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

// Value of a $STANDARD_INFORMATION attribute (always resident, always first).
// Its ModificationTime is the one Explorer shows as "Date modified"; the copy
// in $FILE_NAME is updated lazily and is not reliable. All times are FILETIME
// (100 ns ticks since 1601-01-01 UTC).
struct StandardInformation {
    uint64_t CreationTime;      // 0x00
    uint64_t ModificationTime;  // 0x08  last data write
    uint64_t MftChangeTime;     // 0x10
    uint64_t AccessTime;        // 0x18
    uint32_t FileAttributes;    // 0x20
    // version-dependent fields follow (quota, USN, ...); not needed here
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

// One entry in a $ATTRIBUTE_LIST value. When a file's attributes no longer
// fit in one MFT record they spill into extension records; the attribute
// list in the base record says which record holds which attribute (and, for
// chopped-up non-resident attributes, which VCN range).
struct AttributeListEntry {
    uint32_t Type;
    uint16_t Length;            // length of this entry, 8-byte aligned
    uint8_t  NameLength;
    uint8_t  NameOffset;
    uint64_t StartVcn;          // first VCN covered by that record's portion
    uint64_t FileRef;           // record holding the attribute (low 48 bits)
    uint16_t AttributeId;
    // optional WCHAR name follows
};

#pragma pack(pop)

} // namespace yadua::ntfs
