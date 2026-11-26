# ASCII-only: render PNG from saved shot_text.txt (decode via multiply, avoid PS5 -shl byte trap)
if ($args.Count -ge 1) { $outPath = $args[0] } else { $outPath = "E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_fixed.png" }
$text = [IO.File]::ReadAllText("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_text.txt")
$s = $text.IndexOf("B64 ")
$b64Start = $text.IndexOf("`n", $s) + 1
$b64End = $text.IndexOf("ENDB64")
$b64 = $text.Substring($b64Start, $b64End - $b64Start).Replace("`r","").Replace("`n","").Trim()
$rle = [Convert]::FromBase64String($b64)
Write-Output ("rle bytes: " + $rle.Length + " / header says: " + $text.Substring($s, 12))
$w = 320; $h = 240
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$bd = $bmp.LockBits((New-Object System.Drawing.Rectangle(0,0,$w,$h)), [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$ptr = $bd.Scan0; $stride = $bd.Stride
$total = $w * $h; $px = 0; $i = 0
while ($px -lt $total -and ($i + 4) -le $rle.Length) {
    $c = [int]$rle[$i] + 256 * [int]$rle[$i+1]
    $r = [int]$rle[$i+2] + 256 * [int]$rle[$i+3]
    if ($r -le 0) { Write-Output ("BAD run at packet " + ($i/4)); break }
    $rr = [int]((($c -shr 11) -band 31) * 255 / 31)
    $gg = [int]((($c -shr 5) -band 63) * 255 / 63)
    $bb = [int](($c -band 31) * 255 / 31)
    for ($k = 0; $k -lt $r -and $px -lt $total; $k++) {
        $x = $px % $w; $y = [int]($px / $w)
        $off = $y * $stride + $x * 3
        [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off, [byte]$bb)
        [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off + 1, [byte]$gg)
        [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off + 2, [byte]$rr)
        $px++
    }
    $i += 4
}
$bmp.UnlockBits($bd)
Write-Output ("outPath len=" + $outPath.Length + " val=" + $outPath)
$bmp.Save("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_fixed.png")
$bmp.Dispose()
Write-Output ("pixels=" + $px + " saved")
