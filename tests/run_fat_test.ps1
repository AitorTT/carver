#Requires -Version 5.1

<#
    Tests --free-only on FAT32 and exFAT volumes.

    Both images are built from scratch, no format or mount involved:

      FAT32 - a real BPB, a FAT with two allocated runs, and free clusters
              either side of the second run
      exFAT - a VBR, a root directory holding a $Bitmap entry, and an
              allocation bitmap marking the root, the bitmap itself and one
              live file's cluster

    In each image a PNG sits in a FREE cluster and a JPEG in an ALLOCATED one,
    so --free-only must recover the PNG and refuse the JPEG.

    exFAT is deliberately driven by the allocation bitmap rather than the FAT:
    a contiguous file may be written with NoFatChain set, in which case its FAT
    entries are never filled in and reading the FAT alone would call live data
    free.

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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\fat' }
$script:CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $script:CarverExe)) { throw "carver-cli.exe not found" }

function Invoke-Carver {
    param([string[]] $Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { return (& $script:CarverExe @Arguments 2>&1 | Out-String) }
    finally { $ErrorActionPreference = $previous }
}

function Set-U16 { param([byte[]]$B,[int]$O,[uint16]$V) $x=[BitConverter]::GetBytes($V); [Array]::Copy($x,0,$B,$O,2) }
function Set-U32 { param([byte[]]$B,[int]$O,[uint32]$V) $x=[BitConverter]::GetBytes($V); [Array]::Copy($x,0,$B,$O,4) }
function Set-U64 { param([byte[]]$B,[int]$O,[uint64]$V) $x=[BitConverter]::GetBytes($V); [Array]::Copy($x,0,$B,$O,8) }
function Get-Sha256 { param([string]$P) (Get-FileHash -LiteralPath $P -Algorithm SHA256).Hash.ToLower() }

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

Add-Type -AssemblyName System.Drawing
function New-TestImage {
    param([string]$Path, [System.Drawing.Imaging.ImageFormat]$Format, [int]$Size)
    $bmp = New-Object System.Drawing.Bitmap($Size, $Size)
    try {
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try { $g.Clear([System.Drawing.Color]::FromArgb(180, 40, 90)) } finally { $g.Dispose() }
        $bmp.Save($Path, $Format)
    } finally { $bmp.Dispose() }
    return [System.IO.File]::ReadAllBytes($Path)
}

$pngBytes = New-TestImage -Path (Join-Path $WorkDir 'src.png') -Format ([System.Drawing.Imaging.ImageFormat]::Png) -Size 96
$jpgBytes = New-TestImage -Path (Join-Path $WorkDir 'src.jpg') -Format ([System.Drawing.Imaging.ImageFormat]::Jpeg) -Size 64
$pngHash = (Get-FileHash -LiteralPath (Join-Path $WorkDir 'src.png') -Algorithm SHA256).Hash.ToLower()
$jpgHash = (Get-FileHash -LiteralPath (Join-Path $WorkDir 'src.jpg') -Algorithm SHA256).Hash.ToLower()

$failures = @()

function Assert-Recovered {
    param([string]$Label, [string]$OutputDir, [bool]$ExpectPng, [bool]$ExpectJpg)

    $hashes = @{}
    if (Test-Path $OutputDir) {
        foreach ($f in Get-ChildItem $OutputDir -File) { $hashes[(Get-Sha256 $f.FullName)] = $f.Name }
    }

    if ($ExpectPng) {
        if ($hashes.ContainsKey($script:pngHash)) {
            Write-Host ("  [ok]   {0}: png recovered from free space" -f $Label)
        } else {
            Write-Host ("  [FAIL] {0}: png not recovered" -f $Label)
            $script:failures += "$Label-png"
        }
    }

    if (-not $ExpectJpg -and $hashes.ContainsKey($script:jpgHash)) {
        Write-Host ("  [FAIL] {0}: jpeg recovered from an allocated cluster" -f $Label)
        $script:failures += "$Label-jpg-leak"
    } else {
        Write-Host ("  [ok]   {0}: jpeg correctly skipped (cluster allocated)" -f $Label)
    }
}

# ---------------- FAT32 ----------------
Write-Host '=== FAT32 ==='
$bps = 512
$spc = 1
$reserved = 32
$fatSectors = 520
$totalSectors = 32 + 520 + 66000
$dataStart = ($reserved + $fatSectors) * $bps
$clusterCount = $totalSectors - $reserved - $fatSectors

$fatImage = New-Object byte[] ($totalSectors * $bps)

$boot = New-Object byte[] 512
$boot[0] = 0xEB; $boot[1] = 0x58; $boot[2] = 0x90
[System.Text.Encoding]::ASCII.GetBytes('MSWIN4.1').CopyTo($boot, 3)
Set-U16 $boot 0x0B ([uint16]$bps)
$boot[0x0D] = [byte]$spc
Set-U16 $boot 0x0E ([uint16]$reserved)
$boot[0x10] = 1
Set-U16 $boot 0x11 ([uint16]0)
Set-U16 $boot 0x13 ([uint16]0)
$boot[0x15] = 0xF8
Set-U16 $boot 0x16 ([uint16]0)
Set-U32 $boot 0x20 ([uint32]$totalSectors)
Set-U32 $boot 0x24 ([uint32]$fatSectors)
Set-U32 $boot 0x2C ([uint32]2)
$boot[0x1FE] = 0x55; $boot[0x1FF] = 0xAA
[Array]::Copy($boot, 0, $fatImage, 0, 512)

# FAT: reserved entries, then two allocated runs
$fatOffsetBytes = $reserved * $bps
Set-U32 $fatImage ($fatOffsetBytes + 0) ([uint32]0x0FFFFFF8)
Set-U32 $fatImage ($fatOffsetBytes + 4) ([uint32]0x0FFFFFFF)
foreach ($c in (2..101)) { Set-U32 $fatImage ($fatOffsetBytes + $c * 4) ([uint32]0x0FFFFFFF) }
foreach ($c in (500..599)) { Set-U32 $fatImage ($fatOffsetBytes + $c * 4) ([uint32]0x0FFFFFFF) }

$pngCluster = 200
$jpgCluster = 50
[Array]::Copy($pngBytes, 0, $fatImage, ($dataStart + ($pngCluster - 2) * $bps), $pngBytes.Length)
[Array]::Copy($jpgBytes, 0, $fatImage, ($dataStart + ($jpgCluster - 2) * $bps), $jpgBytes.Length)

$fatPath = Join-Path $WorkDir 'fat32.img'
[System.IO.File]::WriteAllBytes($fatPath, $fatImage)
Write-Host ("  image: {0} bytes, {1} clusters, png at cluster {2} (free), jpeg at {3} (allocated)" -f `
            $fatImage.Length, $clusterCount, $pngCluster, $jpgCluster)

$fatOut = Join-Path $WorkDir 'fat32-out'
$fatLog = Invoke-Carver @($fatPath, $fatOut, '--free-only')
Write-Host ($fatLog.TrimEnd())

if ($fatLog -match 'filesystem\s+:\s+FAT32') {
    Write-Host '  [ok]   reported as FAT32'
} else {
    Write-Host '  [FAIL] not reported as FAT32'
    $failures += 'fat32-kind'
}
if ($fatLog -match 'clusters\s+:\s+66000') {
    Write-Host '  [ok]   cluster count 66000'
} else {
    Write-Host '  [FAIL] cluster count wrong'
    $failures += 'fat32-clusters'
}
if ($fatLog -match 'free\s+:\s+2\s+extents') {
    Write-Host '  [ok]   2 free extents (either side of the second allocated run)'
} else {
    Write-Host '  [FAIL] free extent count wrong'
    $failures += 'fat32-extents'
}
Assert-Recovered -Label 'FAT32' -OutputDir $fatOut -ExpectPng $true -ExpectJpg $false

# ---------------- exFAT ----------------
Write-Host ''
Write-Host '=== exFAT ==='
$sectorShift = 9
$clusterShift = 0
$eps = 1 -shl $sectorShift
$epc = $eps -shl $clusterShift
$heapOffset = 100
$eClusterCount = 4000
$rootCluster = 2
$bitmapCluster = 1000
$bitmapLength = [int][Math]::Ceiling($eClusterCount / 8)
$eDataStart = $heapOffset * $eps
$exfatSize = $eDataStart + $eClusterCount * $epc

$exfatImage = New-Object byte[] $exfatSize
$vbr = New-Object byte[] 512
$vbr[0] = 0xEB; $vbr[1] = 0x76; $vbr[2] = 0x90
[System.Text.Encoding]::ASCII.GetBytes('EXFAT   ').CopyTo($vbr, 3)
Set-U64 $vbr 0x40 ([uint64]0)
Set-U64 $vbr 0x48 ([uint64]($exfatSize / $eps))
Set-U32 $vbr 0x50 ([uint32]24)
Set-U32 $vbr 0x54 ([uint32]32)
Set-U32 $vbr 0x58 ([uint32]$heapOffset)
Set-U32 $vbr 0x5C ([uint32]$eClusterCount)
Set-U32 $vbr 0x60 ([uint32]$rootCluster)
Set-U32 $vbr 0x64 ([uint32]0x12345678)
Set-U16 $vbr 0x68 ([uint16]0x0100)
Set-U16 $vbr 0x6A ([uint16]0)
$vbr[0x6C] = [byte]$sectorShift
$vbr[0x6D] = [byte]$clusterShift
$vbr[0x6E] = 1
$vbr[0x6F] = 0x80
$vbr[0x70] = 0
$vbr[0x1FE] = 0x55; $vbr[0x1FF] = 0xAA
[Array]::Copy($vbr, 0, $exfatImage, 0, 512)

# allocation bitmap: root (2), live file (500) and the bitmap itself (1000)
$bitmap = New-Object byte[] $bitmapLength
foreach ($c in @(2, 500, 1000)) {
    $bit = $c - 2
    $byteIndex = $bit -shr 3
    $bitmap[$byteIndex] = $bitmap[$byteIndex] -bor [byte](1 -shl ($bit -band 7))
}
$bitmapOffset = $eDataStart + ($bitmapCluster - 2) * $epc
[Array]::Copy($bitmap, 0, $exfatImage, $bitmapOffset, $bitmap.Length)

# root directory: one $Bitmap entry
$rootOffset = $eDataStart + ($rootCluster - 2) * $epc
$exfatImage[$rootOffset] = 0x81
Set-U32 $exfatImage ($rootOffset + 0x04) ([uint32]$bitmapCluster)
Set-U64 $exfatImage ($rootOffset + 0x08) ([uint64]$bitmapLength)
$exfatImage[$rootOffset + 32] = 0x00

$ePngCluster = 2000
$eJpgCluster = 500
[Array]::Copy($pngBytes, 0, $exfatImage, ($eDataStart + ($ePngCluster - 2) * $epc), $pngBytes.Length)
[Array]::Copy($jpgBytes, 0, $exfatImage, ($eDataStart + ($eJpgCluster - 2) * $epc), $jpgBytes.Length)

$exfatPath = Join-Path $WorkDir 'exfat.img'
[System.IO.File]::WriteAllBytes($exfatPath, $exfatImage)
Write-Host ("  image: {0} bytes, {1} clusters, png at cluster {2} (free), jpeg at {3} (allocated)" -f `
            $exfatImage.Length, $eClusterCount, $ePngCluster, $eJpgCluster)

$exfatOut = Join-Path $WorkDir 'exfat-out'
$exfatLog = Invoke-Carver @($exfatPath, $exfatOut, '--free-only')
Write-Host ($exfatLog.TrimEnd())

if ($exfatLog -match 'filesystem\s+:\s+exFAT') {
    Write-Host '  [ok]   reported as exFAT'
} else {
    Write-Host '  [FAIL] not reported as exFAT'
    $failures += 'exfat-kind'
}
if ($exfatLog -match 'allocation\s+:\s+exFAT allocation bitmap') {
    Write-Host '  [ok]   used the allocation bitmap, not the FAT'
} else {
    Write-Host '  [FAIL] allocation source not reported as the bitmap'
    $failures += 'exfat-source'
}
if ($exfatLog -match 'free\s+:\s+3\s+extents') {
    Write-Host '  [ok]   3 free extents'
} else {
    Write-Host '  [FAIL] free extent count wrong'
    $failures += 'exfat-extents'
}
Assert-Recovered -Label 'exFAT' -OutputDir $exfatOut -ExpectPng $true -ExpectJpg $false

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
