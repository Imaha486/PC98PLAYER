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
//  ・表示はグラフィック GDC の SCROLL（開始アドレスと行数）と PITCH にしたがう。1 行は PITCH×8 バイト。
//  資料: SL9821 の技術的なはなし（PEGC）、Neko Project 21/W の動作（コードは使っていない）
// -----------------------------------------------------------------------------
#include "machine.h"
#include <string.h>

static const uint32_t VRAM_SIZE = 0x80000;

void pegc_clear(Machine* m) {
    memset(&m->pegc, 0, sizeof(m->pegc));
    m->pegc_vram.assign(VRAM_SIZE, 0);
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
            if (!m->modeff2[3]) return true;   // モード変更の許可（07h）が要る
            if (p.on != (v & 1)) plog("[pegc] %s\n", (v & 1) ? "256 色モードにしました" : "16 色モードに戻しました");
            p.on = v & 1;
            return true;
        }
        if (v == 0x68 || v == 0x69) { p.single = v & 1; return true; }
        return false;
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
    case 0x9A8: *v = p.crt31; return true;
    case 0xA8: if (!p.on) return false; *v = p.palidx; return true;
    case 0xAA: if (!p.on) return false; *v = p.pal[p.palidx][0]; return true;
    case 0xAC: if (!p.on) return false; *v = p.pal[p.palidx][1]; return true;
    case 0xAE: if (!p.on) return false; *v = p.pal[p.palidx][2]; return true;
    }
    return false;
}

static void plane_warn(Machine* m) {
    static int n = 0;
    if (n++ < 4) plog("[pegc] プレーンモード（E0100h bit0=1）の描画にはまだ対応していません\n");
    (void)m;
}

bool pegc_mem_rb(Machine* m, uint32_t a, uint8_t* v) {
    Pegc& p = m->pegc;
    if (!p.on) return false;
    if (a >= 0xA8000 && a < 0xB8000) {
        if (p.mmio[0x100] & 1) { plane_warn(m); *v = 0xFF; return true; }
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
        if (p.mmio[0x100] & 1) { plane_warn(m); return true; }
        int w = a >= 0xB0000;
        m->pegc_vram[(bank_base(m, w) + (a & 0x7FFF)) & (VRAM_SIZE - 1)] = v;
        return true;
    }
    if (a >= 0xB8000 && a < 0xC0000) return true;
    if (a >= 0xE0000 && a < 0xE8000) {
        uint32_t o = a - 0xE0000;
        if (o < sizeof(p.mmio)) {
            if (o == 0x100 && ((p.mmio[o] ^ v) & 1)) plog("[pegc] 描き方: %s\n", (v & 1) ? "プレーン" : "パックドピクセル");
            if (o == 0x102 && ((p.mmio[o] ^ v) & 1)) plog("[pegc] 上位メモリの VRAM（F00000h）: %s（未対応）\n", (v & 1) ? "有効" : "無効");
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
