// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  fontrom.cpp  --  擬似 PC-98 漢字 ROM / ANK ROM
//
//  本物の ROM も第三者のビットマップフォントも使わずに、PC-98 の文字の並びを
//  再現する。字形の出どころは次の 3 つだけ。
//
//   (1) ホストのフォント（Windows では MS ゴシック）
//       JIS 第 1・第 2 水準、記号、NEC 特殊文字（13 区）、NEC 選定 IBM 拡張
//   (2) (1) を変換したもの
//       半角化（全角 → 8 ドット幅: 年月日時分秒、ヰヱヮヵヶ、〔〕〈〉『』【】…）
//       濁点・半濁点の合成（半角のガ行・ザ行・ダ行・バ行・パ行、ヴ）
//   (3) 幾何学的に描くもの
//       罫線（JIS 8 区、PC-98 の 11 区[半角]・12 区[全角] = U+2500〜254B の 76 字）、
//       ANK のブロック・罫線・三角・丸・斜線、制御コードの略号、矢印、
//       トランプの記号（ドット絵を自作）
//
//  外字（76・77 区）は RAM として持ち、ゲームが書き込んだ字形をそのまま出す。
//  PC-98 の漢字 ROM は JIS C 6226-1978（旧 JIS）の並びなので、1983 年の改正で
//  入れ替わった字は既定で入れ替えて引く（INI の KanjiJIS=83 で無効）。
// -----------------------------------------------------------------------------
#include "machine.h"
#include "state.h"
#include "boxtable.h"
#include <string.h>

static FontSource s_font = {nullptr, nullptr, nullptr, nullptr};
static uint8_t  s_kanji[94 * 94][32];
static uint8_t  s_kanji_ok[94 * 94];
static uint8_t  s_ank[256][16];
static uint8_t  s_ank_ok[256];
static uint8_t  s_blank[32];
static uint8_t  s_gaiji[2][128][32];     // 76 区・77 区（外字は第 2 バイト 00h-7Fh の 128 字ずつ）
static bool     s_jis78 = true;

void video_set_font(const FontSource& fs) {
    s_font = fs;
    memset(s_kanji_ok, 0, sizeof(s_kanji_ok));
    memset(s_ank_ok, 0, sizeof(s_ank_ok));
}
void fontrom_set_jis78(bool on) {
    s_jis78 = on;
    memset(s_kanji_ok, 0, sizeof(s_kanji_ok));
}

uint16_t sjis_to_jis(uint16_t sj) {
    uint8_t c1 = sj >> 8, c2 = sj & 0xFF;
    if (c1 >= 0xE0) c1 -= 0x40;
    c1 -= 0x81;
    int row = c1 * 2;
    if (c2 >= 0x9F) { row++; c2 -= 0x9F; }
    else { c2 -= (c2 >= 0x80) ? 0x41 : 0x40; }
    return (uint16_t)(((row + 0x21) << 8) | (c2 + 0x21));
}

// ---- 旧 JIS（1978）と新 JIS（1983）の入れ替え --------------------------------
//  1983 年の改正で字形が入れ替わった組（JIS の規格どうしの対応であって字形データではない）
static const uint16_t k_swap[][2] = {
    {0x3646, 0x7421}, {0x4B6A, 0x7422}, {0x4D5A, 0x7423}, {0x596A, 0x7424},
    {0x724D, 0x3033}, {0x7274, 0x3229}, {0x695A, 0x3342}, {0x5978, 0x3349},
    {0x635E, 0x3376}, {0x5E75, 0x3443}, {0x6B5D, 0x3452}, {0x7074, 0x375B},
    {0x6268, 0x395C}, {0x6922, 0x3C49}, {0x7057, 0x3F59}, {0x6C4D, 0x4128},
    {0x5464, 0x445B}, {0x626A, 0x4557}, {0x5B6D, 0x456E}, {0x5E39, 0x4573},
    {0x6D6E, 0x4676}, {0x6A24, 0x4768}, {0x5B58, 0x4930}, {0x5056, 0x4B79},
    {0x692E, 0x4C79}, {0x6446, 0x4F36},
};
static uint16_t jis78(uint16_t jis) {
    if (!s_jis78) return jis;
    for (auto& p : k_swap) { if (jis == p[0]) return p[1]; if (jis == p[1]) return p[0]; }
    return jis;
}

// ---- ホストから字形をもらう ------------------------------------------------------
static void host_kanji(uint16_t jis, uint8_t out[32]) {
    memset(out, 0, 32);
    if (s_font.kanji) s_font.kanji(s_font.user, jis, out);
}
static void host_ank(uint8_t c, uint8_t out[16]) {
    memset(out, 0, 16);
    if (s_font.ank) s_font.ank(s_font.user, c, out);
}
// 全角の字を 8 ドット幅にする。ホストが縦長フォントで描けるならそれを使い、
// だめなら全角の字形を横に 1/2 に畳む（隣り合う 2 ドットの OR）。
static void narrow_from_full(uint16_t jis, uint8_t out[16]) {
    memset(out, 0, 16);
    if (s_font.narrow) { s_font.narrow(s_font.user, jis, out); return; }
    uint8_t g[32];
    host_kanji(jis, g);
    for (int y = 0; y < 16; y++) {
        uint16_t v = (uint16_t)((g[y * 2] << 8) | g[y * 2 + 1]);
        uint8_t o = 0;
        for (int k = 0; k < 8; k++) if (v & (0xC000 >> (k * 2))) o |= (uint8_t)(0x80 >> k);
        out[y] = o;
    }
}
// 全角の括弧などを、字のある位置で 8 ドット分だけ切り出す（左寄せ／右寄せ）
static void half_from_full_cut(uint16_t jis, bool right_align, uint8_t out[16]) {
    uint8_t g[32];
    host_kanji(jis, g);
    int minx = 16, maxx = -1;
    for (int y = 0; y < 16; y++) {
        uint16_t v = (uint16_t)((g[y * 2] << 8) | g[y * 2 + 1]);
        for (int x = 0; x < 16; x++) if (v & (0x8000 >> x)) { if (x < minx) minx = x; if (x > maxx) maxx = x; }
    }
    memset(out, 0, 16);
    if (maxx < 0) return;
    int start = right_align ? maxx - 7 : minx;
    if (!right_align && maxx - minx < 7 && minx > 0) start = minx - 1;
    if (start < 0) start = 0;
    if (start > 8) start = 8;
    for (int y = 0; y < 16; y++) {
        uint16_t v = (uint16_t)((g[y * 2] << 8) | g[y * 2 + 1]);
        out[y] = (uint8_t)((v << start) >> 8);
    }
}

// ---- 罫線 -----------------------------------------------------------------------
//  w=16（全角）: 縦線 x=8（太線 7-8）、横線 y=7（太線 7-8）
//  w=8 （半角）: 縦線 x=4（太線 3-4）、横線 y=7（太線 7-8）
static void draw_box(const BoxArms& b, int w, uint8_t* out /* w=16: 32B, w=8: 16B */) {
    uint16_t rows[16];
    memset(rows, 0, sizeof(rows));
    int vx = (w == 16) ? 8 : 4;
    int vmaxw = b.u > b.d ? b.u : b.d;        // 縦の太さ（横線の端をどこまで伸ばすか）
    int hmaxw = b.l > b.r ? b.l : b.r;
    auto colbits = [&](int weight) -> uint16_t {
        uint16_t m = (uint16_t)(1u << (15 - vx));
        if (weight == 2) m |= (uint16_t)(1u << (16 - vx));
        return m;
    };
    uint16_t hdash = 0xFFFF, vdash = 0xFFFF;
    if (b.dash == 3) { hdash = 0x7777; vdash = 0x7777; }
    if (b.dash == 4) { hdash = 0x6666; vdash = 0x6666; }
    if (w == 8 && b.dash) hdash = (uint16_t)((hdash & 0xFF00) | 0x00FF);
    // 横の腕
    for (int side = 0; side < 2; side++) {
        int weight = side ? b.r : b.l;
        if (!weight) continue;
        int x0, x1;
        int cmin = vx - (vmaxw == 2 ? 1 : 0), cmax = vx;
        if (!side) { x0 = 0; x1 = vmaxw ? cmax : vx; }
        else { x0 = vmaxw ? cmin : vx; x1 = w - 1; }
        uint16_t m = 0;
        for (int x = x0; x <= x1; x++) if (hdash & (0x8000 >> (w == 16 ? x : x))) m |= (uint16_t)(0x8000 >> x);
        rows[7] |= m;
        if (weight == 2) rows[8] |= m;
    }
    // 縦の腕
    for (int side = 0; side < 2; side++) {
        int weight = side ? b.d : b.u;
        if (!weight) continue;
        int y0, y1;
        int rmax = (hmaxw == 2) ? 8 : 7;
        if (!side) { y0 = 0; y1 = hmaxw ? rmax : 7; }
        else { y0 = 7; y1 = 15; }
        uint16_t m = colbits(weight);
        for (int y = y0; y <= y1; y++) if (vdash & (0x8000 >> y)) rows[y] |= m;
    }
    for (int y = 0; y < 16; y++) {
        if (w == 16) { out[y * 2] = (uint8_t)(rows[y] >> 8); out[y * 2 + 1] = (uint8_t)rows[y]; }
        else out[y] = (uint8_t)(rows[y] >> 8);
    }
}
static void box_unicode(uint16_t u, int w, uint8_t* out) {
    memset(out, 0, w == 16 ? 32 : 16);
    if (u < 0x2500 || u > 0x254B) return;
    draw_box(k_box2500[u - 0x2500], w, out);
}

// ---- ANK の特殊文字 -------------------------------------------------------------
// 制御コードの略号（3x5 の小さな字: 自作）
static const char* k_mini[] = {
    "A010101111101101", "B110101110101110", "C011100100100011", "D110101101101110",
    "E111100110100111", "F111100110100100", "H101101111101101", "I111010010010111",
    "K101110100110101", "L100100100100111", "M101111111101101", "N110101101101101",
    "O010101101101010", "Q010101101111011", "R110101110101101", "S011100010001110",
    "T111010010010010", "X101101010101101", "1010110010010111", "2110001010100111",
    "3110001010001110", "4101101111001001",
};
static void mini_char(char c, int ox, int oy, uint8_t out[16]) {
    for (auto s : k_mini) {
        if (s[0] != c) continue;
        for (int y = 0; y < 5; y++) for (int x = 0; x < 3; x++)
            if (s[1 + y * 3 + x] == '1') out[oy + y] |= (uint8_t)(0x80 >> (ox + x));
        return;
    }
}
static const char* k_ctrl[28] = {
    "", "SH", "SX", "EX", "ET", "EQ", "AK", "BL", "BS", "HT", "LF", "HM", "CL", "CR", "SO", "SI",
    "DE", "D1", "D2", "D3", "D4", "NK", "SN", "EB", "CN", "EM", "SB", "EC",
};
// トランプの記号（8x8 のドット絵: 自作）
static const char* k_suit[4][8] = {
    { "...#....", "..###...", ".#####..", "#######.", "#######.", ".#.#.#..", "...#....", "..###..." },  // ♠
    { ".##.##..", "#######.", "#######.", "#######.", ".#####..", "..###...", "...#....", "........" },  // ♥
    { "...#....", "..###...", ".#####..", "#######.", ".#####..", "..###...", "...#....", "........" },  // ♦
    { "..###...", "..###...", "#.###.#.", "#######.", "#.#.#.#.", "...#....", "..###...", "........" },  // ♣
};

static void make_ank(uint8_t c, uint8_t o[16]) {
    memset(o, 0, 16);
    if (c == 0x00 || c == 0x7F || c == 0xA0) return;
    if (c < 0x1C) {
        const char* n = k_ctrl[c];
        if (n[0]) { mini_char(n[0], 0, 2, o); mini_char(n[1], 4, 9, o); }
        return;
    }
    if (c >= 0x1C && c <= 0x1F) {   // → ← ↑ ↓
        if (c == 0x1C) { o[8] = 0xFE; o[6] = 0x08; o[7] = 0x0C; o[9] = 0x0C; o[10] = 0x08; }
        if (c == 0x1D) { o[8] = 0xFE; o[6] = 0x20; o[7] = 0x60; o[9] = 0x60; o[10] = 0x20; }
        if (c == 0x1E) { for (int y = 4; y < 14; y++) o[y] = 0x10; o[4] = 0x10; o[5] = 0x38; o[6] = 0x7C; }
        if (c == 0x1F) { for (int y = 3; y < 13; y++) o[y] = 0x10; o[12] = 0x10; o[11] = 0x38; o[10] = 0x7C; }
        return;
    }
    if ((c >= 0x20 && c < 0x7F) || (c >= 0xA1 && c <= 0xDF)) { host_ank(c, o); return; }
    if (c >= 0x80 && c <= 0x87) { for (int y = 16 - (c - 0x7F) * 2; y < 16; y++) o[y] = 0xFF; return; }   // 下から 1/8..8/8
    if (c >= 0x88 && c <= 0x8E) { uint8_t v = (uint8_t)(0xFF << (8 - (c - 0x87))); for (int y = 0; y < 16; y++) o[y] = v; return; }  // 左から 1/8..7/8
    auto vline = [&](int y0, int y1) { for (int y = y0; y <= y1; y++) o[y] |= 0x08; };
    switch (c) {
    case 0x8F: vline(0, 15); o[8] = 0xFF; return;                 // ┼
    case 0x90: vline(0, 7); o[8] = 0xFF; return;                  // ┴
    case 0x91: vline(9, 15); o[8] = 0xFF; return;                 // ┬
    case 0x92: vline(0, 15); o[8] = 0xF8; return;                 // ┤
    case 0x93: vline(0, 15); o[8] = 0x0F; return;                 // ├
    case 0x94: o[0] = 0xFF; return;                               // ▔
    case 0x95: o[8] = 0xFF; return;                               // ─
    case 0x96: vline(0, 15); return;                              // │
    case 0x97: for (int y = 0; y < 16; y++) o[y] = 0x01; return;  // ▕
    case 0x98: vline(9, 15); o[8] = 0x0F; return;                 // ┌
    case 0x99: vline(9, 15); o[8] = 0xF8; return;                 // ┐
    case 0x9A: vline(0, 7); o[8] = 0x0F; return;                  // └
    case 0x9B: vline(0, 7); o[8] = 0xF8; return;                  // ┘
    case 0x9C: { static const uint8_t a[8] = {0x01,0x02,0x04,0x04,0x08,0x08,0x08,0x08}; memcpy(o + 8, a, 8); return; }  // ╭
    case 0x9D: { static const uint8_t a[8] = {0xC0,0x20,0x10,0x10,0x08,0x08,0x08,0x08}; memcpy(o + 8, a, 8); return; }  // ╮
    case 0x9E: { static const uint8_t a[9] = {0x08,0x08,0x08,0x08,0x08,0x04,0x04,0x02,0x01}; memcpy(o, a, 9); return; } // ╰
    case 0x9F: { static const uint8_t a[9] = {0x08,0x08,0x08,0x08,0x08,0x10,0x10,0x20,0xC0}; memcpy(o, a, 9); return; } // ╯
    case 0xE0: o[4] = o[10] = 0xFF; return;                       // ═
    case 0xE1: vline(0, 15); o[4] = o[10] = 0x0F; return;         // ╞
    case 0xE2: vline(0, 15); o[4] = o[10] = 0xFF; return;         // ╪
    case 0xE3: vline(0, 15); o[4] = o[10] = 0xF8; return;         // ╡
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: {                  // ◢ ◣ ◥ ◤
        for (int y = 0; y < 16; y++) {
            int n = y / 2 + 1;                                     // 下へ行くほど太い
            if (c >= 0xE6) n = 8 - y / 2;                          // 上が太い
            uint8_t v = (uint8_t)(0xFF << (8 - n));                // 左寄せ
            if (c == 0xE4 || c == 0xE6) v = (uint8_t)(0xFF >> (8 - n));   // 右寄せ
            o[y] = v;
        }
        return; }
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) if (k_suit[c - 0xE8][y][x] == '#') o[4 + y] |= (uint8_t)(0x80 >> x);
        return;
    case 0xEC: case 0xED: {                                       // ● ○
        for (int y = 0; y < 16; y++) for (int x = 0; x < 8; x++) {
            double dx = x - 3.0, dy = (y - 7.5) / 1.0;
            double r2 = dx * dx + dy * dy;
            bool in = r2 <= 3.6 * 3.6 && y >= 4 && y <= 11;
            bool ring = in && r2 >= 2.4 * 2.4;
            if (c == 0xEC ? in : ring) o[y] |= (uint8_t)(0x80 >> x);
        }
        return; }
    case 0xEE: case 0xEF: case 0xF0:                              // ╱ ╲ ╳
        for (int y = 0; y < 15; y++) {
            uint8_t a = (uint8_t)(0x01 << (y / 2)), b = (uint8_t)(0x80 >> (y / 2));
            o[y] = c == 0xEE ? a : c == 0xEF ? b : (uint8_t)(a | b);
        }
        return;
    case 0xF1: narrow_from_full(0x315F, o); return;   // 円
    case 0xF2: narrow_from_full(0x472F, o); return;   // 年
    case 0xF3: narrow_from_full(0x376E, o); return;   // 月
    case 0xF4: narrow_from_full(0x467C, o); return;   // 日
    case 0xF5: narrow_from_full(0x3B7E, o); return;   // 時
    case 0xF6: narrow_from_full(0x4A2C, o); return;   // 分
    case 0xF7: narrow_from_full(0x4943, o); return;   // 秒
    case 0xFC: for (int y = 5; y <= 11; y++) o[y] = (uint8_t)(0x40 >> (y - 5)); return;   // ＼（半角）
    default: return;
    }
}

// ---- 濁点・半濁点 ----------------------------------------------------------------
static void add_dakuten(uint8_t o[16]) {
    for (int y = 0; y < 3; y++) o[y] &= 0xF8;
    o[1] |= 0x05; o[2] |= 0x05;
}
static void add_handakuten(uint8_t o[16]) {
    for (int y = 0; y < 4; y++) o[y] &= 0xF8;
    o[0] |= 0x02; o[1] |= 0x05; o[2] |= 0x02;
}

// ---- 漢字 ROM の 1 字を作る ----------------------------------------------------
static void set_half(uint8_t g[32], const uint8_t h[16]) {
    for (int y = 0; y < 16; y++) { g[y * 2] = h[y]; g[y * 2 + 1] = 0; }
}
static void make_kanji(int row, int cell, uint8_t g[32]) {
    memset(g, 0, 32);
    uint8_t h[16];
    switch (row) {
    case 0x28:     // 8 区: 罫線（JIS）
        if (cell >= 0x21 && cell <= 0x40) box_unicode(k_jis_row8[cell - 0x21], 16, g);
        return;
    case 0x29:     // 9 区: 半角 ASCII
        memcpy(h, font_get_ank((uint8_t)cell), 16); set_half(g, h);
        return;
    case 0x2A:     // 10 区: 半角カナ（濁音つきまで）
        if (cell <= 0x5F) { memcpy(h, font_get_ank((uint8_t)(0xA1 + cell - 0x21)), 16); set_half(g, h); return; }
        switch (cell) {
        case 0x60: narrow_from_full(0x2570, h); break;   // ヰ
        case 0x61: narrow_from_full(0x2571, h); break;   // ヱ
        case 0x62: narrow_from_full(0x256E, h); break;   // ヮ
        case 0x63: narrow_from_full(0x2575, h); break;   // ヵ
        case 0x64: narrow_from_full(0x2576, h); break;   // ヶ
        case 0x65: memcpy(h, font_get_ank(0xB3), 16); add_dakuten(h); break;          // ヴ
        default:
            if (cell >= 0x66 && cell <= 0x74) { memcpy(h, font_get_ank((uint8_t)(0xB6 + cell - 0x66)), 16); add_dakuten(h); }
            else {   // 0x75-0x7E: ハ行の濁音・半濁音
                int k = cell - 0x75;
                memcpy(h, font_get_ank((uint8_t)(0xCA + k / 2)), 16);
                if (k & 1) add_handakuten(h); else add_dakuten(h);
            }
        }
        set_half(g, h);
        return;
    case 0x2B:     // 11 区: 半角罫線・記号
        memset(h, 0, 16);
        if (cell == 0x22) { h[0] = 0x6C; h[1] = 0x24; h[2] = 0x48; }                  // ”（上）
        else if (cell == 0x23) { h[13] = 0x6C; h[14] = 0x24; h[15] = 0x48; }          // „（下）
        else if (cell >= 0x24 && cell <= 0x6F) { box_unicode((uint16_t)(0x2500 + cell - 0x24), 8, h); }
        else if (cell == 0x70) { h[0] = 0x60; h[1] = 0x60; h[2] = 0x20; h[3] = 0x40; }  // ‘
        else if (cell == 0x71) { h[0] = 0x6C; h[1] = 0x6C; h[2] = 0x24; h[3] = 0x48; } // “
        else if (cell == 0x72) { h[0] = 0x20; h[1] = 0x40; h[2] = 0x60; h[3] = 0x60; } // ’
        else if (cell == 0x73) { h[0] = 0x24; h[1] = 0x48; h[2] = 0x6C; h[3] = 0x6C; } // ”
        else if (cell >= 0x74 && cell <= 0x7D) {
            static const uint16_t br[10] = {0x214C, 0x214D, 0x2152, 0x2153, 0x2154, 0x2155, 0x2158, 0x2159, 0x215A, 0x215B};
            half_from_full_cut(br[cell - 0x74], ((cell - 0x74) & 1) == 0, h);
        }
        else if (cell == 0x7E) memcpy(h, font_get_ank('-'), 16);
        set_half(g, h);
        return;
    case 0x2C:     // 12 区: 全角罫線 U+2500〜254B
        if (cell >= 0x24 && cell <= 0x6F) box_unicode((uint16_t)(0x2500 + cell - 0x24), 16, g);
        return;
    case 0x2E: case 0x2F: case 0x75: case 0x78: case 0x7D: case 0x7E:
        return;    // 空き
    case 0x7C:
        if (cell == 0x6F || cell == 0x70) return;
        break;
    }
    host_kanji(jis78((uint16_t)((row << 8) | cell)), g);
}

const uint8_t* font_get_ank(uint8_t c) {
    if (!s_ank_ok[c]) { s_ank_ok[c] = 1; make_ank(c, s_ank[c]); }
    return s_ank[c];
}
const uint8_t* font_get_kanji(uint16_t jis) {
    int hi = (jis >> 8) & 0x7F, lo = jis & 0x7F;
    if (hi == 0x76 || hi == 0x77) return s_gaiji[hi - 0x76][lo];
    if (hi < 0x21 || hi > 0x7E || lo < 0x21 || lo > 0x7E) return s_blank;
    int idx = (hi - 0x21) * 94 + (lo - 0x21);
    if (!s_kanji_ok[idx]) { s_kanji_ok[idx] = 1; make_kanji(hi, lo, s_kanji[idx]); }
    return s_kanji[idx];
}

// ---- 外字 -----------------------------------------------------------------------
bool fontrom_is_gaiji(uint16_t jis) { int hi = (jis >> 8) & 0x7F; return hi == 0x76 || hi == 0x77; }
void fontrom_gaiji_write(uint16_t jis, int line, bool left, uint8_t v) {
    int hi = (jis >> 8) & 0x7F, lo = jis & 0x7F;
    if (hi != 0x76 && hi != 0x77) return;
    s_gaiji[hi - 0x76][lo][(line & 15) * 2 + (left ? 0 : 1)] = v;
}
void fontrom_reset_gaiji() { memset(s_gaiji, 0, sizeof(s_gaiji)); }
void fontrom_state_save(StateW& w) { w.tag("GAI2"); w.bytes(s_gaiji, sizeof(s_gaiji)); }
void fontrom_state_load(StateR& r) {
    if (r.peek_tag("GAIJ")) {           // 旧形式（21h-7Eh の 94 字ずつ）
        r.tag("GAIJ");
        static uint8_t old[2][94][32];
        r.bytes(old, sizeof(old));
        memset(s_gaiji, 0, sizeof(s_gaiji));
        for (int h = 0; h < 2; h++) for (int i = 0; i < 94; i++) memcpy(s_gaiji[h][i + 0x21], old[h][i], 32);
        return;
    }
    r.tag("GAI2"); r.bytes(s_gaiji, sizeof(s_gaiji));
}

// ---- 確認用の一覧画像 -------------------------------------------------------------
//  上: ANK 256 字（2 倍幅）  下: 8〜13 区と 76 区（外字）
void fontrom_make_sheet(std::vector<uint32_t>& px, int* w, int* h) {
    const int W = 640, H = 16 * 20 + 7 * 3 * 20 + 8;
    *w = W; *h = H;
    px.assign((size_t)W * H, 0xFF202028u);
    auto put = [&](int x, int y, bool on) { if (x >= 0 && y >= 0 && x < W && y < H) px[(size_t)y * W + x] = on ? 0xFFFFFFFFu : 0xFF000000u; };
    for (int c = 0; c < 256; c++) {
        const uint8_t* g = font_get_ank((uint8_t)c);
        int ox = (c % 16) * 40, oy = (c / 16) * 20;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 8; x++) { bool on = (g[y] & (0x80 >> x)) != 0; put(ox + x * 2, oy + y, on); put(ox + x * 2 + 1, oy + y, on); }
    }
    static const int rows[7] = {0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x76};
    for (int ri = 0; ri < 7; ri++) for (int cell = 0x21; cell < 0x7F; cell++) {
        const uint8_t* g = font_get_kanji((uint16_t)((rows[ri] << 8) | cell));
        int i = cell - 0x21;
        int ox = (i % 32) * 20, oy = 16 * 20 + 8 + ri * 60 + (i / 32) * 20;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) put(ox + x, oy + y, (((g[y * 2] << 8) | g[y * 2 + 1]) & (0x8000 >> x)) != 0);
    }
}
