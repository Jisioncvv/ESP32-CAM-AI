# ASCII-only: minimal LockBits renderer sanity test
Add-Type -AssemblyName System.Drawing
$w = 320; $h = 240
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$fmt = [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
$bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $fmt)
$ptr = $bd.Scan0; $stride = $bd.Stride
Write-Output ("stride=" + $stride + " scan0=" + $ptr)
for ($i = 0; $i -lt $w * $h; $i++) {
    $x = $i % $w; $y = [int]($i / $w)
    $off = $y * $stride + $x * 3
    [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off, [byte]255)
    [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off + 1, [byte]255)
    [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off + 2, [byte]255)
}
$bmp.UnlockBits($bd)
$bmp.Save("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\render_test.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
$bmp2 = New-Object System.Drawing.Bitmap("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\render_test.png")
$bad = 0
for ($y = 0; $y -lt $h; $y++) {
    $p = $bmp2.GetPixel(5, $y)
    if ($p.R -lt 250) { $bad++; if ($bad -lt 10) { Write-Output ("dark at y=" + $y + " R=" + $p.R) } }
}
Write-Output ("dark pixels col5: " + $bad)
$bmp2.Dispose()
