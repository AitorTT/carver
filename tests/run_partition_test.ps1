#Requires -Version 5.1

<#
    Tests MBR and GPT partition parsing, and recovering from a partition that
    sits inside a whole-disk image.

    Reuses the synthetic NTFS image from the MFT test as the partition payload,
    then wraps it in two disk images:

        an MBR disk  - one type 0x07 entry at LBA 2048
        a GPT disk   - a protective MBR, a GPT header and one basic-data entry

    For each, --partitions must describe it, and --mft-recover must find the
    volume on its own and produce byte-identical files to recovering from the
    bare volume.

    No administrator rights required: everything is a plain image file.
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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\partition' }
$CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir   = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $CarverExe)) { throw "carver-cli.exe not found at '$CarverExe'. Build it first." }

function Set-U16 { param([byte[]]$Buf,[int]$Offset,[uint16]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,2) }
function Set-U32 { param([byte[]]$Buf,[int]$Offset,[uint32]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,4) }
function Set-U64 { param([byte[]]$Buf,[int]$Offset,[uint64]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,8) }
function Get-Sha256 { param([string]$Path) (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }

$sectorSize = 512
$partitionStartLba = 2048

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

# payload: the synthetic NTFS volume with three deleted files
$mftTest = Join-Path $scriptRoot 'run_mft_recover_test.ps1'
& powershell -NoProfile -ExecutionPolicy Bypass -File $mftTest | Out-Null
$payloadPath = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot '..\testdata\mft\ntfs_deleted.img'))
if (-not (Test-Path -LiteralPath $payloadPath)) { throw "missing the NTFS payload image" }

$payload = [System.IO.File]::ReadAllBytes($payloadPath)
$payloadSectors = [int][Math]::Ceiling($payload.Length / $sectorSize)

# reference: recover straight from the bare volume
$referenceOut = Join-Path $WorkDir 'reference'
$null = & $CarverExe $payloadPath $referenceOut --mft-recover 2>&1
$referenceCsv = Join-Path $referenceOut 'recovered.csv'
if (-not (Test-Path $referenceCsv)) { throw "reference recovery produced no index" }
$referenceRows = @(Import-Csv -LiteralPath $referenceCsv)
if ($referenceRows.Count -eq 0) { throw "reference recovery found nothing" }

Write-Host ("payload : {0} bytes ({1} sectors), reference recovers {2} files" -f `
            $payload.Length, $payloadSectors, $referenceRows.Count)

$failures = @()

function Test-DiskImage {
    param([string] $Label, [string] $ImagePath, [string] $SchemeExpected)

    Write-Host ''
    Write-Host ("=== {0} ===" -f $Label)

    # --partitions
    $listing = & $CarverExe --partitions $ImagePath 2>&1 | Out-String
    Write-Host ($listing.TrimEnd())

    if ($listing -match [regex]::Escape($SchemeExpected)) {
        Write-Host ("  [ok]   scheme reported as {0}" -f $SchemeExpected)
    } else {
        Write-Host ("  [FAIL] scheme {0} not reported" -f $SchemeExpected)
        $script:failures += "$Label-scheme"
    }

    if ($listing -match 'NTFS / exFAT|Basic data') {
        Write-Host '  [ok]   partition typed as NTFS / basic data'
    } else {
        Write-Host '  [FAIL] partition type not recognised'
        $script:failures += "$Label-type"
    }

    if ($listing -match 'NTFS boot sector present') {
        Write-Host '  [ok]   NTFS boot sector located inside the partition'
    } else {
        Write-Host '  [FAIL] NTFS boot sector not located'
        $script:failures += "$Label-boot"
    }

    # auto-detection: recover without being told which partition
    $autoOut = Join-Path $WorkDir "$Label-auto"
    $autoLog = & $CarverExe $ImagePath $autoOut --mft-recover 2>&1 | Out-String
    if ($autoLog -match 'auto-selected partition') {
        Write-Host '  [ok]   volume auto-selected without --partition'
    } else {
        Write-Host '  [FAIL] auto-selection did not happen'
        $script:failures += "$Label-auto-detect"
    }

    # explicit selection
    $explicitOut = Join-Path $WorkDir "$Label-explicit"
    $null = & $CarverExe $ImagePath $explicitOut --mft-recover --partition 1 2>&1

    foreach ($variant in @(@{n='auto'; p=$autoOut}, @{n='explicit'; p=$explicitOut})) {
        $csv = Join-Path $variant.p 'recovered.csv'
        if (-not (Test-Path $csv)) {
            Write-Host ("  [FAIL] {0} run produced no index" -f $variant.n)
            $script:failures += "$Label-$($variant.n)-missing"
            continue
        }

        $rows = @(Import-Csv -LiteralPath $csv)
        if ($rows.Count -ne $referenceRows.Count) {
            Write-Host ("  [FAIL] {0} recovered {1} files, reference had {2}" -f $variant.n, $rows.Count, $referenceRows.Count)
            $script:failures += "$Label-$($variant.n)-count"
            continue
        }

        $mismatch = 0
        foreach ($reference in $referenceRows) {
            $match = $rows | Where-Object { $_.original_name -eq $reference.original_name } | Select-Object -First 1
            if ($null -eq $match) { ++$mismatch; continue }
            if ((Get-Sha256 (Join-Path $referenceOut $reference.output_name)) -ne
                (Get-Sha256 (Join-Path $variant.p $match.output_name))) { ++$mismatch }
        }

        if ($mismatch -eq 0) {
            Write-Host ("  [ok]   {0} run: all {1} files byte-identical to the bare-volume reference" -f $variant.n, $rows.Count)
        } else {
            Write-Host ("  [FAIL] {0} run: {1} file(s) differ" -f $variant.n, $mismatch)
            $script:failures += "$Label-$($variant.n)-content"
        }
    }
}

# ---------- MBR disk ----------
$mbrPath = Join-Path $WorkDir 'mbr-disk.img'
$mbrSize = $partitionStartLba * $sectorSize + $payload.Length + 1024 * 1024
$mbrDisk = New-Object byte[] $mbrSize
$mbrDisk[510] = 0x55; $mbrDisk[511] = 0xAA
$entry = 0x1BE
$mbrDisk[$entry + 0] = 0x80
$mbrDisk[$entry + 4] = 0x07
Set-U32 $mbrDisk ($entry + 8) ([uint32]$partitionStartLba)
Set-U32 $mbrDisk ($entry + 12) ([uint32]$payloadSectors)
[Array]::Copy($payload, 0, $mbrDisk, ($partitionStartLba * $sectorSize), $payload.Length)
[System.IO.File]::WriteAllBytes($mbrPath, $mbrDisk)

Test-DiskImage -Label 'mbr' -ImagePath $mbrPath -SchemeExpected 'MBR'

# ---------- GPT disk ----------
$gptPath = Join-Path $WorkDir 'gpt-disk.img'
$gptSize = $partitionStartLba * $sectorSize + $payload.Length + 1024 * 1024
$gptDisk = New-Object byte[] $gptSize

# protective MBR
$gptDisk[510] = 0x55; $gptDisk[511] = 0xAA
$gptDisk[$entry + 4] = 0xEE
Set-U32 $gptDisk ($entry + 8) ([uint32]1)
Set-U32 $gptDisk ($entry + 12) ([uint32]($gptSize / $sectorSize - 1))

# GPT header at LBA 1
$header = $sectorSize
[System.Text.Encoding]::ASCII.GetBytes('EFI PART').CopyTo($gptDisk, $header)
Set-U32 $gptDisk ($header + 0x08) ([uint32]0x00010000)
Set-U32 $gptDisk ($header + 0x0C) ([uint32]92)
Set-U64 $gptDisk ($header + 0x18) ([uint64]1)
Set-U64 $gptDisk ($header + 0x20) ([uint64]($gptSize / $sectorSize - 1))
Set-U64 $gptDisk ($header + 0x28) ([uint64]34)
Set-U64 $gptDisk ($header + 0x30) ([uint64]($gptSize / $sectorSize - 34))
Set-U64 $gptDisk ($header + 0x48) ([uint64]2)
Set-U32 $gptDisk ($header + 0x50) ([uint32]128)
Set-U32 $gptDisk ($header + 0x54) ([uint32]128)

# one basic-data entry, GUID {EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}
$entryOffset = 2 * $sectorSize
$basicData = [byte[]](0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7)
[Array]::Copy($basicData, 0, $gptDisk, $entryOffset, 16)
Set-U64 $gptDisk ($entryOffset + 0x20) ([uint64]$partitionStartLba)
Set-U64 $gptDisk ($entryOffset + 0x28) ([uint64]($partitionStartLba + $payloadSectors - 1))
$nameBytes = [System.Text.Encoding]::Unicode.GetBytes('Carver Test')
[Array]::Copy($nameBytes, 0, $gptDisk, ($entryOffset + 0x38), $nameBytes.Length)
[Array]::Copy($payload, 0, $gptDisk, ($partitionStartLba * $sectorSize), $payload.Length)
[System.IO.File]::WriteAllBytes($gptPath, $gptDisk)

Test-DiskImage -Label 'gpt' -ImagePath $gptPath -SchemeExpected 'GPT'

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
