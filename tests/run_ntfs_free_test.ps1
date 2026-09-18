#Requires -Version 5.1

<#
    Tests --free-only by building a synthetic NTFS image from scratch.

    The image is a valid-enough NTFS volume: a real boot sector, a real $MFT
    record 6 ($Bitmap) with a non-resident $DATA attribute and a runlist, and a
    real bitmap. No administrator rights and no format/mount required.

    Layout (512-byte clusters, 4096 clusters, 2 MiB):
        clusters 0..19    allocated (boot sector + MFT)
        cluster  64       allocated ($Bitmap data)
        clusters 100..199 allocated (simulated live files)
        everything else   free

    A PNG is hidden in a FREE cluster and a JPEG in an ALLOCATED cluster.
    --free-only must recover the PNG and refuse the JPEG; a full scan must
    recover both.
#>

[CmdletBinding()]
param(
    [string] $CarverExe,
    [string] $WorkDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptRoot = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $CarverExe) { $CarverExe = Join-Path $scriptRoot '..\build\carver-cli.exe' }
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\ntfs' }
$CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir   = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $CarverExe)) { throw "carver-cli.exe not found at '$CarverExe'. Build it first." }

Add-Type -AssemblyName System.Drawing

$BytesPerSector   = 512
$SectorsPerCluster = 1
$BytesPerCluster  = $BytesPerSector * $SectorsPerCluster
$TotalClusters    = 4096
$ImageSize        = $TotalClusters * $BytesPerCluster
$MftCluster       = 4
$MftRecordSize    = 1024
$BitmapCluster    = 64

function Set-U16 { param([byte[]]$Buf, [int]$Offset, [uint16]$Value) $b = [BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,2) }
function Set-U32 { param([byte[]]$Buf, [int]$Offset, [uint32]$Value) $b = [BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,4) }
function Set-U64 { param([byte[]]$Buf, [int]$Offset, [uint64]$Value) $b = [BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,8) }
function Set-Ascii { param([byte[]]$Buf, [int]$Offset, [string]$Text) $b = [System.Text.Encoding]::ASCII.GetBytes($Text); [Array]::Copy($b,0,$Buf,$Offset,$b.Length) }

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

Write-Host '=== building synthetic NTFS image ==='

$image = New-Object byte[] $ImageSize

# --- boot sector ---
$boot = New-Object byte[] 512
$boot[0] = 0xEB; $boot[1] = 0x52; $boot[2] = 0x90
Set-Ascii -Buf $boot -Offset 0x03 -Text 'NTFS    '
Set-U16 -Buf $boot -Offset 0x0B -Value ([uint16]$BytesPerSector)
$boot[0x0D] = [byte]$SectorsPerCluster
$boot[0x15] = 0xF8
Set-U32 -Buf $boot -Offset 0x24 -Value ([uint32]0x00800080)
Set-U64 -Buf $boot -Offset 0x28 -Value ([uint64]$TotalClusters)
Set-U64 -Buf $boot -Offset 0x30 -Value ([uint64]$MftCluster)
Set-U64 -Buf $boot -Offset 0x38 -Value ([uint64]20)
$boot[0x40] = 0xF6   # -10  -> 2^10 = 1024 byte MFT records
$boot[0x41] = 0xF4   # -12  -> 4096 byte index buffers
$boot[0x1FE] = 0x55; $boot[0x1FF] = 0xAA
[Array]::Copy($boot, 0, $image, 0, 512)

# --- $MFT record 6: $Bitmap, non-resident $DATA with a single run at cluster 64 ---
$rec = New-Object byte[] $MftRecordSize
Set-Ascii -Buf $rec -Offset 0x00 -Text 'FILE'
Set-U16 -Buf $rec -Offset 0x04 -Value ([uint16]0x30)      # update sequence array offset
Set-U16 -Buf $rec -Offset 0x06 -Value ([uint16]3)         # update sequence count
Set-U16 -Buf $rec -Offset 0x10 -Value ([uint16]1)         # sequence number
Set-U16 -Buf $rec -Offset 0x12 -Value ([uint16]1)         # hard link count
Set-U16 -Buf $rec -Offset 0x14 -Value ([uint16]0x38)      # first attribute offset
Set-U16 -Buf $rec -Offset 0x16 -Value ([uint16]1)         # in use
Set-U32 -Buf $rec -Offset 0x18 -Value ([uint32]0x88)      # used size
Set-U32 -Buf $rec -Offset 0x1C -Value ([uint32]$MftRecordSize)
Set-U16 -Buf $rec -Offset 0x28 -Value ([uint16]1)         # next attribute id

# update sequence array: nonzero USN so the fixup path is genuinely exercised
$usn = [uint16]0x1234
Set-U16 -Buf $rec -Offset 0x30 -Value $usn                # the USN
Set-U16 -Buf $rec -Offset 0x32 -Value ([uint16]0)         # original tail of sector 1
Set-U16 -Buf $rec -Offset 0x34 -Value ([uint16]0)         # original tail of sector 2
Set-U16 -Buf $rec -Offset 510  -Value $usn
Set-U16 -Buf $rec -Offset 1022 -Value $usn

$attr = 0x38
Set-U32 -Buf $rec -Offset $attr       -Value ([uint32]0x80)   # $DATA
Set-U32 -Buf $rec -Offset ($attr+4)   -Value ([uint32]0x48)   # attribute length
$rec[$attr+8]  = 1                                            # non-resident
$rec[$attr+9]  = 0                                            # unnamed
Set-U16 -Buf $rec -Offset ($attr+0x0A) -Value ([uint16]0x40)  # name offset
Set-U16 -Buf $rec -Offset ($attr+0x0C) -Value ([uint16]0)     # flags
Set-U16 -Buf $rec -Offset ($attr+0x0E) -Value ([uint16]0)     # attribute id
Set-U64 -Buf $rec -Offset ($attr+0x10) -Value ([uint64]0)     # starting VCN
Set-U64 -Buf $rec -Offset ($attr+0x18) -Value ([uint64]0)     # last VCN
Set-U16 -Buf $rec -Offset ($attr+0x20) -Value ([uint16]0x40)  # runlist offset
Set-U16 -Buf $rec -Offset ($attr+0x22) -Value ([uint16]0)     # compression unit
Set-U64 -Buf $rec -Offset ($attr+0x28) -Value ([uint64]512)   # allocated size
Set-U64 -Buf $rec -Offset ($attr+0x30) -Value ([uint64]512)   # real size
Set-U64 -Buf $rec -Offset ($attr+0x38) -Value ([uint64]512)   # initialized size

$runList = $attr + 0x40
$rec[$runList]   = 0x11   # 1-byte length, 1-byte offset
$rec[$runList+1] = 0x01   # 1 cluster long
$rec[$runList+2] = [byte]$BitmapCluster
$rec[$runList+3] = 0x00   # end of runlist

Set-U32 -Buf $rec -Offset ($attr + 0x48) -Value ([uint32]::MaxValue)

[Array]::Copy($rec, 0, $image, ($MftCluster * $BytesPerCluster + 6 * $MftRecordSize), $MftRecordSize)

# --- bitmap: mark 0..19, 64 and 100..199 allocated ---
$bitmap = New-Object byte[] ($TotalClusters / 8)
$allocated = @(0..19) + @(64) + @(100..199)
foreach ($cluster in $allocated) {
    $byteIndex = $cluster -shr 3          # integer division; [int]($cluster/8) would round
    $bitmap[$byteIndex] = $bitmap[$byteIndex] -bor [byte](1 -shl ($cluster -band 7))
}
[Array]::Copy($bitmap, 0, $image, ($BitmapCluster * $BytesPerCluster), $bitmap.Length)

# --- hidden files: PNG in free space, JPEG in allocated space ---
function New-SolidBitmap {
    param([string] $Path, [System.Drawing.Imaging.ImageFormat] $Format, [int] $Size)
    $bitmap = New-Object System.Drawing.Bitmap($Size, $Size)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.Clear([System.Drawing.Color]::FromArgb(30, 90, 160)) } finally { $graphics.Dispose() }
        $bitmap.Save($Path, $Format)
    } finally { $bitmap.Dispose() }
}

$pngTemp = Join-Path $WorkDir 'free.png'
$jpgTemp = Join-Path $WorkDir 'allocated.jpg'
New-SolidBitmap -Path $pngTemp -Format ([System.Drawing.Imaging.ImageFormat]::Png) -Size 64
New-SolidBitmap -Path $jpgTemp -Format ([System.Drawing.Imaging.ImageFormat]::Jpeg) -Size 96

$pngBytes = [System.IO.File]::ReadAllBytes($pngTemp)
$jpgBytes = [System.IO.File]::ReadAllBytes($jpgTemp)

$pngOffset = 300 * $BytesPerCluster   # cluster 300 -> free
$jpgOffset = 120 * $BytesPerCluster   # cluster 120 -> allocated (live file)

[Array]::Copy($pngBytes, 0, $image, $pngOffset, $pngBytes.Length)
[Array]::Copy($jpgBytes, 0, $image, $jpgOffset, $jpgBytes.Length)

$imagePath = Join-Path $WorkDir 'ntfs.img'
[System.IO.File]::WriteAllBytes($imagePath, $image)

$pngHash = (Get-FileHash -LiteralPath $pngTemp -Algorithm SHA256).Hash
$jpgHash = (Get-FileHash -LiteralPath $jpgTemp -Algorithm SHA256).Hash

Write-Host ("  image          : {0} bytes, {1} clusters of {2} bytes" -f $ImageSize, $TotalClusters, $BytesPerCluster)
Write-Host ("  png (free)     : offset {0} (cluster {1})" -f $pngOffset, ($pngOffset / $BytesPerCluster))
Write-Host ("  jpeg (alloc'd) : offset {0} (cluster {1})" -f $jpgOffset, ($jpgOffset / $BytesPerCluster))

$expectedFreeClusters = $TotalClusters - $allocated.Count
$expectedFreeBytes = $expectedFreeClusters * $BytesPerCluster

function Invoke-Scan {
    param([string] $OutputDir, [switch] $FreeOnly)
    if (Test-Path $OutputDir) { Remove-Item $OutputDir -Recurse -Force }
    $arguments = @($imagePath, $OutputDir)
    if ($FreeOnly) { $arguments += '--free-only' }
    $output = & $CarverExe @arguments 2>&1 | Out-String
    return $output
}

function Get-RecoveredHashes {
    param([string] $OutputDir)
    $hashes = @{}
    if (Test-Path $OutputDir) {
        foreach ($file in Get-ChildItem $OutputDir -File) {
            if ($file.Name -like '*.png' -or $file.Name -like '*.jpg') {
                $hashes[(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash] = $file.Name
            }
        }
    }
    return $hashes
}

$failures = @()

Write-Host ''
Write-Host '=== scan 1: whole volume (no --free-only) ==='
$fullOut = Invoke-Scan -OutputDir (Join-Path $WorkDir 'full')
$fullHashes = Get-RecoveredHashes -OutputDir (Join-Path $WorkDir 'full')

if ($fullHashes.ContainsKey($pngHash)) {
    Write-Host ("  [ok]   png recovered by a full scan -> {0}" -f $fullHashes[$pngHash])
} else {
    Write-Host '  [FAIL] png NOT recovered by a full scan'
    $failures += 'full-png'
}
if ($fullHashes.ContainsKey($jpgHash)) {
    Write-Host ("  [ok]   jpeg recovered by a full scan -> {0}" -f $fullHashes[$jpgHash])
} else {
    Write-Host '  [FAIL] jpeg NOT recovered by a full scan'
    $failures += 'full-jpg'
}

Write-Host ''
Write-Host '=== scan 2: --free-only ==='
$freeOut = Invoke-Scan -OutputDir (Join-Path $WorkDir 'free') -FreeOnly
$freeHashes = Get-RecoveredHashes -OutputDir (Join-Path $WorkDir 'free')

if ($freeHashes.ContainsKey($pngHash)) {
    Write-Host ("  [ok]   png recovered from free space -> {0}" -f $freeHashes[$pngHash])
} else {
    Write-Host '  [FAIL] png NOT recovered from free space'
    $failures += 'free-png'
}
if ($freeHashes.ContainsKey($jpgHash)) {
    Write-Host ("  [FAIL] jpeg recovered even though its cluster is allocated -> {0}" -f $freeHashes[$jpgHash])
    $failures += 'free-jpg-leak'
} else {
    Write-Host '  [ok]   jpeg correctly skipped (its cluster is marked allocated)'
}

Write-Host ''
Write-Host '=== geometry reported by --free-only ==='
$reportedExtents = [regex]::Match($freeOut, 'free\s+:\s+(\d+)\s+extents')
if ($reportedExtents.Success -and [int]$reportedExtents.Groups[1].Value -eq 3) {
    Write-Host ("  [ok]   free extents: {0} (expected 3)" -f $reportedExtents.Groups[1].Value)
} else {
    Write-Host ("  [FAIL] free extents: {0} (expected 3)" -f $reportedExtents.Groups[1].Value)
    $failures += 'extents'
}

$reportedClusters = [regex]::Match($freeOut, 'clusters\s+:\s+(\d+)')
if ($reportedClusters.Success -and [int]$reportedClusters.Groups[1].Value -eq $TotalClusters) {
    Write-Host ("  [ok]   total clusters: {0}" -f $reportedClusters.Groups[1].Value)
} else {
    Write-Host ("  [FAIL] total clusters: {0} (expected {1})" -f $reportedClusters.Groups[1].Value, $TotalClusters)
    $failures += 'clusters'
}

$expectedMiB = [Math]::Round($expectedFreeBytes / 1MB, 1)
$reportedFree = [regex]::Match($freeOut, 'free\s+:\s+\d+\s+extents,\s+([0-9.]+)\s+MiB')
if ($reportedFree.Success) {
    $value = [double]$reportedFree.Groups[1].Value
    if ([Math]::Abs($value - $expectedMiB) -lt 0.2) {
        Write-Host ("  [ok]   free bytes: {0} MiB (expected {1} MiB)" -f $value, $expectedMiB)
    } else {
        Write-Host ("  [FAIL] free bytes: {0} MiB (expected {1} MiB)" -f $value, $expectedMiB)
        $failures += 'free-bytes'
    }
} else {
    Write-Host '  [FAIL] could not parse the reported free size'
    $failures += 'free-bytes-parse'
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
