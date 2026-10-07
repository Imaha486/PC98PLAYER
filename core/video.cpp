// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  video.cpp  --  画面合成（グラフィック 4 プレーン + テキスト VRAM）とフォント
//
//  字形は fontrom.cpp（擬似漢字 ROM）から引く。
// -----------------------------------------------------------------------------
#include "machine.h"
#include <string.h>

static inline uint32_t rgb4(uint8_t g, uint8_t r, uint8_t b) {
    return 0xFF000000u | ((uint32_t)(r * 17) << 16) | ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
}

static const uint8_t s_blank16[16] = {0};

void video_render(Machine* m, uint32_t* out) {
    // パレット
    uint32_t pal[16];
    if (m->analog) {
        for (int i = 0; i < 16; i++) pal[i] = rgb4(m->pal[i][0], m->pal[i][1], m->pal[i][2]);
    } else {
        static const int lo_idx[4] = {3, 1, 2, 0}, hi_idx[4] = {7, 5, 6, 4};
        uint8_t dp[8];
        for (int k = 0; k < 4; k++) { dp[lo_idx[k]] = (m->degpal[k] >> 4) & 7; dp[hi_idx[k]] = m->degpal[k] & 7; }   // 上位 4bit が #0-3、下位が #4-7
        for (int i = 0; i < 16; i++) {
            uint8_t v = dp[i & 7];
            pal[i] = rgb4((v & 4) ? 15 : 0, (v & 2) ? 15 : 0, (v & 1) ? 15 : 0);
        }
    }

    // グラフィック
    bool gon = m->gdcs.display != 0;
    const uint8_t (*pl)[0x8000] = m->gvram[m->disp_bank];
    uint32_t sad1 = (uint32_t)(m->gdcs.pram[0] | (m->gdcs.pram[1] << 8) | ((m->gdcs.pram[2] & 3) << 16));
    uint32_t len1 = (uint32_t)((m->gdcs.pram[2] >> 4) | ((m->gdcs.pram[3] & 0x3F) << 4));
    uint32_t sad2 = (uint32_t)(m->gdcs.pram[4] | (m->gdcs.pram[5] << 8) | ((m->gdcs.pram[6] & 3) << 16));
    int zoom = ((m->gdcs.zoom >> 4) & 15) + 1;   // ZOOM の上位 4bit が表示の倍率（下位は描画用）
    // 1 本の VRAM の行を何本の走査線で見せるか: GDC の CSRFORM の「1 行の走査線数」（200 ライン表示は 2）、
    // ZOOM、ポート 68h の 200 ライン指定のうち大きいもの
    int rep = (m->gdcs.csrform[0] & 0x1F) + 1;
    if (rep > 4) rep = 1;                       // グラフィックでは 1〜2 のはず。変な値は無視
    if (zoom > rep) rep = zoom;
    if (m->gfx_200 && rep < 2) rep = 2;
    // 1 行の大きさ: GDC の PITCH（ワード数）。横に広い仮想画面を作って SAD で横スクロールするゲームがある
    //（ヴァリアブル・ジオ 2 の対戦画面は 64 ワード = 1024 ドット幅）。表示するのは先頭の 40 ワード（640 ドット）
    uint32_t stride = (uint32_t)(m->gdcs.pitch >= 40 ? m->gdcs.pitch : 40) * 2;
    // LEN は走査線の本数（400 ライン基準）
    if (len1 == 0 || len1 > 400) len1 = 400;
    for (int y = 0; y < 400; y++) {
        uint32_t* o = out + y * 640;
        if (!gon) { for (int x = 0; x < 640; x++) o[x] = 0xFF000000u; continue; }
        uint32_t addr = (uint32_t)y < len1 ? (sad1 * 2 + (uint32_t)(y / rep) * stride)
                                            : (sad2 * 2 + (uint32_t)((y - (int)len1) / rep) * stride);
        for (int bx = 0; bx < 80; bx++) {
            uint32_t a = (addr + bx) & 0x7FFF;
            uint8_t b = pl[0][a], r = pl[1][a], g = pl[2][a], e = m->analog ? pl[3][a] : 0;
            for (int k = 0; k < 8; k++) {
                int sh = 7 - k;
                int ci = ((b >> sh) & 1) | (((r >> sh) & 1) << 1) | (((g >> sh) & 1) << 2) | (((e >> sh) & 1) << 3);
                o[bx * 8 + k] = pal[ci];
            }
        }
    }

    // テキスト
    if (!m->gdcm.display) return;
    int rowh = (m->gdcm.csrform[0] & 0x1F) + 1;
    if (rowh < 8 || rowh > 20) rowh = 16;
    int rows = 400 / rowh;
    bool blink_phase = (m->frame_count / 30) & 1;
    uint32_t tsad = (uint32_t)(m->gdcm.pram[0] | (m->gdcm.pram[1] << 8));
    for (int r = 0; r < rows; r++) {
        // 全角の右半分は「直前のセルが全角の左半分だった」ことで決まる（セルの中身は見ない）。
        // 外字（76・77 区）は同じコードが続くと左→右と交互になる。
        bool kanji2nd = false;
        uint16_t lastjis = 0;
        bool gaiji_right = false;
        uint16_t lastgaiji = 0xFFFF;
        uint8_t last_at = 0;
        for (int c = 0; c < 80; c++) {
            uint32_t cell = (tsad + (uint32_t)r * 80 + (uint32_t)c) & 0xFFF;
            uint8_t lo = m->tvram[cell * 2], hi = m->tvram[cell * 2 + 1];
            uint8_t at = m->tvram[0x2000 + cell * 2];
            uint8_t g2[16];
            const uint8_t* glyph = s_blank16;
            bool semigraph = false;
            if (kanji2nd) {
                kanji2nd = false;
                const uint8_t* k = font_get_kanji(lastjis);
                for (int y = 0; y < 16; y++) g2[y] = k[y * 2 + 1];
                glyph = g2;
                at = (uint8_t)((at & 0xEF) | (last_at & 0x10));
                gaiji_right = false; lastgaiji = 0xFFFF;
            } else if ((at & 0x10) && m->modeff[0]) {
                semigraph = true;
                gaiji_right = false; lastgaiji = 0xFFFF;
            } else if (hi == 0) {
                glyph = font_get_ank(lo);
                gaiji_right = false; lastgaiji = 0xFFFF;
            } else {
                uint16_t jis = (uint16_t)((((lo & 0x7F) + 0x20) << 8) | (hi & 0x7F));
                int row = lo & 0x7F;
                const uint8_t* k = font_get_kanji(jis);
                bool right = false;
                if (row == 0x56 || row == 0x57) {
                    if (gaiji_right && jis == lastgaiji) right = true;
                    gaiji_right = !right; lastgaiji = jis;
                } else {
                    gaiji_right = false; lastgaiji = 0xFFFF;
                    if (row < 0x09 || row > 0x0B) { kanji2nd = true; lastjis = jis; }
                }
                for (int y = 0; y < 16; y++) g2[y] = k[y * 2 + (right ? 1 : 0)];
                glyph = g2;
            }
            last_at = at;
            bool rev = (at & 4) != 0;
            bool ul = (at & 8) != 0;
            bool vl = (at & 0x10) != 0 && !m->modeff[0];
            bool visible = (at & 1) != 0;
            if ((at & 2) && blink_phase) visible = false;
            if (!visible && !rev) continue;
            uint32_t col = 0xFF000000u | ((at & 0x40) ? 0xFF0000u : 0) | ((at & 0x80) ? 0x00FF00u : 0) | ((at & 0x20) ? 0x0000FFu : 0);
            int y0 = r * rowh;
            for (int y = 0; y < rowh && y0 + y < 400; y++) {
                uint8_t bits = 0;
                if (visible) {
                    if (semigraph) {
                        int by = y / (rowh / 4 ? rowh / 4 : 4); if (by > 3) by = 3;
                        bits = (uint8_t)(((lo >> by) & 1 ? 0xF0 : 0) | ((lo >> (4 + by)) & 1 ? 0x0F : 0));
                    } else if (y < 16) bits = glyph[y];
                    if (ul && y == 15) bits = 0xFF;
                    if (vl) bits |= 0x80;
                }
                if (rev) bits = (uint8_t)~bits;
                uint32_t* o = out + (y0 + y) * 640 + c * 8;
                for (int k = 0; k < 8; k++) if (bits & (0x80 >> k)) o[k] = col;
            }
        }
    }
    // カーソル
    if ((m->gdcm.csrform[0] & 0x80) && ((m->frame_count / 16) & 1)) {
        uint32_t ead = m->gdcm.ead & 0xFFF;
        int r = (int)(ead / 80), c = (int)(ead % 80);
        if (r < rows) {
            int top = m->gdcm.csrform[1] & 0x1F, bot = (m->gdcm.csrform[2] >> 3) & 0x1F;
            if (bot < top || bot >= rowh) { top = 0; bot = rowh - 1; }
            for (int y = top; y <= bot; y++) {
                if (r * rowh + y >= 400) break;
                uint32_t* o = out + (r * rowh + y) * 640 + c * 8;
                for (int k = 0; k < 8; k++) o[k] ^= 0x00FFFFFFu;
            }
        }
    }
}
