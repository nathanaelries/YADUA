# Builds yadua.exe (CLI) and yadua-gui.exe (Dear ImGui) with the latest MSVC.
param(
    # Target architecture: x64 (default) or arm64 (cross-compiled from an
    # x64 host). 'amd64' is accepted as an alias for x64 (CI convention).
    [ValidateSet('x64', 'amd64', 'arm64')]
    [string]$Arch = 'x64',

    # Semantic version stamped into the VERSIONINFO resource (CI passes the
    # tag without the leading 'v'). Local builds default to 0.0.0.
    [string]$Version = '0.0.0'
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

# Generate version.h for the VERSIONINFO resource (see assets/yadua.rc).
$verNumeric = ($Version.Split('-')[0].Split('.') + @('0', '0', '0'))[0..2]
@"
#define YADUA_VER_NUM $($verNumeric -join ','),0
#define YADUA_VER_STR "$Version"
"@ | Set-Content "$obj\version.h" -Encoding ascii

# Icon + version resources; OriginalFilename/description differ per binary.
$res = "rc /nologo /i `"$obj`" /fo `"$obj\yadua-cli.res`" `"$root\assets\yadua.rc`"" +
       " && rc /nologo /i `"$obj`" /d YADUA_IS_GUI /fo `"$obj\yadua-gui.res`" `"$root\assets\yadua.rc`""

# CLI: our code only, warnings cranked up.
$cli = "cl $common /W4 `"$root\src\cli.cpp`" `"$root\src\scanner.cpp`"" +
       " `"$obj\yadua-cli.res`" /Fe:`"$root\yadua.exe`" /Fo:`"$obj`"\\"

# GUI: /W3 because the vendored imgui sources are not /W4-clean.
# The manifest requests Administrator elevation (raw volume access).
$guiSrc = @(
    "$root\src\gui.cpp", "$root\src\treemap.cpp", "$root\src\scanner.cpp",
    "$root\src\updater.cpp",
    "$imgui\imgui.cpp", "$imgui\imgui_draw.cpp",
    "$imgui\imgui_tables.cpp", "$imgui\imgui_widgets.cpp",
    "$imgui\backends\imgui_impl_win32.cpp", "$imgui\backends\imgui_impl_dx11.cpp"
) | ForEach-Object { "`"$_`"" }
# NOTE: the /MANIFESTUAC value must contain no spaces. The command line
# travels through cmd /c, and the quoted space-containing form was split by
# the linker and silently dropped — shipping a GUI that never elevated.
# uiAccess defaults to 'false', so only the level needs to be set.
$gui = "cl $common /W3 /DUNICODE /D_UNICODE /I`"$imgui`" /I`"$imgui\backends`" $($guiSrc -join ' ')" +
       " `"$obj\yadua-gui.res`" /Fe:`"$root\yadua-gui.exe`" /Fo:`"$obj`"\\" +
       " /link /SUBSYSTEM:WINDOWS" +
       " /MANIFEST:EMBED /MANIFESTUAC:level='requireAdministrator'" +
       " d3d11.lib d3dcompiler.lib dxgi.lib user32.lib gdi32.lib shell32.lib ole32.lib" +
       " winhttp.lib bcrypt.lib version.lib"

cmd /c "`"$vcvars`" $vcArch >nul 2>&1 && $res && $cli && $gui"
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

# The GUI must self-elevate (raw MFT access is the whole point); fail the
# build if the UAC manifest ever drops out of the binary again.
$guiBytes = [System.Text.Encoding]::ASCII.GetString(
    [System.IO.File]::ReadAllBytes("$root\yadua-gui.exe"))
if ($guiBytes -notmatch 'requireAdministrator') {
    throw "yadua-gui.exe is missing the requireAdministrator UAC manifest!"
}
Write-Host "Built yadua.exe and yadua-gui.exe ($Arch); UAC manifest verified"
