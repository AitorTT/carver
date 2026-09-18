#Requires -Version 5.1

<#
    Real-world carving test for carver-cli.

    Embeds actual image files shipped with Windows (produced by encoders other than
    the ones the synthetic test uses) into a random-junk image, carves it, and
    requires every original to come back byte-identical with no extra files.

    This is the test that catches embedded-thumbnail truncation: several Windows
    touch-keyboard wallpapers carry an EXIF thumbnail whose EOI marker appears
    before the real end of the JPEG, so a naive FF D9 footer scan truncates them.

    No administrator rights required: the carver reads a plain image file.
#>

[CmdletBinding()]
param(
    [string] $CarverExe,
    [string] $WorkDir,
    [int]    $RandomSeed = 99,
    [string] $SourceRoot = "$env:WINDIR\Web"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptRoot = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $CarverExe) { $CarverExe = Join-Path $scriptRoot '..\build\carver-cli.exe' }
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\real' }
$CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir   = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $CarverExe)) {
    throw "carver-cli.exe not found at '$CarverExe'. Build it first."
}

$sources = @()
if (Test-Path -LiteralPath $SourceRoot) {
    $sources = @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -Include *.jpg, *.png -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Length -gt 0 -and $_.Length -lt 3000000 } |
        Sort-Object FullName)
}

if ($sources.Count -eq 0) {
    Write-Host "no source images under '$SourceRoot' - skipping real-image test"
    exit 0
}

$outputDir = Join-Path $WorkDir 'recovered'
$imagePath = Join-Path $WorkDir 'real.img'

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

$rng = New-Object System.Random($RandomSeed)
$block = New-Object byte[] 262144

$stream = [System.IO.File]::Create($imagePath)
$expectations = @()
try {
    foreach ($source in $sources) {
        $junkLength = 200000 + $rng.Next(0, 300000)
        $written = 0
        while ($written -lt $junkLength) {
            $rng.NextBytes($block)
            $take = [int] [Math]::Min($block.Length, $junkLength - $written)
            $stream.Write($block, 0, $take)
            $written += $take
        }

        $bytes = [System.IO.File]::ReadAllBytes($source.FullName)
        $stream.Write($bytes, 0, $bytes.Length)
        $expectations += @{
            Name = $source.Name
            Hash = (Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash
        }
    }

    $rng.NextBytes($block)
    $stream.Write($block, 0, $block.Length)
} finally {
    $stream.Dispose()
}

Write-Host '=== real-image carve test ==='
Write-Host ("  source images : {0}" -f $sources.Count)
Write-Host ("  image size    : {0} bytes" -f (Get-Item -LiteralPath $imagePath).Length)
Write-Host ''

& $CarverExe $imagePath $outputDir
if ($LASTEXITCODE -ne 0) { throw "carver-cli exited with $LASTEXITCODE" }

Write-Host ''
Write-Host '=== verifying ==='

$recovered = @(Get-ChildItem -LiteralPath $outputDir -File)
$recoveredHashes = @{}
foreach ($file in $recovered) {
    $recoveredHashes[(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash] = $file.Name
}

$failures = @()
$exact = 0

foreach ($expectation in $expectations) {
    if ($recoveredHashes.ContainsKey($expectation.Hash)) {
        ++$exact
    } else {
        Write-Host ("  [FAIL] {0} not recovered byte-identical" -f $expectation.Name)
        $failures += $expectation.Name
    }
}

Write-Host ("  byte-identical : {0} / {1}" -f $exact, $expectations.Count)
Write-Host ("  files recovered: {0} (expected {1})" -f $recovered.Count, $expectations.Count)

if ($recovered.Count -ne $expectations.Count) {
    Write-Host '  [FAIL] recovered count differs from embedded count'
    $failures += 'count'
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f (($failures | Select-Object -First 10) -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
