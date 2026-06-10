# Builds yadua.exe (CLI) and yadua-gui.exe (Dear ImGui) with the latest MSVC.
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsRoot) { throw "No MSVC toolchain found (install VS Build Tools with C++ workload)." }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'

$root  = $PSScriptRoot
$obj   = Join-Path $root 'obj'
$imgui = Join-Path $root 'third_party\imgui'
New-Item -ItemType Directory -Force $obj | Out-Null

$common = '/nologo /O2 /std:c++20 /EHsc /D_CRT_SECURE_NO_WARNINGS'

# CLI: our code only, warnings cranked up.
$cli = "cl $common /W4 `"$root\src\cli.cpp`" `"$root\src\scanner.cpp`"" +
       " /Fe:`"$root\yadua.exe`" /Fo:`"$obj`"\\"

# GUI: /W3 because the vendored imgui sources are not /W4-clean.
# The manifest requests Administrator elevation (raw volume access).
$guiSrc = @(
    "$root\src\gui.cpp", "$root\src\scanner.cpp",
    "$imgui\imgui.cpp", "$imgui\imgui_draw.cpp",
    "$imgui\imgui_tables.cpp", "$imgui\imgui_widgets.cpp",
    "$imgui\backends\imgui_impl_win32.cpp", "$imgui\backends\imgui_impl_dx11.cpp"
) | ForEach-Object { "`"$_`"" }
$gui = "cl $common /W3 /DUNICODE /D_UNICODE /I`"$imgui`" /I`"$imgui\backends`" $($guiSrc -join ' ')" +
       " /Fe:`"$root\yadua-gui.exe`" /Fo:`"$obj`"\\" +
       " /link /SUBSYSTEM:WINDOWS" +
       " `"/MANIFESTUAC:level='requireAdministrator' uiAccess='false'`"" +
       " d3d11.lib d3dcompiler.lib dxgi.lib user32.lib gdi32.lib shell32.lib ole32.lib"

cmd /c "`"$vcvars`" >nul 2>&1 && $cli && $gui"
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
Write-Host "Built yadua.exe and yadua-gui.exe"
