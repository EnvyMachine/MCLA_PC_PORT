# Builds MCLA/res/mcla.ico from the box art: the logo + car crop for large sizes,
# a tighter crop of the car's front for the small ones (where the logo is unreadable).
# Usage: powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 -Source <box art.png>
param([Parameter(Mandatory)][string]$Source, [string]$Out)
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\MCLA\res\mcla.ico' }
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Source))
# Crops in the 284x350 box art.
$large = New-Object System.Drawing.Rectangle 0, 26, 284, 284
$small = New-Object System.Drawing.Rectangle 20, 128, 215, 215
$images = @()
foreach ($size in 256, 128, 96, 64, 48, 32, 24, 16) {
    $crop = if ($size -ge 64) { $large } else { $small }
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $g.CompositingQuality = 'HighQuality'
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $size, $size), $crop, [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $images += , @($size, $ms.ToArray())
}
$src.Dispose()

# ICONDIR + ICONDIRENTRY[] + PNG data.
$fs = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $size = $img[0]; $data = $img[1]
    $dim = if ($size -ge 256) { 0 } else { $size }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$data.Length); $w.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($img in $images) { $w.Write([byte[]]$img[1]) }
$w.Flush()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($Out), $fs.ToArray())
"wrote $Out ($($fs.Length) bytes, $($images.Count) sizes)"
