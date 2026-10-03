# Generates src/mhide.ico: a mouse hole in a wall with two white eyes peeking
# out of the dark. Uses only System.Drawing, which ships with Windows PowerShell.
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 48, 64, 256
$pngs = @()

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Everything is laid out on a 16x16 grid and scaled to the target size.
    $k = $s / 16.0
    $wall   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 205, 205, 205))
    $edge   = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 70, 70, 70)), ([Math]::Max(1.0, 0.9 * $k))
    $dark   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 16, 16, 18))
    $white  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)

    # Arch-shaped hole: a semicircle on top of a rectangle, open at the floor.
    $left = 1.5 * $k; $right = 14.5 * $k; $top = 2.5 * $k; $floor = 14.5 * $k
    $w = $right - $left
    $arch = New-Object System.Drawing.Drawing2D.GraphicsPath
    $arch.AddArc($left, $top, $w, $w, 180, 180)
    $arch.AddLine($right, $top + $w / 2, $right, $floor)
    $arch.AddLine($right, $floor, $left, $floor)
    $arch.CloseFigure()

    # A light wall plate behind the hole so the black arch reads on dark taskbars.
    $g.FillRectangle($wall, 0, 0, $s, $s)
    $g.FillPath($dark, $arch)
    $g.DrawPath($edge, $arch)
    # Skirting board along the floor.
    $g.FillRectangle($dark, 0, $floor, $s, $s - $floor)
    $g.DrawLine($edge, 0, $floor, $s, $floor)

    # Two white eyes peeking out of the dark.
    $eyeW = 2.6 * $k; $eyeH = 3.0 * $k; $eyeY = 8.0 * $k
    $g.FillEllipse($white, 4.4 * $k, $eyeY, $eyeW, $eyeH)
    $g.FillEllipse($white, 9.0 * $k, $eyeY, $eyeW, $eyeH)

    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += ,@{ Size = $s; Data = $ms.ToArray() }
    $bmp.Dispose()
}

# ICO container with PNG-compressed entries (supported since Windows Vista).
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $dim = if ($p.Size -ge 256) { 0 } else { $p.Size }
    $w.Write([Byte]$dim); $w.Write([Byte]$dim); $w.Write([Byte]0); $w.Write([Byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$p.Data.Length); $w.Write([UInt32]$offset)
    $offset += $p.Data.Length
}
foreach ($p in $pngs) { $w.Write($p.Data) }
$w.Flush()

$dest = Join-Path (Resolve-Path (Join-Path $PSScriptRoot "..\src")).Path "mhide.ico"
[System.IO.File]::WriteAllBytes($dest, $out.ToArray())
# Also drop a PNG preview next to the script for a quick look.
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot "icon-preview-256.png"), $pngs[-1].Data)
Write-Host "Wrote $dest ($($out.Length) bytes)"
