# 快速验证：B64 首段解码字节 + 完整流按行分析行结构
$b = [Convert]::FromBase64String('///wAAAAUAD/')
Write-Output ("first12: " + (($b | ForEach-Object { '0x{0:X2}' -f $_ }) -join ' '))

$text = [IO.File]::ReadAllText("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_text.txt")
$s = $text.IndexOf("B64 ")
$b64Start = $text.IndexOf("`n", $s) + 1
$b64End = $text.IndexOf("ENDB64")
$b64 = $text.Substring($b64Start, $b64End - $b64Start).Replace("`r","").Replace("`n","").Trim()
$rle = [Convert]::FromBase64String($b64)
Write-Output ("rle bytes: " + $rle.Length + "  first bytes: " + (($rle[0..11] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '))

# 逐包走完 76800 像素，按 320 一行统计：每行白起点/白终点
$px = 0; $pi = 0
$rowWhiteEnd = -1
$rowFirstColor = @()
$anomalies = 0
while ($px -lt 76800 -and $pi + 4 -le $rle.Length) {
    $c = $rle[$pi] -bor ($rle[$pi+1] -shl 8)
    $r = $rle[$pi+2] -bor ($rle[$pi+3] -shl 8)
    $row = [int]($px / 320)
    $col = $px % 320
    if ($col -eq 0 -and $rowWhiteEnd -ge 0 -and $rowWhiteEnd -ne 240) { $anomalies++ }
    if ($col -eq 0) {
        $rowWhiteEnd = -1
        if ($row -lt 3 -or ($row -gt 76 -and $row -lt 80)) {
            Write-Output ("row {0} starts color=0x{1:X4} run={2}" -f $row, $c, $r)
        }
    }
    if ($c -eq 0xFFFF -and $col -eq 0) { $rowWhiteEnd = $col + $r }
    elseif ($c -eq 0xFFFF) { $rowWhiteEnd = $col + $r }
    $px += $r; $pi += 4
}
Write-Output ("total px=" + $px + "  rows whose white-span != 240: " + $anomalies)
