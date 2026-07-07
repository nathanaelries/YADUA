# YADUA — Yet Another Disk Usage Analyzer

Fast NTFS disk-usage analyzer that reads the MFT via raw volume I/O. One build
produces two binaries: `yadua.exe` (CLI) and `yadua-gui.exe` (Dear ImGui +
Win32 + DX11). Roadmap/changelog lives in `TODO.md` — keep it updated.

## Build

```powershell
.\build.ps1              # x64 (default): yadua.exe + yadua-gui.exe in repo root
.\build.ps1 -Arch arm64  # cross-compile
```

Needs VS Build Tools with the C++ workload (located via vswhere). Local builds
stamp version 0.0.0, which also disables the launch update check.

## Verify

- `yadua.exe --debug-usn` (elevated, hidden flag) — end-to-end self-test of
  the raw-MFT scan + USN monitor: creates/deletes a temp file and checks the
  incremental update sees it (prints OK/FAIL).
- `tools\benchmark.ps1` — scan timing vs directory-walk baseline (and WizTree
  when installed).
- The GUI requires elevation (embedded `requireAdministrator` manifest; the
  build fails if it ever drops out). For render-only checks without a UAC
  prompt: `$env:__COMPAT_LAYER='RunAsInvoker'` then launch — raw-MFT scans
  fall back to the directory walk, but the UI is fully exercisable.

## Gotchas

- **Idle-power invariant**: the GUI main loop (`wWinMain` in `src/gui.cpp`)
  renders on demand, not continuously — an idle window must sit at ~0% CPU
  (check `(Get-Process yadua-gui).TotalProcessorTime` over a 10 s window).
  Any new continuously-animating UI (spinner, marquee, blinking status) must
  join the `working` condition in the main loop, or it will freeze whenever
  the app has no input and no background work.
- The USN reader (`UsnMonitor::Loop` in `src/scanner.cpp`) blocks inside
  `FSCTL_READ_USN_JOURNAL` (`BytesToWaitFor = 1`); `Stop()` breaks the wait
  with `CancelSynchronousIo`. Don't reintroduce sleep-polling.
- The GUI deliberately does **not** use `UsnMonitor` (past hangs/crashes —
  see Phase 3 in `TODO.md`); it refreshes via Rescan instead.
