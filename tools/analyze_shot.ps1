# 离线分析 shot_text.txt：解码 b64 → 检查 RLE 包 → 渲染 PNG
$text = [IO.File]::ReadAllText("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_text.txt")
$s = $text.IndexOf("B64 ")
Write-Output ("B64 marker at: {0}" -f $s)
$b64Start = $text.IndexOf("`n", $s) + 1
$b64End = $text.IndexOf("ENDB64")
$b64 = $text.Substring($b64Start, $b64End - $b64Start).Replace("`r","").Replace("`n","").Trim()
Write-Output ("b64 chars: {0}" -f $b64.Length)
$rle = [Convert]::FromBase64String($b64)
Write-Output ("rle bytes: {0}" -f $rle.Length)

# 前 8 个包 + 汇总
$totalPx = 0; $packets = 0
for ($i = 0; $i + 4 -le $rle.Length; $i += 4) {
    $c = $rle[$i] -bor ($rle[$i+1] -shl 8)
    $r = $rle[$i+2] -bor ($rle[$i+3] -shl 8)
    if ($packets -lt 8) { Write-Output ("P{0}: color=0x{1:X4} run={2}" -f $packets, $c, $r) }
    $totalPx += $r; $packets++
    if ($totalPx -ge 76800) { break }
}
Write-Output ("packets={0} totalPx={1}" -f $packets, $totalPx)
