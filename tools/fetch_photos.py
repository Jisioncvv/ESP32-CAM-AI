# -*- coding: utf-8 -*-
"""
fetch_photos.py —— 从设备串口回传 SD 卡照片并分析偏色
用法：
  C:\\Users\\Z2780\\.platformio\\penv\\Scripts\\python.exe fetch_photos.py --port COM5 --count 4
协议（固件 getphoto 命令）：
  发送 "getphoto N\\n" → 设备回 PHOTO <name> <size>\\n + 多行 Base64 + ENDPHOTO\\n
输出：photos_dump/photo_N.jpg + 每张 R/G/B 通道均值与偏色判定
"""
import argparse
import base64
import os
import re
import time

import serial

B64_RE = re.compile(rb'^[A-Za-z0-9+/=]+$')


def fetch_one(ser, idx, out_dir):
    """回传第 idx 新的照片，返回 (path, size) 或 None
    [坑1] ESP32-S3 HWCDC 会丢刚连上后的首批字节 → 握手重试
    [坑2] 每块 1024B 编码后末尾带 '=='，必须逐行解码再拼接
    [坑3] readline 逐字节解析跟不上 USB 速度会溢出丢字节 → 大块读+手动分行"""
    for attempt in range(5):
        ser.reset_input_buffer()
        ser.write(("getphoto %d\n" % idx).encode())
        ser.flush()

        buf = bytearray()
        t0 = time.time()
        last_rx = time.time()
        while time.time() - t0 < 120:
            chunk = ser.read(65536)        # 大块读：速度远超逐行 readline
            if chunk:
                buf += chunk
                last_rx = time.time()
                if buf.rstrip().endswith(b"ENDPHOTO"):
                    break
                continue
            # 无新数据：等流结束（ PHOTO 头没见到 & 静默 3s → 命令被吞，重发 ）
            if time.time() - last_rx > 3:
                break

        # ---- 解析缓冲：PHOTO 头 → Base64 行（过滤日志行）→ ENDPHOTO ----
        hdr_name, hdr_size = None, 0
        b64_lines = []
        done = False
        for line in bytes(buf).splitlines():
            s = line.strip()
            if hdr_name is None:
                if s.startswith(b"PHOTO "):
                    parts = s.split()
                    hdr_name = parts[1].decode(errors="replace")
                    hdr_size = int(parts[2])
            elif s == b"ENDPHOTO":
                done = True
                break
            elif B64_RE.match(s):
                b64_lines.append(s)

        if not done or hdr_name is None or hdr_size == 0:
            if hdr_size == 0:
                return None                # PHOTO NONE 0：没有照片
            print("  第 %d 张第 %d 次尝试失败（命令被吞/无响应），重发..." % (idx, attempt + 1))
            continue

        if not b64_lines:
            print("  第 %d 张第 %d 次尝试失败（无数据行），重发..." % (idx, attempt + 1))
            continue
        try:
            data = b''.join(base64.b64decode(p) for p in b64_lines)
        except Exception as e:
            print("  解码失败：%s，重发..." % e)
            continue

        if len(data) != hdr_size:
            print("  警告：%s 应为 %u 字节，实收 %u（丢字节），整张重传..." %
                  (hdr_name, hdr_size, len(data)))
            continue
        path = os.path.join(out_dir, os.path.basename(hdr_name))
        with open(path, "wb") as f:
            f.write(data)
        return path, len(data)
    return None


def analyze(path):
    """R/G/B 均值 + 偏色判定（R 明显高于 G/B = 偏红）"""
    from PIL import Image, ImageFile
    ImageFile.LOAD_TRUNCATED_IMAGES = True
    img = Image.open(path).convert("RGB").resize((256, 256))
    px = list(img.getdata())
    n = len(px)
    r = sum(p[0] for p in px) / n
    g = sum(p[1] for p in px) / n
    b = sum(p[2] for p in px) / n
    verdict = "正常"
    if r > 1.08 * g and r > 1.08 * b:
        verdict = "偏红"
    elif b > 1.08 * r and b > 1.08 * g:
        verdict = "偏蓝"
    elif g > 1.08 * r and g > 1.08 * b:
        verdict = "偏绿"
    return r, g, b, verdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--count", type=int, default=4)
    ap.add_argument("--out", default="photos_dump")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ser = serial.Serial(args.port, 115200, timeout=0.05)   # 短超时配合大块轮询读
    try:
        ser.set_buffer_size(rx_size=1024 * 1024)   # 加大 Windows 驱动 RX 队列防溢出
    except Exception:
        pass
    print("串口已打开：%s（USB-CDC 全速传输）" % args.port)

    got = []
    for idx in range(1, args.count + 1):
        try:
            res = fetch_one(ser, idx, args.out)
        except Exception as e:
            print("第 %d 张异常：%s" % (idx, e))
            res = None
        if res:
            path, size = res
            print("第 %d 张：%s（%u 字节）" % (idx, path, size))
            got.append(path)
        else:
            print("第 %d 张：SD 上已没有更多照片" % idx)
            break

    print("\n==== 偏色分析（RGB 通道均值，满 255）====")
    for p in got:
        r, g, b, v = analyze(p)
        print("%-28s R=%.1f  G=%.1f  B=%.1f  → %s" % (os.path.basename(p), r, g, b, v))

    ser.close()


if __name__ == "__main__":
    main()
