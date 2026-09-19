#Requires -Version 5.1

<#
    Tests --mft-recover by building a synthetic NTFS image containing deleted
    $MFT records. No administrator rights and no format/mount required.

    Records laid out:
        0   $MFT          (in use, non-resident $DATA describing the MFT itself)
        6   $Bitmap       (in use, non-resident $DATA at cluster 64)
        20  holiday.png   DELETED, non-resident, data in FREE clusters
        21  notes.txt     DELETED, resident data (lives inside the MFT record)
        22  live.txt      IN USE  -> must not be recovered
        23  deleted_dir   DELETED but a directory -> must not be recovered
        24  overwritten.jpg DELETED, data in clusters now marked ALLOCATED
                                 -> recovered but flagged as overwrite risk
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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\mft' }
$CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir   = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $CarverExe)) { throw "carver-cli.exe not found at '$CarverExe'. Build it first." }

Add-Type -AssemblyName System.Drawing

$BytesPerSector    = 512
$SectorsPerCluster = 1
$BytesPerCluster   = 512
$TotalClusters     = 4096
$MftCluster        = 4
$MftRecordSize     = 1024
$BitmapCluster     = 64
$MftClusters       = 50
$MaxRecord         = 24

function Set-U16 { param([byte[]]$Buf,[int]$Offset,[uint16]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,2) }
function Set-U32 { param([byte[]]$Buf,[int]$Offset,[uint32]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,4) }
function Set-U64 { param([byte[]]$Buf,[int]$Offset,[uint64]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,8) }
function Set-Ascii { param([byte[]]$Buf,[int]$Offset,[string]$Text) $b=[System.Text.Encoding]::ASCII.GetBytes($Text); [Array]::Copy($b,0,$Buf,$Offset,$b.Length) }
function Align8 { param([int]$Value) return ($Value + 7) -band -8 }

function New-MftRecord {
    param(
        [bool]   $InUse = $false,
        [bool]   $Directory = $false,
        [string] $Name = '',
        [byte[]] $Resident = $null,
        [int]    $RunLcn = -1,
        [int]    $RunClusters = 0,
        [uint64] $RealSize = 0,
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
    if ($Name -ne '') {
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
        Set-U64 $record ($c+0x28) $RealSize
        Set-U64 $record ($c+0x30) $RealSize
        Set-U32 $record ($c+0x38) 0
        Set-U32 $record ($c+0x3C) 0
        $record[$c+0x40] = [byte]($nameBytes.Length / 2)
        $record[$c+0x41] = 1
        [Array]::Copy($nameBytes, 0, $record, $c+0x42, $nameBytes.Length)
        $pos += $length
    }

    # $DATA
    if ($null -ne $Resident) {
        $length = Align8 (0x18 + $Resident.Length)
        Set-U32 $record $pos 0x80
        Set-U32 $record ($pos+4) ([uint32]$length)
        $record[$pos+8] = 0; $record[$pos+9] = 0
        Set-U16 $record ($pos+0x0A) 0x18
        Set-U16 $record ($pos+0x0C) 0
        Set-U16 $record ($pos+0x0E) 2
        Set-U32 $record ($pos+0x10) ([uint32]$Resident.Length)
        Set-U16 $record ($pos+0x14) 0x18
        [Array]::Copy($Resident, 0, $record, $pos+0x18, $Resident.Length)
        $pos += $length
    } elseif ($RunLcn -ge 0) {
        $lengthBytes = New-Object System.Collections.Generic.List[byte]
        $value = $RunClusters
        while ($value -gt 0) { $lengthBytes.Add([byte]($value -band 0xFF)); $value = $value -shr 8 }
        if ($lengthBytes.Count -eq 0) { $lengthBytes.Add([byte]0) }

        $offsetBytes = New-Object System.Collections.Generic.List[byte]
        $value = $RunLcn
        while ($value -gt 0) { $offsetBytes.Add([byte]($value -band 0xFF)); $value = $value -shr 8 }
        if ($offsetBytes.Count -eq 0) { $offsetBytes.Add([byte]0) }
        if (($offsetBytes[$offsetBytes.Count-1] -band 0x80) -ne 0) { $offsetBytes.Add([byte]0) }

        $runList = New-Object System.Collections.Generic.List[byte]
        $runList.Add([byte]((($offsetBytes.Count -shl 4)) -bor $lengthBytes.Count))
        foreach ($b in $lengthBytes) { $runList.Add($b) }
        foreach ($b in $offsetBytes) { $runList.Add($b) }
        $runList.Add([byte]0)

        $runListOffset = 0x40
        $length = Align8 ($runListOffset + $runList.Count)
        Set-U32 $record $pos 0x80
        Set-U32 $record ($pos+4) ([uint32]$length)
        $record[$pos+8] = 1; $record[$pos+9] = 0
        Set-U16 $record ($pos+0x0A) ([uint16]$runListOffset)
        Set-U16 $record ($pos+0x0C) 0
        Set-U16 $record ($pos+0x0E) 2
        Set-U64 $record ($pos+0x10) ([uint64]0)
        Set-U64 $record ($pos+0x18) ([uint64]($RunClusters - 1))
        Set-U16 $record ($pos+0x20) ([uint16]$runListOffset)
        Set-U16 $record ($pos+0x22) 0
        Set-U64 $record ($pos+0x28) ([uint64]($RunClusters * $BytesPerCluster))
        Set-U64 $record ($pos+0x30) $RealSize
        Set-U64 $record ($pos+0x38) $RealSize
        [Array]::Copy($runList.ToArray(), 0, $record, $pos + $runListOffset, $runList.Count)
        $pos += $length
    }

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

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

Write-Host '=== building synthetic NTFS image with deleted records ==='

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

# source files to hide
$pngTemp = Join-Path $WorkDir 'holiday_src.png'
$jpgTemp = Join-Path $WorkDir 'overwritten_src.jpg'
$bitmap = New-Object System.Drawing.Bitmap(128, 128)
try {
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try { $graphics.Clear([System.Drawing.Color]::FromArgb(20, 140, 90)) } finally { $graphics.Dispose() }
    $bitmap.Save($pngTemp, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Save($jpgTemp, [System.Drawing.Imaging.ImageFormat]::Jpeg)
} finally { $bitmap.Dispose() }

$pngBytes = [System.IO.File]::ReadAllBytes($pngTemp)
$jpgBytes = [System.IO.File]::ReadAllBytes($jpgTemp)
$notesText = 'hello deleted world'
$notesBytes = [System.Text.Encoding]::ASCII.GetBytes($notesText)

$pngClusters = [Math]::Ceiling($pngBytes.Length / $BytesPerCluster)
$jpgClusters = [Math]::Ceiling($jpgBytes.Length / $BytesPerCluster)

$createdFileTime  = [System.DateTime]::new(2024, 1, 2, 3, 4, 5, [System.DateTimeKind]::Utc).ToFileTime()
$createdFileTime2 = [System.DateTime]::new(2023, 6, 7, 8, 9, 10, [System.DateTimeKind]::Utc).ToFileTime()

$PngCluster = 500
$JpgCluster = 300

$records = @{}
$records[0]  = New-MftRecord -InUse $true  -Name '$MFT'    -RunLcn $MftCluster -RunClusters $MftClusters -RealSize ([uint64]($MftClusters * $BytesPerCluster))
$records[6]  = New-MftRecord -InUse $true  -Name '$Bitmap' -RunLcn $BitmapCluster -RunClusters 1 -RealSize 512
$records[20] = New-MftRecord -InUse $false -Name 'holiday.png'     -RunLcn $PngCluster -RunClusters $pngClusters -RealSize ([uint64]$pngBytes.Length) -Created $createdFileTime  -Modified $createdFileTime
$records[21] = New-MftRecord -InUse $false -Name 'notes.txt'       -Resident $notesBytes -RealSize ([uint64]$notesBytes.Length) -Created $createdFileTime2 -Modified $createdFileTime2
$records[22] = New-MftRecord -InUse $true  -Name 'live.txt'        -Resident ([System.Text.Encoding]::ASCII.GetBytes('this file is still in use'))
$records[23] = New-MftRecord -InUse $false -Directory $true -Name 'deleted_dir' -Resident ([System.Text.Encoding]::ASCII.GetBytes('dir'))
$records[24] = New-MftRecord -InUse $false -Name 'overwritten.jpg' -RunLcn $JpgCluster -RunClusters $jpgClusters -RealSize ([uint64]$jpgBytes.Length) -Created $createdFileTime2 -Modified $createdFileTime2

$mftOffset = $MftCluster * $BytesPerCluster
foreach ($recordNumber in $records.Keys) {
    [Array]::Copy($records[$recordNumber], 0, $image, ($mftOffset + $recordNumber * $MftRecordSize), $MftRecordSize)
}

# bitmap: boot + MFT (0..53), $Bitmap (64) and 300..320 (reallocated) are allocated
$bmBytes = New-Object byte[] ($TotalClusters / 8)
$allocated = @(0..53) + @(64) + @(300..320)
foreach ($cluster in $allocated) {
    $byteIndex = $cluster -shr 3
    $bmBytes[$byteIndex] = $bmBytes[$byteIndex] -bor [byte](1 -shl ($cluster -band 7))
}
[Array]::Copy($bmBytes, 0, $image, ($BitmapCluster * $BytesPerCluster), $bmBytes.Length)

# file contents
[Array]::Copy($pngBytes, 0, $image, ($PngCluster * $BytesPerCluster), $pngBytes.Length)
[Array]::Copy($jpgBytes, 0, $image, ($JpgCluster * $BytesPerCluster), $jpgBytes.Length)

$imagePath = Join-Path $WorkDir 'ntfs_deleted.img'
[System.IO.File]::WriteAllBytes($imagePath, $image)

$pngHash = (Get-FileHash -LiteralPath $pngTemp -Algorithm SHA256).Hash
$jpgHash = (Get-FileHash -LiteralPath $jpgTemp -Algorithm SHA256).Hash

Write-Host ("  png   {0} bytes -> {1} clusters at LCN {2} (free)" -f $pngBytes.Length, $pngClusters, $PngCluster)
Write-Host ("  jpg   {0} bytes -> {1} clusters at LCN {2} (allocated)" -f $jpgBytes.Length, $jpgClusters, $JpgCluster)
Write-Host ("  notes {0} bytes (resident)" -f $notesBytes.Length)

$outputDir = Join-Path $WorkDir 'recovered'
if (Test-Path $outputDir) { Remove-Item $outputDir -Recurse -Force }

Write-Host ''
Write-Host '=== running --mft-recover ==='
$scanOutput = & $CarverExe $imagePath $outputDir --mft-recover 2>&1 | Out-String
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

function Get-OutputPath { param([string]$Name) return (Join-Path $outputDir $byOriginal[$Name].output_name) }

# 1. exactly three files recovered
if ($rows.Count -eq 3) {
    Write-Host ("  [ok]   3 files indexed (got {0})" -f $rows.Count)
} else {
    Write-Host ("  [FAIL] expected 3 files, got {0}" -f $rows.Count)
    $failures += 'count'
}

# 2. holiday.png byte-identical
if ($byOriginal.ContainsKey('holiday.png')) {
    $hash = (Get-FileHash -LiteralPath (Get-OutputPath 'holiday.png') -Algorithm SHA256).Hash
    if ($hash -eq $pngHash) {
        Write-Host ("  [ok]   holiday.png recovered byte-identical -> {0}" -f $byOriginal['holiday.png'].output_name)
    } else {
        Write-Host '  [FAIL] holiday.png differs from the original'
        $failures += 'png'
    }
} else {
    Write-Host '  [FAIL] holiday.png not recovered'
    $failures += 'png-missing'
}

# 3. notes.txt resident data exact
if ($byOriginal.ContainsKey('notes.txt')) {
    $text = [System.IO.File]::ReadAllText((Get-OutputPath 'notes.txt'), [System.Text.Encoding]::ASCII)
    if ($text -eq $notesText) {
        Write-Host ("  [ok]   notes.txt resident data exact ({0} bytes)" -f $text.Length)
    } else {
        Write-Host ("  [FAIL] notes.txt content was '{0}'" -f $text)
        $failures += 'notes'
    }
} else {
    Write-Host '  [FAIL] notes.txt not recovered'
    $failures += 'notes-missing'
}

# 4. overwritten.jpg recovered but flagged
if ($byOriginal.ContainsKey('overwritten.jpg')) {
    $hash = (Get-FileHash -LiteralPath (Get-OutputPath 'overwritten.jpg') -Algorithm SHA256).Hash
    $flagged = $byOriginal['overwritten.jpg'].overwrite_risk -eq 'yes'
    if ($hash -eq $jpgHash -and $flagged) {
        Write-Host '  [ok]   overwritten.jpg recovered and flagged overwrite_risk=yes'
    } else {
        Write-Host ("  [FAIL] overwritten.jpg hash match={0} flagged={1}" -f ($hash -eq $jpgHash), $flagged)
        $failures += 'jpg'
    }
} else {
    Write-Host '  [FAIL] overwritten.jpg not recovered'
    $failures += 'jpg-missing'
}

# 5. live file must not be recovered
if ($byOriginal.ContainsKey('live.txt')) {
    Write-Host '  [FAIL] live.txt was recovered even though it is still in use'
    $failures += 'live-leak'
} else {
    Write-Host '  [ok]   live.txt correctly skipped (in use)'
}

# 6. directory must not be recovered
if ($byOriginal.ContainsKey('deleted_dir')) {
    Write-Host '  [FAIL] deleted_dir was recovered even though it is a directory'
    $failures += 'dir-leak'
} else {
    Write-Host '  [ok]   deleted_dir correctly skipped (directory)'
}

# 7. timestamp conversion
if ($byOriginal.ContainsKey('holiday.png')) {
    if ($byOriginal['holiday.png'].created -eq '2024-01-02 03:04:05') {
        Write-Host '  [ok]   FILETIME converted correctly (2024-01-02 03:04:05)'
    } else {
        Write-Host ("  [FAIL] created timestamp was '{0}'" -f $byOriginal['holiday.png'].created)
        $failures += 'timestamp'
    }
}

# 8. reported counters
if ($scanOutput -match 'files written\s+:\s+3') {
    Write-Host '  [ok]   reported files written: 3'
} else {
    Write-Host '  [FAIL] reported files written did not match 3'
    $failures += 'reported'
}
if ($scanOutput -match 'possibly overwritten\s+:\s+1') {
    Write-Host '  [ok]   reported possibly overwritten: 1'
} else {
    Write-Host '  [FAIL] reported possibly overwritten did not match 1'
    $failures += 'reported-risk'
}

# 9. --skip-ext drops files by their original extension
Write-Host ''
Write-Host '=== running --mft-recover with --skip-ext png,jpg ==='

$skipDir = Join-Path $WorkDir 'recovered_skip'
if (Test-Path -LiteralPath $skipDir) { Remove-Item -LiteralPath $skipDir -Recurse -Force }

$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { $skipOutput = & $CarverExe $imagePath $skipDir --mft-recover --skip-ext png,jpg 2>&1 | Out-String }
finally { $ErrorActionPreference = $previous }

$skipCsv = Join-Path $skipDir 'recovered.csv'
if (-not (Test-Path -LiteralPath $skipCsv)) {
    Write-Host '  [FAIL] the skip run wrote no index'
    $failures += 'skip-index'
} else {
    $skipRows = @(Import-Csv -LiteralPath $skipCsv)
    $skipNames = @($skipRows | ForEach-Object { $_.original_name })
    if ($skipNames.Count -eq 1 -and $skipNames[0] -eq 'notes.txt') {
        Write-Host '  [ok]   only notes.txt survived --skip-ext png,jpg'
    } else {
        Write-Host ("  [FAIL] --skip-ext left {0} file(s): {1}" -f $skipNames.Count, ($skipNames -join ', '))
        $failures += 'skip-ext'
    }
}

if ($skipOutput -match 'skipping\s*:\s*\.png, \.jpg') {
    Write-Host '  [ok]   the run reported what it was skipping'
} else {
    Write-Host '  [FAIL] the skipping line was not reported'
    $failures += 'skip-report'
}

# 10. --list-only lists the same entries and sizes but copies no data
Write-Host ''
Write-Host '=== running --mft-recover with --list-only ==='

$listDir = Join-Path $WorkDir 'recovered_list'
if (Test-Path -LiteralPath $listDir) { Remove-Item -LiteralPath $listDir -Recurse -Force }

$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { $listOutput = & $CarverExe $imagePath $listDir --mft-recover --list-only 2>&1 | Out-String }
finally { $ErrorActionPreference = $previous }

$listData = @(Get-ChildItem -LiteralPath $listDir -File -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -ne 'recovered.csv' })
if ($listData.Count -eq 0) {
    Write-Host '  [ok]   no data was written, only the index'
} else {
    Write-Host ("  [FAIL] --list-only wrote {0} data file(s)" -f $listData.Count)
    $failures += 'list-wrote'
}

$listCsv = Join-Path $listDir 'recovered.csv'
if (Test-Path -LiteralPath $listCsv) {
    $listNames = @(Import-Csv -LiteralPath $listCsv | ForEach-Object { $_.original_name })
    if ($listNames.Count -eq $rows.Count) {
        Write-Host ("  [ok]   the index still lists all {0} files" -f $listNames.Count)
    } else {
        Write-Host ("  [FAIL] the index lists {0} of {1} files" -f $listNames.Count, $rows.Count)
        $failures += 'list-index'
    }
} else {
    Write-Host '  [FAIL] --list-only wrote no index'
    $failures += 'list-index'
}

if ($listOutput -match 'files listed\s+:\s+3') {
    Write-Host '  [ok]   reported 3 files listed'
} else {
    Write-Host '  [FAIL] the listed file count did not match 3'
    $failures += 'list-count'
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0