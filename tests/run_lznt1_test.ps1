#Requires -Version 5.1

<#
    Tests LZNT1 decompression against Microsoft's own compressor.

    Rather than trusting a hand written encoder, this compresses known data with
    ntdll's RtlCompressBuffer using COMPRESSION_FORMAT_LZNT1 - the same code
    path NTFS uses - writes the result, decompresses it with --lznt1 and
    requires the original back byte for byte.

    Cases cover the shapes that matter:
      * highly repetitive text, which produces back references
      * data spanning several 4096 byte chunks
      * incompressible data, which the compressor stores as literal chunks
      * a real source file from this repository
      * every size from 1 to 9000 bytes, to catch off-by-one handling

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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\lznt1' }
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

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

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
$STATUS_SUCCESS = 0

$compressWorkSpace = [uint32]0
$fragmentWorkSpace = [uint32]0
$status = [Lznt1Native]::RtlGetCompressionWorkSpaceSize($LZNT1_FORMAT, [ref]$compressWorkSpace, [ref]$fragmentWorkSpace)
if ($status -ne $STATUS_SUCCESS) { throw "RtlGetCompressionWorkSpaceSize failed with $status" }

$workspace = [System.Runtime.InteropServices.Marshal]::AllocHGlobal([int]$compressWorkSpace)

function Compress-Lznt1 {
    param([byte[]] $Data)

    if ($Data.Length -eq 0) { return New-Object byte[] 0 }

    $capacity = [int]($Data.Length * 3 + 65536)
    $buffer = New-Object byte[] $capacity
    [uint32]$final = 0

    $result = [Lznt1Native]::RtlCompressBuffer($LZNT1_FORMAT, $Data, [uint32]$Data.Length,
                                              $buffer, [uint32]$capacity, [uint32]4096,
                                              [ref]$final, $workspace)
    if ($result -ne 0) {
        # RtlCompressBuffer declines an all-zero buffer instead of compressing it,
        # returning an informational status rather than a stream.
        $nonZero = 0
        foreach ($b in $Data) { if ($b -ne 0) { ++$nonZero; break } }
        if ($nonZero -eq 0) { return $null }
        throw ("RtlCompressBuffer failed with 0x{0:X8}" -f $result)
    }

    $out = New-Object byte[] $final
    [Array]::Copy($buffer, 0, $out, 0, [int]$final)
    return $out
}

function Test-RoundTrip {
    param([string] $Label, [byte[]] $Original, [switch] $Quiet)

    $compressedPath = Join-Path $WorkDir 'compressed.bin'
    $outputPath = Join-Path $WorkDir 'output.bin'
    Remove-Item $compressedPath, $outputPath -ErrorAction SilentlyContinue

    $compressed = Compress-Lznt1 -Data $Original
    if ($null -eq $compressed) {
        Write-Host ("  [skip] {0}: compressor reports an all-zero buffer" -f $Label)
        return $true
    }
    [System.IO.File]::WriteAllBytes($compressedPath, $compressed)

    $log = Invoke-Carver @('--lznt1', $compressedPath, $outputPath)
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("  [FAIL] {0}: decompression failed" -f $Label)
        Write-Host ($log.TrimEnd())
        return $false
    }

    if (-not (Test-Path -LiteralPath $outputPath)) {
        Write-Host ("  [FAIL] {0}: no output written" -f $Label)
        return $false
    }

    $restored = [System.IO.File]::ReadAllBytes($outputPath)
    if ($restored.Length -ne $Original.Length) {
        Write-Host ("  [FAIL] {0}: {1} bytes restored, expected {2}" -f $Label, $restored.Length, $Original.Length)
        return $false
    }

    for ($i = 0; $i -lt $Original.Length; ++$i) {
        if ($restored[$i] -ne $Original[$i]) {
            Write-Host ("  [FAIL] {0}: first difference at offset {1}" -f $Label, $i)
            return $false
        }
    }

    $ratio = if ($Original.Length -gt 0) { [math]::Round(100.0 * $compressed.Length / $Original.Length, 1) } else { 100.0 }
    if (-not $Quiet) {
        Write-Host ("  [ok]   {0}: {1} -> {2} bytes ({3}%), restored exactly" -f $Label, $Original.Length, $compressed.Length, $ratio)
    }
    return $true
}

$failures = @()

Write-Host '=== LZNT1 round trips against ntdll ==='

# 1. repetitive text: forces back references
$repetitive = [System.Text.Encoding]::ASCII.GetBytes(("the quick brown fox jumps over the lazy dog`r`n" * 40))
if (-not (Test-RoundTrip -Label 'repetitive text' -Original $repetitive)) { $failures += 'repetitive' }

# 2. larger than one 4096 byte chunk
$multiChunk = [System.Text.Encoding]::ASCII.GetBytes(("abcdefghijklmnopqrstuvwxyz0123456789" * 800))
if (-not (Test-RoundTrip -Label 'multi-chunk (28 KB)' -Original $multiChunk)) { $failures += 'multi-chunk' }

# 3. incompressible data: the compressor stores literal chunks
$rng = New-Object System.Random(20260918)
$random = New-Object byte[] 20000
$rng.NextBytes($random)
if (-not (Test-RoundTrip -Label 'incompressible random' -Original $random)) { $failures += 'random' }

# 4. a real source file
$sourceFile = Join-Path $scriptRoot '..\src\core\lznt1.cpp'
if (Test-Path -LiteralPath $sourceFile) {
    $real = [System.IO.File]::ReadAllBytes($sourceFile)
    if (-not (Test-RoundTrip -Label 'real source file' -Original $real)) { $failures += 'source' }
}

# 5. structured text with long runs, which exercises longer matches
$runs = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt 500; ++$i) { [void]$runs.Append('A' * 50); [void]$runs.Append($i.ToString('D4')) }
$runsBytes = [System.Text.Encoding]::ASCII.GetBytes($runs.ToString())
if (-not (Test-RoundTrip -Label 'long runs' -Original $runsBytes)) { $failures += 'runs' }

# 6. every size from 1 to 9000, which catches off-by-one handling at chunk edges
Write-Host ''
Write-Host '=== exhaustive sizes 1..9000 ==='
$sizeFailures = 0
for ($size = 1; $size -le 9000; ++$size) {
    $data = New-Object byte[] $size
    for ($i = 0; $i -lt $size; ++$i) { $data[$i] = [byte](($i * 7 + ($i -shr 3)) -band 0xFF) }
    $data[0] = 1     # keep the buffer non-zero; an all-zero input is declined by the API
    if (-not (Test-RoundTrip -Label ("size {0}" -f $size) -Original $data -Quiet)) {
        Write-Host ("  [FAIL] size {0} did not round trip" -f $size)
        ++$sizeFailures
        if ($sizeFailures -gt 3) { break }
    }
}
if ($sizeFailures -eq 0) {
    Write-Host '  [ok]   all 9000 sizes from 1 to 9000 round tripped exactly'
} else {
    Write-Host ("  [FAIL] {0} size(s) failed" -f $sizeFailures)
    $failures += 'sizes'
}

[System.Runtime.InteropServices.Marshal]::FreeHGlobal($workspace)

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
