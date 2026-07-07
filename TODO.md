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
- [~] Live updates via USN journal: implemented (GUI watched the change
      journal and folded changes in via `ApplyMftUpdates`), then **removed
      from the GUI** because the background monitor was implicated in hangs
      /crashes. The `UsnMonitor` / `ApplyMftUpdates` library code remains for
      the CLI `debug-usn` command; the GUI now refreshes via Rescan instead.
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
- [x] Standard menu bar (File / Edit / View / Search / Tools / Help) with
      keyboard shortcuts (Ctrl+R, Ctrl+F, Del, Esc); the toolbar keeps only
      the drive picker, Scan, and filter. Retired the cryptic "Apply" and
      "About" toolbar buttons.
- [x] Stability & responsiveness pass (audit-driven):
      - removed the USN live-monitor from the GUI (background thread that
        mutated the tree in place — the likely hang/crash source)
      - debounced the filter box (was an O(all-nodes) recompute per keystroke,
        which made typing lag) and made the match loop allocation-free
      - hardened MFT parsing: reject corrupt run-list nibbles (was a stack
        buffer overflow), bound every attribute length to the record (OOB
        reads), overflow-safe value bounds
      - bounded ApplyMftUpdates (RAII volume handle + cap on record growth) and
        LoadSnapshot (node count vs. file size) against giant allocations
      - added WinHTTP timeouts so a stalled update can't block shutdown
      - fixed a rescan-vs-UI data race (menu/shortcuts read the tree while a
        worker mutated it) and a null render-target deref on resize
      - hardened the scan worker pools (StreamMft + WalkTree) against a mid-scan
        bad_alloc: worker bodies catch-all so an exception can't escape a thread
        (std::terminate); a shared error flag + join scope-guard make the others
        exit cleanly (no deadlock) and the scan reports the failure

- [x] Energy-efficiency pass: the GUI renders on demand instead of spinning
      at monitor refresh rate (blocks in `MsgWaitForMultipleObjectsEx` when
      idle, ~30 fps only while background work animates, skips frames while
      minimized), flip-model swap chain (`FLIP_DISCARD`, blt fallback for
      pre-Win10), and the USN reader blocks in the kernel instead of polling
      every second. Measured idle GUI: 6.7% of a core → 0%.

## Phase 4 — WizTree / WinDirStat competitive parity

- [x] "Size on disk" (allocated) toggle: View menu flips the tree, treemap,
      Files/File Types, and totals between logical size and bytes reserved on
      disk (the choice is remembered)
- [x] **Modified-date column**: the raw-MFT scan reads the
      `$STANDARD_INFORMATION` last-write time (the one Explorer shows) into
      `Node`, and the directory-walk fallback reads `ftLastWriteTime`. A
      sortable, reorderable "Modified" column now appears in the Tree and
      Files views; the snapshot format bumped to v2 (v1 still loads); CSV/JSON
      exports gained a `modified` field. (created/accessed times not surfaced.)
- [x] **Export from the GUI** (File ▸ Export): full tree or the current
      (filtered) file list to CSV, written on a background thread so a big
      volume can't freeze the UI. (JSON still CLI-only.)
- [x] **Scan a specific folder / path**: File ▸ Scan folder... opens a folder
      picker and runs the directory-walk engine (new `ScanFolder`) rooted at
      the chosen path; paths render rooted there. Verified headlessly on src/.
- [x] **Allocated column alongside Size**, plus show/hide + reorder columns:
      an "Allocated" column (hidden by default) in the Tree and Files views,
      right-click a header to show/hide, drag to reorder
- [x] **Remember window size/position and the last-scanned drive** across runs
      (persisted in the settings file)
- [ ] **Multi-select** in the tree and Files list for bulk delete / export
- [x] **Free / unused space**: used/free/total in the toolbar, and the treemap
      now represents the whole disk - a "free" block and an "unaccounted" block
      alongside the scanned tree (SpaceSniffer/WizTree-style), proportional to
      bytes. Only at the volume root of a full volume scan. Layout verified
      headlessly (blocks in-bounds; free area == free/total).
- [x] **File attributes** column: Win32 FILE_ATTRIBUTE_* captured from
      `$STANDARD_INFORMATION` (offset 0x20) and the walk fallback, shown as
      R/H/S/A/C/E letters in the Tree and Files views (hidden by default) and
      in CSV exports. (Owner column deferred - needs per-file security lookups.)
- [~] **In-GUI snapshots**: File ▸ Export ▸ Save snapshot (.ysnap) done
      (background-threaded). The compare/diff **view** is deferred - it's a
      new UI that wants design + eyes-on review; `--diff` stays in the CLI.
- [x] **Treemap color legend** (WinDirStat-style): the full-screen Treemap tab
      shows a one-line key of the biggest file types with their hue swatches.
- [x] **Custom right-click commands** (WinDirStat-style cleanup actions):
      `%LOCALAPPDATA%\YADUA\commands.txt` holds "Label|command" lines (%1 = the
      selected path); they appear under a "Commands" submenu and run via
      CreateProcess. A commented template is written on first run.
