# ASCII-only: decode RLE from shot_text.txt, write PNG via zlib (stdlib only)
import base64, struct, zlib, sys

text = open(r"E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_text.txt", "r", errors="ignore").read()
s = text.index("B64 ")
start = text.index("\n", s) + 1
end = text.index("ENDB64")
b64 = text[start:end].replace("\r", "").replace("\n", "").strip()
rle = base64.b64decode(b64)

W, H = 320, 240
rows = []
i = 0
pos = 0
while pos < W * H and i + 4 <= len(rle):
    c = rle[i] | (rle[i+1] << 8)
    r = rle[i+2] | (rle[i+3] << 8)
    if r <= 0:
        break
    r5 = (c >> 11) & 31; g6 = (c >> 5) & 63; b5 = c & 31
    rr = r5 * 255 // 31; gg = g6 * 255 // 63; bb = b5 * 255 // 31
    for k in range(r):
        if pos + k < W * H:
            y, x = divmod(pos + k, W)
            if y >= len(rows):
                rows.append(bytearray())
            rows[y] += bytes((rr, gg, bb))
    pos += r; i += 4

raw = b"".join(b"\x00" + bytes(r) for r in rows)
def chunk(t, d):
    c = t + d
    return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw, 9))
png += chunk(b"IEND", b"")
out = sys.argv[1] if len(sys.argv) > 1 else r"E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_python.png"
open(out, "wb").write(png)
print("PNG written:", out, "pixels:", pos)
