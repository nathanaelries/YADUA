<p align="center"><img src="assets/logo.svg" width="440" alt="YADUA"></p>

# YADUA — Yet Another Disk Usage Analyzer

Fast NTFS disk-space analyzer (WizTree-style). Instead of recursively walking
directories, it reads the volume's Master File Table directly with raw volume
I/O and rebuilds the whole tree in memory.

Measured on this machine: full C: scan (1.81M MFT records, 1.2M files) in
**~4 seconds**, bounded by sequential read speed of the 1.73 GB MFT. A reader
thread streams the MFT while a pool of worker threads parses the records.

## Download

Every version tag publishes to the
[Releases page](https://github.com/nathanaelries/YADUA/releases)
(amd64 + arm64, SHA-256 checksums for everything):

- **Installer** (`YADUA-setup-*.exe`) — installs GUI + CLI with Start Menu
  shortcut, optional desktop icon, optional CLI on PATH, and an uninstaller.
- **Portable** (`YADUA-*-portable.zip`) — both executables, run from
  anywhere, no installation and no traces outside the folder.

> **Antivirus note:** the binaries are currently unsigned, and an unsigned,
> freshly-released tool that requests elevation and reads raw NTFS volumes is
> exactly the profile SmartScreen/Defender heuristics distrust, so false
> positives can happen. The exes carry full version metadata and you can
> verify downloads against `SHA256SUMS.txt`. If Defender flags a download,
> you can report the false positive to Microsoft (Security Intelligence →
> file submission). Code signing is on the roadmap.

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
A standard menu bar (**File / Edit / View / Search / Tools / Help**) exposes
every action; the toolbar keeps just the drive picker, Scan, and the filter
box. Keyboard: `Ctrl+R` scan, `Ctrl+F` find, `Del` recycle the selection,
`Esc` clear the filter.

- **Tree tab** — sortable, reorderable columns (Name, Size, % of parent,
  Files, Folders, Modified — click a header to sort, drag to reorder),
  percent-of-parent bars, filter box that auto-expands to matches. Filters
  combine name terms, extensions, and sizes: `setup *.iso >100mb`. A docked
  treemap panel sits below the tree (resizable splitter, toggle in *View ▸
  Treemap panel*); zoom and selection stay in sync with the full-screen
  Treemap tab.
- **Files tab** — a flat, sortable list of the largest files anywhere on the
  volume (WizTree-style), ignoring the folder hierarchy, with per-file
  percent-of-volume bar, modified date, and the containing folder. Honors the
  same filter box, so `*.iso >100mb` narrows it instantly; right-click gives
  the full context menu (delete, reveal, properties, ...).
- **File Types tab** — space grouped by extension: total size, file count,
  and percent of the volume, largest first, each with the extension's color
  swatch. Click a row to filter every view to that type.
- **Treemap tab** — WinDirStat-style squarified treemap colored by file
  extension, with toggleable cushion shading. Hover for details,
  double-click to zoom into a folder, Up/Top to zoom out.
- **Size on disk** — *View ▸ Size on disk (allocated)* switches every view
  (tree, files, types, treemap, totals) between logical file size and the
  bytes actually reserved on disk. The choice is remembered.
- **Right-click** (any view) — Open in Explorer, Copy path, Properties,
  Delete to Recycle Bin (with confirmation; the view updates in place,
  no rescan needed), and per-folder Rescan to refresh one subtree.
- **Live updates** — after an MFT scan the toolbar shows filesystem changes
  seen via the NTFS USN journal ("N changes since scan"); *Tools ▸ Apply
  filesystem changes* folds them in by re-reading only the affected MFT
  records, in well under a second.

## Scan modes

NTFS volumes scanned from an elevated process use the fast raw-MFT path.
Anything else — non-admin runs, FAT32/exFAT/ReFS volumes — automatically
falls back to a multi-threaded directory walk: slower (minutes instead of
seconds on a big volume, and locked directories are skipped), but it works
everywhere.

## Updates

The GUI can check for, verify, and install its own updates. It is
**consent-gated** (nothing installs without you clicking Update) and
**fails closed** — a bad signature, hash, or version means no update.

On launch (opt-out in *About → Check for updates on launch*) YADUA fetches a
small signed manifest from the GitHub **latest** release, verifies its ECDSA
P-256 signature against a public key compiled into the binary, refuses anything
that is not strictly newer (downgrade protection), then — once you click
**Update now** — downloads the installer, checks its size and SHA-256 against
the signed manifest, and runs it. The signature (not TLS, not `SHA256SUMS`) is
the trust anchor: even a compromised download channel cannot get past it.

Auto-update is **disabled until a signing key is embedded** (the shipped
`src/update_pubkey.h` is an all-zero placeholder). See
[docs/updates.md](docs/updates.md) for the full threat model, key custody, and
the one-time setup (`tools/gen-signing-key.ps1`). The signing key fingerprint
is shown in *About* so you can cross-check it here:

> **Update signing key (SHA-256):**
> `21020898359bc6b50db84b55e647a39432b64482b81b5c212f453f61f29a8a7b`

## CLI (run from an elevated prompt)

```powershell
yadua.exe C: --top 50 --csv results.csv --json results.json
yadua.exe C: --all --csv full.csv      # one row per file/folder on the volume
yadua.exe C: --all --json full.json    # adds a nested "tree" object
```

Options: `--top N` (list length, default 50), `--csv FILE`, `--json FILE`,
`--all` (export the entire tree instead of just the top-N lists),
`--walk` (force the directory-walk scanner), `--threads N` (default auto).
Exports carry logical size, allocated size, and the last-modified date per
entry.

## Snapshots & diff

Track where disk space went between two points in time:

```powershell
yadua.exe C: --snapshot before.ysnap     # save a snapshot during a scan
# ... days pass ...
yadua.exe --diff before.ysnap C:         # compare against a live scan
yadua.exe --diff before.ysnap after.ysnap --top 30
```

The diff reports total growth, added/removed file counts, and the largest
changes (grown/shrunk folders and files, additions, deletions). Snapshots are
a compact binary format (~116 MB for 1.5M entries).

## Benchmarks

`tools/benchmark.ps1` times the scan strategies on your machine. On the dev
machine (1.2M files, NVMe): raw-MFT scan **3.8 s**, directory-walk baseline
(the WinDirStat-style technique) **35 s**. WizTree is also timed when
installed; note its scriptable mode includes a full CSV export, so that
number is not a pure scan time.

## Code layout

- `src/ntfs.h` — NTFS on-disk structures (FILE records, attributes)
- `src/scanner.h/.cpp` — the scan engine (raw MFT streaming, parallel parse,
  aggregation, child index); used by both frontends
- `src/cli.cpp` — console frontend + exports
- `src/gui.cpp` — Dear ImGui frontend (tree view, deletion, sorting, filters)
- `src/treemap.h/.cpp` — squarified treemap layout + rendering
- `src/updater.h/.cpp` — signed auto-update (WinHTTP + BCrypt ECDSA/SHA-256);
  `src/update_pubkey.h` holds the embedded public key
- `tools/gen-signing-key.ps1`, `tools/sign-manifest.ps1` — offline keygen and
  release-time manifest signing (see `docs/updates.md`)
- `assets/` — logo/icon SVGs, generated PNG set, `yadua.ico` + `.rc` resource

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
