#Requires -Version 5.1

<#
    Tests damaged-sector handling. No failing hardware needed: the test hook
    CARVER_FAULT_AT / CARVER_FAULT_LEN makes RawDevice::readAt fail for one
    512-byte region, exactly as a real bad sector would.

    A 1 MiB image is filled with random bytes and a PNG is planted before and
    after a bad sector at offset 128 KiB.

      carve  - both PNGs must still be recovered, and the run must report the
               unreadable sector instead of aborting.
      image  - the copy must keep the source length, zero fill only the bad
               sector, and report it; its SHA-256 must match a reference built
               by zeroing that one sector.

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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\badsector' }
$script:CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $script:CarverExe)) { throw "carver-cli.exe not found" }

Add-Type -AssemblyName System.Drawing

function Invoke-Carver {
    param([string[]] $Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { return (& $script:CarverExe @Arguments 2>&1 | Out-String) }
    finally { $ErrorActionPreference = $previous }
}

function New-TestImage {
    param([string]$Path, [System.Drawing.Imaging.ImageFormat]$Format, [int]$Size, [System.Drawing.Color]$Color)
    $bmp = New-Object System.Drawing.Bitmap($Size, $Size)
    try {
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try { $g.Clear($Color) } finally { $g.Dispose() }
        $bmp.Save($Path, $Format)
    } finally { $bmp.Dispose() }
    return [System.IO.File]::ReadAllBytes($Path)
}

function Get-Sha256 { param([string]$Path) (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

$ImageSize = 1MB
$FaultAt   = 128KB
$FaultLen  = 512
$OffA      = 64KB
$OffB      = 256KB

$pngA = New-TestImage -Path (Join-Path $WorkDir 'a_src.png') -Format ([System.Drawing.Imaging.ImageFormat]::Png) -Size 96 -Color ([System.Drawing.Color]::FromArgb(30, 90, 200))
$pngB = New-TestImage -Path (Join-Path $WorkDir 'b_src.png') -Format ([System.Drawing.Imaging.ImageFormat]::Png) -Size 64 -Color ([System.Drawing.Color]::FromArgb(200, 120, 20))
$hashA = (Get-FileHash -LiteralPath (Join-Path $WorkDir 'a_src.png') -Algorithm SHA256).Hash.ToLower()
$hashB = (Get-FileHash -LiteralPath (Join-Path $WorkDir 'b_src.png') -Algorithm SHA256).Hash.ToLower()

$image = New-Object byte[] $ImageSize
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
try { $rng.GetBytes($image) } finally { $rng.Dispose() }
[Array]::Copy($pngA, 0, $image, $OffA, $pngA.Length)
[Array]::Copy($pngB, 0, $image, $OffB, $pngB.Length)

$imagePath = Join-Path $WorkDir 'damaged.img'
[System.IO.File]::WriteAllBytes($imagePath, $image)

# expected image: the same bytes with only the bad sector zeroed
$expected = [byte[]]$image.Clone()
for ($i = 0; $i -lt $FaultLen; $i++) { $expected[$FaultAt + $i] = 0 }
$expectedPath = Join-Path $WorkDir 'expected.img'
[System.IO.File]::WriteAllBytes($expectedPath, $expected)

Write-Host ("image: {0} bytes, png A at {1}, bad sector at {2} ({3} bytes), png B at {4}" -f `
            $ImageSize, $OffA, $FaultAt, $FaultLen, $OffB)

$failures = @()

# ---------------- carving ----------------
Write-Host ''
Write-Host '=== carving across a bad sector ==='
$carveOut = Join-Path $WorkDir 'carve_out'
$env:CARVER_FAULT_AT = [string]$FaultAt
$env:CARVER_FAULT_LEN = [string]$FaultLen
try {
    $carveLog = Invoke-Carver @($imagePath, $carveOut)
} finally {
    Remove-Item Env:\CARVER_FAULT_AT -ErrorAction SilentlyContinue
    Remove-Item Env:\CARVER_FAULT_LEN -ErrorAction SilentlyContinue
}
Write-Host $carveLog.TrimEnd()

$hashes = @{}
if (Test-Path -LiteralPath $carveOut) {
    foreach ($f in Get-ChildItem -LiteralPath $carveOut -File) { $hashes[(Get-Sha256 $f.FullName)] = $f.Name }
}

if ($hashes.ContainsKey($hashA)) {
    Write-Host '  [ok]   png A (before the bad sector) recovered'
} else {
    Write-Host '  [FAIL] png A not recovered'
    $failures += 'carve-a'
}
if ($hashes.ContainsKey($hashB)) {
    Write-Host '  [ok]   png B (after the bad sector) recovered'
} else {
    Write-Host '  [FAIL] png B not recovered: the scan did not continue past the damage'
    $failures += 'carve-b'
}
if ($carveLog -match 'unreadable\s+:\s+\d+') {
    Write-Host '  [ok]   the run reported the unreadable sector'
} else {
    Write-Host '  [FAIL] the run did not report the unreadable sector'
    $failures += 'carve-report'
}

# ---------------- imaging ----------------
Write-Host ''
Write-Host '=== imaging across a bad sector ==='
$copyPath = Join-Path $WorkDir 'copy.img'
$env:CARVER_FAULT_AT = [string]$FaultAt
$env:CARVER_FAULT_LEN = [string]$FaultLen
try {
    $copyLog = Invoke-Carver @('--image', $imagePath, $copyPath, '--force')
} finally {
    Remove-Item Env:\CARVER_FAULT_AT -ErrorAction SilentlyContinue
    Remove-Item Env:\CARVER_FAULT_LEN -ErrorAction SilentlyContinue
}
Write-Host $copyLog.TrimEnd()

if (Test-Path -LiteralPath $copyPath) {
    $copyBytes = [System.IO.File]::ReadAllBytes($copyPath)
    if ($copyBytes.Length -eq $ImageSize) {
        Write-Host ("  [ok]   the copy keeps the source length ({0} bytes)" -f $copyBytes.Length)
    } else {
        Write-Host ("  [FAIL] the copy is {0} bytes, expected {1}" -f $copyBytes.Length, $ImageSize)
        $failures += 'image-size'
    }

    $zeroed = $true
    for ($i = 0; $i -lt $FaultLen; $i++) {
        if ($copyBytes[$FaultAt + $i] -ne 0) { $zeroed = $false; break }
    }
    if ($zeroed) {
        Write-Host '  [ok]   the bad sector was zero filled'
    } else {
        Write-Host '  [FAIL] the bad sector was not zero filled'
        $failures += 'image-zero'
    }

    if ((Get-Sha256 $copyPath) -eq (Get-Sha256 $expectedPath)) {
        Write-Host '  [ok]   the copy matches the source with only the bad sector zeroed'
    } else {
        Write-Host '  [FAIL] the copy differs from the reference'
        $failures += 'image-hash'
    }
} else {
    Write-Host '  [FAIL] no copy was written'
    $failures += 'image-missing'
}

if ($copyLog -match 'bad sectors\s+:\s+\d+') {
    Write-Host '  [ok]   the run reported the bad sectors'
} else {
    Write-Host '  [FAIL] the run did not report the bad sectors'
    $failures += 'image-report'
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
