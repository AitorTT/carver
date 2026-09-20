#Requires -Version 5.1

<#
    Tests pausing and resuming a carve.

    CARVER_CARVE_STOP_AFTER stops the scan after N bytes have been scanned,
    which is what pressing Ctrl+C does; the run then saves a checkpoint in the
    output folder. A second run with --resume carries on from that checkpoint,
    and --resume-from <bytes> carries on from a chosen offset instead.

    Four PNGs sit at 64 KiB, 192 KiB, 320 KiB and 448 KiB. The first run is
    stopped around 256 KiB, so the last PNG must only appear after resuming.

    No administrator rights required.
#>

[CmdletBinding()]
param(
    [string] $CarverExe,
    [string] $WorkDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptRoot = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.Manifest }
if (-not $CarverExe) { $CarverExe = Join-Path $scriptRoot '..\build\carver-cli.exe' }
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\carve_resume' }
$script:CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $script:CarverExe)) { throw "carver-cli.exe not found" }

Add-Type -AssemblyName System.Drawing

function Invoke-Carver {
    param([string[]] $Arguments, [int] $StopAfter = 0)
    if ($StopAfter -gt 0) { $env:CARVER_CARVE_STOP_AFTER = [string]$StopAfter }
    else { Remove-Item Env:\CARVER_CARVE_STOP_AFTER -ErrorAction SilentlyContinue }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { return (& $script:CarverExe @Arguments 2>&1 | Out-String) }
    finally {
        $ErrorActionPreference = $previous
        Remove-Item Env:\CARVER_CARVE_STOP_AFTER -ErrorAction SilentlyContinue
    }
}

function New-TestImage {
    param([string]$Path, [int]$Size, [System.Drawing.Color]$Color)
    $bmp = New-Object System.Drawing.Bitmap($Size, $Size)
    try {
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try { $g.Clear($Color) } finally { $g.Dispose() }
        $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $bmp.Dispose() }
    return [System.IO.File]::ReadAllBytes($Path)
}

function Get-Sha256 { param([string]$Path) (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }

function Get-RecoveredHashes {
    param([string]$Dir)
    $set = @{}
    if (Test-Path -LiteralPath $Dir) {
        foreach ($f in Get-ChildItem -LiteralPath $Dir -File) {
            if ($f.Name -eq 'carve-state.txt') { continue }
            $set[(Get-Sha256 $f.FullName)] = $f.Name
        }
    }
    return $set
}

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

$ImageSize = 512KB
$Offsets   = @(64KB, 192KB, 320KB, 448KB)
$Colors    = @(
    [System.Drawing.Color]::FromArgb(200, 0, 0),
    [System.Drawing.Color]::FromArgb(0, 200, 0),
    [System.Drawing.Color]::FromArgb(0, 0, 200),
    [System.Drawing.Color]::FromArgb(200, 200, 0)
)

$image = New-Object byte[] $ImageSize
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
try { $rng.GetBytes($image) } finally { $rng.Dispose() }

$hashes = @()
for ($i = 0; $i -lt $Offsets.Count; $i++) {
    $src = Join-Path $WorkDir ("src{0}.png" -f $i)
    $bytes = New-TestImage -Path $src -Size (64 + $i * 16) -Color $Colors[$i]
    [Array]::Copy($bytes, 0, $image, $Offsets[$i], $bytes.Length)
    $hashes += (Get-Sha256 $src)
}

$imagePath = Join-Path $WorkDir 'sample.img'
[System.IO.File]::WriteAllBytes($imagePath, $image)
Write-Host ("image: {0} bytes, pngs at {1}" -f $ImageSize, ($Offsets -join ', '))

$failures = @()
$label = @('A', 'B', 'C', 'D')

# ---------------- pause ----------------
Write-Host ''
Write-Host '=== carve, stopped after 256 KiB ==='
$pauseDir = Join-Path $WorkDir 'paused'
$stopAfter = 256KB
$pauseLog = Invoke-Carver @($imagePath, $pauseDir) -StopAfter $stopAfter
Write-Host ($pauseLog.TrimEnd() | Select-String -Pattern 'files recovered|bytes scanned|paused|resume with' | ForEach-Object { $_.Line })

$statePath = Join-Path $pauseDir 'carve-state.txt'
if (Test-Path -LiteralPath $statePath) {
    Write-Host '  [ok]   a checkpoint was written'
} else {
    Write-Host '  [FAIL] no checkpoint was written'
    $failures += 'pause-state'
}
if ($pauseLog -match 'paused') {
    Write-Host '  [ok]   the run reported that it paused'
} else {
    Write-Host '  [FAIL] the run did not report a pause'
    $failures += 'pause-report'
}

$paused = Get-RecoveredHashes $pauseDir
if ($paused.ContainsKey($hashes[0]) -and $paused.ContainsKey($hashes[1])) {
    Write-Host '  [ok]   png A and B (before the stop) were recovered'
} else {
    Write-Host '  [FAIL] png A or B missing before the stop'
    $failures += 'pause-early'
}
if ($paused.ContainsKey($hashes[3])) {
    Write-Host '  [FAIL] png D (after the stop) was already recovered'
    $failures += 'pause-late'
} else {
    Write-Host '  [ok]   png D (after the stop) was not reached'
}

# ---------------- resume ----------------
Write-Host ''
Write-Host '=== carve again with --resume ==='
$resumeLog = Invoke-Carver @($imagePath, $pauseDir, '--resume')
Write-Host ($resumeLog.TrimEnd() | Select-String -Pattern 'resumed from|files recovered|bytes scanned|paused' | ForEach-Object { $_.Line })

if ($resumeLog -match 'resumed from') {
    Write-Host '  [ok]   the run reported that it resumed'
} else {
    Write-Host '  [FAIL] the resume was not reported'
    $failures += 'resume-report'
}

$resumed = Get-RecoveredHashes $pauseDir
$allPresent = $true
for ($i = 0; $i -lt 4; $i++) { if (-not $resumed.ContainsKey($hashes[$i])) { $allPresent = $false } }
if ($allPresent) {
    Write-Host '  [ok]   all four pngs recovered after resuming'
} else {
    Write-Host '  [FAIL] not every png was recovered after resuming'
    $failures += 'resume-missing'
}
if ($resumed.Count -eq 4) {
    Write-Host '  [ok]   exactly four files, no duplicates'
} else {
    Write-Host ("  [FAIL] {0} files present, expected 4" -f $resumed.Count)
    $failures += 'resume-count'
}

# ---------------- resume from a chosen offset ----------------
Write-Host ''
Write-Host '=== carve with --resume-from (offset 320 KiB) ==='
$fromDir = Join-Path $WorkDir 'from_offset'
$fromLog = Invoke-Carver @($imagePath, $fromDir, '--resume-from', ([string](320KB)))
Write-Host ($fromLog.TrimEnd() | Select-String -Pattern 'resumed from|files recovered|bytes scanned' | ForEach-Object { $_.Line })

$from = Get-RecoveredHashes $fromDir
if ($from.ContainsKey($hashes[2]) -and $from.ContainsKey($hashes[3]) -and
    -not $from.ContainsKey($hashes[0]) -and -not $from.ContainsKey($hashes[1])) {
    Write-Host '  [ok]   only the pngs at or after the offset were recovered'
} else {
    Write-Host ("  [FAIL] unexpected files recovered: {0}" -f ($from.Count))
    $failures += 'from-offset'
}
if ($fromLog -match 'resumed from') {
    Write-Host '  [ok]   the resume offset was reported'
} else {
    Write-Host '  [FAIL] the resume offset was not reported'
    $failures += 'from-report'
}

# ---------------- a completed run clears the checkpoint ----------------
Write-Host ''
Write-Host '=== carve to completion leaves no checkpoint behind ==='
$doneDir = Join-Path $WorkDir 'complete'
$doneLog = Invoke-Carver @($imagePath, $doneDir)
$done = Get-RecoveredHashes $doneDir
if ($done.Count -eq 4) {
    Write-Host '  [ok]   all four pngs recovered in one pass'
} else {
    Write-Host ("  [FAIL] {0} files recovered in one pass" -f $done.Count)
    $failures += 'complete-count'
}
if (Test-Path -LiteralPath (Join-Path $doneDir 'carve-state.txt')) {
    Write-Host '  [FAIL] a checkpoint was left after a completed run'
    $failures += 'complete-state'
} else {
    Write-Host '  [ok]   the checkpoint was removed on completion'
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
