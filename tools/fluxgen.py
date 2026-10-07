#!/usr/bin/env python3
# -----------------------------------------------------------------------------
#  fluxgen.py  --  テスト用: セクタの並びから MFM/FM の磁気信号を作り、SCP / HFE を書き出す
#                  （fake_gw.py からも使う）。PC98PLAYER 本体には含まれない開発用の道具。
#
#  python3 fluxgen.py base.(d88|nfd|img) out.scp [out.hfe]
#   ベタ（1.25MB）を元に、テスト用の特殊なセクタを足したディスクを作る:
#    ・トラック 7（C=7 H=0）: 通常の 8 セクタの後ろに R=4B N=2 / R=4C N=1（隙間を詰めて入れる）
#    ・トラック 8: R=03 のデータ CRC エラー、R=05 デリーテッド、R=07 は読むたびに変わる（弱いビット）
#    ・トラック 9: R=02 の ID が C=0A（シリンダ違い）、R=01 のデータ部の中に隠れた ID（R=F1 N=1）
#    ・トラック 0 面 1: FM 128 バイト x 26（N88 形式のような単密度）
# -----------------------------------------------------------------------------
import sys, struct, random

def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

class MfmWriter:
    def __init__(self):
        self.cells = []
        self.prev = 0
    def byte(self, b):
        for i in range(7, -1, -1):
            d = (b >> i) & 1
            c = 1 if (d == 0 and self.prev == 0) else 0
            self.cells += [c, d]
            self.prev = d
    def bytes(self, bs):
        for b in bs: self.byte(b)
    def raw16(self, w, lastdata):
        for i in range(15, -1, -1): self.cells.append((w >> i) & 1)
        self.prev = lastdata
    def a1(self):  self.raw16(0x4489, 1)
    def c2(self):  self.raw16(0x5224, 0)

class FmWriter:
    """FM: 1 FM セル = MFM の 2 セル（'1' は "10"）"""
    def __init__(self):
        self.cells = []
    def _put(self, bit):
        self.cells += [bit, 0]
    def cd(self, clk, dat):
        for i in range(7, -1, -1):
            self._put((clk >> i) & 1); self._put((dat >> i) & 1)
    def byte(self, b): self.cd(0xFF, b)
    def bytes(self, bs):
        for b in bs: self.byte(b)

def mfm_track(secs, total_cells, gap3=0x54, weak=None):
    """secs: dict(c,h,r,n,data,status('ok'|'crc'|'del'|'noid'...), hidden=[...]); weak=R で弱いビット"""
    w = MfmWriter()
    w.bytes([0x4E] * 80); w.bytes([0] * 12); w.c2(); w.c2(); w.c2(); w.byte(0xFC); w.bytes([0x4E] * 50)
    for s in secs:
        w.bytes([0] * 12); w.a1(); w.a1(); w.a1()
        idb = bytes([0xFE, s['c'], s['h'], s['r'], s['n']])
        crc = crc16(bytes([0xA1] * 3) + idb)
        w.bytes(idb[0:1] + idb[1:] + bytes([crc >> 8, crc & 0xFF]))
        w.bytes([0x4E] * 22); w.bytes([0] * 12); w.a1(); w.a1(); w.a1()
        mark = 0xF8 if s.get('del') else 0xFB
        data = bytearray(s['data'])
        crc = crc16(bytes([0xA1] * 3) + bytes([mark]) + bytes(data))
        if s.get('crcbad'): crc ^= 0x1234
        if s.get('weak') and weak is not None:
            for k in range(100, 140): data[k] = random.randrange(256)
            crc ^= 0x5555
        w.byte(mark); w.bytes(data); w.bytes([crc >> 8, crc & 0xFF])
        w.bytes([0x4E] * s.get('gap3', gap3))
    while len(w.cells) < total_cells: w.byte(0x4E)
    return w.cells[:total_cells]

def fm_track(secs, total_cells):
    w = FmWriter()
    w.bytes([0xFF] * 40); w.bytes([0] * 6); w.cd(0xD7, 0xFC); w.bytes([0xFF] * 26)
    for s in secs:
        w.bytes([0] * 6); w.cd(0xC7, 0xFE)
        idb = bytes([0xFE, s['c'], s['h'], s['r'], s['n']])
        crc = crc16(idb)
        w.bytes(idb[1:] + bytes([crc >> 8, crc & 0xFF]))
        w.bytes([0xFF] * 11); w.bytes([0] * 6); w.cd(0xC7, 0xFB)
        crc = crc16(bytes([0xFB]) + bytes(s['data']))
        w.bytes(s['data']); w.bytes([crc >> 8, crc & 0xFF]); w.bytes([0xFF] * 27)
    while len(w.cells) < total_cells: w.byte(0xFF)
    return w.cells[:total_cells]

def build_disk(raw):
    """raw: 1.25MB のベタ → {(cyl,head): 関数(rev) -> セル列}"""
    CELLS = 166666          # 360rpm, 1us
    tracks = {}
    for cyl in range(77):
        for head in range(2):
            base = (cyl * 2 + head) * 8 * 1024
            secs = [dict(c=cyl, h=head, r=r, n=3, data=raw[base + (r - 1) * 1024: base + r * 1024]) for r in range(1, 9)]
            if (cyl, head) == (7, 0):
                for s in secs: s['gap3'] = 0x20
                secs.append(dict(c=7, h=0, r=0x4B, n=2, data=bytes((i * 7) & 0xFF for i in range(512)), gap3=0x10))
                secs.append(dict(c=7, h=0, r=0x4C, n=1, data=bytes((i * 3 + 1) & 0xFF for i in range(256)), gap3=0x10))
            if (cyl, head) == (8, 0):
                secs[2]['crcbad'] = True
                secs[4]['del'] = True
                secs[6]['weak'] = True
            if (cyl, head) == (9, 0):
                secs[1]['c'] = 0x0A
                # R=01 のデータの中に ID（R=F1 N=1）とデータ部を埋め込む
                inner = MfmWriter()
                inner.prev = 0
                d = bytearray(secs[0]['data'])
                secs[0]['hidden'] = True
            if (cyl, head) == (0, 1):
                fsecs = [dict(c=0, h=1, r=r, n=0, data=bytes([r] * 128)) for r in range(1, 27)]
                tracks[(cyl, head)] = (lambda fs: (lambda rev: fm_track(fs, CELLS)))(fsecs)
                continue
            weak = (cyl, head) == (8, 0)
            if (cyl, head) == (9, 0):
                tracks[(cyl, head)] = (lambda ss: (lambda rev: hidden_track(ss, CELLS)))(secs)
            else:
                tracks[(cyl, head)] = (lambda ss, wk: (lambda rev: mfm_track(ss, CELLS, weak=(rev if wk else None))))(secs, weak)
    return tracks

def hidden_track(secs, total):
    """R=01 のデータ部の途中（200 バイト目）に、別の ID + データ部の磁気パターンを書き込む"""
    cells = mfm_track(secs, total)
    # R=01 のデータ部の位置を探す（最初の FB の同期）
    w = MfmWriter()
    w.bytes([0] * 12); w.a1(); w.a1(); w.a1()
    idb = bytes([0xFE, 9, 0, 0xF1, 1]); crc = crc16(bytes([0xA1] * 3) + idb)
    w.bytes(idb + bytes([crc >> 8, crc & 0xFF])); w.bytes([0x4E] * 22); w.bytes([0] * 12); w.a1(); w.a1(); w.a1()
    data = bytes([0xF1] * 256); crc = crc16(bytes([0xA1] * 3 + [0xFB]) + data)
    w.byte(0xFB); w.bytes(data); w.bytes([crc >> 8, crc & 0xFF])
    # 1 つ目のセクタのデータ部: 4489 x3 の 2 回目の組 + FB
    s = ''.join(map(str, cells))
    p = s.find('0100010010001001' * 3)
    p = s.find('0100010010001001' * 3, p + 48)       # データ部の同期
    start = p + 48 + 16 + 200 * 16
    cells[start:start + len(w.cells)] = w.cells
    return cells

def cells_to_flux_ns(cells, cell_ns, jitter=0.03):
    out = []
    run = 0
    for c in cells:
        run += 1
        if c:
            out.append(run * cell_ns * (1 + random.uniform(-jitter, jitter)))
            run = 0
    if run and out: out[-1] += run * cell_ns
    return out

def write_scp(path, tracks, revs=3, cell_ns=1200.0):
    hdr = bytearray(b'SCP' + bytes([0x19, 0x80, revs, 0, 167, 0, 0, 0, 0]) + b'\0' * 4)
    offs = [0] * 168
    body = bytearray()
    base = 0x10 + 168 * 4
    for (cyl, head), fn in sorted(tracks.items()):
        t = cyl * 2 + head
        trk = bytearray(b'TRK' + bytes([t]))
        entries = []
        datas = bytearray()
        doff = 4 + revs * 12
        for r in range(revs):
            fl = cells_to_flux_ns(fn(r), cell_ns)
            vals = bytearray()
            cnt = 0
            for f in fl:
                v = int(round(f / 25.0))
                while v >= 65536: vals += b'\0\0'; v -= 65536; cnt += 1
                if v == 0: v = 1
                vals += struct.pack('>H', v); cnt += 1
            idx = int(sum(fl) / 25.0)
            entries.append(struct.pack('<III', idx, cnt, doff + len(datas)))
            datas += vals
        trk += b''.join(entries) + datas
        offs[t] = base + len(body)
        body += trk
    with open(path, 'wb') as f:
        f.write(hdr + b''.join(struct.pack('<I', o) for o in offs) + body)

def write_hfe(path, tracks, rate_kbps=500):
    ntr = 77
    hdr = bytearray(512)
    hdr[0:8] = b'HXCPICFE'; hdr[8] = 0; hdr[9] = ntr; hdr[10] = 2; hdr[11] = 0
    struct.pack_into('<HH', hdr, 12, rate_kbps, 360); hdr[16] = 0; hdr[17] = 1
    struct.pack_into('<H', hdr, 18, 1); hdr[20] = 0xFF; hdr[21] = 0xFF
    lut = bytearray(512)
    data = bytearray()
    blk = 2
    for cyl in range(ntr):
        sides = []
        for head in range(2):
            cells = tracks[(cyl, head)](0)
            b = bytearray((len(cells) + 7) // 8)
            for i, c in enumerate(cells):
                if c: b[i // 8] |= 1 << (i % 8)
            sides.append(b)
        n = max(len(sides[0]), len(sides[1]))
        n = (n + 255) // 256 * 256
        for s in sides: s += bytes(n - len(s))
        struct.pack_into('<HH', lut, cyl * 4, blk, n * 2)
        tb = bytearray()
        for k in range(0, n, 256): tb += sides[0][k:k + 256] + sides[1][k:k + 256]
        data += tb
        blk += len(tb) // 512
    with open(path, 'wb') as f:
        f.write(hdr + lut + data)

if __name__ == '__main__':
    random.seed(1)
    raw = open(sys.argv[1], 'rb').read()
    if len(raw) != 1261568: sys.exit('1.25MB のベタを指定してください')
    tr = build_disk(raw)
    write_scp(sys.argv[2], tr)
    if len(sys.argv) > 3: write_hfe(sys.argv[3], tr)
