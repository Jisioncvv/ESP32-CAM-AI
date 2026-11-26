# 无 DTR 串口抓取（铁律：绝不拉 DTR，否则复位后进 DOWNLOAD 模式卡死）
# 参数硬编码：后台运行环境无法可靠传参
$Seconds = 150
$OutFile = "E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\serial_log.txt"
$port = New-Object System.IO.Ports.SerialPort('COM5', 115200)
$port.DtrEnable = $false
$port.RtsEnable = $false
$port.ReadTimeout = 200
$port.Open()
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$log = New-Object System.Text.StringBuilder
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try {
        $n = $port.BytesToRead
        if ($n -gt 0) {
            $buf = New-Object byte[] $n
            $port.Read($buf, 0, $n) | Out-Null
            [void]$log.Append([System.Text.Encoding]::UTF8.GetString($buf))
        } else {
            Start-Sleep -Milliseconds 50
        }
    } catch {
        $port.Close(); Start-Sleep -Milliseconds 500; $port.Open()
    }
}
$port.Close()
[System.IO.File]::WriteAllText($OutFile, $log.ToString(), [System.Text.Encoding]::UTF8)
Write-Output ("captured " + $log.Length + " chars")
