# -*- coding: utf-8 -*-
"""
web_fetch_photos.py —— 设备连上电脑热点后，从 Web 端直接拉取照片并分析
用法：
  python web_fetch_photos.py --ip 192.168.137.118 --count 4
  （IP 从固件串口 [WEB] STA 已连接 ... IP = x.x.x.x 处获取）
依赖设备已 STA 连接热点；照片列表解析 /photos 页面中的 photo_N.jpg 链接。
分析输出：RGB 均值（偏色）+ 亮度直方图削顶率（过曝）+ 暗部占比。
"""
import argparse
import os
import re
import urllib.request

NAME_RE = re.compile(r'photo_(\d+)\.jpg')


def http_get(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return r.read()


def list_photos(base):
    """从 /photos 页面解析照片名（photo_<数字>.jpg），按编号降序"""
    html = http_get(base + "/photos").decode(errors="replace")
    nums = sorted(set(int(m) for m in NAME_RE.findall(html)), reverse=True)
    return ["photo_%d.jpg" % n for n in nums]


def fetch_latest(base, count, out_dir):
    names = list_photos(base)[:count]
    os.makedirs(out_dir, exist_ok=True)
    got = []
    for name in names:
        path = os.path.join(out_dir, name)
        data = http_get(base + "/download?name=" + name)
        with open(path, "wb") as f:
            f.write(data)
        print("下载 %s（%u 字节）" % (name, len(data)))
        got.append(path)
    return got


def analyze(path):
    """RGB 均值（偏色）+ 亮度分布（过曝/欠曝）"""
    from PIL import Image, ImageFile
    ImageFile.LOAD_TRUNCATED_IMAGES = True
    img = Image.open(path).convert("RGB").resize((256, 256))
    px = list(img.getdata())
    n = len(px)
    r = sum(p[0] for p in px) / n
    g = sum(p[1] for p in px) / n
    b = sum(p[2] for p in px) / n
    lum = [0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in px]
    mean_l = sum(lum) / n
    clip = sum(1 for v in lum if v > 250) / n * 100    # 削顶率%（过曝）
    dark = sum(1 for v in lum if v < 8) / n * 100      # 暗部占比%（死黑）
    m = (r + g + b) / 3
    cast = ""
    if r > 1.05 * m and b > 1.05 * m:
        cast = "品红偏色"
    elif r > 1.08 * g and r > 1.08 * b:
        cast = "偏红"
    elif g > 1.08 * r and g > 1.08 * b:
        cast = "偏绿"
    elif b > 1.08 * r and b > 1.08 * g:
        cast = "偏蓝"
    return r, g, b, mean_l, clip, dark, cast


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True, help="设备 STA 模式 IP")
    ap.add_argument("--count", type=int, default=4)
    ap.add_argument("--out", default="photos_web")
    args = ap.parse_args()

    base = "http://" + args.ip
    got = fetch_latest(base, args.count, args.out)

    print("\n==== 照片分析（RGB 均值 / 亮度 / 削顶率 / 暗部占比）====")
    for p in got:
        r, g, b, ml, clip, dark, cast = analyze(p)
        print("%-14s R=%.1f G=%.1f B=%.1f  亮度=%.0f  削顶=%.1f%%  暗部=%.1f%%  %s"
              % (os.path.basename(p), r, g, b, ml, clip, dark, cast))


if __name__ == "__main__":
    main()
