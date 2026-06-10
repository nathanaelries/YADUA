# Builds yadua.exe (CLI) and yadua-gui.exe (Dear ImGui) with the latest MSVC.
param(
    # Target architecture: x64 (default) or arm64 (cross-compiled from an
    # x64 host). 'amd64' is accepted as an alias for x64 (CI convention).
    [ValidateSet('x64', 'amd64', 'arm64')]
    [string]$Arch = 'x64'
)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsRoot) { throw "No MSVC toolchain found (install VS Build Tools with C++ workload)." }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvarsall.bat'
$vcArch = if ($Arch -eq 'arm64') { 'amd64_arm64' } else { 'x64' }

$root  = $PSScriptRoot
$obj   = Join-Path $root 'obj'
$imgui = Join-Path $root 'third_party\imgui'
New-Item -ItemType Directory -Force $obj | Out-Null

$common = '/nologo /O2 /std:c++20 /EHsc /D_CRT_SECURE_NO_WARNINGS'

# App icon resource, linked into both executables.
$res = "rc /nologo /fo `"$obj\yadua.res`" `"$root\assets\yadua.rc`""

# CLI: our code only, warnings cranked up.
$cli = "cl $common /W4 `"$root\src\cli.cpp`" `"$root\src\scanner.cpp`"" +
       " `"$obj\yadua.res`" /Fe:`"$root\yadua.exe`" /Fo:`"$obj`"\\"

# GUI: /W3 because the vendored imgui sources are not /W4-clean.
# The manifest requests Administrator elevation (raw volume access).
$guiSrc = @(
    "$root\src\gui.cpp", "$root\src\treemap.cpp", "$root\src\scanner.cpp",
    "$imgui\imgui.cpp", "$imgui\imgui_draw.cpp",
    "$imgui\imgui_tables.cpp", "$imgui\imgui_widgets.cpp",
    "$imgui\backends\imgui_impl_win32.cpp", "$imgui\backends\imgui_impl_dx11.cpp"
) | ForEach-Object { "`"$_`"" }
$gui = "cl $common /W3 /DUNICODE /D_UNICODE /I`"$imgui`" /I`"$imgui\backends`" $($guiSrc -join ' ')" +
       " `"$obj\yadua.res`" /Fe:`"$root\yadua-gui.exe`" /Fo:`"$obj`"\\" +
       " /link /SUBSYSTEM:WINDOWS" +
       " `"/MANIFESTUAC:level='requireAdministrator' uiAccess='false'`"" +
       " d3d11.lib d3dcompiler.lib dxgi.lib user32.lib gdi32.lib shell32.lib ole32.lib"

cmd /c "`"$vcvars`" $vcArch >nul 2>&1 && $res && $cli && $gui"
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
Write-Host "Built yadua.exe and yadua-gui.exe ($Arch)"
