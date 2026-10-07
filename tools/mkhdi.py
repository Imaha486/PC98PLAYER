#!/usr/bin/env python3
# -----------------------------------------------------------------------------
#  mkhdi.py  --  テスト用: PC-98 形式のハードディスクイメージ（HDI / NHD）を作る
#  python3 mkhdi.py out.hdi|out.nhd  → 区画 2 つ（FAT16 / FAT12）、日本語名・サブフォルダ・
#  断片化したファイル・削除済みの項目・ボリュームラベル入り。期待する中身を out.expect/ に書く。
#  PC98PLAYER 本体には含まれない開発用の道具。
# -----------------------------------------------------------------------------
import sys, os, struct, random

SS, SPT, HEADS, CYLS = 256, 33, 8, 200

class Fat:
    def __init__(self, nsec_bytes, bps, spc, fat16, rootn=512):
        self.bps, self.spc, self.fat16, self.rootn = bps, spc, fat16, rootn
        self.tot = nsec_bytes // bps
        self.rsv, self.nf = 1, 2
        # FAT の大きさを決める
        spf = 1
        while True:
            root = (rootn * 32 + bps - 1) // bps
            data = self.rsv + self.nf * spf + root
            ncl = (self.tot - data) // spc
            need = (ncl + 2) * (2 if fat16 else 1.5)
            if spf * bps >= need: break
            spf += 1
        self.spf, self.root_sec, self.data_sec, self.ncl = spf, self.rsv + self.nf * spf, data, ncl
        self.img = bytearray(self.tot * bps)
        self.fat = [0] * (ncl + 2)
        self.fat[0] = 0xFFF8 if fat16 else 0xFF8; self.fat[1] = 0xFFFF if fat16 else 0xFFF
        self.next_free = 2
        self.root = bytearray()
    def alloc(self, n, frag=False):
        cl = []
        c = self.next_free
        while len(cl) < n:
            if self.fat[c] == 0: cl.append(c)
            c += 2 if frag else 1
        self.next_free = max(cl) + 1 if not frag else self.next_free
        for i, x in enumerate(cl):
            self.fat[x] = cl[i + 1] if i + 1 < len(cl) else (0xFFFF if self.fat16 else 0xFFF)
        return cl
    def cbytes(self): return self.bps * self.spc
    def write_data(self, data, frag=False):
        if not data: return 0
        n = (len(data) + self.cbytes() - 1) // self.cbytes()
        cl = self.alloc(n, frag)
        for i, c in enumerate(cl):
            off = (self.data_sec + (c - 2) * self.spc) * self.bps
            chunk = data[i * self.cbytes():(i + 1) * self.cbytes()]
            self.img[off:off + len(chunk)] = chunk
        return cl[0]
    @staticmethod
    def entry(name83, attr, clus, size, date=0x2A21, time=0x6000):
        return name83 + bytes([attr]) + bytes(10) + struct.pack('<HHHI', time, date, clus, size)
    def finish(self, label=b'TESTDISK   '):
        b = bytearray(self.bps)
        b[0:3] = b'\xeb\x3c\x90'; b[3:11] = b'NEC 2.00'
        struct.pack_into('<HBHBHHBH', b, 11, self.bps, self.spc, self.rsv, self.nf, self.rootn,
                         self.tot if self.tot < 65536 else 0, 0xF8, self.spf)
        struct.pack_into('<I', b, 32, self.tot if self.tot >= 65536 else 0)
        self.img[0:self.bps] = b
        fb = bytearray(self.spf * self.bps)
        if self.fat16:
            for i, v in enumerate(self.fat): struct.pack_into('<H', fb, i * 2, v)
        else:
            for i, v in enumerate(self.fat):
                o = i * 3 // 2
                if i & 1: fb[o] = (fb[o] & 0x0F) | ((v << 4) & 0xF0); fb[o + 1] = (v >> 4) & 0xFF
                else: fb[o] = v & 0xFF; fb[o + 1] = (fb[o + 1] & 0xF0) | ((v >> 8) & 0x0F)
        for k in range(self.nf):
            o = (self.rsv + k * self.spf) * self.bps
            self.img[o:o + len(fb)] = fb
        root = self.entry(label[:11], 0x08, 0, 0) + self.root
        o = self.root_sec * self.bps
        self.img[o:o + len(root)] = root
        return self.img

def n83(name):
    b = name.encode('cp932')
    if b'.' in b: n, x = b.split(b'.', 1)
    else: n, x = b, b''
    return n.ljust(8, b' ') + x.ljust(3, b' ')

def build_part(fat, tree, expect_dir):
    """tree: {name: bytes | dict}"""
    os.makedirs(expect_dir, exist_ok=True)
    def mkdir_entries(t, edir, parent_clus):
        ents = bytearray()
        for name, v in t.items():
            if isinstance(v, dict):
                sub_ed = os.path.join(edir, name); os.makedirs(sub_ed, exist_ok=True)
                # 先に場所を取る（1 クラスタ）
                c = fat.alloc(1)[0]
                body = Fat.entry(b'.          ', 0x10, c, 0) + Fat.entry(b'..         ', 0x10, parent_clus, 0)
                body += mkdir_entries(v, sub_ed, c)
                body = body.ljust(fat.cbytes(), b'\0')[:fat.cbytes()]
                off = (fat.data_sec + (c - 2) * fat.spc) * fat.bps
                fat.img[off:off + len(body)] = body
                ents += Fat.entry(n83(name), 0x10, c, 0)
            else:
                frag = name.startswith('FRAG')
                c = fat.write_data(v, frag)
                ents += Fat.entry(n83(name), 0x20, c, len(v))
                open(os.path.join(edir, name), 'wb').write(v)
        ents += b'\xe5' + b'DELETED TXT'[1:] + bytes([0x20]) + bytes(20)   # 削除済み
        return ents
    fat.root = mkdir_entries(tree, expect_dir, 0)

def main():
    out = sys.argv[1]
    random.seed(7)
    rnd = lambda n: bytes(random.randrange(256) for _ in range(n))
    disk = bytearray(SS * SPT * HEADS * CYLS)
    # 区画 1: シリンダ 1〜59（FAT16, 1024 バイト/セクタ）、区画 2: 60〜99（FAT12, 2 セクタ/クラスタ）
    parts = [(1, 149, True, 1, "MS-DOS 6.20"), (150, 199, False, 2, "DATA")]
    exp = out + '.expect'
    tables = bytearray(512)
    for k, (c0, c1, f16, spc, pname) in enumerate(parts):
        nbytes = (c1 - c0 + 1) * HEADS * SPT * SS
        fat = Fat(nbytes, 1024, spc, f16)
        if k == 0:
            tree = {'GAME.EXE': rnd(5000), 'AUTOEXEC.BAT': b'@ECHO OFF\r\nGAME\r\n', 'ゲーム.TXT': 'テスト'.encode('cp932'),
                    'EMPTY.DAT': b'', 'DATA': {'A.DAT': rnd(3000), 'FRAG.DAT': rnd(9000), 'SUB': {'DEEP.DAT': rnd(100)}}}
        else:
            tree = {'SAVE.DAT': rnd(1234), 'セーブ': {'S1.DAT': rnd(777)}}
        build_part(fat, tree, os.path.join(exp, 'P%d' % (k + 1)))
        img = fat.finish()
        off = c0 * HEADS * SPT * SS
        disk[off:off + len(img)] = img
        e = bytearray(32)
        e[0] = 0xA0 if k == 0 else 0x20; e[1] = 0xA1 if k == 0 else 0x21
        struct.pack_into('<BBH', e, 8, 0, 0, c0)
        struct.pack_into('<BBH', e, 12, 0, 0, c1)
        e[16:32] = pname.encode().ljust(16, b' ')
        tables[k * 32:(k + 1) * 32] = e
    disk[SS:SS + 512] = tables
    disk[0:4] = b'\xeb\x0a\x90\x90'; disk[4:8] = b'IPL1'
    if out.lower().endswith('.nhd'):
        h = bytearray(0x200)
        h[0:15] = b'T98HDDIMAGE.R0\0'
        struct.pack_into('<IIHHH', h, 0x110, 0x200, CYLS, HEADS, SPT, SS)
    else:
        h = bytearray(4096)
        struct.pack_into('<8I', h, 0, 0, 0, 4096, len(disk), SS, SPT, HEADS, CYLS)
    open(out, 'wb').write(h + disk)

main()
