# ハーネス用のフォント（IPA ゴシック）を PC-98 の並びで書き出す
from PIL import Image, ImageDraw, ImageFont
import sys
f16 = ImageFont.truetype('/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf', 16)
out = bytearray()
def render(ch, w):
    im = Image.new('1', (w, 16), 0)
    d = ImageDraw.Draw(im)
    d.text((0, -1), ch, font=f16, fill=1)
    px = im.load()
    rows = []
    for y in range(16):
        v = 0
        for x in range(w):
            v = (v << 1) | (1 if px[x, y] else 0)
        rows.append(v)
    return rows
for hi in range(0x21, 0x7F):
    for lo in range(0x21, 0x7F):
        b = bytes([hi | 0x80, lo | 0x80])
        try: ch = b.decode('euc_jp')
        except Exception:
            try:
                # NEC 特殊文字（13 区）は cp932 の 0x87xx
                c1 = hi; c2 = lo
                s1 = ((c1 + 1) >> 1) + (0x70 if c1 <= 0x5E else 0xB0)
                s2 = c2 + (0x1F if c1 & 1 else 0x7D)
                if s2 >= 0x7F and (c1 & 1): s2 += 1
                ch = bytes([s1, s2]).decode('cp932')
            except Exception: ch = None
        if ch is None:
            out += bytes(32); continue
        rows = render(ch, 16)
        for r in rows: out += bytes([(r >> 8) & 0xFF, r & 0xFF])
for c in range(256):
    if 0x20 <= c <= 0x7E: ch = chr(c)
    elif 0xA1 <= c <= 0xDF: ch = bytes([c]).decode('cp932')
    else: ch = None
    if c == 0x5C: ch = '¥'
    if ch is None: out += bytes(16); continue
    rows = render(ch, 8)
    out += bytes(rows)
open(sys.argv[1], 'wb').write(out)
print(len(out))
