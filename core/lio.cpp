// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  lio.cpp  --  ROM 内のグラフィック LIO（INT A0h-AFh）の入口
//
//  実機では F990:0000 からグラフィック LIO の ROM があり、先頭は割込みベクタの表:
//    +0: 組の数（11h）、+4 から 4 バイトずつ（ベクタ番号 word, 入口のオフセット word）が並ぶ
//    （A0h〜AFh と CEh。セグメントは F990h）。プログラムによってはこの表を自分で割込みベクタへ
//    写してから INT A0h（GINIT）を呼ぶ（Ray、ランス３など）。ここが空だと写す回数が 0（=65536 回）に
//    なったり、0000:0000 の続きを実行したりして止まってしまう。
//  描画の機能（GINIT・GSCREEN・GVIEW・GCOLOR1/2・GCLS・GPSET・GLINE・GGET・GPUT1・GPOINT2）は
//  ここで VRAM に直接描く。GCIRCLE・GPAINT・GPUT2・GROLL は何もしないで正常終了を返す。
//  パラメータは DS:BX から読む（並びは N88-BASIC の LIO と同じ。Trace=1 で呼び出しを記録）。
//  座標は画面の絶対座標で、GVIEW の範囲の外は描かない（はみ出しは切り取る。GPUT1/GGET は範囲外ならエラー 5）。
// -----------------------------------------------------------------------------
#include "machine.h"
#include <string.h>

static const uint16_t LIOSEG = 0xF990;
static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }

void lio_init(Machine* m) {
    uint8_t* r = m->ram;
    uint32_t base = lin(LIOSEG, 0);
    for (int i = 0; i < 0x100; i++) r[base + i] = 0;
    // +0: 組の数、+4〜: (ベクタ番号, オフセット) が 17 組（A0h〜AFh、CEh）。入口は +80h から 4 バイトずつ
    r[base + 0] = 17;
    for (int i = 0; i < 17; i++) {
        uint16_t off = (uint16_t)(0x80 + i * 4);
        int v = i < 16 ? 0xA0 + i : 0xCE;
        r[base + 4 + i * 4] = (uint8_t)v; r[base + 5 + i * 4] = 0;
        r[base + 6 + i * 4] = (uint8_t)off; r[base + 7 + i * 4] = (uint8_t)(off >> 8);
        uint8_t n = i < 16 ? (uint8_t)(HLE_LIO + i) : (uint8_t)HLE_NOP;
        r[base + off] = 0xF1; r[base + off + 1] = n; r[base + off + 2] = 0xCF;   // HLE n; IRET
        if (i < 16) {
            r[v * 4] = (uint8_t)off; r[v * 4 + 1] = (uint8_t)(off >> 8);
            r[v * 4 + 2] = (uint8_t)LIOSEG; r[v * 4 + 3] = (uint8_t)(LIOSEG >> 8);
        }
    }
}

// ---- 描画の状態 ---------------------------------------------------------------
struct LioState {
    uint8_t scrnmode = 3;    // 0: 640x200 カラー / 1: 640x200 単色 / 2: 640x400 単色 / 3: 640x400 カラー
    uint8_t pos = 0;         // 640x200 のとき 1 で VRAM の後半 200 ライン
    uint8_t access = 0;      // 描くバンク
    uint8_t fg = 7, bg = 0;
    uint8_t palmode = 0;     // 0: デジタル 8 色 / 1: アナログ 8 色 / 2: アナログ 16 色
    int16_t vx1 = 0, vy1 = 0, vx2 = 639, vy2 = 399;
};
static LioState s_lio;

static int lio_maxline() { return (s_lio.scrnmode <= 1) ? 199 : 399; }
static int lio_planes() { return s_lio.palmode == 2 ? 4 : 3; }
static int lio_palmax() { return s_lio.palmode == 2 ? 16 : 8; }
static int clipx1() { return s_lio.vx1 < 0 ? 0 : s_lio.vx1; }
static int clipy1() { return s_lio.vy1 < 0 ? 0 : s_lio.vy1; }
static int clipx2() { return s_lio.vx2 > 639 ? 639 : s_lio.vx2; }
static int clipy2() { int m = lio_maxline(); return s_lio.vy2 > m ? m : s_lio.vy2; }

static uint32_t vaddr(int x, int y) {
    uint32_t a = (uint32_t)(y * 80 + (x >> 3));
    if (s_lio.scrnmode <= 1 && (s_lio.pos & 1)) a += 16000;
    return a & 0x7FFF;
}
static void pset(Machine* m, int x, int y, int pal) {
    if (x < clipx1() || x > clipx2() || y < clipy1() || y > clipy2()) return;
    uint32_t a = vaddr(x, y);
    uint8_t bit = (uint8_t)(0x80 >> (x & 7));
    uint8_t (*pl)[0x8000] = m->gvram[s_lio.access & 1];
    for (int p = 0; p < lio_planes(); p++) { if (pal & (1 << p)) pl[p][a] |= bit; else pl[p][a] &= (uint8_t)~bit; }
}
static int pget(Machine* m, int x, int y) {
    uint32_t a = vaddr(x, y);
    uint8_t bit = (uint8_t)(0x80 >> (x & 7));
    int c = 0;
    for (int p = 0; p < lio_planes(); p++) if (m->gvram[s_lio.access & 1][p][a] & bit) c |= 1 << p;
    return c;
}
static void hline(Machine* m, int x1, int x2, int y, int pal) {
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    for (int x = x1; x <= x2; x++) pset(m, x, y, pal);
}
// 線（style は 16 ドットの点線の型。最上位ビットから順に使う）
static void line(Machine* m, int x1, int y1, int x2, int y2, int pal, uint16_t style) {
    int dx = x2 > x1 ? x2 - x1 : x1 - x2, sx = x1 < x2 ? 1 : -1;
    int dy = y2 > y1 ? y1 - y2 : y2 - y1, sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (int i = 0; i < 4096 * 4; i++) {
        if (style & 0x8000) pset(m, x1, y1, pal);
        style = (uint16_t)((style << 1) | (style >> 15));
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}
static void box(Machine* m, int x1, int y1, int x2, int y2, int pal, uint16_t style) {
    line(m, x1, y1, x2, y1, pal, style); line(m, x2, y1, x2, y2, pal, style);
    line(m, x2, y2, x1, y2, pal, style); line(m, x1, y2, x1, y1, pal, style);
}
// 塗りつぶし。tile があれば（1 行 = プレーン数バイト、縦に繰り返す）その模様で
static void fill(Machine* m, int x1, int y1, int x2, int y2, int pal, const uint8_t* tile, int tleng) {
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int planes = lio_planes();
    int rows = tile ? tleng / planes : 0;
    for (int y = y1; y <= y2; y++) {
        if (!tile) { hline(m, x1, x2, y, pal); continue; }
        const uint8_t* t = tile + (y % rows) * planes;
        for (int x = x1; x <= x2; x++) {
            int c = 0;
            for (int p = 0; p < planes; p++) if (t[p] & (0x80 >> (x & 7))) c |= 1 << p;
            pset(m, x, y, c);
        }
    }
}

static uint16_t rw16(Machine* m, uint32_t a) { return (uint16_t)(m->ram[a & 0xFFFFF] | (m->ram[(a + 1) & 0xFFFFF] << 8)); }
static uint8_t rb8(Machine* m, uint32_t a) { return m->ram[a & 0xFFFFF]; }
static uint8_t rev8(uint8_t v) { uint8_t r = 0; for (int i = 0; i < 8; i++) if (v & (1 << i)) r |= (uint8_t)(0x80 >> i); return r; }

static void set_digital(Machine* m, int pal, int col) {
    static const int lo_idx[4] = {3, 1, 2, 0}, hi_idx[4] = {7, 5, 6, 4};
    for (int k = 0; k < 4; k++) {
        if (lo_idx[k] == pal) m->degpal[k] = (uint8_t)((m->degpal[k] & 0x0F) | ((col & 7) << 4));
        if (hi_idx[k] == pal) m->degpal[k] = (uint8_t)((m->degpal[k] & 0xF0) | (col & 7));
    }
}

static int lio_ginit(Machine* m) {
    s_lio = LioState();
    m->analog = 0;
    for (int i = 0; i < 8; i++) set_digital(m, i, i);
    for (int i = 0; i < 16; i++) { m->pal[i][0] = (i & 4) ? 7 : 0; m->pal[i][1] = (i & 2) ? 7 : 0; m->pal[i][2] = (i & 1) ? 7 : 0; if (i >= 8) for (int c = 0; c < 3; c++) m->pal[i][c] = m->pal[i][c] ? 15 : 0; }
    return 0;
}
static int lio_gscreen(Machine* m, uint32_t a) {
    uint8_t mode = rb8(m, a), sw = rb8(m, a + 1), act = rb8(m, a + 2), disp = rb8(m, a + 3);
    uint8_t sm = mode == 0xFF ? s_lio.scrnmode : mode;
    if (sm >= 4 || (sw != 0xFF && sw >= 4)) return 5;
    bool changed = sm != s_lio.scrnmode;
    s_lio.scrnmode = sm;
    if (act != 0xFF) {
        if (sm == 0) { s_lio.pos = act & 1; s_lio.access = (act >> 1) & 1; }
        else if (sm == 3) { s_lio.pos = 0; s_lio.access = act & 1; }
        else { s_lio.pos = 0; s_lio.access = (act >> 3) & 1; }   // 単色: プレーン指定は描画では無視（カラーと同じに描く）
    } else if (changed) { s_lio.pos = 0; s_lio.access = 0; }
    if (changed || mode != 0xFF) { s_lio.vx1 = 0; s_lio.vy1 = 0; s_lio.vx2 = 639; s_lio.vy2 = (int16_t)lio_maxline(); }
    m->draw_bank = s_lio.access;
    // 表示: disp=0 で消す。それ以外は表示（バンクは上位ビット）
    if (disp != 0xFF || changed) {
        uint8_t d = disp == 0xFF ? 1 : disp;
        int bank = (d >> 4) & 1;
        if ((d & 0x0F) == 0) m->gdcs.display = 0;
        else {
            m->gdcs.display = 1;
            m->disp_bank = (uint8_t)bank;
            memset(m->gdcs.pram, 0, 4);
            if (sm >= 2) { m->gfx_200 = 0; m->gdcs.zoom = 0; }
            else {
                m->gfx_200 = 1; m->gfx_200_lower = 0; m->gdcs.zoom = 0;
                if ((d & 0x0F) == 2) { m->gdcs.pram[0] = (uint8_t)(8000 & 0xFF); m->gdcs.pram[1] = (uint8_t)(8000 >> 8); }
                m->gdcs.pram[3] = 0x40;
            }
        }
    }
    (void)sw;
    return 0;
}
static int lio_gview(Machine* m, uint32_t a) {
    s_lio.vx1 = (int16_t)rw16(m, a); s_lio.vy1 = (int16_t)rw16(m, a + 2);
    s_lio.vx2 = (int16_t)rw16(m, a + 4); s_lio.vy2 = (int16_t)rw16(m, a + 6);
    uint8_t bgc = rb8(m, a + 8), lnc = rb8(m, a + 9);
    if (bgc != 0xFF) fill(m, clipx1(), clipy1(), clipx2(), clipy2(), bgc, nullptr, 0);
    if (lnc != 0xFF) {   // 枠は範囲の外側 1 ドットに描く
        LioState keep = s_lio;
        s_lio.vx1 = 0; s_lio.vy1 = 0; s_lio.vx2 = 639; s_lio.vy2 = 399;
        box(m, keep.vx1 - 1, keep.vy1 - 1, keep.vx2 + 1, keep.vy2 + 1, lnc, 0xFFFF);
        s_lio = keep;
    }
    return 0;
}
static int lio_gcolor1(Machine* m, uint32_t a) {
    uint8_t bgc = rb8(m, a + 1), bdc = rb8(m, a + 2), fgc = rb8(m, a + 3), pm = rb8(m, a + 4);
    if (bgc != 0xFF) s_lio.bg = bgc;
    if (fgc != 0xFF) s_lio.fg = fgc;
    if (bdc != 0xFF) m->border = bdc;
    if (pm != 0xFF) { if (pm > 2) return 5; s_lio.palmode = pm; m->analog = pm ? 1 : 0; }
    return 0;
}
static int lio_gcolor2(Machine* m, uint32_t a) {
    uint8_t pal = rb8(m, a), c1 = rb8(m, a + 1), c2 = rb8(m, a + 2);
    if (pal >= lio_palmax()) return 5;
    if (s_lio.palmode == 0) set_digital(m, pal, c1 & 7);
    else { m->pal[pal][2] = c1 & 15; m->pal[pal][1] = (uint8_t)(c1 >> 4); m->pal[pal][0] = c2 & 15; }
    return 0;
}
static int lio_gline(Machine* m, uint32_t a) {
    int x1 = (int16_t)rw16(m, a), y1 = (int16_t)rw16(m, a + 2), x2 = (int16_t)rw16(m, a + 4), y2 = (int16_t)rw16(m, a + 6);
    uint8_t pal = rb8(m, a + 8), type = rb8(m, a + 9), sw = rb8(m, a + 10);
    uint8_t st0 = rb8(m, a + 11), st1 = rb8(m, a + 12), patleng = rb8(m, a + 13);
    uint16_t toff = rw16(m, a + 14), tseg = rw16(m, a + 16);
    if (pal == 0xFF) pal = s_lio.fg;
    if (pal >= lio_palmax() || sw > 2) return 5;
    if (type < 2) {
        if (sw == 2) return 5;
        uint16_t style = sw == 1 ? (uint16_t)((rev8(st0) << 8) | rev8(st1)) : 0xFFFF;
        // 点線の型は LSB から使う並びなので、逆順にして最上位から使う
        style = (uint16_t)((rev8((uint8_t)style) << 8) | rev8((uint8_t)(style >> 8)));
        if (type == 0) line(m, x1, y1, x2, y2, pal, style); else box(m, x1, y1, x2, y2, pal, style);
        return 0;
    }
    if (type != 2) return 5;
    uint8_t tile[256]; int tl = 0;
    if (sw == 2) {
        tl = patleng;
        if (tl == 0 || tl % lio_planes()) return 5;
        for (int i = 0; i < tl; i++) tile[i] = rb8(m, ((uint32_t)tseg << 4) + (uint16_t)(toff + i));
    }
    if (sw == 1) {   // 枠の色 = pal、中の色 = style の 1 バイト目
        uint8_t in = st0 == 0xFF ? s_lio.fg : st0;
        if (in >= lio_palmax()) return 5;
        fill(m, x1, y1, x2, y2, in, nullptr, 0);
        if (st0 != 0xFF) box(m, x1, y1, x2, y2, pal, 0xFFFF);
        return 0;
    }
    fill(m, x1, y1, x2, y2, pal, sw == 2 ? tile : nullptr, tl);
    return 0;
}
static int lio_gpset(Machine* m, uint32_t a) {
    int x = (int16_t)rw16(m, a), y = (int16_t)rw16(m, a + 2); uint8_t pal = rb8(m, a + 4);
    if (pal == 0xFF) pal = s_lio.fg;
    if (pal >= lio_palmax()) return 5;
    pset(m, x, y, pal);
    return 0;
}
static int lio_gcls(Machine* m) {
    fill(m, clipx1(), clipy1(), clipx2(), clipy2(), s_lio.bg, nullptr, 0);
    return 0;
}
static int lio_gget(Machine* m, uint32_t a) {
    int x1 = (int16_t)rw16(m, a), y1 = (int16_t)rw16(m, a + 2), x2 = (int16_t)rw16(m, a + 4), y2 = (int16_t)rw16(m, a + 6);
    uint32_t buf = ((uint32_t)rw16(m, a + 10) << 4) + rw16(m, a + 8);
    unsigned leng = rw16(m, a + 12);
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (x1 < clipx1() || y1 < clipy1() || x2 > clipx2() || y2 > clipy2()) return 5;
    int w = x2 - x1 + 1, h = y2 - y1 + 1, bpr = (w + 7) >> 3, planes = lio_planes();
    if (leng < (unsigned)(bpr * h * planes + 4)) return 7;
    auto wb = [&](uint32_t o, uint8_t v) { m->ram[(buf + o) & 0xFFFFF] = v; };
    wb(0, (uint8_t)w); wb(1, (uint8_t)(w >> 8)); wb(2, (uint8_t)h); wb(3, (uint8_t)(h >> 8));
    uint32_t o = 4;
    for (int y = y1; y <= y2; y++)
        for (int p = 0; p < planes; p++)
            for (int b = 0; b < bpr; b++) {
                uint8_t v = 0;
                for (int k = 0; k < 8; k++) { int x = x1 + b * 8 + k; if (x <= x2 && (pget(m, x, y) & (1 << p))) v |= (uint8_t)(0x80 >> k); }
                wb(o++, v);
            }
    return 0;
}
// GPUT1: 0=PSET 1=NOT 2=OR 3=AND 4=XOR。colorsw=1 なら 1 枚のビット列を fg/bg の 2 色で置く
static int lio_gput1(Machine* m, uint32_t a) {
    int x0 = (int16_t)rw16(m, a), y0 = (int16_t)rw16(m, a + 2);
    uint32_t buf = ((uint32_t)rw16(m, a + 6) << 4) + rw16(m, a + 4);
    unsigned leng = rw16(m, a + 8);
    uint8_t mode = rb8(m, a + 10), csw = rb8(m, a + 11), fgc = rb8(m, a + 12), bgc = rb8(m, a + 13);
    int w = rw16(m, buf), h = rw16(m, buf + 2);
    if (mode > 4 || csw > 1) return 5;
    if (w == 0 || h == 0) return 0;
    int bpr = (w + 7) >> 3, planes = lio_planes();
    if (leng < (unsigned)(bpr * h + 4)) return 5;
    if (csw && (fgc >= lio_palmax() || bgc >= lio_palmax())) return 5;
    if (x0 < clipx1() || y0 < clipy1() || x0 + w - 1 > clipx2() || y0 + h - 1 > clipy2()) return 5;
    uint32_t o = buf + 4;
    for (int y = 0; y < h; y++) {
        for (int p = 0; p < planes; p++) {
            uint32_t row = csw ? buf + 4 + (uint32_t)(y * bpr) : o;
            for (int x = 0; x < w; x++) {
                bool on = (rb8(m, row + (x >> 3)) >> (7 - (x & 7))) & 1;
                // このプレーンに置く値: 色付き(colorsw)なら fg/bg の該当ビット、そうでなければデータそのもの
                bool v = csw ? (((on ? fgc : bgc) >> p) & 1) : on;
                int px = x0 + x, py = y0 + y;
                uint32_t ad = vaddr(px, py); uint8_t bit = (uint8_t)(0x80 >> (px & 7));
                uint8_t& d = m->gvram[s_lio.access & 1][p][ad];
                bool cur = (d & bit) != 0, nv = cur;
                switch (mode) {
                case 0: nv = v; break;
                case 1: nv = !v; break;
                case 2: nv = cur || v; break;
                case 3: nv = cur && v; break;
                case 4: nv = cur != v; break;
                }
                if (nv) d |= bit; else d &= (uint8_t)~bit;
            }
            if (!csw) o += (uint32_t)bpr;
        }
    }
    return 0;
}
static int lio_gpoint2(Machine* m, uint32_t a) {
    int x = (int16_t)rw16(m, a), y = (int16_t)rw16(m, a + 2);
    uint8_t c = 0xFF;
    if (x >= clipx1() && x <= clipx2() && y >= clipy1() && y <= clipy2()) c = (uint8_t)pget(m, x, y);
    m->cpu.r[EAX] = (m->cpu.r[EAX] & 0xFFFFFF00u) | c;
    return 0;
}

void lio_hle(Machine* m, uint8_t n) {
    static const char* const names[16] = {
        "GINIT", "GSCREEN", "GVIEW", "GCOLOR1", "GCOLOR2", "GCLS", "GPSET", "GLINE",
        "GCIRCLE", "GPAINT1", "GPAINT2", "GGET", "GPUT1", "GPUT2", "GROLL", "GPOINT2" };
    int f = (n - HLE_LIO) & 15;
    uint32_t a = lin(m->cpu.sr[DS_], (uint16_t)m->cpu.r[EBX]);
    if (m->cfg.trace) {
        plog("[lio] INT %02Xh %s DS:BX=%04X:%04X param:", 0xA0 + f, names[f], m->cpu.sr[DS_], (unsigned)(m->cpu.r[EBX] & 0xFFFF));
        for (int i = 0; i < 16; i++) plog(" %02X", m->ram[(a + i) & 0xFFFFF]);
        plog("\n");
    }
    int ret = 0;
    switch (f) {
    case 0x0: ret = lio_ginit(m); break;
    case 0x1: ret = lio_gscreen(m, a); break;
    case 0x2: ret = lio_gview(m, a); break;
    case 0x3: ret = lio_gcolor1(m, a); break;
    case 0x4: ret = lio_gcolor2(m, a); break;
    case 0x5: ret = lio_gcls(m); break;
    case 0x6: ret = lio_gpset(m, a); break;
    case 0x7: ret = lio_gline(m, a); break;
    case 0xB: ret = lio_gget(m, a); break;
    case 0xC: ret = lio_gput1(m, a); break;
    case 0xF: ret = lio_gpoint2(m, a); break;
    default: break;   // GCIRCLE・GPAINT1/2・GPUT2・GROLL は何もしない
    }
    m->cpu.r[EAX] = (m->cpu.r[EAX] & 0xFFFF00FFu) | ((uint32_t)(ret & 0xFF) << 8);   // AH: 0=正常終了 5=引数の誤り
}
