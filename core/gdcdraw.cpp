// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  gdcdraw.cpp  --  グラフィック GDC（μPD7220）の描画機能
//
//  VECTW で与えた図形パラメータと、VECTE（直線・矩形・円弧）/ TEXTE（グラフィック
//  キャラクタ = 8x8 パターンの敷き詰め）で VRAM に点を打つ。WDAT の直接書き込みも扱う。
//  μPD7220 の公開資料にもとづいて書いたもの。
//
//  ・アドレス: EAD はワード単位。PC-98 では bit14-15 がプレーン（0=B 1=R 2=G 3=E）、
//    dAD（0..15）はワード内の点の位置で 0 が左端
//  ・方向 DIR: 0=下 1=右下 2=右 3=右上 4=上 5=左上 6=左 7=左下（反時計回り）
//  ・描き方: WDAT コマンドの下位 2bit（0=置換 1=反転 2=消去 3=セット）
//  ・GRCG が有効なら、点を立てる書き込みは GRCG のタイルとして全プレーンへ入る
// -----------------------------------------------------------------------------
#include "machine.h"
#include <math.h>

namespace {

const int kDX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
const int kDY[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };

struct Pen {
    Machine* m;
    Gdc*     g;
    int      plane;       // EAD の bit14-15
    int32_t  x, y;        // ドット単位（x は 0..pitch*16-1、y は行）
    int      pitch;       // 1 行のワード数
    uint8_t  mode;        // 0=置換 1=反転 2=消去 3=セット
};

inline uint8_t* plane_byte(Machine* m, int p, uint32_t off) {
    return &m->gvram[m->draw_bank][p][off & 0x7FFF];
}

// 1 点の操作。bit=パターンの値
void plot(Pen& pn, int bit) {
    Machine* m = pn.m;
    int32_t w = pn.pitch * 16;
    int32_t x = ((pn.x % w) + w) % w;
    int32_t word = pn.y * pn.pitch + x / 16;
    int dot = x & 15;
    uint32_t off = (uint32_t)(word * 2 + (dot >> 3));
    uint8_t bm = (uint8_t)(0x80 >> (dot & 7));
    // この点を 1 にするか 0 にするか（変えないなら -1）
    int want;
    switch (pn.mode & 3) {
    case 0: want = bit ? 1 : 0; break;          // 置換
    case 1: want = bit ? 2 : -1; break;         // 反転（2 = 反転の印）
    case 2: want = bit ? 0 : -1; break;         // 消去
    default: want = bit ? 1 : -1; break;        // セット
    }
    if (want < 0) return;
    if (m->grcg_mode & 0x80) {
        // GRCG 経由: 書き込んだ点にタイルの色が入る（消去・0 の置換は何もしない）
        if (want == 0) return;
        bool rmw = (m->grcg_mode & 0x40) != 0;
        for (int p = 0; p < 4; p++) {
            if (m->grcg_mode & (1 << p)) continue;
            uint8_t* b = plane_byte(m, p, off);
            if (rmw) *b = (uint8_t)((*b & ~bm) | (m->grcg_tile[p] & bm));
            else *b = m->grcg_tile[p];
        }
        return;
    }
    uint8_t* b = plane_byte(m, pn.plane, off);
    if (want == 2) *b ^= bm;
    else if (want == 1) *b |= bm;
    else *b &= (uint8_t)~bm;
}

inline void step(Pen& pn, int dir) { pn.x += kDX[dir & 7]; pn.y += kDY[dir & 7]; }

// VECTW のパラメータ（14bit 符号つき）
int p14(const uint8_t* v, int i) {
    int r = v[i] | ((v[i + 1] & 0x3F) << 8);
    if (r & 0x2000) r -= 0x4000;
    return r;
}

Pen make_pen(Machine* m, Gdc* g) {
    Pen pn;
    pn.m = m; pn.g = g;
    pn.pitch = g->pitch ? g->pitch : 40;
    if (g == &m->gdcs && m->gdc_clk5) pn.pitch = (pn.pitch + 1) / 2;   // 5MHz: PITCH はバイト単位 → ワードに直す
    if (pn.pitch > 80) pn.pitch = 80;
    uint32_t ead = g->ead;
    pn.plane = (int)((ead >> 14) & 3);
    uint32_t word = ead & 0x3FFF;
    pn.y = (int32_t)(word / (uint32_t)pn.pitch);
    pn.x = (int32_t)((word % (uint32_t)pn.pitch) * 16 + (g->dad & 15));
    pn.mode = g->mode_write & 3;
    return pn;
}
void store_pen(const Pen& pn, Gdc* g) {
    int32_t w = pn.pitch * 16;
    int32_t x = ((pn.x % w) + w) % w;
    int32_t y = pn.y;
    uint32_t word = (uint32_t)((y * pn.pitch + x / 16) & 0x3FFF);
    g->ead = (g->ead & ~0x3FFFu) | word;
    g->dad = (uint8_t)(x & 15);
}

uint16_t line_pattern(Gdc* g) { return (uint16_t)(g->pram[8] | (g->pram[9] << 8)); }

void draw_line(Pen& pn, int dir, int dc, int d, int d2, int d1) {
    uint16_t pat = line_pattern(pn.g);
    bool diag_first = (dir & 1) != 0;    // 奇数方向は dir 自体が斜め
    int err = d;
    for (int i = 0; i <= dc; i++) {
        plot(pn, (pat >> (i & 15)) & 1);
        if (i == dc) break;
        bool take_diag = err >= 0;
        if (take_diag) err += d2; else err += d1;
        // 斜めに進むのは dir と dir+1 のうち斜めの方
        int diag = diag_first ? dir : dir + 1;
        int axis = diag_first ? dir + 1 : dir;
        step(pn, take_diag ? diag : axis);
    }
}

void draw_rect(Pen& pn, int dir, int d, int d2) {
    uint16_t pat = line_pattern(pn.g);
    int k = 0;
    int len[4] = { d, d2, d, d2 };
    for (int side = 0; side < 4; side++) {
        int dd = (dir + side * 2) & 7;
        for (int i = 0; i < len[side]; i++) { plot(pn, (pat >> (k++ & 15)) & 1); step(pn, dd); }
    }
}

// 円弧（1/8 円ぶん）。r = D+1。接線方向 dir に進みながら中心側（dir+2）へ寄る
void draw_arc(Pen& pn, int dir, int dc, int d, int dm) {
    uint16_t pat = line_pattern(pn.g);
    int r = d + 1;
    int prev_off = 0;
    int32_t sx = pn.x, sy = pn.y;
    int side = (dir + 2) & 7;
    for (int t = 0; t <= dc; t++) {
        double rr = (double)r * r - (double)t * t;
        int off = rr > 0 ? (int)(r - sqrt(rr) + 0.5) : r;
        pn.x = sx + kDX[dir & 7] * t + kDX[side] * off;
        pn.y = sy + kDY[dir & 7] * t + kDY[side] * off;
        if (t >= dm) plot(pn, (pat >> (t & 15)) & 1);
        prev_off = off;
    }
    (void)prev_off;
}

// グラフィックキャラクタ: PRAM 8..15 の 8x8 パターンを、dir の向きに D+1 点 x (DC+1) 行並べる
void draw_gchar(Pen& pn, int dir, int dc, int d) {
    Gdc* g = pn.g;
    int zoom = (g->zoom & 0x0F) + 1;              // 描画の拡大率（ZOOM の下位 4bit）
    int cols = d + 1, rows = dc + 1;
    int32_t sx = pn.x, sy = pn.y;
    int across = (dir + 2) & 7;
    for (int r = 0; r < rows; r++) {
        uint8_t line = g->pram[8 + 7 - ((r / zoom) & 7)];
        for (int c = 0; c < cols; c++) {
            pn.x = sx + kDX[dir & 7] * c + kDX[across] * r;
            pn.y = sy + kDY[dir & 7] * c + kDY[across] * r;
            plot(pn, (line >> ((c / zoom) & 7)) & 1);
        }
    }
}

} // namespace

// VECTE（0x6C）/ TEXTE（0x68）/ WDAT（0x20-0x3F）
void gdc_draw_command(Machine* m, Gdc* g) {
    if (g != &m->gdcs) return;                     // テキスト側は描画しない
    uint8_t cmd = g->cmd;
    Pen pn = make_pen(m, g);
    if ((cmd & 0xE0) == 0x20) {
        // WDAT: パラメータの語（または下位/上位バイト）を 1 語ぶんのパターンとして置く
        int type = (cmd >> 3) & 3;                 // 0=語 2=下位バイト 3=上位バイト
        uint16_t data = type == 0 ? (uint16_t)(g->params[0] | (g->params[1] << 8))
                      : type == 2 ? g->params[0] : (uint16_t)(g->params[0] << 8);
        int dir = g->vect[0] & 7;
        for (int i = 0; i < 16; i++) {
            Pen q = pn;
            q.x = (pn.x & ~15) + i;
            if ((type == 2 && i >= 8) || (type == 3 && i < 8)) continue;
            plot(q, (data >> i) & 1);
        }
        // 1 語ぶん進める
        if ((dir & 7) == 2) pn.x += 16; else step(pn, dir);
        store_pen(pn, g);
        g->pcount = 0;                             // 続けて次の語を受け取る
        return;
    }
    const uint8_t* v = g->vect;
    int figs = v[0];
    int dir = figs & 7;
    int dc = p14(v, 1) & 0x3FFF;
    int d = p14(v, 3), d2 = p14(v, 5), d1 = p14(v, 7), dm = p14(v, 9);
    if (m->cfg.trace) plog("[gdc] %s figs=%02X dir=%d dc=%d d=%d d2=%d d1=%d dm=%d ead=%05X dad=%d mode=%d\n",
                           cmd == 0x68 ? "TEXTE" : "VECTE", figs, dir, dc, d, d2, d1, dm, g->ead, g->dad, pn.mode);
    if (cmd == 0x68 || (figs & 0x10)) {            // グラフィックキャラクタ（TEXTE、または GC 指定）
        draw_gchar(pn, dir, dc, d);
    } else if (figs & 0x40) {                      // 矩形
        draw_rect(pn, dir, d, d2);
    } else if (figs & 0x20) {                      // 円弧
        draw_arc(pn, dir, dc, d, dm);
    } else if (figs & 0x08) {                      // 直線
        draw_line(pn, dir, dc, d, d2, d1);
    } else {                                       // 点
        plot(pn, line_pattern(g) & 1);
    }
    store_pen(pn, g);
}
