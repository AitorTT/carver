#Requires -Version 5.1

<#
    Tests --fat-recover by building a synthetic FAT32 image with deleted
    directory entries. No administrator rights and no format/mount required.

    The volume layout:
        cluster 2   root directory
        cluster 3   live subdirectory SUBDIR

    Files placed:
        "holiday photo.png"  DELETED, long name, data in FREE clusters 10/11
        over~1.jpg           DELETED, long name, data where cluster 41 has
                             been reallocated -> recovered but flagged
        "NOTES   TXT"        DELETED, 8.3 only -> first character is lost, so
                             the recovered name is OTES.TXT
        "LIVE    TXT"        IN USE  -> must not be recovered
        SUBDIR               IN USE directory -> walked, holds:
            "inner image.png" DELETED, long name, data in FREE clusters 70/71
        "DELDIR"             DELETED directory -> must not be recovered
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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\fat_recover' }
$CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir   = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $CarverExe)) { throw "carver-cli.exe not found at '$CarverExe'. Build it first." }

Add-Type -AssemblyName System.Drawing

$Bps         = 512
$Spc         = 1
$Reserved    = 32
$FatSectors  = 520
$TotalSectors = $Reserved + $FatSectors + 66000
$DataStart   = ($Reserved + $FatSectors) * $Bps
$FatOffset   = $Reserved * $Bps

function Set-U16 { param([byte[]]$B,[int]$O,[uint16]$V) $x=[BitConverter]::GetBytes($V); [Array]::Copy($x,0,$B,$O,2) }
function Set-U32 { param([byte[]]$B,[int]$O,[uint32]$V) $x=[BitConverter]::GetBytes($V); [Array]::Copy($x,0,$B,$O,4) }
function Get-Sha256 { param([byte[]]$Bytes)
    $tmp = Join-Path $WorkDir ([Guid]::NewGuid().ToString('N'))
    [System.IO.File]::WriteAllBytes($tmp, $Bytes)
    try { return (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash.ToLower() }
    finally { Remove-Item -LiteralPath $tmp -Force }
}

function Write-ShortEntry {
    param(
        [byte[]]$Buf, [int]$Offset, [string]$Name8, [string]$Ext3,
        [byte]$Attr, [uint32]$StartCluster, [uint32]$Size,
        [uint16]$CDate = 0, [uint16]$CTime = 0, [uint16]$MDate = 0, [uint16]$MTime = 0,
        [bool]$Deleted = $false
    )
    $name11 = $Name8.PadRight(8).Substring(0, 8) + $Ext3.PadRight(3).Substring(0, 3)
    for ($i = 0; $i -lt 11; $i++) { $Buf[$Offset + $i] = [byte][char]$name11[$i] }
    if ($Deleted) { $Buf[$Offset] = 0xE5 }
    $Buf[$Offset + 11] = $Attr
    $Buf[$Offset + 12] = 0
    Set-U16 $Buf ($Offset + 14) $CTime
    Set-U16 $Buf ($Offset + 16) $CDate
    Set-U16 $Buf ($Offset + 20) ([uint16](($StartCluster -shr 16) -band 0xFFFF))
    Set-U16 $Buf ($Offset + 22) $MTime
    Set-U16 $Buf ($Offset + 24) $MDate
    Set-U16 $Buf ($Offset + 26) ([uint16]($StartCluster -band 0xFFFF))
    Set-U32 $Buf ($Offset + 28) $Size
    return $Offset + 32
}

function Get-LfnChecksum {
    param([string]$Name11)
    $sum = 0
    foreach ($ch in $Name11.ToCharArray()) {
        $sum = ((($sum -band 1) -shl 7) + ($sum -shr 1) + [int][char]$ch) -band 0xFF
    }
    return [byte]$sum
}

function Write-LongName {
    param([byte[]]$Buf, [int]$Offset, [string]$LongName, [string]$Name8, [string]$Ext3, [bool]$Deleted)
    $name11 = $Name8.PadRight(8).Substring(0, 8) + $Ext3.PadRight(3).Substring(0, 3)
    $checksum = Get-LfnChecksum $name11
    $chars = $LongName.ToCharArray()
    $total = [int][Math]::Ceiling($chars.Count / 13.0)

    for ($seq = $total; $seq -ge 1; $seq--) {
        $entry = New-Object byte[] 32
        $sequence = $seq
        if ($seq -eq $total) { $sequence = $sequence -bor 0x40 }
        if ($Deleted) { $sequence = 0xE5 }
        $entry[0] = [byte]$sequence
        $entry[11] = 0x0F
        $entry[13] = $checksum

        $groupStart = ($seq - 1) * 13
        for ($k = 0; $k -lt 13; $k++) {
            $pos = $groupStart + $k
            if ($pos -lt $chars.Count) { $cp = [int][char]$chars[$pos] }
            elseif ($pos -eq $chars.Count) { $cp = 0 }
            else { $cp = 0xFFFF }

            if ($k -lt 5)        { $o = 1 + $k * 2 }
            elseif ($k -lt 11)   { $o = 14 + ($k - 5) * 2 }
            else                 { $o = 28 + ($k - 11) * 2 }

            $entry[$o]     = [byte]($cp -band 0xFF)
            $entry[$o + 1] = [byte](($cp -shr 8) -band 0xFF)
        }

        [Array]::Copy($entry, 0, $Buf, $Offset, 32)
        $Offset += 32
    }
    return $Offset
}

function Copy-ToCluster {
    param([byte[]]$Buf, [int]$Cluster, [byte[]]$Data)
    [Array]::Copy($Data, 0, $Buf, ($DataStart + ($Cluster - 2) * $Bps), $Data.Length)
}

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

Write-Host '=== building synthetic FAT32 image with deleted entries ==='

# source contents
$pngTemp = Join-Path $WorkDir 'holiday_src.png'
$bmp = New-Object System.Drawing.Bitmap(96, 96)
try {
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try { $g.Clear([System.Drawing.Color]::FromArgb(40, 120, 200)) } finally { $g.Dispose() }
    $bmp.Save($pngTemp, [System.Drawing.Imaging.ImageFormat]::Png)
} finally { $bmp.Dispose() }
$pngBytes = [System.IO.File]::ReadAllBytes($pngTemp)

$innerTemp = Join-Path $WorkDir 'inner_src.png'
$bmp2 = New-Object System.Drawing.Bitmap(80, 80)
try {
    $g2 = [System.Drawing.Graphics]::FromImage($bmp2)
    try { $g2.Clear([System.Drawing.Color]::FromArgb(200, 80, 40)) } finally { $g2.Dispose() }
    $bmp2.Save($innerTemp, [System.Drawing.Imaging.ImageFormat]::Png)
} finally { $bmp2.Dispose() }
$innerBytes = [System.IO.File]::ReadAllBytes($innerTemp)

$notesBytes = [System.Text.Encoding]::ASCII.GetBytes(('deleted notes line ' * 60))
# build a JPEG and pad it past one cluster so recovery spans two
$jpgTemp = Join-Path $WorkDir 'overwritten_src.jpg'
$bmp3 = New-Object System.Drawing.Bitmap(64, 64)
try {
    $g3 = [System.Drawing.Graphics]::FromImage($bmp3)
    try { $g3.Clear([System.Drawing.Color]::FromArgb(20, 160, 90)) } finally { $g3.Dispose() }
    $bmp3.Save($jpgTemp, [System.Drawing.Imaging.ImageFormat]::Jpeg)
} finally { $bmp3.Dispose() }
$rawJpg = [System.IO.File]::ReadAllBytes($jpgTemp)
$jpgBytes = New-Object byte[] 900
[Array]::Copy($rawJpg, 0, $jpgBytes, 0, [Math]::Min($rawJpg.Length, 900))
[System.IO.File]::WriteAllBytes($jpgTemp, $jpgBytes)

$pngHash   = Get-Sha256 $pngBytes
$innerHash = Get-Sha256 $innerBytes
$notesHash = Get-Sha256 $notesBytes
$jpgHash   = Get-Sha256 $jpgBytes

if ($notesBytes.Length -le $Bps) { throw 'notes must span two clusters' }
if ($jpgBytes.Length -le $Bps)   { throw 'jpg must span two clusters' }

$image = New-Object byte[] ($TotalSectors * $Bps)

# boot sector (FAT32)
$boot = New-Object byte[] 512
$boot[0] = 0xEB; $boot[1] = 0x58; $boot[2] = 0x90
[System.Text.Encoding]::ASCII.GetBytes('MSWIN4.1').CopyTo($boot, 3)
Set-U16 $boot 0x0B ([uint16]$Bps)
$boot[0x0D] = [byte]$Spc
Set-U16 $boot 0x0E ([uint16]$Reserved)
$boot[0x10] = 1
Set-U16 $boot 0x11 ([uint16]0)
Set-U16 $boot 0x13 ([uint16]0)
$boot[0x15] = 0xF8
Set-U16 $boot 0x16 ([uint16]0)
Set-U32 $boot 0x20 ([uint32]$TotalSectors)
Set-U32 $boot 0x24 ([uint32]$FatSectors)
Set-U32 $boot 0x2C ([uint32]2)
$boot[0x1FE] = 0x55; $boot[0x1FF] = 0xAA
[Array]::Copy($boot, 0, $image, 0, 512)

# FAT: reserve, root (2) and subdir (3); cluster 41 is reallocated
Set-U32 $image ($FatOffset + 0) ([uint32]0x0FFFFFF8)
Set-U32 $image ($FatOffset + 4) ([uint32]0x0FFFFFFF)
Set-U32 $image ($FatOffset + 2 * 4) ([uint32]0x0FFFFFFF)
Set-U32 $image ($FatOffset + 3 * 4) ([uint32]0x0FFFFFFF)
Set-U32 $image ($FatOffset + 41 * 4) ([uint32]0x0FFFFFFF)

# timestamps: 2024-01-02 03:04:04 (FAT stores seconds in two-second steps)
$date = [uint16](((2024 - 1980) -shl 9) -bor (1 -shl 5) -bor 2)
$time = [uint16]((3 -shl 11) -bor (4 -shl 5) -bor 2)

$root = New-Object byte[] 512
$pos = 0
$pos = Write-LongName -Buf $root -Offset $pos -LongName 'holiday photo.png' -Name8 'HOLIDA~1' -Ext3 'PNG' -Deleted $true
$pos = Write-ShortEntry -Buf $root -Offset $pos -Name8 'HOLIDA~1' -Ext3 'PNG' -Attr 0x20 -StartCluster 10 -Size $pngBytes.Length -CDate $date -CTime $time -MDate $date -MTime $time -Deleted $true
$pos = Write-LongName -Buf $root -Offset $pos -LongName 'overwritten.jpg' -Name8 'OVERWR~1' -Ext3 'JPG' -Deleted $true
$pos = Write-ShortEntry -Buf $root -Offset $pos -Name8 'OVERWR~1' -Ext3 'JPG' -Attr 0x20 -StartCluster 40 -Size $jpgBytes.Length -CDate $date -CTime $time -MDate $date -MTime $time -Deleted $true
$pos = Write-ShortEntry -Buf $root -Offset $pos -Name8 'NOTES' -Ext3 'TXT' -Attr 0x20 -StartCluster 20 -Size $notesBytes.Length -Deleted $true
$pos = Write-ShortEntry -Buf $root -Offset $pos -Name8 'LIVE' -Ext3 'TXT' -Attr 0x20 -StartCluster 50 -Size 512
$pos = Write-ShortEntry -Buf $root -Offset $pos -Name8 'SUBDIR' -Ext3 '' -Attr 0x10 -StartCluster 3 -Size 0
$pos = Write-ShortEntry -Buf $root -Offset $pos -Name8 'DELDIR' -Ext3 '' -Attr 0x10 -StartCluster 60 -Size 0 -Deleted $true
[Array]::Copy($root, 0, $image, $DataStart, 512)

$sub = New-Object byte[] 512
$spos = 0
$spos = Write-LongName -Buf $sub -Offset $spos -LongName 'inner image.png' -Name8 'INNERI~1' -Ext3 'PNG' -Deleted $true
$spos = Write-ShortEntry -Buf $sub -Offset $spos -Name8 'INNERI~1' -Ext3 'PNG' -Attr 0x20 -StartCluster 70 -Size $innerBytes.Length -Deleted $true
[Array]::Copy($sub, 0, $image, ($DataStart + 512), 512)

Copy-ToCluster $image 10 $pngBytes
Copy-ToCluster $image 20 $notesBytes
Copy-ToCluster $image 40 $jpgBytes
Copy-ToCluster $image 70 $innerBytes

$imagePath = Join-Path $WorkDir 'fat32_deleted.img'
[System.IO.File]::WriteAllBytes($imagePath, $image)
Write-Host ("  image: {0} bytes, deleted files at free clusters 10,20,70 and reallocated 40" -f $image.Length)

$failures = @()
function Get-Carver {
    param([string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { return (& $script:CarverExe @Arguments 2>&1 | Out-String) }
    finally { $ErrorActionPreference = $previous }
}

Write-Host ''
Write-Host '=== running --fat-recover ==='
$outputDir = Join-Path $WorkDir 'recovered'
if (Test-Path $outputDir) { Remove-Item $outputDir -Recurse -Force }
$scanOutput = Get-Carver @($imagePath, $outputDir, '--fat-recover')
Write-Host $scanOutput.TrimEnd()

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

if ($rows.Count -eq 4) {
    Write-Host ("  [ok]   4 files indexed (got {0})" -f $rows.Count)
} else {
    Write-Host ("  [FAIL] expected 4 files, got {0}" -f $rows.Count)
    $failures += 'count'
}

if ($byOriginal.ContainsKey('holiday photo.png')) {
    $hash = (Get-FileHash -LiteralPath (Get-OutputPath 'holiday photo.png') -Algorithm SHA256).Hash.ToLower()
    if ($hash -eq $pngHash) {
        Write-Host '  [ok]   "holiday photo.png" long name rebuilt, bytes identical'
    } else {
        Write-Host '  [FAIL] "holiday photo.png" differs from the original'
        $failures += 'png'
    }
} else {
    Write-Host '  [FAIL] "holiday photo.png" not recovered'
    $failures += 'png-missing'
}

if ($byOriginal.ContainsKey('inner image.png')) {
    $hash = (Get-FileHash -LiteralPath (Get-OutputPath 'inner image.png') -Algorithm SHA256).Hash.ToLower()
    if ($hash -eq $innerHash) {
        Write-Host '  [ok]   "inner image.png" recovered from the subdirectory'
    } else {
        Write-Host '  [FAIL] "inner image.png" differs from the original'
        $failures += 'inner'
    }
} else {
    Write-Host '  [FAIL] "inner image.png" not recovered (subdirectory not walked?)'
    $failures += 'inner-missing'
}

if ($byOriginal.ContainsKey('OTES.TXT')) {
    $hash = (Get-FileHash -LiteralPath (Get-OutputPath 'OTES.TXT') -Algorithm SHA256).Hash.ToLower()
    if ($hash -eq $notesHash) {
        Write-Host '  [ok]   8.3-only deleted name falls back to OTES.TXT, bytes identical'
    } else {
        Write-Host '  [FAIL] OTES.TXT differs from the original'
        $failures += 'notes'
    }
} else {
    Write-Host '  [FAIL] the 8.3-only deleted file was not recovered'
    $failures += 'notes-missing'
}

if ($byOriginal.ContainsKey('overwritten.jpg')) {
    $hash = (Get-FileHash -LiteralPath (Get-OutputPath 'overwritten.jpg') -Algorithm SHA256).Hash.ToLower()
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

if ($byOriginal.ContainsKey('LIVE.TXT')) {
    Write-Host '  [FAIL] LIVE.TXT was recovered even though it is still in use'
    $failures += 'live-leak'
} else {
    Write-Host '  [ok]   LIVE.TXT correctly skipped (in use)'
}

if ($byOriginal.ContainsKey('DELDIR')) {
    Write-Host '  [FAIL] DELDIR was recovered even though it is a directory'
    $failures += 'dir-leak'
} else {
    Write-Host '  [ok]   DELDIR correctly skipped (deleted directory)'
}

if ($byOriginal.ContainsKey('holiday photo.png') -and $byOriginal['holiday photo.png'].created -eq '2024-01-02 03:04:04') {
    Write-Host '  [ok]   DOS date/time converted correctly (2024-01-02 03:04:04)'
} elseif ($byOriginal.ContainsKey('holiday photo.png')) {
    Write-Host ("  [FAIL] created timestamp was '{0}'" -f $byOriginal['holiday photo.png'].created)
    $failures += 'timestamp'
}

if ($scanOutput -match 'files written\s+:\s+4') {
    Write-Host '  [ok]   reported files written: 4'
} else {
    Write-Host '  [FAIL] reported files written did not match 4'
    $failures += 'reported'
}
if ($scanOutput -match 'possibly overwritten\s+:\s+1') {
    Write-Host '  [ok]   reported possibly overwritten: 1'
} else {
    Write-Host '  [FAIL] reported possibly overwritten did not match 1'
    $failures += 'reported-risk'
}

# --skip-ext png,jpg leaves only the txt file
Write-Host ''
Write-Host '=== running --fat-recover with --skip-ext png,jpg ==='
$skipDir = Join-Path $WorkDir 'recovered_skip'
if (Test-Path -LiteralPath $skipDir) { Remove-Item -LiteralPath $skipDir -Recurse -Force }
$skipOutput = Get-Carver @($imagePath, $skipDir, '--fat-recover', '--skip-ext', 'png,jpg')
$skipCsv = Join-Path $skipDir 'recovered.csv'
if (Test-Path -LiteralPath $skipCsv) {
    $skipNames = @(Import-Csv -LiteralPath $skipCsv | ForEach-Object { $_.original_name })
    if ($skipNames.Count -eq 1 -and $skipNames[0] -eq 'OTES.TXT') {
        Write-Host '  [ok]   only OTES.TXT survived --skip-ext png,jpg'
    } else {
        Write-Host ("  [FAIL] --skip-ext left {0} file(s): {1}" -f $skipNames.Count, ($skipNames -join ', '))
        $failures += 'skip-ext'
    }
} else {
    Write-Host '  [FAIL] the skip run wrote no index'
    $failures += 'skip-index'
}

# --only-ext txt matches the same single file
Write-Host ''
Write-Host '=== running --fat-recover with --only-ext txt ==='
$onlyDir = Join-Path $WorkDir 'recovered_only'
if (Test-Path -LiteralPath $onlyDir) { Remove-Item -LiteralPath $onlyDir -Recurse -Force }
$onlyOutput = Get-Carver @($imagePath, $onlyDir, '--fat-recover', '--only-ext', 'txt')
$onlyCsv = Join-Path $onlyDir 'recovered.csv'
if (Test-Path -LiteralPath $onlyCsv) {
    $onlyNames = @(Import-Csv -LiteralPath $onlyCsv | ForEach-Object { $_.original_name })
    if ($onlyNames.Count -eq 1 -and $onlyNames[0] -eq 'OTES.TXT') {
        Write-Host '  [ok]   --only-ext txt left just OTES.TXT'
    } else {
        Write-Host ("  [FAIL] --only-ext txt left {0} file(s): {1}" -f $onlyNames.Count, ($onlyNames -join ', '))
        $failures += 'only-ext'
    }
} else {
    Write-Host '  [FAIL] the only run wrote no index'
    $failures += 'only-index'
}

# --list-only lists all four without writing data
Write-Host ''
Write-Host '=== running --fat-recover with --list-only ==='
$listDir = Join-Path $WorkDir 'recovered_list'
if (Test-Path -LiteralPath $listDir) { Remove-Item -LiteralPath $listDir -Recurse -Force }
$listOutput = Get-Carver @($imagePath, $listDir, '--fat-recover', '--list-only')
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
    if ($listNames.Count -eq 4) {
        Write-Host '  [ok]   the index still lists all 4 files'
    } else {
        Write-Host ("  [FAIL] the index lists {0} files" -f $listNames.Count)
        $failures += 'list-index'
    }
} else {
    Write-Host '  [FAIL] --list-only wrote no index'
    $failures += 'list-index'
}
if ($listOutput -match 'files listed\s+:\s+4') {
    Write-Host '  [ok]   reported 4 files listed'
} else {
    Write-Host '  [FAIL] the listed file count did not match 4'
    $failures += 'list-count'
}

# a whole-disk image: MBR at sector 0, the FAT32 partition starting at 2048
Write-Host ''
Write-Host '=== running --fat-recover on an MBR disk image ==='
$mbrSectors = 2048
$disk = New-Object byte[] ($mbrSectors * $Bps + $image.Length)
$mbr = New-Object byte[] 512
$mbr[0x1BE + 4] = 0x0C
Set-U32 $mbr (0x1BE + 8) ([uint32]$mbrSectors)
Set-U32 $mbr (0x1BE + 12) ([uint32]($image.Length / $Bps))
$mbr[0x1FE] = 0x55; $mbr[0x1FF] = 0xAA
[Array]::Copy($mbr, 0, $disk, 0, 512)
[Array]::Copy($image, 0, $disk, ($mbrSectors * $Bps), $image.Length)
$diskPath = Join-Path $WorkDir 'fat32_disk.img'
[System.IO.File]::WriteAllBytes($diskPath, $disk)

$diskDir = Join-Path $WorkDir 'recovered_disk'
if (Test-Path -LiteralPath $diskDir) { Remove-Item -LiteralPath $diskDir -Recurse -Force }
$diskOutput = Get-Carver @($diskPath, $diskDir, '--fat-recover')
$diskCsv = Join-Path $diskDir 'recovered.csv'
if (Test-Path -LiteralPath $diskCsv) {
    $diskNames = @(Import-Csv -LiteralPath $diskCsv | ForEach-Object { $_.original_name })
    if ($diskNames.Count -eq 4) {
        Write-Host '  [ok]   the FAT32 partition was auto-selected and all 4 files recovered'
    } else {
        Write-Host ("  [FAIL] the disk image yielded {0} file(s)" -f $diskNames.Count)
        $failures += 'disk-count'
    }
} else {
    Write-Host '  [FAIL] the disk image run wrote no index'
    $failures += 'disk-index'
}
if ($diskOutput -match 'auto-selected partition') {
    Write-Host '  [ok]   reported the auto-selected partition'
} else {
    Write-Host '  [FAIL] the auto-selected partition was not reported'
    $failures += 'disk-auto'
}

# refused on an NTFS image
Write-Host ''
Write-Host '=== running --fat-recover on a non-FAT image ==='
$bogus = Join-Path $WorkDir 'bogus.img'
$bogusBytes = New-Object byte[] 4096
$bogusBytes[0x1FE] = 0x55; $bogusBytes[0x1FF] = 0xAA
[System.IO.File]::WriteAllBytes($bogus, $bogusBytes)
$bogusOut = Get-Carver @($bogus, (Join-Path $WorkDir 'bogus_out'), '--fat-recover')
if ($bogusOut -match 'unrecognised volume|not FAT|no NTFS or FAT volume') {
    Write-Host '  [ok]   a non-FAT image is refused'
} else {
    Write-Host '  [FAIL] a non-FAT image was not refused'
    $failures += 'non-fat'
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
