import serial, time, base64, struct, zlib, sys
PORT="COM5"
def open_port():
    s=serial.Serial(PORT,115200,timeout=0.3)
    time.sleep(7)          # 等开机自检 3s + 桌面就绪
    while s.in_waiting: s.read(65536)
    return s
def send(s,c,w=1.2):
    s.write((c+"\n").encode()); s.flush(); time.sleep(w)
    while s.in_waiting: s.read(65536)
def shot(s,name):
    s.write(b"shot\n"); s.flush()
    buf=b""; t=time.time()
    while time.time()-t < 30:
        d=s.read(65536)
        if d:
            buf+=d
            if b"ENDB64" in buf: break
    txt=buf.decode("utf-8","replace")
    if "B64 " not in txt:
        print(name,"NO DATA"); return
    i0=txt.index("B64 "); st=txt.index("\n",i0)+1; en=txt.index("ENDB64",st)
    import re
    b64=re.sub(r"[^A-Za-z0-9+/=]","",txt[st:en])
    rle=base64.b64decode(b64)
    W,H=320,240; px=[(255,255,255)]*(W*H); pos=0;i=0
    while pos<W*H and i+4<=len(rle):
        c=rle[i]|(rle[i+1]<<8); r=rle[i+2]|(rle[i+3]<<8)
        if r<=0: break
        c=((c&0xFF)<<8)|(c>>8)
        px[pos:pos+r]=[(((c>>11)&31)*255//31, ((c>>5)&63)*255//63, (c&31)*255//31)]*r
        pos+=r; i+=4
    rows=[bytes(v for p in px[y*W:(y+1)*W] for v in p) for y in range(H)]
    raw=b"".join(b"\x00"+r for r in rows)
    def chunk(t,d):
        c=t+d; return struct.pack(">I",len(d))+c+struct.pack(">I",zlib.crc32(c)&0xFFFFFFFF)
    png=b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",W,H,8,2,0,0,0))+chunk(b"IDAT",zlib.compress(raw,9))+chunk(b"IEND",b"")
    open(name,"wb").write(png); print(name,"ok")
if __name__=="__main__":
    s=open_port()
    shot(s,"ui_tetris.png")
    send(s,"app 3",2.5); send(s,"gk cam",2.5); shot(s,"ui_tetris.png")
    send(s,"app 3",2.5); send(s,"gk cam",1.0); send(s,"app 3",2.5); send(s,"gk down",0.6); send(s,"gk cam",2.5); shot(s,"ui_snake.png")
    send(s,"app 3",2.5)
    send(s,"gk down",0.6); send(s,"gk down",0.6); send(s,"gk cam",2.5); shot(s,"ui_2048.png")
    s.close()
