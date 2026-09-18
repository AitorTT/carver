#Requires -Version 5.1

<#
    Tests ATTRIBUTE_LIST following.

    Builds an NTFS image where one deleted file's attributes are split across two
    MFT records, which is what NTFS does when a record runs out of room:

        record 25 (base)        $STANDARD_INFORMATION, $DATA extent 1 (id 0),
                                and an ATTRIBUTE_LIST pointing at record 26
        record 26 (extension)   $DATA extent 2 (id 1) and $FILE_NAME (id 2)

    The file's two clusters are deliberately non-contiguous, so recovering it
    correctly proves three things at once:
      * the ATTRIBUTE_LIST was parsed and followed
      * the two $DATA extents were merged in starting-VCN order
      * the file name was taken from the extension record

    Without attribute-list following the file would come out truncated at the
    first extent (1024 of 2000 bytes).

    No administrator rights and no format/mount required.
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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\attrlist' }
$CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir   = [System.IO.Path]::GetFullPath($WorkDir)

if (-not (Test-Path -LiteralPath $CarverExe)) { throw "carver-cli.exe not found at '$CarverExe'. Build it first." }

$BytesPerSector    = 512
$SectorsPerCluster = 1
$BytesPerCluster   = 512
$TotalClusters     = 4096
$MftCluster        = 4
$MftRecordSize     = 1024
$BitmapCluster     = 64
$MftClusters       = 60          # 30 records, enough for records 0..29

function Set-U16 { param([byte[]]$Buf,[int]$Offset,[uint16]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,2) }
function Set-U32 { param([byte[]]$Buf,[int]$Offset,[uint32]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,4) }
function Set-U64 { param([byte[]]$Buf,[int]$Offset,[uint64]$Value) $b=[BitConverter]::GetBytes($Value); [Array]::Copy($b,0,$Buf,$Offset,8) }
function Set-Ascii { param([byte[]]$Buf,[int]$Offset,[string]$Text) $b=[System.Text.Encoding]::ASCII.GetBytes($Text); [Array]::Copy($b,0,$Buf,$Offset,$b.Length) }
function Align8 { param([int]$Value) return ($Value + 7) -band -8 }

function Add-ResidentAttribute {
    param([byte[]]$Record, [int]$Position, [uint32]$Type, [uint16]$Id, [byte[]]$Content)

    $length = Align8 (0x18 + $Content.Length)
    Set-U32 $Record $Position $Type
    Set-U32 $Record ($Position+4) ([uint32]$length)
    $Record[$Position+8] = 0
    $Record[$Position+9] = 0
    Set-U16 $Record ($Position+0x0A) 0x18
    Set-U16 $Record ($Position+0x0C) 0
    Set-U16 $Record ($Position+0x0E) $Id
    Set-U32 $Record ($Position+0x10) ([uint32]$Content.Length)
    Set-U16 $Record ($Position+0x14) 0x18
    [Array]::Copy($Content, 0, $Record, $Position + 0x18, $Content.Length)
    return $Position + $length
}

function Add-NonResidentData {
    param([byte[]]$Record, [int]$Position, [uint16]$Id, [uint64]$StartingVcn, [uint64]$LastVcn,
          [int]$Lcn, [int]$Clusters, [uint64]$RealSize)

    $lengthBytes = New-Object System.Collections.Generic.List[byte]
    $value = $Clusters
    while ($value -gt 0) { $lengthBytes.Add([byte]($value -band 0xFF)); $value = $value -shr 8 }
    if ($lengthBytes.Count -eq 0) { $lengthBytes.Add([byte]0) }

    $offsetBytes = New-Object System.Collections.Generic.List[byte]
    $value = $Lcn
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
    Set-U32 $Record $Position 0x80
    Set-U32 $Record ($Position+4) ([uint32]$length)
    $Record[$Position+8] = 1
    $Record[$Position+9] = 0
    Set-U16 $Record ($Position+0x0A) ([uint16]$runListOffset)
    Set-U16 $Record ($Position+0x0C) 0
    Set-U16 $Record ($Position+0x0E) $Id
    Set-U64 $Record ($Position+0x10) $StartingVcn
    Set-U64 $Record ($Position+0x18) $LastVcn
    Set-U16 $Record ($Position+0x20) ([uint16]$runListOffset)
    Set-U16 $Record ($Position+0x22) 0
    Set-U64 $Record ($Position+0x28) ([uint64]($Clusters * $BytesPerCluster))
    Set-U64 $Record ($Position+0x30) $RealSize
    Set-U64 $Record ($Position+0x38) $RealSize
    [Array]::Copy($runList.ToArray(), 0, $Record, $Position + $runListOffset, $runList.Count)
    return $Position + $length
}

function Add-FileName {
    param([byte[]]$Record, [int]$Position, [uint16]$Id, [string]$Name, [uint64]$RealSize, [uint64]$Created, [int]$Parent = 5)

    $nameBytes = [System.Text.Encoding]::Unicode.GetBytes($Name)
    $content = New-Object byte[] (0x42 + $nameBytes.Length)
    Set-U64 $content 0x00 ([uint64]$Parent)
    Set-U64 $content 0x08 $Created
    Set-U64 $content 0x10 $Created
    Set-U64 $content 0x18 $Created
    Set-U64 $content 0x20 $Created
    Set-U64 $content 0x28 $RealSize
    Set-U64 $content 0x30 $RealSize
    $content[0x40] = [byte]($nameBytes.Length / 2)
    $content[0x41] = 1
    [Array]::Copy($nameBytes, 0, $content, 0x42, $nameBytes.Length)

    return Add-ResidentAttribute -Record $Record -Position $Position -Type 0x30 -Id $Id -Content $content
}

function New-AttributeListEntry {
    param([uint32]$Type, [uint64]$StartingVcn, [uint64]$BaseRecord, [uint16]$Id)

    $entry = New-Object byte[] 0x20
    Set-U32 $entry 0x00 $Type
    Set-U16 $entry 0x04 0x20
    $entry[0x06] = 0
    $entry[0x07] = 0
    Set-U64 $entry 0x08 $StartingVcn
    Set-U64 $entry 0x10 $BaseRecord
    Set-U16 $entry 0x18 $Id
    return $entry
}

function New-Record {
    param(
        [bool]$InUse = $false,
        [bool]$Directory = $false,
        [uint64]$BaseRecord = 0,
        [byte[]]$Content = $null
    )

    $record = New-Object byte[] 1024
    if ($null -ne $Content) { [Array]::Copy($Content, 0, $record, 0, [Math]::Min($Content.Length, 1024)) }
    Set-Ascii $record 0 'FILE'
    Set-U16 $record 0x04 0x30
    Set-U16 $record 0x06 3
    Set-U16 $record 0x10 1
    Set-U16 $record 0x12 1
    Set-U16 $record 0x14 0x38
    Set-U64 $record 0x20 $BaseRecord

    $flags = 0
    if ($InUse) { $flags = $flags -bor 1 }
    if ($Directory) { $flags = $flags -bor 2 }
    Set-U16 $record 0x16 ([uint16]$flags)
    Set-U32 $record 0x1C 1024
    Set-U16 $record 0x28 4

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

Write-Host '=== building NTFS image with a split-attribute file ==='

$image = New-Object byte[] ($TotalClusters * $BytesPerCluster)

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

# the file we are going to recover: 2000 bytes, stored as 2 non-contiguous runs
$original = New-Object byte[] 2000
$rng = New-Object System.Random(4242)
$rng.NextBytes($original)
$firstClusters  = 2                     # clusters 600..601 -> 1024 bytes
$secondClusters = 2                     # clusters 700..701 -> remaining 976 bytes
$firstLcn  = 600
$secondLcn = 700

[Array]::Copy($original, 0, $image, ($firstLcn * $BytesPerCluster), ($firstClusters * $BytesPerCluster))
[Array]::Copy($original, ($firstClusters * $BytesPerCluster), $image, ($secondLcn * $BytesPerCluster),
              ($original.Length - $firstClusters * $BytesPerCluster))

$createdFileTime = [System.DateTime]::new(2025, 3, 4, 5, 6, 7, [System.DateTimeKind]::Utc).ToFileTime()

# --- record 25: base record ---
$si = New-Object byte[] 0x48
Set-U64 $si 0x00 $createdFileTime
Set-U64 $si 0x08 $createdFileTime
Set-U64 $si 0x10 $createdFileTime
Set-U64 $si 0x18 $createdFileTime

$listEntries = New-Object System.Collections.Generic.List[byte]
foreach ($b in (New-AttributeListEntry -Type 0x80 -StartingVcn 0 -BaseRecord 25 -Id 0)) { $listEntries.Add($b) }
foreach ($b in (New-AttributeListEntry -Type 0x80 -StartingVcn $firstClusters -BaseRecord 26 -Id 1)) { $listEntries.Add($b) }
foreach ($b in (New-AttributeListEntry -Type 0x30 -StartingVcn 0 -BaseRecord 26 -Id 2)) { $listEntries.Add($b) }

$base = New-Record -InUse $false
$pos = 0x38
$pos = Add-ResidentAttribute -Record $base -Position $pos -Type 0x10 -Id 0 -Content $si
$pos = Add-NonResidentData -Record $base -Position $pos -Id 0 -StartingVcn 0 -LastVcn ($firstClusters - 1) `
                           -Lcn $firstLcn -Clusters $firstClusters -RealSize ([uint64]$original.Length)
$pos = Add-ResidentAttribute -Record $base -Position $pos -Type 0x20 -Id 3 -Content $listEntries.ToArray()
Set-U32 $base $pos ([uint32]::MaxValue)
$pos += 8
Set-U32 $base 0x18 ([uint32]$pos)

# --- record 26: extension record (base-record reference points at record 25) ---
$extension = New-Record -InUse $false -BaseRecord 25
$pos = 0x38
$pos = Add-NonResidentData -Record $extension -Position $pos -Id 1 -StartingVcn $firstClusters `
                           -LastVcn ($firstClusters + $secondClusters - 1) -Lcn $secondLcn `
                           -Clusters $secondClusters -RealSize ([uint64]$original.Length)
$pos = Add-FileName -Record $extension -Position $pos -Id 2 -Name 'split.bin' `
                    -RealSize ([uint64]$original.Length) -Created $createdFileTime
Set-U32 $extension $pos ([uint32]::MaxValue)
$pos += 8
Set-U32 $extension 0x18 ([uint32]$pos)

# --- records 0 and 6 ---
$mftRecord = New-Record -InUse $true
$pos = 0x38
$pos = Add-NonResidentData -Record $mftRecord -Position $pos -Id 0 -StartingVcn 0 -LastVcn ($MftClusters - 1) `
                           -Lcn $MftCluster -Clusters $MftClusters -RealSize ([uint64]($MftClusters * $BytesPerCluster))
Set-U32 $mftRecord $pos ([uint32]::MaxValue)
$pos += 8
Set-U32 $mftRecord 0x18 ([uint32]$pos)

$bitmapRecord = New-Record -InUse $true
$pos = 0x38
$pos = Add-NonResidentData -Record $bitmapRecord -Position $pos -Id 0 -StartingVcn 0 -LastVcn 0 `
                           -Lcn $BitmapCluster -Clusters 1 -RealSize 512
Set-U32 $bitmapRecord $pos ([uint32]::MaxValue)
$pos += 8
Set-U32 $bitmapRecord 0x18 ([uint32]$pos)

$mftOffset = $MftCluster * $BytesPerCluster
foreach ($pair in @(@{n=0; r=$mftRecord}, @{n=6; r=$bitmapRecord}, @{n=25; r=$base}, @{n=26; r=$extension})) {
    [Array]::Copy($pair.r, 0, $image, ($mftOffset + $pair.n * $MftRecordSize), $MftRecordSize)
}

# bitmap: boot + MFT (0..63) and $Bitmap (64) allocated; the file's clusters stay free
$bmBytes = New-Object byte[] ($TotalClusters / 8)
foreach ($cluster in (@(0..63) + @(64))) {
    $byteIndex = $cluster -shr 3
    $bmBytes[$byteIndex] = $bmBytes[$byteIndex] -bor [byte](1 -shl ($cluster -band 7))
}
[Array]::Copy($bmBytes, 0, $image, ($BitmapCluster * $BytesPerCluster), $bmBytes.Length)

$imagePath = Join-Path $WorkDir 'attrlist.img'
[System.IO.File]::WriteAllBytes($imagePath, $image)

$originalPath = Join-Path $WorkDir 'original.bin'
[System.IO.File]::WriteAllBytes($originalPath, $original)
$originalHash = (Get-FileHash -LiteralPath $originalPath -Algorithm SHA256).Hash

Write-Host ("  file      : 2000 bytes across clusters {0}-{1} and {2}-{3} (non-contiguous)" -f `
            $firstLcn, ($firstLcn + $firstClusters - 1), $secondLcn, ($secondLcn + $secondClusters - 1))
Write-Host '  record 25 : $STANDARD_INFORMATION + $DATA extent 1 + ATTRIBUTE_LIST'
Write-Host '  record 26 : $DATA extent 2 + $FILE_NAME'

$outputDir = Join-Path $WorkDir 'recovered'
if (Test-Path $outputDir) { Remove-Item $outputDir -Recurse -Force }

Write-Host ''
Write-Host '=== running --mft-recover ==='
$scanOutput = & $CarverExe $imagePath $outputDir --mft-recover 2>&1 | Out-String
Write-Host $scanOutput.TrimEnd()

$failures = @()
$csvPath = Join-Path $outputDir 'recovered.csv'

Write-Host ''
Write-Host '=== verifying ==='

if (-not (Test-Path $csvPath)) {
    Write-Host '  [FAIL] recovered.csv was not written'
    Write-Host ''
    Write-Host 'FAILED: no index'
    exit 1
}

$rows = @(Import-Csv -LiteralPath $csvPath)
$row = $rows | Where-Object { $_.original_name -eq 'split.bin' } | Select-Object -First 1

if ($rows.Count -eq 1) {
    Write-Host '  [ok]   exactly one file recovered (the extension record was not treated as a file)'
} else {
    Write-Host ("  [FAIL] expected 1 recovered file, got {0}: {1}" -f $rows.Count, (($rows | ForEach-Object { $_.output_name }) -join ', '))
    $failures += 'count'
}

if ($scanOutput -match 'files written\s+:\s+1') {
    Write-Host '  [ok]   reported files written: 1'
} else {
    Write-Host '  [FAIL] reported files written was not 1'
    $failures += 'reported'
}

if ($null -eq $row) {
    Write-Host '  [FAIL] split.bin was not recovered at all'
    Write-Host ("         indexed names: {0}" -f (($rows | ForEach-Object { $_.original_name }) -join ', '))
    $failures += 'missing'
} else {
    Write-Host ("  [ok]   name came from the extension record: {0}" -f $row.original_name)

    $recoveredPath = Join-Path $outputDir $row.output_name
    $recoveredLength = (Get-Item -LiteralPath $recoveredPath).Length

    if ($recoveredLength -eq $original.Length) {
        Write-Host ("  [ok]   length is {0} bytes (both extents merged)" -f $recoveredLength)
    } else {
        Write-Host ("  [FAIL] length is {0}, expected {1} (extent merge failed)" -f $recoveredLength, $original.Length)
        $failures += 'length'
    }

    $recoveredHash = (Get-FileHash -LiteralPath $recoveredPath -Algorithm SHA256).Hash
    if ($recoveredHash -eq $originalHash) {
        Write-Host '  [ok]   content byte-identical across the non-contiguous runs'
    } else {
        Write-Host '  [FAIL] content differs from the original'
        $failures += 'content'
    }

    if ($row.created -eq '2025-03-04 05:06:07') {
        Write-Host '  [ok]   timestamp from the base record'
    } else {
        Write-Host ("  [FAIL] timestamp was '{0}'" -f $row.created)
        $failures += 'timestamp'
    }
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
