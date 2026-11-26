# Zoom in on the dashed line region around x=160
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_fixed.png")
$crop = New-Object System.Drawing.Bitmap(80, 240)
$g = [System.Drawing.Graphics]::FromImage($crop)
$g.DrawImage($src, (New-Object System.Drawing.Rectangle(0,0,80,240)), (New-Object System.Drawing.Rectangle(120,0,80,240)), [System.Drawing.GraphicsUnit]::Pixel)
$big = New-Object System.Drawing.Bitmap(240, 480)
$g2 = [System.Drawing.Graphics]::FromImage($big)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g2.DrawImage($crop, 0, 0, 240, 480)
$big.Save("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\zoom_center.png", [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $g2.Dispose(); $crop.Dispose(); $big.Dispose(); $src.Dispose()
Write-Output "zoom saved"
