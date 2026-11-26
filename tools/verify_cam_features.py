"""
验证相机 App 本轮新增功能（勾图/倒计时/曝光补偿/成片确认）
用法：python tools/verify_cam_features.py
说明：串口打开会复位板子，所有步骤在同一次连接内完成。
      抓图用 shot（复用相机预览 Sprite 整帧 RLE+Base64）。
"""
import base64
import re
import struct
import time
import zlib

import serial

PORT = "COM5"
W, H = 320, 240

ser = serial.Serial(PORT, 115200, timeout=0.3)


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


def png(name, px, w, h):
    rows = [bytes(v for p in px[y * w:(y + 1) * w] for v in p) for y in range(h)]
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))
    open(name, "wb").write(data)


def rle_to_px(rle, w, h, swap=True):
    px = [(0, 0, 0)] * (w * h)
    pos = i = 0
    while pos < w * h and i + 4 <= len(rle):
        c = rle[i] | (rle[i + 1] << 8)
        r = rle[i + 2] | (rle[i + 3] << 8)
        if r <= 0:
            break
        if swap:
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
    txt = read_until("ENDB64")
    if "B64 " not in txt:
        print(name, "NO DATA", txt[:120])
        return
    i0 = txt.index("B64 ")
    st = txt.index("\n", i0) + 1
    en = txt.index("ENDB64", st)
    # 相机预览在跑时会往串口打 FPS 日志，混在 base64 里会污染流：
    # 只保留"整行都是 base64 字符"的行
    chunks = []
    for ln in txt[st:en].splitlines():
        ln = ln.strip()
        if ln and re.fullmatch(r"[A-Za-z0-9+/=]+", ln):
            chunks.append(ln)
    rle = base64.b64decode("".join(chunks))
    png(name, rle_to_px(rle, W, H), W, H)
    print("saved", name, len(rle), "bytes RLE")


print("== 等启动 ==")
time.sleep(8)
boot = send("", 1.0)
for ln in boot.splitlines():
    if "[CAM]" in ln or "档位" in ln:
        print("BOOT:", ln.strip())

print("== 进相机 App ==")
print(send("app 0", 6.0)[-300:])

# 1) 构图线
send("grid", 1.2)
shot("v_grid.png")

# 2) 曝光补偿提示
send("ev 2", 0.4)
shot("v_ev.png")
send("ev 0", 0.4)

# 3) 定时倒数（3s）—— 设 3s 后触发，抓 1.3s 时的画面
send("tmr 3s", 0.5)
send("cd", 0.1)          # 立刻返回
time.sleep(1.2)
shot("v_countdown.png")

# 4) 等这次拍完（3s 倒数 + 3.6s 拍照 + 恢复），再抓成片确认
time.sleep(9)
shot("v_shot_confirm.png")
send("tmr 0", 0.5)

ser.close()
print("done")
