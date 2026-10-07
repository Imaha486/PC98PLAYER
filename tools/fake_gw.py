#!/usr/bin/env python3
# -----------------------------------------------------------------------------
#  fake_gw.py  --  テスト用: Greaseweazle のふりをする（pty の上で同じ通信手順に答える）
#  python3 fake_gw.py base_1.25MB.img   → 端末のパスを表示して待つ
#  ディスクの中身は fluxgen.py と同じ（特殊なセクタ入り）。PC98PLAYER 本体には含まれない。
# -----------------------------------------------------------------------------
import os, sys, struct, random, select, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fluxgen

FREQ = 72000000
CELL_NS = 1200.0        # 300rpm のドライブで 360rpm の 2HD を読んだ場合

def w28(v):
    return bytes([1 | ((v << 1) & 0xFF), 1 | ((v >> 6) & 0xFE), 1 | ((v >> 13) & 0xFE), 1 | ((v >> 20) & 0xFE)])

def enc_flux(ticks):
    out = bytearray()
    if ticks >= 1525:
        out += b'\xff\x02' + w28(ticks - 249); ticks = 249
    if ticks < 250: out.append(max(1, ticks))
    else:
        hi = (ticks - 250) // 255; lo = (ticks - 250) % 255
        out += bytes([250 + hi, lo + 1])
    return bytes(out)

def main():
    raw = open(sys.argv[1], 'rb').read()
    tracks = fluxgen.build_disk(raw)
    m, s = os.openpty()
    print(os.ttyname(s), flush=True)
    cyl = head = 0; motor = False; disk = os.environ.get('NODISK') is None
    buf = bytearray()
    def rd(n):
        nonlocal buf
        while len(buf) < n:
            r, _, _ = select.select([m], [], [], 600)
            if not r: sys.exit(0)
            buf += os.read(m, 4096)
        d = bytes(buf[:n]); del buf[:n]; return d
    status = 0
    while True:
        c = rd(2); cmd, ln = c[0], c[1]
        p = rd(ln - 2)
        ack = lambda a=0: os.write(m, bytes([cmd, a]))
        if cmd == 0:     # GetInfo
            ack(); os.write(m, struct.pack('<4BI', 1, 5, 1, 22, FREQ) + bytes([4, 0, 1]) + bytes(32 - 11))
        elif cmd == 14 or cmd == 12 or cmd == 13: ack()
        elif cmd == 6: motor = bool(p[1]); ack()
        elif cmd == 2: cyl = struct.unpack('b', p[:1])[0]; ack()
        elif cmd == 3: head = p[0]; ack()
        elif cmd == 7:   # ReadFlux
            ticks, maxidx = struct.unpack('<IH', p)
            ack()
            out = bytearray()
            if not disk or (cyl, head) not in tracks:
                status = 2 if not disk else 0
                if disk:   # 未フォーマット: ノイズ
                    t = 0; nidx = 0
                    while nidx < maxidx:
                        v = random.randrange(100, 400); out += enc_flux(v); t += v
                        if t > FREQ // 5: out += b'\xff\x01' + w28(0); nidx += 1; t = 0
            else:
                status = 0
                revs = [tracks[(cyl, head)](r) for r in range(maxidx)]
                start = random.randrange(len(revs[0]))
                cells = revs[0][start:]
                bounds = []
                for r in range(1, maxidx):
                    bounds.append(len(cells)); cells = cells + revs[r]
                bounds.append(len(cells))
                cells = cells[:bounds[-1]]
                # 反転ごとに tick を出す。区切り（索引）は位置で差し込む
                bi = 0; run = 0; since = 0
                for i, cbit in enumerate(cells):
                    run += 1
                    while bi < len(bounds) and i + 1 == bounds[bi]:
                        pend = int(run * CELL_NS * FREQ / 1e9) if not cbit else 0
                        out += b'\xff\x01' + w28(pend); bi += 1
                    if cbit:
                        t = int(run * CELL_NS * (1 + random.uniform(-0.03, 0.03)) * FREQ / 1e9)
                        out += enc_flux(t); run = 0
            out.append(0)
            os.write(m, bytes(out))
        elif cmd == 9: ack(status)
        else: ack(1)

main()
