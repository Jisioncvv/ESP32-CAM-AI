"""
相机新增功能最终验收抓图（构图线 / 亮度档位提示 / 定时倒数 / 成片确认）。
用法：python tools/verify_cam_final.py
"""
import base64
import re
import struct
import time
import zlib

import serial

PORT = "COM5"
W, H = 320, 240
ser = serial.Serial(PORT, 115200, timeout=0.2)


def flush():
    while ser.in_waiting:
        ser.read(65536)


def send(cmd, wait=1.0):
    flush()
    ser.write((cmd + "\n").encode())
    ser.flush()
    t = time.time()
    buf = b""
    while time.time() - t < wait:
        d = ser.read(8192)
        if d:
            buf += d
    return buf.decode("utf-8", "replace")


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


def png(name, px, w, h):
    rows = [bytes(v for p in px[y * w:(y + 1) * w] for v in p) for y in range(h)]
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    open(name, "wb").write(b"\x89PNG\r\n\x1a\n"
                            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                            + chunk(b"IDAT", zlib.compress(raw, 9))
                            + chunk(b"IEND", b""))


def rle_to_px(rle, w, h):
    px = [(0, 0, 0)] * (w * h)
    pos = i = 0
    while pos < w * h and i + 4 <= len(rle):
        c = rle[i] | (rle[i + 1] << 8)
        r = rle[i + 2] | (rle[i + 3] << 8)
        if r <= 0:
            break
        c = ((c & 0xFF) << 8) | (c >> 8)
        px[pos:pos + r] = [(((c >> 11) & 31) * 255 // 31,
                            ((c >> 5) & 63) * 255 // 63,
                            (c & 31) * 255 // 31)] * r
        pos += r
        i += 4
    return px


def shot(name):
    flush()
    ser.write(b"shot\n")
    ser.flush()
    buf = b""
    t = time.time()
    while time.time() - t < 45:
        d = ser.read(65536)
        if d:
            buf += d
            if b"ENDB64" in buf:
                break
    txt = buf.decode("utf-8", "replace")
    if "B64 " not in txt:
        print(name, "NO DATA")
        return
    i0 = txt.index("B64 ")
    st = txt.index("\n", i0) + 1
    en = txt.index("ENDB64", st)
    chunks = [ln.strip() for ln in txt[st:en].splitlines()
              if ln.strip() and re.fullmatch(r"[A-Za-z0-9+/=]+", ln.strip())]
    png(name, rle_to_px(base64.b64decode("".join(chunks)), W, H), W, H)
    print("shot ->", name)


print("boot:", wait_boot())
send("app 0", 8.0)
send("psz 1", 0.5)
send("wb 0", 0.5)
send("bri 0", 0.5)
send("tmr 0", 0.5)

# 1) 构图线 + 亮度提示（两者叠加）
send("grid", 0.8)
send("bri 1", 0.15)
shot("v_final_bright.png")

# 2) 定时倒数第 2 秒
send("tmr 3s", 0.5)
send("cd", 0.1)
time.sleep(1.4)
shot("v_final_cd.png")

# 3) 等成片确认缩略图（日志同步点）
buf = b""
t = time.time()
while time.time() - t < 30:
    d = ser.read(4096)
    if d:
        buf += d
        if b"ok=1" in buf and b"thumb" in buf:
            break
shot("v_final_thumb.png")

send("tmr 0", 0.4)
send("bri 1", 0.4)      # 保持默认 +1
ser.close()
print("done")
