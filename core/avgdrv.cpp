// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  avgdrv.cpp  --  内蔵の AVGDRV.SYS（PC-9821 の 256 色グラフィックドライバ、INT D8h）
//
//  AVGDRV.SYS は MS-DOS（PC-9821 用）に付いてくるドライバで、装置 AVGDRV$$ と INT D8h を持つ。
// -----------------------------------------------------------------------------
#include "machine.h"
#include <string.h>
#include <stdlib.h>

#define ROMSEG 0xF000
uint16_t bios_rom_alloc(Machine* m, int n);
uint16_t bios_hle_stub(Machine* m, uint8_t n);
static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }

void avgdrv_install(Machine* m) {
    if (m->avg.on) return;
    memset(&m->avg, 0, sizeof(m->avg));
    uint16_t o = bios_hle_stub(m, HLE_AVGDRV);
    m->ram[0xD8 * 4 + 0] = (uint8_t)o; m->ram[0xD8 * 4 + 1] = (uint8_t)(o >> 8);
    m->ram[0xD8 * 4 + 2] = (uint8_t)ROMSEG; m->ram[0xD8 * 4 + 3] = (uint8_t)(ROMSEG >> 8);
    uint16_t h = bios_rom_alloc(m, 0x20);
    uint32_t b = lin(ROMSEG, h);
    uint32_t nul = dos_nul_addr();
    memcpy(m->ram + b, m->ram + nul, 4);
    m->ram[b + 4] = 0x00; m->ram[b + 5] = 0x80;
    m->ram[b + 6] = (uint8_t)(h + 0x12); m->ram[b + 7] = (uint8_t)((h + 0x12) >> 8);
    m->ram[b + 8] = (uint8_t)(h + 0x12); m->ram[b + 9] = (uint8_t)((h + 0x12) >> 8);
    memcpy(m->ram + b + 0x0A, "AVGDRV$$", 8);
    m->ram[b + 0x12] = 0xCB;
    m->ram[nul + 0] = (uint8_t)h; m->ram[nul + 1] = (uint8_t)(h >> 8);
    m->ram[nul + 2] = (uint8_t)ROMSEG; m->ram[nul + 3] = (uint8_t)(ROMSEG >> 8);
    m->avg.on = 1;
    plog("[avg] 内蔵の AVGDRV.SYS（INT D8h）を用意しました\n");
}

void avgdrv_hle(Machine* m) {
    Cpu* c = &m->cpu;
    uint16_t ax = (uint16_t)c->r[EAX];
    static int seen[256];
    uint8_t ah = (uint8_t)(ax >> 8);
    if (m->cfg.trace && seen[ah] < (getenv("AVGLOG") ? 100000 : 20)) { seen[ah]++; plog("[avg] INT D8h AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X DS=%04X ES=%04X @%04X:%04X\n", ax,
        (unsigned)(uint16_t)c->r[EBX], (unsigned)(uint16_t)c->r[ECX], (unsigned)(uint16_t)c->r[EDX], (unsigned)(uint16_t)c->r[ESI], (unsigned)(uint16_t)c->r[EDI],
        c->sr[DS_], c->sr[ES_], mem_rw(m, lin(c->sr[SS_], (uint16_t)c->r[ESP] + 2)), mem_rw(m, lin(c->sr[SS_], (uint16_t)c->r[ESP]))); }
    Pegc& pg = m->pegc;
    uint32_t esbx = lin(c->sr[ES_], (uint16_t)c->r[EBX]);
    uint8_t bh = (uint8_t)(c->r[EBX] >> 8), bl = (uint8_t)c->r[EBX];
    switch (ah) {
    case 0x00:   // 画面モード（BH=21h: 256 色。BL bit0: 0 = 2 画面（256KB ずつ）、1 = 1 画面（512KB を続けて使う））
        if (bh == 0x21) {
            m->modeff2[3] = 1;
            pg.on = 1; pg.single = bl & 1;
            m->gdcs.display = 1;
            // グラフィック GDC を 1 画面ぶん（640 バイト x 400/480 行）に合わせる
            {
                uint16_t lines = pg.crt31 ? 480 : 400;
                uint16_t lw = (uint16_t)(lines << 4);
                memset(m->gdcs.pram, 0, 8);
                m->gdcs.pram[2] = (uint8_t)lw; m->gdcs.pram[3] = (uint8_t)(lw >> 8);
                m->gdcs.pitch = m->gdc_clk5 ? 80 : 40;
            }
            plog("[avg] 256 色・%s にしました\n", pg.single ? "1 画面" : "2 画面");
        } else if (bh == 0x20 || bh == 0x00) { pg.on = 0; pg.single = 0; }
        break;
    case 0x0A: {  // パレット。1 色は青・赤・緑の順（8 ビット）。BMP の BGR をこの順に並べ替えて渡すソフトがある
        //  AL bit7=1: ES:BX の表から。CX=0 なら 256 色ぶん、そうでなければ CH 番から CL 番まで
        //  AL bit7=0: BH 番の 1 色を BL（青）・CH（赤）・CL（緑）に
        uint8_t al = (uint8_t)ax;
        uint16_t cx = (uint16_t)c->r[ECX];
        if (al & 0x80) {
            int first = cx ? (cx >> 8) : 0, last = cx ? (cx & 0xFF) : 255;
            for (int i = first; i <= last; i++) {
                uint32_t e = esbx + (uint32_t)(i - first) * 3;
                pg.pal[i][0] = mem_rb(m, e + 2); pg.pal[i][1] = mem_rb(m, e + 1); pg.pal[i][2] = mem_rb(m, e);
            }
        } else {
            pg.pal[bh][0] = (uint8_t)cx; pg.pal[bh][1] = (uint8_t)(cx >> 8); pg.pal[bh][2] = bl;
        }
        break; }
    }
    c->r[EAX] &= 0xFFFF0000u;
    set_cf(m, false);
}
