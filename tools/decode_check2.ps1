# 稳健版：全流包直方图 + 关键字节 + PNG 渲染
Add-Type -AssemblyName System.Drawing
$text = [IO.File]::ReadAllText("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_text.txt")
$s = $text.IndexOf("B64 ")
$b64Start = $text.IndexOf("`n", $s) + 1
$b64End = $text.IndexOf("ENDB64")
$b64 = $text.Substring($b64Start, $b64End - $b64Start).Replace("`r","").Replace("`n","").Trim()
$rle = [Convert]::FromBase64String($b64)
Write-Output ("rle bytes: " + $rle.Length)
Write-Output ("bytes 12..23: " + (($rle[12..23] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '))

$hist = @{}
$px = 0; $pi = 0; $colors = New-Object 'System.Collections.Generic.List[int]'
while ($px -lt 76800 -and $pi + 4 -le $rle.Length) {
    $c = [int]$rle[$pi] + 256 * [int]$rle[$pi+1]
    $r = [int]$rle[$pi+2] + 256 * [int]$rle[$pi+3]
    if ($hist.ContainsKey($c)) { $hist[$c] += $r } else { $hist[$c] = $r }
    $colors.Add($c)
    $px += $r; $pi += 4
}
Write-Output ("packets=" + $colors.Count + "  totalPx=" + $px)
$hist.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 12 | ForEach-Object {
    Write-Output ("color=0x{0:X4}  pixels={1}" -f $_.Key, $_.Value)
}

# 渲染 PNG（行 320 宽）
$bmp = New-Object System.Drawing.Bitmap(320, 240)
$rect = New-Object System.Drawing.Rectangle(0, 0, 320, 240)
$fmt = [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
$bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $fmt)
$px = 0; $pi = 0
$ptr = $bd.Scan0
$stride = $bd.Stride
while ($px -lt 76800 -and $pi + 4 -le $rle.Length) {
    $c = [int]$rle[$pi] + 256 * [int]$rle[$pi+1]
    $r = [int]$rle[$pi+2] + 256 * [int]$rle[$pi+3]
    $r5 = ($c -shr 11) -band 31; $g6 = ($c -shr 5) -band 63; $b5 = $c -band 31
    $rr = [int]($r5 * 255 / 31); $gg = [int]($g6 * 255 / 63); $bb = [int]($b5 * 255 / 31)
    for ($k = 0; $k -lt $r; $k++) {
        $idx = $px + $k
        if ($idx -ge 76800) { break }
        $row = [int]($idx / 320); $col = $idx % 320
        $off = $row * $stride + $col * 3
        [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off, [byte]$bb)
        [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off + 1, [byte]$gg)
        [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off + 2, [byte]$rr)
    }
    $px += $r; $pi += 4
}
$bmp.UnlockBits($bd)
$bmp.Save("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_desktop_offline.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "PNG saved: shot_desktop_offline.png"
