#Requires -Version 5.1

<#
    Tests resumable imaging.

    An interrupted image used to be thrown away, so a multi-hour run over a
    failing drive had to start from zero. Now the partial file and a checkpoint
    are kept, and --resume continues from where it stopped.

    The interruption is produced through CARVER_IMAGE_STOP_AFTER, which makes
    the CLI's progress callback cancel after a given number of bytes - the same
    path a real cancellation takes.

    Checks:
      1. an interrupted run leaves a partial image plus a checkpoint
      2. resuming completes it and the result is byte-identical to the source,
         with the resumed SHA-256 matching an independently computed hash
      3. the checkpoint is removed once the image completes
      4. overwriting an existing image without --resume is refused
      5. resuming against a mismatched range is refused

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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\resume' }
$script:CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $script:CarverExe)) { throw "carver-cli.exe not found at '$script:CarverExe'. Build it first." }

# Native stderr becomes an ErrorRecord, which $ErrorActionPreference='Stop' would
# turn into a terminating error; the carver deliberately writes failures there.
function Invoke-Carver {
    param([string[]] $Arguments)

    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        return (& $script:CarverExe @Arguments 2>&1 | Out-String)
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Get-Sha256 { param([string]$Path) (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

# a source to interrupt partway through
$mftTest = Join-Path $scriptRoot 'run_mft_recover_test.ps1'
& powershell -NoProfile -ExecutionPolicy Bypass -File $mftTest | Out-Null
$source = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot '..\testdata\mft\ntfs_deleted.img'))
if (-not (Test-Path -LiteralPath $source)) { throw "missing the source image" }

$sourceSize = (Get-Item -LiteralPath $source).Length
$destination = Join-Path $WorkDir 'partial.img'
$statePath = "$destination.carver-state"
$failures = @()

Write-Host '=== interrupted image ==='
$stopAfter = [int]($sourceSize / 3)
# a small chunk so the cancel lands mid-image: cancellation granularity is one chunk
$chunk = '65536'
$env:CARVER_IMAGE_STOP_AFTER = "$stopAfter"
$interrupted = Invoke-Carver @('--image', $source, $destination, '--chunk', $chunk)
$interruptedExit = $LASTEXITCODE
Remove-Item Env:CARVER_IMAGE_STOP_AFTER
Write-Host ("  cancelled after ~{0} bytes (exit {1})" -f $stopAfter, $interruptedExit)

if ($interruptedExit -ne 0) {
    Write-Host '  [ok]   interrupted run reported failure'
} else {
    Write-Host '  [FAIL] interrupted run reported success'
    $failures += 'interrupt-exit'
}

if (Test-Path -LiteralPath $destination) {
    Write-Host ("  [ok]   partial image kept ({0} bytes of {1})" -f (Get-Item -LiteralPath $destination).Length, $sourceSize)
} else {
    Write-Host '  [FAIL] no partial image was kept'
    $failures += 'partial-missing'
}

if (Test-Path -LiteralPath $statePath) {
    Write-Host '  [ok]   checkpoint written'
} else {
    Write-Host '  [FAIL] no checkpoint written'
    $failures += 'state-missing'
}

if ($interrupted -match 'resume with') {
    Write-Host '  [ok]   output tells the user how to resume'
} else {
    Write-Host '  [FAIL] no resume hint printed'
    $failures += 'hint'
}

Write-Host ''
Write-Host '=== resuming ==='
$resumed = Invoke-Carver @('--image', $source, $destination, '--resume', '--chunk', $chunk)
$resumedExit = $LASTEXITCODE
Write-Host ($resumed.TrimEnd())

if ($resumedExit -eq 0) {
    Write-Host '  [ok]   resumed run succeeded'
} else {
    Write-Host '  [FAIL] resumed run failed'
    $failures += 'resume-exit'
}

if ($resumed -match 'resume : continuing from') {
    Write-Host '  [ok]   run reported that it was resuming'
} else {
    Write-Host '  [FAIL] run did not report resuming'
    $failures += 'resume-not-reported'
}

$expected = Get-Sha256 $source
if ((Get-Sha256 $destination) -eq $expected) {
    Write-Host '  [ok]   resumed image is byte-identical to the source'
} else {
    Write-Host '  [FAIL] resumed image differs from the source'
    $failures += 'resumed-content'
}

$reported = [regex]::Match($resumed, 'sha256\s+:\s+([0-9a-f]{64})')
if ($reported.Success -and $reported.Groups[1].Value -eq $expected) {
    Write-Host '  [ok]   resumed SHA-256 matches an independent hash of the source'
} else {
    Write-Host '  [FAIL] reported hash did not match'
    $failures += 'resumed-hash'
}

if (Test-Path -LiteralPath $statePath) {
    Write-Host '  [FAIL] checkpoint still present after a completed image'
    $failures += 'state-left'
} else {
    Write-Host '  [ok]   checkpoint removed on completion'
}

Write-Host ''
Write-Host '=== guards ==='

$overwrite = Invoke-Carver @('--image', $source, $destination)
if ($LASTEXITCODE -ne 0 -and $overwrite -match 'already exists') {
    Write-Host '  [ok]   existing image refused without --resume or --force'
} else {
    Write-Host '  [FAIL] existing image was not refused'
    $failures += 'overwrite-guard'
}

$forcedOut = Join-Path $WorkDir 'forced.img'
$null = Invoke-Carver @('--image', $source, $forcedOut)
$null = Invoke-Carver @('--image', $source, $forcedOut, '--force')
if ($LASTEXITCODE -eq 0 -and (Get-Sha256 $forcedOut) -eq $expected) {
    Write-Host '  [ok]   --force overwrites an existing image'
} else {
    Write-Host '  [FAIL] --force did not overwrite correctly'
    $failures += 'force'
}

# a checkpoint for one range must not be usable with another
$rangeDest = Join-Path $WorkDir 'range.img'
$env:CARVER_IMAGE_STOP_AFTER = "$stopAfter"
$null = Invoke-Carver @('--image', $source, $rangeDest, '--start', '0', '--end', '1048576', '--chunk', $chunk)
Remove-Item Env:CARVER_IMAGE_STOP_AFTER
$wrongRange = Invoke-Carver @('--image', $source, $rangeDest, '--start', '0', '--end', '524288', '--resume', '--chunk', $chunk)
if ($LASTEXITCODE -ne 0 -and $wrongRange -match 'different range') {
    Write-Host '  [ok]   resume refused when the requested range differs'
} else {
    Write-Host '  [FAIL] resume was allowed with a mismatched range'
    $failures += 'range-guard'
}

# and not with a different source either
$otherSource = Join-Path $WorkDir 'other.img'
[System.IO.File]::WriteAllBytes($otherSource, (New-Object byte[] 2097152))
$env:CARVER_IMAGE_STOP_AFTER = "$stopAfter"
$null = Invoke-Carver @('--image', $source, $rangeDest, '--start', '0', '--end', '1048576', '--chunk', $chunk)
Remove-Item Env:CARVER_IMAGE_STOP_AFTER
$wrongSource = Invoke-Carver @('--image', $otherSource, $rangeDest, '--start', '0', '--end', '1048576', '--resume', '--chunk', $chunk)
if ($LASTEXITCODE -ne 0 -and $wrongSource -match 'different source') {
    Write-Host '  [ok]   resume refused when the source differs'
} else {
    Write-Host '  [FAIL] resume was allowed with a different source'
    $failures += 'source-guard'
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
