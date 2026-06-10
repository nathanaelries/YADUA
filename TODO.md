# YADUA Roadmap

## Phase 1 — Core scanner (console)

- [x] Raw volume access + `FSCTL_GET_NTFS_VOLUME_DATA`
- [x] $MFT run-list decoding and sequential streaming reads
- [x] FILE record parsing (fixups, $FILE_NAME, unnamed $DATA, extension records)
- [x] In-memory tree from parent references, bottom-up folder aggregation
- [x] Top-N folders/files report with timing stats
- [x] CSV export (top-N lists)
- [x] JSON export (`--json out.json`)
- [x] Full-tree export (`--all`: CSV gets one row per file/folder, JSON gets
      a nested `tree` object with children sorted by size)
- [x] Multi-threaded record parsing (reader thread streams the MFT while a
      worker pool parses; scan is now bounded by raw volume read speed)
- [ ] Scan multiple/all fixed volumes in one run
- [ ] Progress reporting during the read (percent of MFT streamed)

### Parser correctness gaps

- [ ] Parse `$ATTRIBUTE_LIST` so a heavily fragmented $MFT (run list overflowing
      into extension records) scans instead of bailing with an error
- [ ] Surface orphaned subtrees in a visible "lost+found" bucket instead of
      silently dropping their contribution
- [ ] Mark reparse points (junctions, symlinks, OneDrive placeholders) and
      exclude/flag them in totals
- [ ] Optional: count alternate data streams (named $DATA), excluding
      $BadClus:$Bad
- [ ] Optional: include directory index ($INDEX_ALLOCATION) sizes so
      "size on disk" matches Explorer more closely
- [ ] Fallback scan path (recursive `FindFirstFileExW`) for non-NTFS volumes
      (FAT32/exFAT/ReFS) and non-admin runs

## Phase 2 — Interactive UI

- [x] Tree view sorted by size, expand/collapse (Dear ImGui, Win32 + DX11)
- [x] Percent-of-parent bars
- [x] Filter by name (case-insensitive, auto-expands to matches)
- [x] Right-click: Open in Explorer, Copy path
- [x] Rescan button, background scan with progress bar
- [x] Treemap visualization (squarified, extension-colored, zoom via
      double-click, hover tooltips, cached layout)
- [x] Right-click: Delete to Recycle Bin (confirmation modal, background
      shell op, in-memory tree update without rescan), Properties
- [x] Sortable columns (name/size/files/folders, asc/desc, tri-state back
      to size order)
- [ ] Filter by size / extension
- [ ] Per-folder rescan
- [ ] File-type icons in the tree; treemap cushion shading
- [ ] Tree <-> treemap selection sync

## Phase 3 — Polish

- [ ] Auto-elevation (relaunch with UAC prompt when not admin)
- [ ] Live updates via USN journal (track changes without rescanning)
- [ ] Export/import scan snapshots; diff two scans over time
- [ ] x64 + ARM64 release builds, CI via GitHub Actions
- [ ] Benchmark suite vs. WizTree / WinDirStat / TreeSize
