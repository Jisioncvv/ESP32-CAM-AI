import serial, time, base64, struct, zlib, sys
ser = serial.Serial("COM5", 115200, timeout=0.3)
time.sleep(6)
def flush():
    while ser.in_waiting: ser.read(65536)
def dump(name, pre=None, wait=3.0):
    flush()
    if pre:
        ser.write(pre); ser.flush(); time.sleep(wait); flush()
    ser.write(b"shot\n"); ser.flush()
    buf=b""; t=time.time()
    while time.time()-t < 30:
        d=ser.read(65536)
        if d:
            buf+=d
            if b"ENDB64" in buf: break
    txt=buf.decode("utf-8","replace")
    if "B64 " not in txt:
        print(name,"NO DATA:",txt[:200]); return
    s=txt.index("B64 "); st=txt.index("\n",s)+1; en=txt.index("ENDB64",st)
    rle=base64.b64decode(txt[st:en].replace("\r","").replace("\n","").strip())
    W,H=320,240; px=[(255,255,255)]*(W*H); pos=0;i=0
    while pos<W*H and i+4<=len(rle):
        c=rle[i]|(rle[i+1]<<8); r=rle[i+2]|(rle[i+3]<<8)
        if r<=0: break
        c=((c&0xFF)<<8)|(c>>8)
        col=(((c>>11)&31)*255//31, ((c>>5)&63)*255//63, (c&31)*255//31)
        px[pos:pos+r]=[col]*r
        pos+=r; i+=4
    rows=[bytes(v for p in px[y*W:(y+1)*W] for v in p) for y in range(H)]
    raw=b"".join(b"\x00"+r for r in rows)
    def chunk(t,d):
        c=t+d; return struct.pack(">I",len(d))+c+struct.pack(">I",zlib.crc32(c)&0xFFFFFFFF)
    png=b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",W,H,8,2,0,0,0))+chunk(b"IDAT",zlib.compress(raw,9))+chunk(b"IEND",b"")
    open(name,"wb").write(png); print(name,"ok","covered",pos)
if __name__ == "__main__":
    n = sys.argv[1] if len(sys.argv)>1 else "ui_shot.png"
    pre = sys.argv[2].encode()+b"\n" if len(sys.argv)>2 else None
    dump(n, pre)
    ser.close()
