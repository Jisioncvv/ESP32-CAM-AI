# 串口截图 v2：Base64 文本协议接收 RLE 压缩 RGB565 → 还原 PNG
# 用法: serial_shot.ps1 [-Cmd S|D] [-Out shot.png]   S=测试图案 D=真实桌面
param([string]$Out = "E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot.png", [string]$Cmd = "S")

$p = New-Object System.IO.Ports.SerialPort COM5,115200,None,8,One
$p.DtrEnable = $false; $p.RtsEnable = $false   # 绝不拉 DTR（否则复位后进下载模式）
$p.ReadBufferSize = 65536
$p.Open()
Start-Sleep -Milliseconds 600
$p.DiscardInBuffer()
$p.Write($Cmd)

$ms = New-Object System.IO.MemoryStream
$buf = New-Object byte[] 8192
$deadline = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $deadline) {
    $n = $p.BytesToRead
    if ($n -gt 0) {
        $r = $p.Read($buf, 0, [Math]::Min($n, 8192))
        $ms.Write($buf, 0, $r)
        $txt = [System.Text.Encoding]::ASCII.GetString($ms.ToArray())
        if ($txt.Contains("ENDB64")) { break }
    } else { Start-Sleep -Milliseconds 5 }
}
$p.Close()

$data = $ms.ToArray()
$text = [System.Text.Encoding]::ASCII.GetString($data)
[System.IO.File]::WriteAllText("E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_text.txt", $text)
if (-not $text.Contains("ENDB64")) { Write-Output "ERROR: no ENDB64"; exit 1 }
# 诊断行（B64 标记之前）
$diagEnd = $text.IndexOf("B64 ")
if ($diagEnd -gt 0) { Write-Output ($text.Substring(0, $diagEnd).Trim()) }
$b64Start = $text.IndexOf("`n", $text.IndexOf("B64 ")) + 1
$b64End = $text.IndexOf("ENDB64")
$b64 = $text.Substring($b64Start, $b64End - $b64Start).Replace("`r", "").Replace("`n", "").Trim()
Write-Output ("b64 chars: {0}" -f $b64.Length)
$rle = [Convert]::FromBase64String($b64)
Write-Output ("rle bytes: {0}" -f $rle.Length)

$w = 320; $h = 240
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$ptr = $bd.Scan0; $stride = $bd.Stride
$total = $w * $h; $px = 0; $i = 0
while ($px -lt $total -and ($i + 4) -le $rle.Length) {
    # 注意：PS5 的 -shl 对 Byte 有溢出陷阱（高字节会归零），必须用乘法
    $c565 = [int]$rle[$i] + 256 * [int]$rle[$i + 1]
    $run  = [int]$rle[$i + 2] + 256 * [int]$rle[$i + 3]
    if ($run -eq 0) { break }   # 防御：异常流
    $rr = [int]((($c565 -shr 11) -band 0x1F) * 255 / 31)
    $gg = [int]((($c565 -shr 5)  -band 0x3F) * 255 / 63)
    $bb = [int](($c565 -band 0x1F) * 255 / 31)
    for ($k = 0; $k -lt $run -and $px -lt $total; $k++) {
        $x = $px % $w; $y = [int]($px / $w)
        $off = $y * $stride + $x * 3
        [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off,     [byte]$bb)
        [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off + 1, [byte]$gg)
        [System.Runtime.InteropServices.Marshal]::WriteByte($ptr, $off + 2, [byte]$rr)
        $px++
    }
    $i += 4
}
$bmp.UnlockBits($bd)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("saved {0} pixels -> {1}" -f $px, $Out)
