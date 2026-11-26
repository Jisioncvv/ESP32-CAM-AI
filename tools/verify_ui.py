"""UI 验证抓图：小键盘对齐 + 相册九宫格 + 相册全屏（fshot）

用法: python tools/verify_ui.py
产出: verify_*.png（与设备屏幕一致的真彩图）
"""
import serial, time, base64, struct, zlib, re, sys

PORT = "COM5"
ser = serial.Serial(PORT, 115200, timeout=0.3)
time.sleep(8)          # 开机动画 3s + 桌面就绪


def flush():
    while ser.in_waiting:
        ser.read(65536)


def send(c, w=1.6):
    flush()
    ser.write((c + "\n").encode())
    ser.flush()
    time.sleep(w)
    flush()


def read_until(tag, timeout=40):
    buf = b""
    t = time.time()
    tb = tag.encode()
    while time.time() - t < timeout:
        d = ser.read(65536)
        if d:
            buf += d
            if tb in buf:
                break
    return buf.decode("utf-8", "replace")


def png(path, px, W, H):
    rows = [bytes(v for p in px[y * W:(y + 1) * W] for v in p) for y in range(H)]
    raw = b"".join(b"\x00" + r for r in rows)

    def ch(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    open(path, "wb").write(b"\x89PNG\r\n\x1a\n"
                           + ch(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                           + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))
    print(path, "ok", W, H)


def rle_to_px(rle, W, H, swap=False):
    px = [(0, 0, 0)] * (W * H)
    pos = i = 0
    while pos < W * H and i + 4 <= len(rle):
        c = rle[i] | (rle[i + 1] << 8)
        r = rle[i + 2] | (rle[i + 3] << 8)
        if r <= 0:
            break
        if swap:                            # Sprite 缓冲需换字节；PSRAM 解码缓冲不需要
            c = ((c & 0xFF) << 8) | (c >> 8)
        px[pos:pos + r] = [(((c >> 11) & 31) * 255 // 31,
                            ((c >> 5) & 63) * 255 // 63,
                            (c & 31) * 255 // 31)] * r
        pos += r
        i += 4
    return px


def shot(name, tag="B64", W=320, H=240):
    flush()
    ser.write(b"shot\n")
    ser.flush()
    txt = read_until("ENDB64")
    if tag not in txt:
        print(name, "NO DATA", txt[:120])
        return
    i0 = txt.index(tag)
    st = txt.index("\n", i0) + 1
    en = txt.index("ENDB64", st)
    rle = base64.b64decode(re.sub(r"[^A-Za-z0-9+/=]", "", txt[st:en]))
    png(name, rle_to_px(rle, W, H, swap=True), W, H)


def fshot(name):
    flush()
    ser.write(b"fshot\n")
    ser.flush()
    txt = read_until("ENDB64", timeout=60)
    m = re.search(r"FDBG (\d+) (\d+) (\d+)", txt)
    if not m:
        print(name, "NO DATA", txt[:120])
        return
    W, H = int(m.group(1)), int(m.group(2))
    st = txt.index("\n", m.start()) + 1
    en = txt.index("ENDB64", st)
    rle = base64.b64decode(re.sub(r"[^A-Za-z0-9+/=]", "", txt[st:en]))
    png(name, rle_to_px(rle, W, H), W, H)


def grid_shot(name):
    flush()
    ser.write(b"gshot\n")
    ser.flush()
    txt = read_until("ENDB64")
    if "B64 " not in txt:
        print(name, "NO DATA", txt[:120])
        return
    # gshot 是 80x60（1/4 抽样），按 4 倍放大回 320x240 便于肉眼比对
    i0 = txt.index("B64 ")
    st = txt.index("\n", i0) + 1
    en = txt.index("ENDB64", st)
    rle = base64.b64decode(re.sub(r"[^A-Za-z0-9+/=]", "", txt[st:en]))
    small = rle_to_px(rle, 80, 60)
    px = [small[(y // 4) * 80 + (x // 4)] for y in range(240) for x in range(320)]
    png(name, px, 320, 240)


def reboot():
    send("reboot", 1.0)
    time.sleep(9)
    flush()


# ---------------- 1. 小键盘（深色 / 浅色） ----------------
# 注意：WiFi 子界面的 TOP/DOWN 是直接轮询按键电平的（nav_step），串口
# 注入的 gk 无效；但"确认"统一走 g_wifiUiOpenReq，所以用 wopen 连点即可：
#   设置页 → wopen 进 WiFi 菜单 → wopen 扫描 → wopen 选首个热点(加密→键盘)
send("theme d", 1.2)
send("app 2", 3.0)
send("wopen", 3.0)
send("wopen", 5.0)
send("wopen", 4.0)
shot("verify_kb_dark.png")

send("theme l", 1.5)
send("app 2", 3.0)
send("wopen", 3.0)
send("wopen", 5.0)
send("wopen", 4.0)
shot("verify_kb_light.png")

# ---------------- 2. 相册九宫格（重启回桌面，避免 WiFi 子界面吃掉 app 命令） ----------------
reboot()
send("theme d", 1.2)
send("app 1", 7.0)          # 进相册（解码 9 张缩略图）
grid_shot("verify_grid.png")

# ---------------- 3. 相册全屏（fshot 回传缓存像素） ----------------
send("gopen", 3.0)
fshot("verify_full_1.png")

send("theme d", 1.0)
ser.close()
