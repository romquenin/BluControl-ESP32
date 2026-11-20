# Player-view 48x32 images "PAGE n", converted to the bytes the console sends:
# 1) 180 deg rotation (VMU upside down in the pad, as KallistiOS / NetBSD do)
# 2) row-major, 6 bytes per row, MSB = left pixel
# 3) bus order: bytes of each 32-bit word reversed (SH4 little endian words)
from PIL import Image, ImageDraw
W,H=48,32
small={ # 3x5
'P':["111","101","111","100","100"],
'A':["010","101","111","101","101"],
'G':["011","100","101","101","011"],
'E':["111","100","110","100","111"]}
# big digits 10x18 drawn from 5x9 at scale 2
big={
'1':["00100","01100","00100","00100","00100","00100","00100","00100","01110"],
'2':["01110","10001","00001","00010","00100","01000","10000","10000","11111"],
'3':["11110","00001","00001","00001","01110","00001","00001","00001","11110"],
'4':["00010","00110","01010","10010","10010","11111","00010","00010","00010"],
'5':["11111","10000","10000","11110","00001","00001","00001","10001","01110"]}
def image(n):
    px=[[0]*W for _ in range(H)]
    # "PAGE": 4 letters 3 wide + 1 gap = 15 wide, at top
    x0=(W-15)//2; y0=2
    for k,ch in enumerate("PAGE"):
        for r,row in enumerate(small[ch]):
            for c,v in enumerate(row):
                if v=='1': px[y0+r][x0+k*4+c]=1
    # big digit: 5x9 scaled x3 horizontally? use scale 3 -> 15x27 too tall; scale 2 -> 10x18
    s=2; dw,dh=5*s,9*s; dx=(W-dw)//2; dy=9+ (H-9-dh)//2
    for r,row in enumerate(big[str(n)]):
        for c,v in enumerate(row):
            if v=='1':
                for a in range(s):
                    for b in range(s):
                        px[dy+r*s+a][dx+c*s+b]=1
    return px
def to_bus(px):
    rot=[[px[H-1-y][W-1-x] for x in range(W)] for y in range(H)]
    mem=bytearray(192)
    for y in range(H):
        for x in range(W):
            if rot[y][x]: mem[y*6+x//8]|=0x80>>(x%8)
    bus=bytearray(192)
    for i in range(0,192,4): bus[i:i+4]=mem[i:i+4][::-1]
    return bytes(bus)
out=[]
imgs=[]
for n in range(1,6):
    px=image(n); out.append(to_bus(px))
    im=Image.new('RGB',(W*6+8,H*6+8),(150,190,170)); d=ImageDraw.Draw(im)
    for y in range(H):
        for x in range(W):
            if px[y][x]: d.rectangle([4+x*6,4+y*6,4+x*6+5,4+y*6+5],fill=(30,40,90))
    imgs.append(im)
sheet=Image.new('RGB',(imgs[0].width*5+40,imgs[0].height+10),(255,255,255))
for i,im in enumerate(imgs): sheet.paste(im,(5+i*(im.width+8),5))
sheet.save('apercu_pages.png')
with open('page_lcd.h','w') as f:
    f.write('/* Generated: "PAGE n" for the VMU screen, bus byte order (see gen.py) */\n')
    f.write('static const uint8_t mc_page_lcd[5][192] = {\n')
    for b in out:
        f.write('    {'+', '.join('0x%02X'%v for v in b)+'},\n')
    f.write('};\n')
print('ok', len(out))
