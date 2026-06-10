# Benchmarks YADUA's scan strategies on this machine, and WizTree if it is
# installed (WinDirStat/TreeSize have no clean CLI; YADUA's --walk uses the
# same recursive-enumeration technique they do, so it stands in as their
# representative timing).
#
# Run from an elevated prompt: .\tools\benchmark.ps1 [-Drive C:] [-Runs 3]
param(
    [string]$Drive = 'C:',
    [int]$Runs = 3
)
$ErrorActionPreference = 'Stop'

$yadua = Join-Path (Split-Path $PSScriptRoot) 'yadua.exe'
if (-not (Test-Path $yadua)) { throw "yadua.exe not found - run .\build.ps1 first" }

$elevated = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    Write-Warning "Not elevated: the raw-MFT path will fail; results will be walk-only."
}

function Time-Runs([string]$label, [int]$count, [scriptblock]$body) {
    $times = for ($i = 1; $i -le $count; $i++) {
        $t = (Measure-Command $body).TotalSeconds
        Write-Host ("  {0} run {1}: {2:N2} s" -f $label, $i, $t)
        $t
    }
    [pscustomobject]@{
        Scanner = $label
        Runs    = $count
        Best    = '{0:N2} s' -f ($times | Measure-Object -Minimum).Minimum
        Average = '{0:N2} s' -f ($times | Measure-Object -Average).Average
    }
}

$results = @()
Write-Host "Benchmarking $Drive ..."
$results += Time-Runs 'YADUA (raw MFT)' $Runs { & $yadua $Drive --top 1 *> $null }
$results += Time-Runs 'YADUA (directory walk, WinDirStat-style)' 1 {
    & $yadua $Drive --top 1 --walk *> $null
}

# WizTree, if present (it also reads the MFT - the closest real comparison).
$wiztree = @("$env:ProgramFiles\WizTree\WizTree64.exe",
             "${env:ProgramFiles(x86)}\WizTree\WizTree64.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if ($wiztree) {
    $csv = Join-Path $env:TEMP 'wiztree-bench.csv'
    $results += Time-Runs 'WizTree (CSV export)' $Runs {
        Start-Process $wiztree -ArgumentList "`"$Drive`" /export=`"$csv`" /admin=1" -Wait
    }
    Remove-Item $csv -ErrorAction SilentlyContinue
} else {
    Write-Host "  (WizTree not installed - skipping the head-to-head)"
}

Write-Host ""
$results | Format-Table -AutoSize
Write-Host "Note: file-system cache state dominates; compare same-session numbers only."
