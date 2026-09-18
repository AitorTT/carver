#Requires -Version 5.1

<#
    End-to-end test for carver-cli.

    Builds a synthetic image filled with pseudo-random junk (seeded, so runs are
    deterministic) and embeds real files at chosen offsets, including files that
    deliberately straddle a read-chunk boundary.

    Random junk is the point: it is what exposes weak signatures. Any recovered
    file beyond the expected set is a false positive and fails the run.

    No administrator rights required: the carver reads a plain image file.
#>

[CmdletBinding()]
param(
    [string] $CarverExe,
    [string] $WorkDir,
    [int]    $ChunkSize = 65536,
    [int]    $RandomSeed = 1337
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptRoot = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $CarverExe) { $CarverExe = Join-Path $scriptRoot '..\build\carver-cli.exe' }
if (-not $WorkDir)   { $WorkDir   = Join-Path $scriptRoot '..\testdata' }
$CarverExe = [System.IO.Path]::GetFullPath($CarverExe)
$WorkDir   = [System.IO.Path]::GetFullPath($WorkDir)

function New-SolidBitmap {
    param([string] $Path, [System.Drawing.Imaging.ImageFormat] $Format, [int] $Size)

    $bitmap = New-Object System.Drawing.Bitmap($Size, $Size)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.Clear([System.Drawing.Color]::FromArgb(200, 60, 30))
            $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
            try {
                $graphics.FillEllipse($brush, 1, 1, $Size - 2, $Size - 2)
            } finally {
                $brush.Dispose()
            }
        } finally {
            $graphics.Dispose()
        }
        $bitmap.Save($Path, $Format)
    } finally {
        $bitmap.Dispose()
    }
}

function New-TestWav {
    param([string] $Path, [int] $SampleCount = 8000)

    $dataSize = $SampleCount * 2
    $stream = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter($stream)
    try {
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([uint32] (36 + $dataSize))
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('WAVE'))
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('fmt '))
        $writer.Write([uint32] 16)
        $writer.Write([uint16] 1)
        $writer.Write([uint16] 1)
        $writer.Write([uint32] 44100)
        $writer.Write([uint32] (44100 * 2))
        $writer.Write([uint16] 2)
        $writer.Write([uint16] 16)
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([uint32] $dataSize)

        for ($sample = 0; $sample -lt $SampleCount; ++$sample) {
            $writer.Write([int16] ([Math]::Sin($sample / 20.0) * 16000))
        }
        $writer.Flush()

        [System.IO.File]::WriteAllBytes($Path, $stream.ToArray())
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function New-TextPdf {
    param([string] $Path)

    $builder = New-Object System.Text.StringBuilder
    [void] $builder.Append("%PDF-1.4`n")
    [void] $builder.Append("1 0 obj`n<< /Type /Catalog /Pages 2 0 R >>`nendobj`n")
    [void] $builder.Append("2 0 obj`n<< /Type /Pages /Kids [3 0 R] /Count 1 >>`nendobj`n")
    [void] $builder.Append("3 0 obj`n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>`nendobj`n")
    [void] $builder.Append("trailer`n<< /Root 1 0 R /Size 4 >>`n")
    [void] $builder.Append("%%EOF")

    [System.IO.File]::WriteAllText($Path, $builder.ToString(), [System.Text.Encoding]::ASCII)
}

function Get-Sha256 {
    param([string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

if (-not (Test-Path -LiteralPath $CarverExe)) {
    throw "carver-cli.exe not found at '$CarverExe'. Build it first."
}

Add-Type -AssemblyName System.Drawing

$originalsDir = Join-Path $WorkDir 'originals'
$outputDir    = Join-Path $WorkDir 'recovered'
$imagePath    = Join-Path $WorkDir 'image.img'

foreach ($directory in @($WorkDir, $originalsDir, $outputDir)) {
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

Write-Host '=== generating source files ==='

$jpegPath = Join-Path $originalsDir 'photo.jpg'
$pngPath  = Join-Path $originalsDir 'graphic.png'
$gifPath  = Join-Path $originalsDir 'animation.gif'
$bmpPath  = Join-Path $originalsDir 'bitmap.bmp'
$wavPath  = Join-Path $originalsDir 'sound.wav'
$pdfPath  = Join-Path $originalsDir 'document.pdf'
$zipPath  = Join-Path $originalsDir 'archive.zip'
$pePath   = $CarverExe

New-SolidBitmap -Path $jpegPath -Format ([System.Drawing.Imaging.ImageFormat]::Jpeg) -Size 96
New-SolidBitmap -Path $pngPath  -Format ([System.Drawing.Imaging.ImageFormat]::Png)  -Size 64
New-SolidBitmap -Path $gifPath  -Format ([System.Drawing.Imaging.ImageFormat]::Gif)  -Size 48
New-SolidBitmap -Path $bmpPath  -Format ([System.Drawing.Imaging.ImageFormat]::Bmp)  -Size 64
New-TestWav     -Path $wavPath
New-TextPdf     -Path $pdfPath
Compress-Archive -Path $pdfPath -DestinationPath $zipPath -CompressionLevel Optimal

$jpeg = [System.IO.File]::ReadAllBytes($jpegPath)
$png  = [System.IO.File]::ReadAllBytes($pngPath)
$gif  = [System.IO.File]::ReadAllBytes($gifPath)
$bmp  = [System.IO.File]::ReadAllBytes($bmpPath)
$wav  = [System.IO.File]::ReadAllBytes($wavPath)
$pdf  = [System.IO.File]::ReadAllBytes($pdfPath)
$zip  = [System.IO.File]::ReadAllBytes($zipPath)
$pe   = [System.IO.File]::ReadAllBytes($pePath)

Write-Host ("  jpeg  {0,9} bytes" -f $jpeg.Length)
Write-Host ("  png   {0,9} bytes" -f $png.Length)
Write-Host ("  gif   {0,9} bytes" -f $gif.Length)
Write-Host ("  bmp   {0,9} bytes" -f $bmp.Length)
Write-Host ("  wav   {0,9} bytes" -f $wav.Length)
Write-Host ("  pdf   {0,9} bytes" -f $pdf.Length)
Write-Host ("  zip   {0,9} bytes" -f $zip.Length)
Write-Host ("  pe    {0,9} bytes  (carver-cli.exe itself)" -f $pe.Length)

Write-Host ''
Write-Host '=== assembling image ==='

$pngHeaderOffset = $ChunkSize - 3
$gifStart        = $ChunkSize * 2 + 1 - $gif.Length
$bmpOffset       = 300000
$wavOffset       = 400000
$jpegOffset      = 500000
$pdfOffset       = 600000
$peOffset        = 700000
$zipOffset       = $peOffset + $pe.Length + 100000
$endOffset       = $zipOffset + $zip.Length + 200000

if ($gifStart -le ($pngHeaderOffset + $png.Length)) {
    throw 'layout overlap: gif placement collides with png'
}

$script:Rng = New-Object System.Random($RandomSeed)

function Write-Junk {
    param([System.IO.Stream] $Stream, [int64] $Count)

    $block = New-Object byte[] 262144
    $remaining = [int64] $Count
    while ($remaining -gt 0) {
        $script:Rng.NextBytes($block)
        $take = [int] [Math]::Min($remaining, $block.Length)
        $Stream.Write($block, 0, $take)
        $remaining -= $take
    }
}

function Write-JunkTo {
    param([System.IO.Stream] $Stream, [int64] $Target)

    $delta = $Target - $Stream.Position
    if ($delta -lt 0) {
        throw ("layout overlap: target {0} is behind position {1}" -f $Target, $Stream.Position)
    }
    Write-Junk -Stream $Stream -Count $delta
}

$stream = [System.IO.File]::Create($imagePath)
try {
    Write-JunkTo -Stream $stream -Target $pngHeaderOffset
    $stream.Write($png, 0, $png.Length)

    Write-JunkTo -Stream $stream -Target $gifStart
    $stream.Write($gif, 0, $gif.Length)

    Write-JunkTo -Stream $stream -Target $bmpOffset
    $stream.Write($bmp, 0, $bmp.Length)

    Write-JunkTo -Stream $stream -Target $wavOffset
    $stream.Write($wav, 0, $wav.Length)

    Write-JunkTo -Stream $stream -Target $jpegOffset
    $stream.Write($jpeg, 0, $jpeg.Length)

    Write-JunkTo -Stream $stream -Target $pdfOffset
    $stream.Write($pdf, 0, $pdf.Length)

    Write-JunkTo -Stream $stream -Target $peOffset
    $stream.Write($pe, 0, $pe.Length)

    Write-JunkTo -Stream $stream -Target $zipOffset
    $stream.Write($zip, 0, $zip.Length)

    Write-JunkTo -Stream $stream -Target $endOffset
} finally {
    $stream.Dispose()
}

Write-Host ("  image {0} bytes of random junk + embedded files" -f (Get-Item -LiteralPath $imagePath).Length)
Write-Host ("  png header at {0}, straddles chunk boundary {1}" -f $pngHeaderOffset, $ChunkSize)
Write-Host ("  gif footer ends at {0}, straddles chunk boundary {1}" -f ($gifStart + $gif.Length), ($ChunkSize * 2))

Write-Host ''
Write-Host '=== carving ==='
& $CarverExe $imagePath $outputDir --chunk $ChunkSize
if ($LASTEXITCODE -ne 0) { throw "carver-cli exited with $LASTEXITCODE" }

Write-Host ''
Write-Host '=== verifying ==='

$recovered = @(Get-ChildItem -LiteralPath $outputDir -File)
$failures = @()

$recoveredHashes = @{}
foreach ($file in $recovered) {
    $recoveredHashes[(Get-Sha256 $file.FullName)] = $file.Name
}

$exactCases = @(
    @{ Name = 'png';  Path = $pngPath },
    @{ Name = 'gif';  Path = $gifPath },
    @{ Name = 'bmp';  Path = $bmpPath },
    @{ Name = 'wav';  Path = $wavPath },
    @{ Name = 'jpeg'; Path = $jpegPath },
    @{ Name = 'pdf';  Path = $pdfPath }
)

foreach ($case in $exactCases) {
    $hash = Get-Sha256 $case.Path
    if ($recoveredHashes.ContainsKey($hash)) {
        Write-Host ("  [ok]   {0,-5} exact, byte-identical -> {1}" -f $case.Name, $recoveredHashes[$hash])
    } else {
        Write-Host ("  [FAIL] {0,-5} not recovered byte-identical" -f $case.Name)
        $failures += $case.Name
    }
}

$peHash = Get-Sha256 $pePath
if ($recoveredHashes.ContainsKey($peHash)) {
    Write-Host ("  [ok]   pe    exact, byte-identical (incl. COFF symbol table) -> {0}" -f $recoveredHashes[$peHash])
} else {
    Write-Host '  [FAIL] pe    not recovered byte-identical'
    $failures += 'pe'
}

$zipDetected = $false
foreach ($file in $recovered) {
    $head = [System.IO.File]::ReadAllBytes($file.FullName)
    if ($head.Length -ge 4 -and $head[0] -eq 0x50 -and $head[1] -eq 0x4B -and
        $head[2] -eq 0x03 -and $head[3] -eq 0x04) {
        $zipDetected = $true
        Write-Host ("  [ok]   zip   header detected, length unbounded -> {0}" -f $file.Name)
        break
    }
}
if (-not $zipDetected) {
    Write-Host '  [FAIL] zip   header not detected'
    $failures += 'zip'
}

$expectedCount = $exactCases.Count + 2
Write-Host ''
if ($recovered.Count -eq $expectedCount) {
    Write-Host ("  [ok]   false positives: none ({0} files recovered, {1} expected)" -f $recovered.Count, $expectedCount)
} else {
    Write-Host ("  [FAIL] false positives: {0} files recovered, {1} expected" -f $recovered.Count, $expectedCount)
    $failures += 'false-positives'
}

Write-Host ''
foreach ($file in $recovered) {
    Write-Host ("  {0,-30} {1,10} bytes" -f $file.Name, $file.Length)
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: {0}" -f ($failures -join ', '))
    exit 1
}

Write-Host 'ALL CHECKS PASSED'
exit 0
