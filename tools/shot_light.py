import serial, time, base64, struct, zlib, re, sys
ser = serial.Serial("COM5", 115200, timeout=0.3)
time.sleep(7)
def flush():
    while ser.in_waiting: ser.read(65536)
def send(c, w=1.6):
    flush(); ser.write((c+"\n").encode()); ser.flush(); time.sleep(w); flush()
def shot(name):
    flush(); ser.write(b"shot\n"); ser.flush()
    buf=b""; t=time.time()
    while time.time()-t < 30:
        d=ser.read(65536)
        if d:
            buf+=d
            if b"ENDB64" in buf: break
    txt=buf.decode("utf-8","replace")
    if "B64 " not in txt:
        print(name,"NO DATA", txt[:150]); return
    i0=txt.index("B64 "); st=txt.index("\n",i0)+1; en=txt.index("ENDB64",st)
    rle=base64.b64decode(re.sub(r"[^A-Za-z0-9+/=]","",txt[st:en]))
    W,H=320,240; px=[(0,0,0)]*(W*H); pos=0;i=0
    while pos<W*H and i+4<=len(rle):
        c=rle[i]|(rle[i+1]<<8); r=rle[i+2]|(rle[i+3]<<8)
        if r<=0: break
        c=((c&0xFF)<<8)|(c>>8)
        px[pos:pos+r]=[(((c>>11)&31)*255//31, ((c>>5)&63)*255//63, (c&31)*255//31)]*r
        pos+=r; i+=4
    rows=[bytes(v for p in px[y*W:(y+1)*W] for v in p) for y in range(H)]
    raw=b"".join(b"\x00"+r for r in rows)
    def ch(t,d):
        c=t+d; return struct.pack(">I",len(d))+c+struct.pack(">I",zlib.crc32(c)&0xFFFFFFFF)
    open(name,"wb").write(b"\x89PNG\r\n\x1a\n"+ch(b"IHDR",struct.pack(">IIBBBBB",W,H,8,2,0,0,0))+ch(b"IDAT",zlib.compress(raw,9))+ch(b"IEND",b""))
    print(name,"ok",pos)

send("theme l", 1.2)
send("app 2", 3.0); shot("lt_settings.png")
send("gk cam", 3.0); shot("lt_wifi_menu.png")
send("gk cam", 4.0); shot("lt_wifi_list.png")
send("gk down", 0.6); send("gk cam", 3.5); shot("lt_wifi_kb.png")
send("app 3", 3.0); shot("lt_games.png")
send("app 4", 3.0); shot("lt_notes.png")
send("app 1", 4.0); shot("lt_album.png")
send("app 0", 2.0)
send("theme d", 1.0)
ser.close()
