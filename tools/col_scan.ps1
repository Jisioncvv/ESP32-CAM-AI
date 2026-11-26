# ASCII-only: list non-white pixels in column range. Args: -In <file> -X0 -X1 -MaxY
param([string]$In = "shot_fixed.png", [int]$X0 = 156, [int]$X1 = 164, [int]$MaxY = 240)
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\" + $In)
$hits = 0
for ($y = 0; $y -lt $MaxY; $y++) {
    $row = ""
    for ($x = $X0; $x -le $X1; $x++) {
        $p = $bmp.GetPixel($x, $y)
        if ($p.R -lt 240 -or $p.G -lt 240 -or $p.B -lt 240) {
            $row += ("x={0} y={1} RGB=({2},{3},{4})  " -f $x, $y, $p.R, $p.G, $p.B)
            $hits++
        }
    }
    if ($row -ne "") { Write-Output $row }
}
Write-Output ("total non-white px in cols {0}-{1}: {2}" -f $X0, $X1, $hits)
$bmp.Dispose()
