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
- [x] Scan multiple volumes in one run (`yadua.exe C: D:`, exports get a
      per-drive suffix)
- [x] Progress reporting during the read (live percent on stderr when
      attached to a console; silent when output is redirected)

### Parser correctness gaps

- [x] Parse `$ATTRIBUTE_LIST` so a heavily fragmented $MFT (run list
      overflowing into extension records) scans instead of bailing
- [x] Surface orphaned subtrees in a visible `[orphaned]` bucket at the
      volume root instead of silently dropping their contribution
- [x] Mark reparse points (junctions, symlinks, OneDrive placeholders):
      flagged in scan stats, "[link]" suffix in the tree, treemap tooltip
      note (their content never double-counts: children parent to the
      target directory in the MFT, not the link)
- [x] Count alternate data streams (named $DATA); sparse ADS excluded from
      logical size so $BadClus:$Bad can't inflate totals
- [x] Include directory index ($INDEX_ALLOCATION) clusters in "size on
      disk", seeded into each directory's own cumulative total
- [x] Fallback scan path: multi-threaded `FindFirstFileExW` walk for
      non-NTFS volumes (FAT32/exFAT/ReFS) and non-admin runs; selected
      automatically by `ScanVolumeAuto`, skips reparse dirs to avoid
      junction cycles, approximates allocated sizes by cluster rounding

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
- [x] Filter by size / extension (`*.iso`, `ext:iso`, `>100mb`, `<2gb`,
      combined with name terms, AND semantics)
- [x] Tree <-> treemap selection sync ("Reveal in tree" / "Show in treemap"
      context actions, shared selection highlight)
- [x] Branding: SVG logo + icon, multi-size .ico embedded in both exes
- [x] Split-pane Tree tab: docked treemap panel under the tree with a
      draggable splitter and toolbar toggle (Treemap tab keeps the
      full-screen view; both share zoom/selection state)
- [x] Per-folder rescan (context menu; splice-rescans the subtree via a
      filesystem walk without a full rescan; allocated sizes in the
      rescanned part become cluster-rounded estimates)
- [x] Treemap cushion shading (toggleable pillow gradient per file rect)
- [x] File-type coloring in the tree (names tinted with the same extension
      hue the treemap uses; shell icons deliberately skipped)
- [x] Flat "Files" view: the largest files anywhere on the volume in one
      sortable, filterable list (WizTree-style), independent of the folder
      tree, virtualized so millions of rows stay smooth
- [x] "File Types" view: space grouped by extension (size, count, percent of
      volume), click a row to filter every view to that type

## Phase 3 — Polish

- [x] Auto-elevation (GUI manifest requests Administrator; non-elevated
      scans still work via the directory-walk fallback)
- [x] Live updates via USN journal: after an MFT scan the GUI watches the
      change journal, shows a "live: N changes" counter, and Apply folds
      the changes in by re-reading just the affected MFT records (no
      rescan). Journal wrap/loss is detected and surfaced.
- [x] Export/import scan snapshots; diff two scans over time
      (`--snapshot out.ysnap`, `--diff old.ysnap new.ysnap` or
      `--diff old.ysnap C:` to compare against a live scan; compact
      binary format, ~116 MB for 1.5M entries)
- [x] x64 + ARM64 release builds, CI via GitHub Actions (tag-triggered
      release workflow with checksums + generated notes)
- [x] Installer (Inno Setup: GUI + CLI, shortcuts, optional PATH) and
      portable ZIPs per architecture
- [x] VERSIONINFO resource stamped from the release tag
- [ ] Code signing (Azure Trusted Signing or similar) to stop antivirus /
      SmartScreen false positives on the unsigned binaries
- [x] Consent-gated auto-update: check the GitHub "latest" release, verify an
      ECDSA P-256 signature over a manifest against a public key embedded in
      the binary, enforce downgrade protection, then verify the installer's
      SHA-256 before running it. Fails closed. CI signs the manifest with an
      offline key; build-provenance attestation added. See docs/updates.md.
      (Scaffolded: disabled until a real signing key is generated + embedded
      via tools/gen-signing-key.ps1 and the YADUA_SIGNING_KEY secret is set.)
- [x] Benchmark suite (`tools/benchmark.ps1`): times the raw-MFT scan, the
      directory-walk baseline (WinDirStat-style technique), and WizTree's
      CSV export when installed. Measured here: MFT 3.8 s vs walk 35 s
      for 1.2M files
