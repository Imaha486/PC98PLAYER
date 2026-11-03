// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  pegc.cpp  --  PC-9821 の 256 色グラフィック（PEGC）
//
//  ・6Ah に 07h（モード変更の許可）のあと 21h で 256 色、20h で 16 色に戻る。
//    69h で「1 画面」（512KB の VRAM を続けて使う）、68h で「2 画面」（256KB ずつ、A4h で切替）。
//  ・09A8h: 水平周波数（0 = 24kHz・400 ライン、1 = 31kHz・480 ライン）
//  ・パレット: A8h に番号（0-255）、AAh・ACh・AEh に緑・赤・青（8 ビット）
//  ・VRAM（512KB）はパックドピクセル（1 バイト 1 ドット）で、A8000h-AFFFFh と B0000h-B7FFFh の
//    2 つの 32KB の窓から見る。窓の位置は E0004h・E0006h（32KB 単位の番号）。
//  ・E0000h-E7FFFh はメモリにマップされたレジスタ（E0100h bit0 = プレーンモード など）
//  ・プレーンモードでは 1 バイトが 8 ドット。ROP・ビットマスク・書き込み禁止のプレーン・色の比較がある
//  ・表示はグラフィック GDC の SCROLL（開始アドレスと行数）と PITCH にしたがう。1 行は PITCH×8 バイト。
//  資料: SL9821 の技術的なはなし（PEGC）、Neko Project 21/W の動作（コードは使っていない）
// -----------------------------------------------------------------------------
#include "machine.h"
#include <string.h>

static const uint32_t VRAM_SIZE = 0x80000;

void pegc_clear(Machine* m) {
    memset(&m->pegc, 0, sizeof(m->pegc));
    m->pegc_vram.assign(VRAM_SIZE, 0);
    m->pegc_planar = PegcPlanar();
    m->pegc.mmio[0x10C] = m->pegc.mmio[0x10D] = 0xFF;   // ビットマスク: 全部書く
}

static inline uint32_t bank_base(Machine* m, int w) {
    uint16_t b = (uint16_t)(m->pegc.mmio[4 + w * 2] | (m->pegc.mmio[5 + w * 2] << 8));
    return (uint32_t)(b & 15) << 15;
}

bool pegc_io_out(Machine* m, uint16_t port, uint8_t v) {
    Pegc& p = m->pegc;
    switch (port) {
    case 0x6A:
        if (v == 0x20 || v == 0x21) {
            if (v == 0x21) pc9821_probe(m, "6Ah で 256 色");
            if (!is_pc9821(m)) return true;    // 9801（256 色なし）として動かしている
            if (!m->modeff2[3]) return true;   // モード変更の許可（07h）が要る
            if (p.on != (v & 1)) plog("[pegc] %s\n", (v & 1) ? "256 色モードにしました" : "16 色モードに戻しました");
            p.on = v & 1;
            return true;
        }
        if (v == 0x68 || v == 0x69) { p.single = v & 1; return true; }
        return false;
    case 0x9A0: pc9821_probe(m, "09A0h"); if (!is_pc9821(m)) return false; p.ff_sel = v; p.ff_selected = 1; return true;
    case 0x9A8: p.crt31 = v & 1; return true;
    case 0xA8: if (!p.on) return false; p.palidx = v; return true;
    case 0xAA: if (!p.on) return false; p.pal[p.palidx][0] = v; return true;
    case 0xAC: if (!p.on) return false; p.pal[p.palidx][1] = v; return true;
    case 0xAE: if (!p.on) return false; p.pal[p.palidx][2] = v; return true;
    }
    return false;
}

bool pegc_io_in(Machine* m, uint16_t port, uint8_t* v) {
    Pegc& p = m->pegc;
    switch (port) {
    case 0x9A0: {
        pc9821_probe(m, "09A0h");
        // モードフリップフロップの読み出し: 番号を書いてから読むと bit0 にその状態。bit1 はいつもグラフィック GDC 5MHz
        uint8_t b = m->gdc_clk5 ? 0x02 : 0x00;
        if (!p.ff_selected || !is_pc9821(m)) { *v = b; return true; }   // 番号を書く前・9801 は従来どおり
        switch (p.ff_sel) {
        case 0x00: *v = 0xFF; return true;
        case 0x01: b |= m->modeff[1] & 1; break;
        case 0x02: b |= m->modeff[4] & 1; break;
        case 0x03: b |= m->modeff[7] & 1; break;
        case 0x04: b |= m->modeff2[0] & 1; break;      // アナログ（16 色）パレット
        case 0x07: b |= m->modeff2[2] & 1; break;      // EGC
        case 0x08: b |= m->modeff2[3] & 1; break;      // モード変更の許可
        case 0x09: b |= m->gdc_clk5 ? 1 : 0; break;    // GDC 5MHz
        case 0x0A: b |= p.on; break;                   // 256 色モード
        case 0x0B: b |= 1; break;
        case 0x0D: b |= p.single; break;               // VRAM 連結（1 画面）
        default: break;
        }
        *v = b; return true; }
    case 0x9A8: *v = p.crt31; return true;
    case 0xA8: if (!p.on) return false; *v = p.palidx; return true;
    case 0xAA: if (!p.on) return false; *v = p.pal[p.palidx][0]; return true;
    case 0xAC: if (!p.on) return false; *v = p.pal[p.palidx][1]; return true;
    case 0xAE: if (!p.on) return false; *v = p.pal[p.palidx][2]; return true;
    }
    return false;
}

// ---- プレーンモード（E0100h bit0=1） ------------------------------------------------
// 窓の 1 バイトが 8 ドット（左のドットが bit7）。8 ビットの色を 8 枚のプレーンとして、
// 書き込み禁止のプレーン（E0104h）・ビットマスク（E010Ch）・ROP（E0108h）で書き、
// 読むと転送元のラッチに色を取り込む（E010Ah bit0 なら前景色 E0114h と比べた結果を返す）。
//  E0108h: bit0-7 ROP の真理値表（転送元・転送先・パターンの組で引く） bit8 転送元は CPU のデータ
//          bit10-11 パターン（0: 読んだ色 1: 背景色 E0118h 2: 前景色 E0114h 3: 前景色と背景色で引く）
//          bit12 ROP を使う（0 なら転送元をそのまま） bit13 読んだ色をパターンにも取り込む
//  E0112h: 下位 4 ビット = 読み出しのずらし、bit8-11 = 書き込みのずらし
static inline uint16_t mm16(const Pegc& p, int o) { return (uint16_t)(p.mmio[o] | (p.mmio[o + 1] << 8)); }
static inline int dot_bit(int i) { return (i & ~7) | (7 - (i & 7)); }   // ドット i に当たるデータのビット
static uint32_t plane_dot0(Machine* m, uint32_t a) {
    uint32_t page = m->pegc.single ? 0 : (uint32_t)(m->draw_bank & 1) * 0x40000;
    return ((a - 0xA8000) * 8 + page) & (VRAM_SIZE - 1);
}
bool pegc_window(Machine* m, uint32_t a) { return m->pegc.on && a >= 0xA8000 && a < 0xB8000; }

uint16_t pegc_read(Machine* m, uint32_t a, int bytes) {
    Pegc& p = m->pegc;
    uint16_t r = 0;
    if (!(p.mmio[0x100] & 1)) {                         // パックドピクセル
        for (int k = 0; k < bytes; k++) {
            uint32_t at = a + (uint32_t)k;
            r |= (uint16_t)(m->pegc_vram[(bank_base(m, at >= 0xB0000) + (at & 0x7FFF)) & (VRAM_SIZE - 1)] << (8 * k));
        }
        return r;
    }
    if (!p.single && a >= 0xB0000) return 0xFFFF;
    PegcPlanar& L = m->pegc_planar;
    uint16_t mode = mm16(p, 0x108);
    uint8_t writable = (uint8_t)~p.mmio[0x104];
    bool cpu_src = (mode & 0x100) != 0;
    uint32_t d0 = plane_dot0(m, a);
    for (int i = 0; i < bytes * 8; i++) {
        uint8_t c = m->pegc_vram[(d0 + (uint32_t)i) & (VRAM_SIZE - 1)];
        L.source[i] = c;
        if ((mode & 0x2000) && !cpu_src) L.pattern[i] = c;
        if ((p.mmio[0x10A] & 1) && !cpu_src && !((c ^ p.mmio[0x114]) & writable)) r |= (uint16_t)(1u << dot_bit(i));
    }
    return r;
}

void pegc_write(Machine* m, uint32_t a, uint16_t v, int bytes) {
    Pegc& p = m->pegc;
    if (!(p.mmio[0x100] & 1)) {                         // パックドピクセル
        for (int k = 0; k < bytes; k++) {
            uint32_t at = a + (uint32_t)k;
            m->pegc_vram[(bank_base(m, at >= 0xB0000) + (at & 0x7FFF)) & (VRAM_SIZE - 1)] = (uint8_t)(v >> (8 * k));
        }
        return;
    }
    if (!p.single && a >= 0xB0000) return;
    PegcPlanar& L = m->pegc_planar;
    uint16_t mode = mm16(p, 0x108), bitmask = mm16(p, 0x10C), sh = mm16(p, 0x112);
    uint8_t writable = (uint8_t)~p.mmio[0x104];
    uint8_t fg = p.mmio[0x114], bg = p.mmio[0x118];
    int shift = (int)(sh & 15) - (int)((sh >> 8) & 15);
    int pat_sel = (mode >> 10) & 3;
    uint32_t d0 = plane_dot0(m, a);
    for (int i = 0; i < bytes * 8; i++) {
        int b = dot_bit(i);
        if (!(bitmask & (1u << b))) continue;
        uint8_t& dst = m->pegc_vram[(d0 + (uint32_t)i) & (VRAM_SIZE - 1)];
        uint8_t src = (mode & 0x100) ? (uint8_t)((v >> b) & 1 ? 0xFF : 0x00) : L.source[(i + shift + 32) & 31];
        uint8_t out;
        if (!(mode & 0x1000)) out = src ? 0xFF : 0x00;
        else {
            uint8_t pat = pat_sel == 0 ? L.pattern[i] : pat_sel == 1 ? bg : fg;
            uint8_t mid = pat_sel == 3 ? fg : dst;      // 3: 転送先の代わりに前景色、パターンは背景色
            if (pat_sel == 3) pat = bg;
            out = 0;
            for (int k = 0; k < 8; k++) {
                int idx = (((src >> k) & 1) << 2) | (((mid >> k) & 1) << 1) | ((pat >> k) & 1);
                out |= (uint8_t)(((mode >> idx) & 1) << k);
            }
        }
        dst = (uint8_t)((dst & ~writable) | (out & writable));
    }
}

bool pegc_mem_rb(Machine* m, uint32_t a, uint8_t* v) {
    Pegc& p = m->pegc;
    if (!p.on) return false;
    if (a >= 0xA8000 && a < 0xB8000) {
        if (p.mmio[0x100] & 1) { *v = (uint8_t)pegc_read(m, a, 1); return true; }
        int w = a >= 0xB0000;
        *v = m->pegc_vram[(bank_base(m, w) + (a & 0x7FFF)) & (VRAM_SIZE - 1)];
        return true;
    }
    if (a >= 0xB8000 && a < 0xC0000) { *v = 0xFF; return true; }
    if (a >= 0xE0000 && a < 0xE8000) {
        uint32_t o = a - 0xE0000;
        *v = o < sizeof(p.mmio) ? p.mmio[o] : 0xFF;
        return true;
    }
    return false;
}

bool pegc_mem_wb(Machine* m, uint32_t a, uint8_t v) {
    Pegc& p = m->pegc;
    if (!p.on) return false;
    if (a >= 0xA8000 && a < 0xB8000) {
        if (p.mmio[0x100] & 1) { pegc_write(m, a, v, 1); return true; }
        int w = a >= 0xB0000;
        m->pegc_vram[(bank_base(m, w) + (a & 0x7FFF)) & (VRAM_SIZE - 1)] = v;
        return true;
    }
    if (a >= 0xB8000 && a < 0xC0000) return true;
    if (a >= 0xE0000 && a < 0xE8000) {
        uint32_t o = a - 0xE0000;
        if (o < sizeof(p.mmio)) {
            if (o == 0x100 && ((p.mmio[o] ^ v) & 1)) plog("[pegc] 描き方: %s\n", (v & 1) ? "プレーン" : "パックドピクセル");
            if (o == 0x102 && ((p.mmio[o] ^ v) & 1)) plog("[pegc] 上位メモリの VRAM（F00000h）: %s\n", (v & 1) ? "有効" : "無効");
            p.mmio[o] = v;
        }
        return true;
    }
    return false;
}

// 256 色の画面（グラフィック）を描く。行数を返す
int pegc_render(Machine* m, uint32_t* out) {
    Pegc& p = m->pegc;
    if (!p.on) return 0;
    int H = p.crt31 ? 480 : 400;   // 31kHz（09A8h）のときだけ 480 行
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0xFF000000u | ((uint32_t)p.pal[i][1] << 16) | ((uint32_t)p.pal[i][0] << 8) | p.pal[i][2];
    bool gon = m->gdcs.display != 0;
    uint32_t pitch = m->gdc_clk5 ? m->gdcs.pitch : (uint32_t)m->gdcs.pitch * 2;
    pitch &= 0xFE;
    if (pitch == 0) pitch = 80;
    uint32_t page = p.single ? 0 : (uint32_t)(m->disp_bank & 1) * 0x40000;
    uint32_t mask = p.single ? (VRAM_SIZE - 1) : 0x3FFFF;
    // SCROLL の 2 つの領域（開始アドレスはワード単位。8 バイト単位にするため 2 倍）
    int y = 0;
    for (int area = 0; area < 2 && y < H; area++) {
        const uint8_t* pr = m->gdcs.pram + area * 4;
        uint32_t sad = (uint32_t)(pr[0] | (pr[1] << 8) | ((pr[2] & 3) << 16));
        uint32_t len = (uint32_t)(((pr[2] | (pr[3] << 8)) & 0x3FFF) >> 4);
        if (len == 0) len = (uint32_t)H;
        uint32_t vad = (sad << 1) & (mask >> 3);
        for (uint32_t l = 0; l < len && y < H; l++, y++) {
            uint32_t* o = out + y * 640;
            if (!gon) { for (int x = 0; x < 640; x++) o[x] = 0xFF000000u; continue; }
            uint32_t base = page + ((vad * 8) & mask);
            for (int x = 0; x < 640; x++) o[x] = pal[m->pegc_vram[(base + (uint32_t)x) & (VRAM_SIZE - 1)]];
            vad = (vad + pitch) & (mask >> 3);
        }
        if (area == 0 && len >= (uint32_t)H) break;
    }
    for (; y < H; y++) for (int x = 0; x < 640; x++) out[y * 640 + x] = 0xFF000000u;
    return H;
}
