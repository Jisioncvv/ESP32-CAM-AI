Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Bitmap]::FromFile("e:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\assets\penguin.jpg")
Write-Output "Source: $($src.Width)x$($src.Height)"

# crop penguin body (with margin)
$side = 1180
$crop = New-Object System.Drawing.Bitmap($side, $side)
$g = [System.Drawing.Graphics]::FromImage($crop)
$g.DrawImage($src, (New-Object System.Drawing.Rectangle(0,0,$side,$side)),
             (New-Object System.Drawing.Rectangle(365,450,$side,$side)),
             [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose()

# resize to 96x96
$size = 96
$small = New-Object System.Drawing.Bitmap($size, $size)
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g2.DrawImage($crop, 0, 0, $size, $size)
$g2.Dispose()

# convert to RGB565 (R5G6B5) C array (little-endian uint16, use setSwapBytes(true))
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("/*")
$lines.Add(" * splash_penguin.h - boot splash penguin icon (96x96 RGB565)")
$lines.Add(" * generated from assets/penguin.jpg by assets/convert_penguin.ps1")
$lines.Add(" * byte order: little-endian uint16, call tft.setSwapBytes(true) before pushImage")
$lines.Add(" */")
$lines.Add("#pragma once")
$lines.Add("#include <Arduino.h>")
$lines.Add("")
$lines.Add("#define PENGUIN_W 96")
$lines.Add("#define PENGUIN_H 96")
$lines.Add("")
$lines.Add("static const uint16_t gPenguin[PENGUIN_W * PENGUIN_H] PROGMEM = {")

for ($y = 0; $y -lt $size; $y++) {
    $vals = @()
    for ($x = 0; $x -lt $size; $x++) {
        $c = $small.GetPixel($x, $y)
        $r5 = [int]([math]::Round($c.R * 31 / 255))
        $g6 = [int]([math]::Round($c.G * 63 / 255))
        $b5 = [int]([math]::Round($c.B * 31 / 255))
        $v = ($r5 -shl 11) -bor ($g6 -shl 5) -bor $b5
        $vals += ("0x{0:X4}" -f $v)
    }
    $lines.Add("  " + ($vals -join ", ") + ",")
}
$lines.Add("};")

$lines | Out-File -FilePath "e:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\src\splash_penguin.h" -Encoding ascii
$src.Dispose(); $crop.Dispose(); $small.Dispose()
Write-Output "Written: src\splash_penguin.h (96x96 = 18KB flash)"
