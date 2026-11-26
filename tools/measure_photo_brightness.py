"""
成片亮度 vs 收敛预算：找出让照片亮度追上实时预览的 settle 时间。
用法：python tools/measure_photo_brightness.py
输出每档的 照片平均亮度 Y，与预览亮度对比（表 + 结论）。
"""
import io
import os
import sys
import time

import serial
from PIL import Image, ImageStat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_photos import fetch_one  # noqa: E402

PORT = "COM5"
OUT = ".tmp_bright"
os.makedirs(OUT, exist_ok=True)

ser = serial.Serial(PORT, 115200, timeout=0.3)


def wait_boot(marker="进入桌面", timeout=35):
    buf = b""
    t = time.time()
    while time.time() - t < timeout:
        d = ser.read(4096)
        if d:
            buf += d
            if marker.encode() in buf:
                time.sleep(1.0)
                return True
    return False


def send(cmd, wait=1.0):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    t = time.time()
    buf = b""
    while time.time() - t < wait:
        d = ser.read(8192)
        if d:
            buf += d
    return buf.decode("utf-8", "replace")


def brightness(jpg_bytes):
    im = Image.open(io.BytesIO(jpg_bytes)).convert("RGB")
    st = ImageStat.Stat(im)
    r, g, b = st.mean
    return 0.299 * r + 0.587 * g + 0.114 * b, (r, g, b), im.size


print("等待启动:", wait_boot())
send("app 0", 7.0)
send("psz 1", 0.5)
send("wb 0", 0.5)

rows = []
for ms in (900, 1500, 2500, 3500):
    send("setl %d" % ms, 0.5)
    t0 = time.time()
    send("snap", 16)
    got = fetch_one(ser, 1, OUT)      # idx=1 = 最新一张（0 无效，会回退到同一张）
    if not got:
        print("settle=%d 抓取失败" % ms)
        continue
    path = got[0] if isinstance(got, tuple) else got
    data = open(path, "rb").read()
    y, rgb, size = brightness(data)
    rows.append((ms, y, rgb, size, time.time() - t0))
    print("settle=%4dms  Y=%5.1f  RGB=(%.0f,%.0f,%.0f)  %s  %.1fs"
          % (ms, y, rgb[0], rgb[1], rgb[2], size, time.time() - t0))

print()
print("| 收敛预算 | 成片亮度 Y | 分辨率 |")
print("|---|---|---|")
for ms, y, rgb, size, el in rows:
    print("| %d ms | %.1f | %dx%d |" % (ms, y, size[0], size[1]))
ser.close()
