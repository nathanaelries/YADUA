# Builds yadua.exe with the latest installed MSVC toolchain.
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsRoot) { throw "No MSVC toolchain found (install VS Build Tools with C++ workload)." }

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$src = Join-Path $PSScriptRoot 'src\main.cpp'
$out = Join-Path $PSScriptRoot 'yadua.exe'
$obj = Join-Path $PSScriptRoot 'main.obj'

cmd /c "`"$vcvars`" >nul && cl /nologo /O2 /std:c++20 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS `"$src`" /Fe:`"$out`" /Fo:`"$obj`""
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
Remove-Item $obj -ErrorAction SilentlyContinue
Write-Host "Built $out"
