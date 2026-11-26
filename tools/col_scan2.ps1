# ASCII-only: exact y list of dark pixels at column 160
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_fixed.png")
$ys = @()
for ($y = 0; $y -lt 240; $y++) {
    $p = $bmp.GetPixel(160, $y)
    if ($p.R -lt 128) { $ys += $y }
}
$bmp.Dispose()
Write-Output ("count=" + $ys.Count)
Write-Output ("ys: " + ($ys -join ","))
