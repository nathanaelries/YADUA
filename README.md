# YADUA — Yet Another Disk Usage Analyzer

Fast NTFS disk-space analyzer (WizTree-style). Instead of recursively walking
directories, it reads the volume's Master File Table directly with raw volume
I/O and rebuilds the whole tree in memory.

Measured on this machine: full C: scan (1.81M MFT records, 1.2M files) in
**~5 seconds**, bounded by sequential read speed of the 1.73 GB MFT.

## Build

```powershell
.\build.ps1     # finds MSVC via vswhere, builds yadua.exe (C++20, /O2 /W4)
```

## Run (requires Administrator — raw volume access)

```powershell
yadua.exe C: --top 50 --csv results.csv
```

## How it works

1. `CreateFileW("\\.\C:")` opens the raw volume.
2. `FSCTL_GET_NTFS_VOLUME_DATA` returns cluster size, record size, MFT start
   LCN and valid length.
3. MFT record 0 describes the MFT itself; its `$DATA` run list gives every
   extent of the MFT on disk.
4. The MFT is streamed with 4 MB sequential reads.
5. Each 1 KB FILE record yields name + parent reference (`$FILE_NAME`) and
   logical/allocated size (unnamed `$DATA`).
6. The tree is rebuilt from parent references and folder sizes aggregated
   bottom-up.

See the extensive comments in `src/main.cpp` for the on-disk structures
(fixups/update sequence arrays, run-list encoding, extension records,
namespace handling, etc.).

## Roadmap

- [x] Phase 1: console scanner, top-N folders/files, CSV export
- [ ] JSON export, full-tree export
- [ ] Phase 2: interactive tree view UI, treemap, search/filter,
      Explorer integration
