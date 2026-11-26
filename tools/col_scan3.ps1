# ASCII-only: dark pixel y-lists for columns 158..163 to test two-px dotted line theory
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_fixed.png")
for ($x = 157; $x -le 163; $x++) {
    $ys = @()
    for ($y = 0; $y -lt 100; $y++) {
        $p = $bmp.GetPixel($x, $y)
        if ($p.R -lt 128) { $ys += $y }
    }
    Write-Output ("col {0}: n={1} ys={2}" -f $x, $ys.Count, ($ys -join ","))
}
$bmp.Dispose()
