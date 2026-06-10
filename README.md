# YADUA — Yet Another Disk Usage Analyzer

Fast NTFS disk-space analyzer (WizTree-style). Instead of recursively walking
directories, it reads the volume's Master File Table directly with raw volume
I/O and rebuilds the whole tree in memory.

Measured on this machine: full C: scan (1.81M MFT records, 1.2M files) in
**~4 seconds**, bounded by sequential read speed of the 1.73 GB MFT. A reader
thread streams the MFT while a pool of worker threads parses the records.

## Build

```powershell
.\build.ps1     # finds MSVC via vswhere, builds both targets (C++20, /O2)
```

Produces:

- `yadua.exe` — console scanner with CSV/JSON export
- `yadua-gui.exe` — interactive tree view (Dear ImGui, Win32 + DirectX 11;
  vendored under `third_party/imgui`)

## GUI

Launch `yadua-gui.exe` (it requests elevation itself), pick a drive, hit Scan.
Tree is sorted by size with percent-of-parent bars; type in the filter box to
narrow by name; right-click any row to open it in Explorer or copy its path.

## CLI (run from an elevated prompt)

```powershell
yadua.exe C: --top 50 --csv results.csv --json results.json
yadua.exe C: --all --csv full.csv      # one row per file/folder on the volume
yadua.exe C: --all --json full.json    # adds a nested "tree" object
```

Options: `--top N` (list length, default 50), `--csv FILE`, `--json FILE`,
`--all` (export the entire tree instead of just the top-N lists),
`--threads N` (parser threads, default auto).

## Code layout

- `src/ntfs.h` — NTFS on-disk structures (FILE records, attributes)
- `src/scanner.h/.cpp` — the scan engine (raw MFT streaming, parallel parse,
  aggregation, child index); used by both frontends
- `src/cli.cpp` — console frontend + exports
- `src/gui.cpp` — Dear ImGui frontend

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

See [TODO.md](TODO.md). Short version: finish the console scanner
(JSON/full-tree export, multi-threaded parsing, parser edge cases), then an
interactive tree view + treemap UI (Phase 2).
