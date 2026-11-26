# ASCII-only: print raw pixel darkness of T render around x=232..248, y=0..40
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_desktop_offline.png")
for ($y = 0; $y -lt 40; $y++) {
    $row = ""
    for ($x = 232; $x -le 248; $x++) {
        $p = $bmp.GetPixel($x, $y)
        if ($p.R -lt 200) { $row += "X" } elseif ($p.R -lt 250) { $row += "o" } else { $row += "." }
    }
    Write-Output ("y={0,3} {1}" -f $y, $row)
}
$bmp.Dispose()
