# -*- coding: utf-8 -*-
"""check_photo_ui.py —— 验证「设置 → Photo」子页与拍照档位
用法：python tools/check_photo_ui.py
流程：读开机日志（确认 NVS 档位）→ 进设置 → 切到 Photo 焦点 → 进子页 → 抓整帧
"""
import base64
import re
import struct
import time
import zlib

import serial


def png(path, px, W, H):
    rows = [bytes(v for p in px[y * W:(y + 1) * W] for v in p) for y in range(H)]
    raw = b"".join(b"\x00" + r for r in rows)

    def ch(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + ch(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
        + ch(b"IDAT", zlib.compress(raw, 9))
        + ch(b"IEND", b"")
    )


def rle_to_px(rle, W, H):
    px = [(0, 0, 0)] * (W * H)
    pos = i = 0
    while pos < W * H and i + 4 <= len(rle):
        c = rle[i] | (rle[i + 1] << 8)
        r = rle[i + 2] | (rle[i + 3] << 8)
        if r <= 0:
            break
        c = ((c & 0xFF) << 8) | (c >> 8)      # Sprite 回传需换字节
        px[pos:pos + r] = [(((c >> 11) & 31) * 255 // 31,
                            ((c >> 5) & 63) * 255 // 63,
                            (c & 31) * 255 // 31)] * r
        pos += r
        i += 4
    return px


ser = serial.Serial("COM5", 115200, timeout=0.3)
try:
    ser.set_buffer_size(rx_size=1024 * 1024)
except Exception:
    pass


def flush():
    while ser.in_waiting:
        ser.read(65536)


def send(cmd, wait=1.2):
    flush()
    ser.write((cmd + "\n").encode())
    ser.flush()
    time.sleep(wait)
    flush()


def read_until(tag, timeout=40):
    tb = tag.encode()
    buf = b""
    t = time.time()
    while time.time() - t < timeout:
        d = ser.read(65536)
        if d:
            buf += d
            if tb in buf:
                break
    return buf.decode("utf-8", "replace")


def shot(name):
    flush()
    ser.write(b"shot\n")
    ser.flush()
    txt = read_until("ENDB64")
    if "B64 " not in txt:
        print(name, "NO DATA", txt[:120])
        return
    i0 = txt.index("B64 ")
    st = txt.index("\n", i0) + 1
    en = txt.index("ENDB64", st)
    rle = base64.b64decode(re.sub(r"[^A-Za-z0-9+/=]", "", txt[st:en]))
    png(name, rle_to_px(rle, 320, 240), 320, 240)
    print("saved", name)


print("=== 开机日志 ===")
boot = read_until("[MAIN] 初始化完成", timeout=20)
for ln in boot.splitlines():
    if "[CAM]" in ln or "档位" in ln:
        print(" ", ln.strip())

print("\n=== 设置 → Photo 子页 ===")
send("theme d", 1.5)
send("app 2", 3.0)          # 设置
shot("verify_settings.png")  # 三按钮布局
send("gdn 2", 1.5)          # 焦点 0 → 2（Photo）
send("wopen", 2.0)          # 进入 Photo 子页
shot("verify_photo_sub.png")

send("gdn 1", 1.0)          # 焦点 → Color
send("gdn 1", 1.0)          # 焦点 → White balance
send("wopen", 1.5)          # 切一次白平衡（Auto → Sunny），看是否落盘
shot("verify_photo_wb.png")
send("theme l", 1.5)
shot("verify_photo_light.png")

print("\n=== 白平衡档位实测（连拍 3 张，看偏色是否稳定）===")
send("wopen", 1.0)          # 再切一次 → Cloudy
send("wopen", 1.0)          # → Office
send("wopen", 1.0)          # → Home
send("wopen", 1.0)          # → Auto（回到自动）
send("appcam", 4.0)
for i in range(2):
    flush()
    ser.write(b"snap\n")
    ser.flush()
    out = read_until("[PHOTO] 前台耗时", timeout=25)
    for ln in out.splitlines():
        if "前台耗时" in ln or "大图 " in ln:
            print(" ", ln.strip())
    time.sleep(1.5)
ser.close()
print("done")
