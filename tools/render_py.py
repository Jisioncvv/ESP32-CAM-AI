# ASCII-only: decode RLE from shot_text.txt and write a BMP (no deps, reliable)
import base64, struct, sys

text = open(r"E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_text.txt", "r", errors="ignore").read()
s = text.index("B64 ")
start = text.index("\n", s) + 1
end = text.index("ENDB64")
b64 = text[start:end].replace("\r", "").replace("\n", "").strip()
rle = base64.b64decode(b64)
print("rle bytes:", len(rle))

W, H = 320, 240
px = [0] * (W * H)
i = 0
pos = 0
strays = []
def tile_ok(c, x, y):
    if c == 0xFFFF: return True
    if c == 0xDE33: return 118 <= x <= 201 and 78 <= y <= 161
    if c == 0xA0FC: return 206 <= x <= 265 and 90 <= y <= 149
    if c == 0x60FE: return 54 <= x <= 113 and 90 <= y <= 149
    if c == 0x728C: return 270 <= x <= 313 and 98 <= y <= 141
    if c == 0xC6F9: return 6 <= x <= 49 and 98 <= y <= 141
    if c == 0x18C6: return 122 <= x <= 204 and 82 <= y <= 164
    return False

while pos < W * H and i + 4 <= len(rle):
    c = rle[i] | (rle[i+1] << 8)
    r = rle[i+2] | (rle[i+3] << 8)
    if r <= 0:
        print("BAD run at packet", i // 4); break
    x = pos % W; y = pos // W
    if not tile_ok(c, x, y):
        strays.append((c, x, y, min(r, W * H - pos)))
    for k in range(r):
        if pos + k < W * H:
            px[pos + k] = c
    pos += r; i += 4
print("pixels:", pos, "strays:", len(strays))
for st in strays[:20]:
    print("stray c=0x%04X x=%d y=%d run=%d" % st)

# write 24bpp BMP
row_size = (W * 3 + 3) & ~3
img_size = row_size * H
file_size = 54 + img_size
hdr = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, 54)
dib = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, img_size, 2835, 2835, 0, 0)
data = bytearray()
for y in range(H - 1, -1, -1):
    row = bytearray()
    for x in range(W):
        c = px[y * W + x]
        r5 = (c >> 11) & 31; g6 = (c >> 5) & 63; b5 = c & 31
        row += bytes((b5 * 255 // 31, g6 * 255 // 63, r5 * 255 // 31))
    row += bytes(row_size - len(row))
    data += row
open(sys.argv[1] if len(sys.argv) > 1 else r"E:\Trae_workspace\ESP32_S3\ESP32_CAM_AI\shot_python.bmp", "wb").write(hdr + dib + bytes(data))
print("BMP written")
