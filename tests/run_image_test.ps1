#Requires -Version 5.1

<#
    Tests --image.

    Covers three things:
      1. a whole-source image is byte-identical, with the SHA-256 the tool
         reports matching a hash computed independently by PowerShell
      2. a partial range image contains exactly that slice
      3. a recovery run against the *copy* produces the same files, byte for
         byte, as a run against the original - which is the point of imaging

    No administrator rights required: the source is a plain image file.
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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\imaging' }
$CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir   = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $CarverExe)) { throw "carver-cli.exe not found at '$CarverExe'. Build it first." }

function Get-Sha256 {
    param([string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower()
}

function Get-SliceSha256 {
    param([string] $Path, [int] $Offset, [int] $Length)

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $buffer = New-Object byte[] $Length
        $stream.Position = $Offset
        $read = 0
        while ($read -lt $Length) {
            $got = $stream.Read($buffer, $read, $Length - $read)
            if ($got -le 0) { break }
            $read += $got
        }
        if ($read -ne $Length) { throw "short read: $read of $Length" }
    } finally {
        $stream.Dispose()
    }

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ((($sha.ComputeHash($buffer)) | ForEach-Object { '{0:x2}' -f $_ }) -join '')
    } finally {
        $sha.Dispose()
    }
}

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

$failures = @()

# Build a source image with known deleted-file contents by reusing the MFT test.
$mftTest = Join-Path $scriptRoot 'run_mft_recover_test.ps1'
if (-not (Test-Path -LiteralPath $mftTest)) { throw "missing $mftTest" }
& powershell -NoProfile -ExecutionPolicy Bypass -File $mftTest | Out-Null

$source = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot '..\testdata\mft\ntfs_deleted.img'))
if (-not (Test-Path -LiteralPath $source)) { throw "test source image was not produced" }

$sourceSize = (Get-Item -LiteralPath $source).Length

Write-Host '=== imaging ==='
Write-Host ("  source: {0} ({1} bytes)" -f $source, $sourceSize)

# --- 1. whole image ---
$fullCopy = Join-Path $WorkDir 'full.img'
$fullOutput = & $CarverExe --image $source $fullCopy 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    Write-Host '  [FAIL] whole-image copy failed'
    Write-Host ($fullOutput.TrimEnd())
    $failures += 'full-run'
}

if (Test-Path -LiteralPath $fullCopy) {
    $expected = Get-Sha256 $source
    $actual = Get-Sha256 $fullCopy

    $reported = [regex]::Match($fullOutput, 'sha256\s+:\s+([0-9a-f]{64})')
    if ($reported.Success) {
        if ($reported.Groups[1].Value -eq $expected) {
            Write-Host '  [ok]   reported SHA-256 matches an independent hash of the source'
        } else {
            Write-Host ("  [FAIL] reported hash {0} != source hash {1}" -f $reported.Groups[1].Value, $expected)
            $failures += 'full-hash-reported'
        }
    } else {
        Write-Host '  [FAIL] no SHA-256 reported'
        $failures += 'full-hash-missing'
    }

    if ($actual -eq $expected) {
        Write-Host ("  [ok]   whole image is byte-identical ({0} bytes)" -f (Get-Item -LiteralPath $fullCopy).Length)
    } else {
        Write-Host '  [FAIL] whole image differs from the source'
        $failures += 'full-content'
    }
} else {
    Write-Host '  [FAIL] whole-image file was not created'
    $failures += 'full-missing'
}

# --- 2. partial range ---
$offset = 4096
$length = 77824
$partialCopy = Join-Path $WorkDir 'partial.img'
$partialOutput = & $CarverExe --image $source $partialCopy --start $offset --end ($offset + $length) 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    Write-Host '  [FAIL] partial-image copy failed'
    $failures += 'partial-run'
}

if (Test-Path -LiteralPath $partialCopy) {
    $expectedSlice = Get-SliceSha256 $source $offset $length
    $actualSlice = Get-Sha256 $partialCopy
    $partialSize = (Get-Item -LiteralPath $partialCopy).Length

    if ($partialSize -eq $length) {
        Write-Host ("  [ok]   partial image is {0} bytes (requested {1})" -f $partialSize, $length)
    } else {
        Write-Host ("  [FAIL] partial image is {0} bytes, expected {1}" -f $partialSize, $length)
        $failures += 'partial-size'
    }

    if ($actualSlice -eq $expectedSlice) {
        Write-Host ("  [ok]   partial image matches bytes {0}..{1} exactly" -f $offset, ($offset + $length))
    } else {
        Write-Host '  [FAIL] partial image does not match the requested slice'
        $failures += 'partial-content'
    }
} else {
    Write-Host '  [FAIL] partial-image file was not created'
    $failures += 'partial-missing'
}

# --- 3. recovery from the copy must equal recovery from the original ---
Write-Host ''
Write-Host '=== recovery from the copy ==='

$originalOut = Join-Path $WorkDir 'from-original'
$copyOut = Join-Path $WorkDir 'from-copy'

$null = & $CarverExe $source $originalOut --mft-recover 2>&1
$null = & $CarverExe $fullCopy $copyOut --mft-recover 2>&1

$originalCsv = Join-Path $originalOut 'recovered.csv'
$copyCsv = Join-Path $copyOut 'recovered.csv'

if (-not (Test-Path $originalCsv) -or -not (Test-Path $copyCsv)) {
    Write-Host '  [FAIL] one of the recovery runs produced no index'
    $failures += 'recovery-missing'
} else {
    $originalRows = @(Import-Csv -LiteralPath $originalCsv)
    $copyRows = @(Import-Csv -LiteralPath $copyCsv)

    if ($originalRows.Count -eq 0) {
        Write-Host '  [FAIL] the reference recovery found nothing, so the comparison is meaningless'
        $failures += 'recovery-empty'
    } elseif ($originalRows.Count -ne $copyRows.Count) {
        Write-Host ("  [FAIL] copy recovered {0} files, original recovered {1}" -f $copyRows.Count, $originalRows.Count)
        $failures += 'recovery-count'
    } else {
        Write-Host ("  [ok]   same file count from both ({0})" -f $copyRows.Count)

        $mismatched = 0
        foreach ($row in $originalRows) {
            $match = $copyRows | Where-Object { $_.original_name -eq $row.original_name } | Select-Object -First 1
            if ($null -eq $match) {
                ++$mismatched
                continue
            }
            $a = Get-Sha256 (Join-Path $originalOut $row.output_name)
            $b = Get-Sha256 (Join-Path $copyOut $match.output_name)
            if ($a -ne $b) { ++$mismatched }
        }

        if ($mismatched -eq 0) {
            Write-Host '  [ok]   every recovered file is byte-identical from both sources'
        } else {
            Write-Host ("  [FAIL] {0} recovered file(s) differ" -f $mismatched)
            $failures += 'recovery-content'
        }
    }
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
