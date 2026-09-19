#Requires -Version 5.1

<#
    Tests the read windowing in RawDevice::readAt.

    Volume and physical disk handles only accept reads whose offset and length
    are multiples of the sector size, so readAt rounds the request outward, reads
    into a sector aligned buffer and copies back just the window the caller asked
    for. That rounding is invisible for aligned requests but easy to get wrong for
    the unaligned ones, and it broke reading $Bitmap on real drives, whose size is
    ceil(clusters / 8) and therefore almost never sector aligned.

    A file backed device is used here because the rounding logic is identical and
    no administrator rights are needed. The check is that --probe returns exactly
    the bytes at the requested offset and length, including requests that are
    unaligned at either end and requests that run past the end of the file.
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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\probe' }
$script:CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $script:CarverExe)) { throw "carver-cli.exe not found at '$script:CarverExe'. Build it first." }

function Invoke-Carver {
    param([string[]] $Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { return (& $script:CarverExe @Arguments 2>&1 | Out-String) }
    finally { $ErrorActionPreference = $previous }
}

function Get-DumpedBytes {
    param([string] $Output)

    $bytes = New-Object System.Collections.Generic.List[byte]
    foreach ($line in ($Output -split "`r?`n")) {
        if ($line -notmatch '^[0-9A-F]{8}  ') { continue }
        # the line is "offset  hex columns |ascii|", and the ascii column can hold
        # letters that look like hex pairs, so only read up to the first bar
        $hexPart = ($line -split '\|')[0]
        if ($hexPart.Length -le 10) { continue }
        $columns = $hexPart.Substring(10)
        $count = 0
        foreach ($match in [regex]::Matches($columns, '[0-9A-F]{2}')) {
            if ($count -ge 16) { break }
            $bytes.Add([Convert]::ToByte($match.Value, 16))
            ++$count
        }
    }
    return ,$bytes.ToArray()
}

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

Write-Host '=== building a known image file ==='

$size = 4096
$content = New-Object byte[] $size
for ($i = 0; $i -lt $size; ++$i) { $content[$i] = [byte]($i -band 0xFF) }

$imagePath = Join-Path $WorkDir 'pattern.bin'
[System.IO.File]::WriteAllBytes($imagePath, $content)
Write-Host ("  {0} bytes, byte[i] = i & 0xFF" -f $size)

$failures = @()

function Test-Probe {
    param([string] $Label, [int] $Offset, [int] $Length, [int] $ExpectedCount)

    $output = Invoke-Carver -Arguments @('--probe', $imagePath, '--offset', "$Offset", '--length', "$Length")
    $actual = Get-DumpedBytes -Output $output

    if ($actual.Count -ne $ExpectedCount) {
        Write-Host ("  [FAIL] {0}: dumped {1} bytes, expected {2}" -f $Label, $actual.Count, $ExpectedCount)
        $script:failures += $Label
        return
    }

    for ($i = 0; $i -lt $ExpectedCount; ++$i) {
        $want = $content[$Offset + $i]
        if ($actual[$i] -ne $want) {
            Write-Host ("  [FAIL] {0}: byte {1} is 0x{2:X2}, expected 0x{3:X2}" -f $Label, $i, $actual[$i], $want)
            $script:failures += $Label
            return
        }
    }

    if ($output -notmatch ("{0} bytes read" -f $ExpectedCount)) {
        Write-Host ("  [FAIL] {0}: did not report {1} bytes read" -f $Label, $ExpectedCount)
        $script:failures += $Label
        return
    }

    Write-Host ("  [ok]   {0}: {1} bytes from offset {2} are correct" -f $Label, $ExpectedCount, $Offset)
}

Write-Host ''
Write-Host '=== probing ==='

# sector aligned, the case that always worked
Test-Probe -Label 'aligned' -Offset 0 -Length 512 -ExpectedCount 512

# unaligned at both ends, the shape that breaks a raw volume read
Test-Probe -Label 'unaligned at both ends' -Offset 100 -Length 300 -ExpectedCount 300

# a single unaligned byte
Test-Probe -Label 'single unaligned byte' -Offset 513 -Length 1 -ExpectedCount 1

# crosses several sectors with a ragged tail
Test-Probe -Label 'ragged tail' -Offset 777 -Length 1000 -ExpectedCount 1000

# runs past the end of the file, so the read must come back short
Test-Probe -Label 'short read at end' -Offset 4000 -Length 200 -ExpectedCount 96

Write-Host ''
if ($failures.Count -eq 0) {
    Write-Host 'ALL CHECKS PASSED'
    exit 0
}

Write-Host ("FAILED: {0}" -f ($failures -join ', '))
exit 1
