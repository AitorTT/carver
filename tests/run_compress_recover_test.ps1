#Requires -Version 5.1

<#
    Tests --mft-recover on NTFS compressed files, by building a synthetic NTFS
    image whose deleted records carry a compressed $DATA attribute.

    The compressed bytes are produced by ntdll's RtlCompressBuffer, the same
    code path NTFS itself uses, so the fixture is real LZNT1 and not a private
    encoding that only this decoder understands.

    Records laid out:
        0   $MFT
        6   $Bitmap
        20  compressed.txt  DELETED, compressed $DATA spanning TWO compression
                            units: unit 0 holds compressed data (fewer clusters
                            allocated than the unit has, so it is compressed),
                            unit 1 is stored verbatim (every cluster allocated).
        21  stored.bin      DELETED, compression flag set but the single unit is
                            fully allocated, so it must be copied verbatim.

    Both files must come back byte for byte. If the compressed flag were ignored
    the recovery would write the raw LZNT1 stream and every hash check would
    fail, so this test genuinely covers the decompression path.

    No administrator rights required.
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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\compress' }
$script:CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $script:CarverExe)) { throw "carver-cli.exe not found at '$script:CarverExe'. Build it first." }

$BytesPerSector    = 512
$SectorsPerCluster = 1
$BytesPerCluster   = 512
$TotalClusters     = 2048
$MftCluster        = 4
$MftRecordSize     = 1024
$MftClusters       = 50
$BitmapCluster     = 64
$UnitClusters      = 16
$UnitExponent      = 4
$UnitSize          = $UnitClusters * $BytesPerCluster
$MaxRecord         = 24

function Set-U16 { param([byte[]]$Buf,[int]$Offset,[uint16]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,2) }
function Set-U32 { param([byte[]]$Buf,[int]$Offset,[uint32]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,4) }
function Set-U64 { param([byte[]]$Buf,[int]$Offset,[uint64]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,8) }
function Set-Ascii { param([byte[]]$Buf,[int]$Offset,[string]$Text) $b=[System.Text.Encoding]::ASCII.GetBytes($Text); [Array]::Copy($b,0,$Buf,$Offset,$b.Length) }
function Align8 { param([int]$Value) return ($Value + 7) -band -8 }

# Encodes runs the way NTFS does: cluster numbers are relative to the previous
# run, and a cluster number of -1 produces a sparse run (no offset field).
function New-RunList {
    param([int[]]$Cluster, [int[]]$Length)

    $list = New-Object System.Collections.Generic.List[byte]
    $previous = 0

    for ($i = 0; $i -lt $Length.Count; ++$i) {
        $lengthBytes = New-Object System.Collections.Generic.List[byte]
        $value = $Length[$i]
        while ($value -gt 0) { $lengthBytes.Add([byte]($value -band 0xFF)); $value = $value -shr 8 }
        if ($lengthBytes.Count -eq 0) { $lengthBytes.Add([byte]0) }

        if ($Cluster[$i] -lt 0) {
            $list.Add([byte]($lengthBytes.Count))
            foreach ($b in $lengthBytes) { $list.Add($b) }
            continue
        }

        $delta = $Cluster[$i] - $previous
        $previous = $Cluster[$i]

        $offsetBytes = New-Object System.Collections.Generic.List[byte]
        $value = [uint32]$delta
        while ($value -gt 0) { $offsetBytes.Add([byte]($value -band 0xFF)); $value = $value -shr 8 }
        if ($offsetBytes.Count -eq 0) { $offsetBytes.Add([byte]0) }
        if (($offsetBytes[$offsetBytes.Count-1] -band 0x80) -ne 0) { $offsetBytes.Add([byte]0) }

        $list.Add([byte]((($offsetBytes.Count -shl 4)) -bor $lengthBytes.Count))
        foreach ($b in $lengthBytes) { $list.Add($b) }
        foreach ($b in $offsetBytes) { $list.Add($b) }
    }

    $list.Add([byte]0)
    return ,$list.ToArray()
}

function New-MftRecord {
    param(
        [bool]   $InUse = $false,
        [bool]   $Directory = $false,
        [string] $Name = '',
        [int[]]  $RunCluster = @(),
        [int[]]  $RunLength = @(),
        [uint64] $RealSize = 0,
        [uint64] $AllocatedSize = 0,
        [uint64] $LastVcn = 0,
        [bool]   $Compressed = $false,
        [int]    $UnitExponent = 0,
        [uint64] $Created = 0,
        [uint64] $Modified = 0,
        [int]    $Parent = 5
    )

    $record = New-Object byte[] 1024
    Set-Ascii $record 0 'FILE'
    Set-U16 $record 0x04 0x30
    Set-U16 $record 0x06 3
    Set-U16 $record 0x10 1
    Set-U16 $record 0x12 1
    Set-U16 $record 0x14 0x38

    $flags = 0
    if ($InUse)     { $flags = $flags -bor 1 }
    if ($Directory) { $flags = $flags -bor 2 }
    Set-U16 $record 0x16 ([uint16]$flags)
    Set-U32 $record 0x1C 1024
    Set-U16 $record 0x28 4

    $pos = 0x38

    # $STANDARD_INFORMATION
    $si = New-Object byte[] 0x48
    Set-U64 $si 0x00 $Created
    Set-U64 $si 0x08 $Modified
    Set-U64 $si 0x10 $Modified
    Set-U64 $si 0x18 $Modified
    $length = Align8 (0x18 + $si.Length)
    Set-U32 $record $pos 0x10
    Set-U32 $record ($pos+4) ([uint32]$length)
    $record[$pos+8] = 0; $record[$pos+9] = 0
    Set-U16 $record ($pos+0x0A) 0x18
    Set-U16 $record ($pos+0x0C) 0
    Set-U16 $record ($pos+0x0E) 0
    Set-U32 $record ($pos+0x10) ([uint32]$si.Length)
    Set-U16 $record ($pos+0x14) 0x18
    [Array]::Copy($si, 0, $record, $pos+0x18, $si.Length)
    $pos += $length

    # $FILE_NAME
    $nameBytes = [System.Text.Encoding]::Unicode.GetBytes($Name)
    $contentLength = 0x42 + $nameBytes.Length
    $length = Align8 (0x18 + $contentLength)
    Set-U32 $record $pos 0x30
    Set-U32 $record ($pos+4) ([uint32]$length)
    $record[$pos+8] = 0; $record[$pos+9] = 0
    Set-U16 $record ($pos+0x0A) 0x18
    Set-U16 $record ($pos+0x0C) 0
    Set-U16 $record ($pos+0x0E) 1
    Set-U32 $record ($pos+0x10) ([uint32]$contentLength)
    Set-U16 $record ($pos+0x14) 0x18

    $c = $pos + 0x18
    Set-U64 $record $c $Parent
    Set-U64 $record ($c+0x08) $Created
    Set-U64 $record ($c+0x10) $Modified
    Set-U64 $record ($c+0x18) $Modified
    Set-U64 $record ($c+0x20) $Modified
    Set-U64 $record ($c+0x28) $AllocatedSize
    Set-U64 $record ($c+0x30) $RealSize
    Set-U32 $record ($c+0x38) 0
    Set-U32 $record ($c+0x3C) 0
    $record[$c+0x40] = [byte]($nameBytes.Length / 2)
    $record[$c+0x41] = 1
    [Array]::Copy($nameBytes, 0, $record, $c+0x42, $nameBytes.Length)
    $pos += $length

    # $DATA (non-resident)
    $runList = New-RunList -Cluster $RunCluster -Length $RunLength
    $runListOffset = 0x40
    $length = Align8 ($runListOffset + $runList.Count)
    Set-U32 $record $pos 0x80
    Set-U32 $record ($pos+4) ([uint32]$length)
    $record[$pos+8] = 1; $record[$pos+9] = 0
    Set-U16 $record ($pos+0x0A) ([uint16]$runListOffset)
    Set-U16 $record ($pos+0x0C) ([uint16]$(if ($Compressed) { 1 } else { 0 }))
    Set-U16 $record ($pos+0x0E) 2
    Set-U64 $record ($pos+0x10) ([uint64]0)
    Set-U64 $record ($pos+0x18) $LastVcn
    Set-U16 $record ($pos+0x20) ([uint16]$runListOffset)
    $record[$pos+0x22] = [byte]$UnitExponent
    Set-U64 $record ($pos+0x28) $AllocatedSize
    Set-U64 $record ($pos+0x30) $RealSize
    Set-U64 $record ($pos+0x38) $RealSize
    [Array]::Copy($runList, 0, $record, $pos + $runListOffset, $runList.Count)
    $pos += $length

    Set-U32 $record $pos ([uint32]::MaxValue)
    $pos += 8
    Set-U32 $record 0x18 ([uint32]$pos)

    $usn = [uint16]0x1234
    Set-U16 $record 0x30 $usn
    Set-U16 $record 0x32 0
    Set-U16 $record 0x34 0
    Set-U16 $record 510 $usn
    Set-U16 $record 1022 $usn

    return ,$record
}

function Invoke-Carver {
    param([string[]] $Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { return (& $script:CarverExe @Arguments 2>&1 | Out-String) }
    finally { $ErrorActionPreference = $previous }
}

$signature = @"
using System;
using System.Runtime.InteropServices;

public static class Lznt1Native {
    [DllImport("ntdll.dll")]
    public static extern int RtlGetCompressionWorkSpaceSize(ushort format, out uint compressSize, out uint fragmentSize);

    [DllImport("ntdll.dll")]
    public static extern int RtlCompressBuffer(ushort format,
        byte[] uncompressed, uint uncompressedSize,
        byte[] compressed, uint compressedCapacity,
        uint chunkSize, out uint finalSize, IntPtr workspace);
}
"@

Add-Type -TypeDefinition $signature

$LZNT1_FORMAT = [uint16]0x0002

$compressWorkSpace = [uint32]0
$fragmentWorkSpace = [uint32]0
$status = [Lznt1Native]::RtlGetCompressionWorkSpaceSize($LZNT1_FORMAT, [ref]$compressWorkSpace, [ref]$fragmentWorkSpace)
if ($status -ne 0) { throw "RtlGetCompressionWorkSpaceSize failed with $status" }
$workspace = [System.Runtime.InteropServices.Marshal]::AllocHGlobal([int]$compressWorkSpace)

function Compress-Lznt1 {
    param([byte[]] $Data)

    $capacity = [int]($Data.Length * 3 + 65536)
    $buffer = New-Object byte[] $capacity
    [uint32]$final = 0

    $result = [Lznt1Native]::RtlCompressBuffer($LZNT1_FORMAT, $Data, [uint32]$Data.Length,
                                              $buffer, [uint32]$capacity, [uint32]4096,
                                              [ref]$final, $workspace)
    if ($result -ne 0) { throw ("RtlCompressBuffer failed with 0x{0:X8}" -f $result) }

    $out = New-Object byte[] $final
    [Array]::Copy($buffer, 0, $out, 0, $final)
    return ,$out
}

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

Write-Host '=== building synthetic NTFS image with compressed deleted records ==='

# Part A: highly repetitive, so LZNT1 actually shrinks it.
$phrase = [System.Text.Encoding]::ASCII.GetBytes('the quick brown fox jumps over the lazy dog. ')
$partA = New-Object byte[] $UnitSize
for ($i = 0; $i -lt $partA.Length; ++$i) { $partA[$i] = $phrase[$i % $phrase.Length] }

# Part B: random, so it cannot be compressed and belongs in a verbatim unit.
$random = [System.Random]::new(20240607)
$partB = New-Object byte[] $UnitSize
$random.NextBytes($partB)

$compressedA = Compress-Lznt1 -Data $partA
$usedA = [int][Math]::Ceiling($compressedA.Length / $BytesPerCluster)

Write-Host ("  part A {0} bytes -> {1} compressed bytes -> {2} of {3} clusters" -f $partA.Length, $compressedA.Length, $usedA, $UnitClusters)
Write-Host ("  part B {0} bytes -> stored verbatim in {1} clusters" -f $partB.Length, $UnitClusters)

if ($usedA -ge $UnitClusters) {
    Write-Host 'FAILED: the compressor did not shrink part A enough to make a compressed unit'
    exit 1
}

$ClusterA = 200
$ClusterB = 220
$ClusterStored = 300

$imageSize = $TotalClusters * $BytesPerCluster
$image = New-Object byte[] $imageSize

# boot sector
$boot = New-Object byte[] 512
$boot[0] = 0xEB; $boot[1] = 0x52; $boot[2] = 0x90
Set-Ascii $boot 0x03 'NTFS    '
Set-U16 $boot 0x0B ([uint16]$BytesPerSector)
$boot[0x0D] = [byte]$SectorsPerCluster
$boot[0x15] = 0xF8
Set-U32 $boot 0x24 ([uint32]0x00800080)
Set-U64 $boot 0x28 ([uint64]$TotalClusters)
Set-U64 $boot 0x30 ([uint64]$MftCluster)
Set-U64 $boot 0x38 ([uint64]20)
$boot[0x40] = 0xF6
$boot[0x41] = 0xF4
$boot[0x1FE] = 0x55; $boot[0x1FF] = 0xAA
[Array]::Copy($boot, 0, $image, 0, 512)

$createdFileTime = [System.DateTime]::new(2024, 5, 6, 7, 8, 9, [System.DateTimeKind]::Utc).ToFileTime()

$records = @{}
$records[0] = New-MftRecord -InUse $true -Name '$MFT' -RunCluster @($MftCluster) -RunLength @($MftClusters) -RealSize ([uint64]($MftClusters * $BytesPerCluster)) -AllocatedSize ([uint64]($MftClusters * $BytesPerCluster)) -LastVcn ([uint64]($MftClusters - 1))
$records[6] = New-MftRecord -InUse $true -Name '$Bitmap' -RunCluster @($BitmapCluster) -RunLength @(1) -RealSize 512 -AllocatedSize 512 -LastVcn 0

# compressed.txt: unit 0 compressed, unit 1 verbatim
$records[20] = New-MftRecord -Name 'compressed.txt' `
    -RunCluster @($ClusterA, -1, $ClusterB) `
    -RunLength  @($usedA, ($UnitClusters - $usedA), $UnitClusters) `
    -RealSize ([uint64]($UnitSize * 2)) `
    -AllocatedSize ([uint64](($usedA + $UnitClusters) * $BytesPerCluster)) `
    -LastVcn ([uint64](($UnitClusters * 2) - 1)) `
    -Compressed $true -UnitExponent $UnitExponent `
    -Created $createdFileTime -Modified $createdFileTime

# stored.bin: compression flag set, but the unit is fully allocated
$records[21] = New-MftRecord -Name 'stored.bin' `
    -RunCluster @($ClusterStored) -RunLength @($UnitClusters) `
    -RealSize ([uint64]$UnitSize) `
    -AllocatedSize ([uint64]($UnitClusters * $BytesPerCluster)) `
    -LastVcn ([uint64]($UnitClusters - 1)) `
    -Compressed $true -UnitExponent $UnitExponent `
    -Created $createdFileTime -Modified $createdFileTime

$mftOffset = $MftCluster * $BytesPerCluster
foreach ($recordNumber in $records.Keys) {
    [Array]::Copy($records[$recordNumber], 0, $image, ($mftOffset + $recordNumber * $MftRecordSize), $MftRecordSize)
}

# bitmap: only the boot area, the $MFT and the $Bitmap are allocated, so every
# data cluster of both files still reads as free
$bmBytes = New-Object byte[] ($TotalClusters / 8)
foreach ($cluster in (@(0..53) + @($BitmapCluster))) {
    $byteIndex = $cluster -shr 3
    $bmBytes[$byteIndex] = $bmBytes[$byteIndex] -bor [byte](1 -shl ($cluster -band 7))
}
[Array]::Copy($bmBytes, 0, $image, ($BitmapCluster * $BytesPerCluster), $bmBytes.Length)

# file contents
[Array]::Copy($compressedA, 0, $image, ($ClusterA * $BytesPerCluster), $compressedA.Length)
[Array]::Copy($partB, 0, $image, ($ClusterB * $BytesPerCluster), $partB.Length)
[Array]::Copy($partB, 0, $image, ($ClusterStored * $BytesPerCluster), $partB.Length)

$imagePath = Join-Path $WorkDir 'ntfs_compressed.img'
[System.IO.File]::WriteAllBytes($imagePath, $image)

$expected = New-Object byte[] ($UnitSize * 2)
[Array]::Copy($partA, 0, $expected, 0, $partA.Length)
[Array]::Copy($partB, 0, $expected, $partA.Length, $partB.Length)

$expectedPath = Join-Path $WorkDir 'compressed_expected.bin'
[System.IO.File]::WriteAllBytes($expectedPath, $expected)
$storedPath = Join-Path $WorkDir 'stored_expected.bin'
[System.IO.File]::WriteAllBytes($storedPath, $partB)

$expectedHash = (Get-FileHash -LiteralPath $expectedPath -Algorithm SHA256).Hash
$storedHash = (Get-FileHash -LiteralPath $storedPath -Algorithm SHA256).Hash

$outputDir = Join-Path $WorkDir 'recovered'
if (Test-Path $outputDir) { Remove-Item $outputDir -Recurse -Force }

Write-Host ''
Write-Host '=== running --mft-recover ==='
$scanOutput = Invoke-Carver -Arguments @($imagePath, $outputDir, '--mft-recover')
Write-Host $scanOutput.TrimEnd()

$failures = @()

$csvPath = Join-Path $outputDir 'recovered.csv'
if (-not (Test-Path $csvPath)) {
    Write-Host '  [FAIL] recovered.csv was not written'
    Write-Host ''
    Write-Host 'FAILED: no index'
    exit 1
}

$rows = @(Import-Csv -LiteralPath $csvPath)
$byOriginal = @{}
foreach ($row in $rows) { $byOriginal[$row.original_name] = $row }

Write-Host ''
Write-Host '=== verifying ==='

if ($rows.Count -eq 2) {
    Write-Host ("  [ok]   2 files indexed (got {0})" -f $rows.Count)
} else {
    Write-Host ("  [FAIL] expected 2 files, got {0}" -f $rows.Count)
    $failures += 'count'
}

function Get-OutputPath { param([string]$Name) return (Join-Path $outputDir $byOriginal[$Name].output_name) }

if ($byOriginal.ContainsKey('compressed.txt')) {
    $row = $byOriginal['compressed.txt']
    $path = Get-OutputPath 'compressed.txt'
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($actual -eq $expectedHash) {
        Write-Host '  [ok]   compressed.txt restored byte for byte (compressed + verbatim units)'
    } else {
        $size = (Get-Item -LiteralPath $path).Length
        Write-Host ("  [FAIL] compressed.txt does not match ({0} bytes, expected {1})" -f $size, $expected.Length)
        $failures += 'compressed'
    }
    if ($row.overwrite_risk -eq 'no' -and $row.clusters_free -eq 'yes') {
        Write-Host '  [ok]   compressed.txt reported as recoverable, clusters still free'
    } else {
        Write-Host ("  [FAIL] compressed.txt flags are wrong (overwrite_risk={0}, clusters_free={1})" -f $row.overwrite_risk, $row.clusters_free)
        $failures += 'flags'
    }
} else {
    Write-Host '  [FAIL] compressed.txt was not recovered'
    $failures += 'compressed'
}

if ($byOriginal.ContainsKey('stored.bin')) {
    $path = Get-OutputPath 'stored.bin'
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($actual -eq $storedHash) {
        Write-Host '  [ok]   stored.bin restored byte for byte (fully allocated unit kept verbatim)'
    } else {
        $size = (Get-Item -LiteralPath $path).Length
        Write-Host ("  [FAIL] stored.bin does not match ({0} bytes, expected {1})" -f $size, $partB.Length)
        $failures += 'stored'
    }
} else {
    Write-Host '  [FAIL] stored.bin was not recovered'
    $failures += 'stored'
}

Write-Host ''
if ($failures.Count -eq 0) {
    Write-Host 'ALL CHECKS PASSED'
    exit 0
}

Write-Host ("FAILED: {0}" -f ($failures -join ', '))
exit 1
