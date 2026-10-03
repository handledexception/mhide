# Renders assets/logo.png (light) and assets/logo-dark.png from the same geometry as
# src/mhide.ico and assets/logo.svg. Uses only System.Drawing.
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File tools\make-logo.ps1
Add-Type -AssemblyName System.Drawing

function Render-Logo([string]$path, [System.Drawing.Color]$textColor) {
    $scale = 2                      # 2x for crisp display when shown at 420x120
    $w = 420 * $scale; $h = 120 * $scale
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    # Icon on a 16-unit grid, 96px at 1x, placed at (12,12).
    $k = 6.0 * $scale; $ox = 12 * $scale; $oy = 12 * $scale
    $g.TranslateTransform($ox, $oy)

    $plate = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 205, 205, 205))
    $edge  = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 70, 70, 70)), (0.9 * $k)
    $edge.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $dark  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 16, 16, 18))
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)

    # Rounded plate (rx = 1.5 units).
    $r = 1.5 * $k; $s = 16 * $k
    $platePath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $platePath.AddArc(0, 0, 2*$r, 2*$r, 180, 90)
    $platePath.AddArc($s - 2*$r, 0, 2*$r, 2*$r, 270, 90)
    $platePath.AddArc($s - 2*$r, $s - 2*$r, 2*$r, 2*$r, 0, 90)
    $platePath.AddArc(0, $s - 2*$r, 2*$r, 2*$r, 90, 90)
    $platePath.CloseFigure()
    $g.FillPath($plate, $platePath)

    # Arch hole.
    $left = 1.5 * $k; $right = 14.5 * $k; $top = 2.5 * $k; $floor = 14.5 * $k; $aw = $right - $left
    $arch = New-Object System.Drawing.Drawing2D.GraphicsPath
    $arch.AddArc($left, $top, $aw, $aw, 180, 180)
    $arch.AddLine($right, $top + $aw / 2, $right, $floor)
    $arch.AddLine($right, $floor, $left, $floor)
    $arch.CloseFigure()
    $g.FillPath($dark, $arch)
    $g.DrawPath($edge, $arch)

    # Skirting board, clipped to the rounded plate.
    $g.SetClip($platePath)
    $g.FillRectangle($dark, 0, $floor, $s, $s - $floor)
    $g.DrawLine($edge, 0, $floor, $s, $floor)
    $g.ResetClip()

    # Eyes.
    $g.FillEllipse($white, 4.4 * $k, 8.0 * $k, 2.6 * $k, 3.0 * $k)
    $g.FillEllipse($white, 9.0 * $k, 8.0 * $k, 2.6 * $k, 3.0 * $k)

    # Wordmark.
    $g.ResetTransform()
    $font = New-Object System.Drawing.Font "Segoe UI Semibold", (49.5 * $scale), ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
    $brush = New-Object System.Drawing.SolidBrush $textColor
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    $g.DrawString("mhide", $font, $brush, (124 * $scale), (28 * $scale), $fmt)

    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "Wrote $path"
}

$assets = (Resolve-Path (Join-Path $PSScriptRoot "..\assets")).Path
Render-Logo (Join-Path $assets "logo.png")      ([System.Drawing.Color]::FromArgb(255, 51, 51, 51))
Render-Logo (Join-Path $assets "logo-dark.png") ([System.Drawing.Color]::FromArgb(255, 230, 230, 230))
