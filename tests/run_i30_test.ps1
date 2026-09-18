#Requires -Version 5.1

<#
    Tests --i30.

    Builds a synthetic NTFS volume whose directory carries a real $I30 index:

        record 5   root directory
        record 30  directory "Docs", parent 5, holding a $INDEX_ROOT (0x90)
                   named $I30 with two live entries and one leftover name in
                   the unused tail of the node

    The two live entries point at MFT record numbers that do not exist, which
    is the case this mode exists for: the file's own record is gone, so the
    directory index is the only surviving record of the name.

    Checks:
      * both live names are listed with the path rebuilt from the parent chain
      * the slack name is listed and flagged as coming from slack
      * sizes and the record numbers come out of the index entries

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
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata\i30' }
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

function Set-U16 { param([byte[]]$B,[int]$O,[uint16]$V) $x=[BitConverter]::GetBytes($V); [Array]::Copy($x,0,$B,$O,2) }
function Set-U32 { param([byte[]]$B,[int]$O,[uint32]$V) $x=[BitConverter]::GetBytes($V); [Array]::Copy($x,0,$B,$O,4) }
function Set-U64 { param([byte[]]$B,[int]$O,[uint64]$V) $x=[BitConverter]::GetBytes($V); [Array]::Copy($x,0,$B,$O,8) }
function Set-Ascii { param([byte[]]$B,[int]$O,[string]$T) $x=[System.Text.Encoding]::ASCII.GetBytes($T); [Array]::Copy($x,0,$B,$O,$x.Length) }
function Align8 { param([int]$V) return ($V + 7) -band -8 }

# One index entry: file reference, lengths, flags, then the FILE_NAME key.
function New-IndexEntry {
    param(
        [uint64]$MftRecord = 0,
        [uint16]$Sequence = 1,
        [string]$Name = '',
        [uint64]$RealSize = 0,
        [uint64]$Parent = 30,
        [bool]$Terminal = $false
    )

    if ($Terminal) {
        $entry = New-Object byte[] 0x10
        Set-U16 $entry 0x08 ([uint16]0x10)
        Set-U16 $entry 0x0A ([uint16]0)
        Set-U32 $entry 0x0C ([uint32]0x02)
        return $entry
    }

    $nameBytes = [System.Text.Encoding]::Unicode.GetBytes($Name)
    $keyLength = 0x42 + $nameBytes.Length
    $entryLength = Align8 (0x10 + $keyLength)

    $entry = New-Object byte[] $entryLength
    Set-U64 $entry 0x00 ($MftRecord -bor ([uint64]$Sequence -shl 48))
    Set-U16 $entry 0x08 ([uint16]$entryLength)
    Set-U16 $entry 0x0A ([uint16]$keyLength)
    Set-U32 $entry 0x0C ([uint32]0)

    $key = 0x10
    Set-U64 $entry $key ([uint64]$Parent)
    Set-U64 $entry ($key + 0x08) ([uint64]0)
    Set-U64 $entry ($key + 0x10) ([uint64]0)
    Set-U64 $entry ($key + 0x18) ([uint64]0)
    Set-U64 $entry ($key + 0x20) ([uint64]0)
    Set-U64 $entry ($key + 0x28) ([uint64]$RealSize)
    Set-U64 $entry ($key + 0x30) ([uint64]$RealSize)
    Set-U32 $entry ($key + 0x38) ([uint32]0)
    Set-U32 $entry ($key + 0x3C) ([uint32]0)
    $entry[$key + 0x40] = [byte]($nameBytes.Length / 2)
    $entry[$key + 0x41] = 1
    [Array]::Copy($nameBytes, 0, $entry, ($key + 0x42), $nameBytes.Length)
    return $entry
}

# Resident attribute carrying a name, which is how $I30 attributes are stored.
function Add-ResidentNamedAttribute {
    param([byte[]]$Record, [int]$Position, [uint32]$Type, [uint16]$Id, [string]$Name, [byte[]]$Content)

    $nameBytes = [System.Text.Encoding]::Unicode.GetBytes($Name)
    $nameOffset = 0x18
    $contentOffset = Align8 ($nameOffset + $nameBytes.Length)
    $length = Align8 ($contentOffset + $Content.Length)

    Set-U32 $Record $Position $Type
    Set-U32 $Record ($Position + 4) ([uint32]$length)
    $Record[$Position + 8] = 0
    $Record[$Position + 9] = [byte]($nameBytes.Length / 2)
    Set-U16 $Record ($Position + 0x0A) ([uint16]$nameOffset)
    Set-U16 $Record ($Position + 0x0C) ([uint16]0)
    Set-U16 $Record ($Position + 0x0E) $Id
    Set-U32 $Record ($Position + 0x10) ([uint32]$Content.Length)
    Set-U16 $Record ($Position + 0x14) ([uint16]$contentOffset)
    [Array]::Copy($nameBytes, 0, $Record, ($Position + $nameOffset), $nameBytes.Length)
    [Array]::Copy($Content, 0, $Record, ($Position + $contentOffset), $Content.Length)
    return $Position + $length
}

function Add-ResidentAttribute {
    param([byte[]]$Record, [int]$Position, [uint32]$Type, [uint16]$Id, [byte[]]$Content)

    $length = Align8 (0x18 + $Content.Length)
    Set-U32 $Record $Position $Type
    Set-U32 $Record ($Position + 4) ([uint32]$length)
    $Record[$Position + 8] = 0
    $Record[$Position + 9] = 0
    Set-U16 $Record ($Position + 0x0A) 0x18
    Set-U16 $Record ($Position + 0x0C) 0
    Set-U16 $Record ($Position + 0x0E) $Id
    Set-U32 $Record ($Position + 0x10) ([uint32]$Content.Length)
    Set-U16 $Record ($Position + 0x14) 0x18
    [Array]::Copy($Content, 0, $Record, ($Position + 0x18), $Content.Length)
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
    Set-U32 $Record ($Position + 4) ([uint32]$length)
    $Record[$Position + 8] = 1
    $Record[$Position + 9] = 0
    Set-U16 $Record ($Position + 0x0A) ([uint16]$runListOffset)
    Set-U16 $Record ($Position + 0x0C) 0
    Set-U16 $Record ($Position + 0x0E) $Id
    Set-U64 $Record ($Position + 0x10) $StartingVcn
    Set-U64 $Record ($Position + 0x18) $LastVcn
    Set-U16 $Record ($Position + 0x20) ([uint16]$runListOffset)
    Set-U16 $Record ($Position + 0x22) 0
    Set-U64 $Record ($Position + 0x28) ([uint64]($Clusters * 512))
    Set-U64 $Record ($Position + 0x30) $RealSize
    Set-U64 $Record ($Position + 0x38) $RealSize
    [Array]::Copy($runList.ToArray(), 0, $Record, $Position + $runListOffset, $runList.Count)
    return $Position + $length
}

function Add-FileName {
    param([byte[]]$Record, [int]$Position, [uint16]$Id, [string]$Name, [uint64]$RealSize, [int]$Parent = 5)
    $nameBytes = [System.Text.Encoding]::Unicode.GetBytes($Name)
    $content = New-Object byte[] (0x42 + $nameBytes.Length)
    Set-U64 $content 0x00 ([uint64]$Parent)
    Set-U64 $content 0x28 $RealSize
    Set-U64 $content 0x30 $RealSize
    $content[0x40] = [byte]($nameBytes.Length / 2)
    $content[0x41] = 1
    [Array]::Copy($nameBytes, 0, $content, 0x42, $nameBytes.Length)
    return Add-ResidentAttribute -Record $Record -Position $Position -Type 0x30 -Id $Id -Content $content
}

function New-Record {
    param([bool]$InUse = $false, [bool]$Directory = $false, [uint64]$BaseRecord = 0, [uint64]$Parent = 5)

    $record = New-Object byte[] 1024
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

$BytesPerSector = 512
$TotalClusters = 4096
$MftCluster = 4
$MftRecordSize = 1024
$BitmapCluster = 100
$MftClusters = 64              # 32 records, 0..31

Write-Host '=== building an NTFS volume with a $I30 index ==='

$image = New-Object byte[] ($TotalClusters * $BytesPerSector)

$boot = New-Object byte[] 512
$boot[0] = 0xEB; $boot[1] = 0x52; $boot[2] = 0x90
Set-Ascii $boot 0x03 'NTFS    '
Set-U16 $boot 0x0B ([uint16]$BytesPerSector)
$boot[0x0D] = 1
$boot[0x15] = 0xF8
Set-U32 $boot 0x24 ([uint32]0x00800080)
Set-U64 $boot 0x28 ([uint64]$TotalClusters)
Set-U64 $boot 0x30 ([uint64]$MftCluster)
Set-U64 $boot 0x38 ([uint64]20)
$boot[0x40] = 0xF6
$boot[0x41] = 0xF4             # 4096 byte index buffers
$boot[0x1FE] = 0x55; $boot[0x1FF] = 0xAA
[Array]::Copy($boot, 0, $image, 0, 512)

# --- the $I30 node: two live entries and a leftover name in the slack ---
$live1 = New-IndexEntry -MftRecord 31 -Sequence 1 -Name 'report.pdf' -RealSize 123456
$live2 = New-IndexEntry -MftRecord 32 -Sequence 1 -Name 'photo.jpg'  -RealSize 654321
$terminal = New-IndexEntry -Terminal $true

$entriesSize = $live1.Length + $live2.Length + $terminal.Length
$entriesOffset = 0x10
$usedEnd = $entriesOffset + $entriesSize

$slackName = 'deleted_notes.txt'
$slack = New-IndexEntry -MftRecord 99 -Sequence 1 -Name $slackName -RealSize 4242
$slackPosition = Align8 $usedEnd

$contentLength = 0x10 + $slackPosition + $slack.Length
$indexRoot = New-Object byte[] $contentLength
Set-U32 $indexRoot 0x00 ([uint32]0x30)      # indexes $FILE_NAME
$indexRoot[0x04] = 1                         # collation rule
Set-U32 $indexRoot 0x05 ([uint32]4096)       # index block size
$indexRoot[0x09] = 1                         # clusters per index block
Set-U32 $indexRoot 0x10 ([uint32]$entriesOffset)
Set-U32 $indexRoot 0x14 ([uint32]$entriesSize)
Set-U32 $indexRoot 0x18 ([uint32]$entriesSize)
Set-U32 $indexRoot 0x1C ([uint32]0)

[Array]::Copy($live1, 0, $indexRoot, (0x10 + $entriesOffset), $live1.Length)
[Array]::Copy($live2, 0, $indexRoot, (0x10 + $entriesOffset + $live1.Length), $live2.Length)
[Array]::Copy($terminal, 0, $indexRoot, (0x10 + $entriesOffset + $live1.Length + $live2.Length), $terminal.Length)
[Array]::Copy($slack, 0, $indexRoot, (0x10 + $slackPosition), $slack.Length)

# --- records ---
$mftRecord = New-Record -InUse $true
$pos = 0x38
$pos = Add-NonResidentData -Record $mftRecord -Position $pos -Id 0 -StartingVcn 0 -LastVcn ($MftClusters - 1) `
                           -Lcn $MftCluster -Clusters $MftClusters -RealSize ([uint64]($MftClusters * 512))
Set-U32 $mftRecord $pos ([uint32]::MaxValue); $pos += 8
Set-U32 $mftRecord 0x18 ([uint32]$pos)

$bitmapRecord = New-Record -InUse $true
$pos = 0x38
$pos = Add-NonResidentData -Record $bitmapRecord -Position $pos -Id 0 -StartingVcn 0 -LastVcn 0 `
                           -Lcn $BitmapCluster -Clusters 1 -RealSize 512
Set-U32 $bitmapRecord $pos ([uint32]::MaxValue); $pos += 8
Set-U32 $bitmapRecord 0x18 ([uint32]$pos)

$rootRecord = New-Record -InUse $true -Directory $true -Parent 5
$pos = 0x38
$pos = Add-FileName -Record $rootRecord -Position $pos -Id 1 -Name '.' -RealSize 0 -Parent 5
Set-U32 $rootRecord $pos ([uint32]::MaxValue); $pos += 8
Set-U32 $rootRecord 0x18 ([uint32]$pos)

$docsRecord = New-Record -InUse $true -Directory $true -Parent 5
$pos = 0x38
$pos = Add-FileName -Record $docsRecord -Position $pos -Id 1 -Name 'Docs' -RealSize 0 -Parent 5
$pos = Add-ResidentNamedAttribute -Record $docsRecord -Position $pos -Type 0x90 -Id 2 -Name '$I30' -Content $indexRoot
Set-U32 $docsRecord $pos ([uint32]::MaxValue); $pos += 8
Set-U32 $docsRecord 0x18 ([uint32]$pos)

$mftOffset = $MftCluster * $BytesPerSector
foreach ($pair in @(@{n=0;r=$mftRecord}, @{n=5;r=$rootRecord}, @{n=6;r=$bitmapRecord}, @{n=30;r=$docsRecord})) {
    [Array]::Copy($pair.r, 0, $image, ($mftOffset + $pair.n * $MftRecordSize), $MftRecordSize)
}

$bmBytes = New-Object byte[] ($TotalClusters / 8)
foreach ($cluster in (@(0..($MftCluster + $MftClusters - 1)) + @($BitmapCluster))) {
    $byteIndex = $cluster -shr 3
    $bmBytes[$byteIndex] = $bmBytes[$byteIndex] -bor [byte](1 -shl ($cluster -band 7))
}
[Array]::Copy($bmBytes, 0, $image, ($BitmapCluster * $BytesPerSector), $bmBytes.Length)

$imagePath = Join-Path $WorkDir 'i30.img'
[System.IO.File]::WriteAllBytes($imagePath, $image)

Write-Host ("  live entries : report.pdf (record 31), photo.jpg (record 32)")
Write-Host ("  slack entry  : {0}" -f $slackName)

$csvPath = Join-Path $WorkDir 'i30.csv'
$output = Invoke-Carver @('--i30', $imagePath, $csvPath)
Write-Host ''
Write-Host ($output.TrimEnd())

$failures = @()

if (-not (Test-Path -LiteralPath $csvPath)) {
    Write-Host ''
    Write-Host 'FAILED: no csv written'
    exit 1
}

$rows = @(Import-Csv -LiteralPath $csvPath)
$byName = @{}
foreach ($row in $rows) { $byName[$row.name] = $row }

Write-Host ''
Write-Host '=== verifying ==='

function Assert-Name {
    param([string]$Name, [string]$Path, [string]$Size, [string]$Source)

    if (-not $byName.ContainsKey($Name)) {
        Write-Host ("  [FAIL] {0} not listed" -f $Name)
        $script:failures += "missing-$Name"
        return
    }

    $row = $byName[$Name]
    $ok = $true
    if ($row.path -ne $Path) { Write-Host ("  [FAIL] {0}: path '{1}', expected '{2}'" -f $Name, $row.path, $Path); $ok = $false }
    if ($row.size -ne $Size) { Write-Host ("  [FAIL] {0}: size {1}, expected {2}" -f $Name, $row.size, $Size); $ok = $false }
    if ($row.source -ne $Source) { Write-Host ("  [FAIL] {0}: source '{1}', expected '{2}'" -f $Name, $row.source, $Source); $ok = $false }

    if ($ok) {
        Write-Host ("  [ok]   {0} -> {1} ({2} bytes, {3})" -f $Name, $row.path, $row.size, $row.source)
    } else {
        $script:failures += "wrong-$Name"
    }
}

Assert-Name -Name 'report.pdf' -Path 'Docs/report.pdf' -Size '123456' -Source 'index'
Assert-Name -Name 'photo.jpg'  -Path 'Docs/photo.jpg'  -Size '654321' -Source 'index'
Assert-Name -Name $slackName   -Path "Docs/$slackName" -Size '4242'   -Source 'index slack'

if ($output -match 'names from indexes\s+:\s+2') {
    Write-Host '  [ok]   reported 2 names from the index'
} else {
    Write-Host '  [FAIL] index name count wrong'
    $failures += 'count-live'
}

if ($output -match 'names from slack\s+:\s+1') {
    Write-Host '  [ok]   reported 1 name from slack'
} else {
    Write-Host '  [FAIL] slack name count wrong'
    $failures += 'count-slack'
}

$mftRecordNumber = $byName['report.pdf'].mft_record
if ($mftRecordNumber -eq '31') {
    Write-Host '  [ok]   record number taken from the index entry'
} else {
    Write-Host ("  [FAIL] record number was {0}, expected 31" -f $mftRecordNumber)
    $failures += 'record'
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
