// SPDX-License-Identifier: MIT
//  memio.h -- CPU から見たメモリ・I/O の入口（Machine 側が実装する）
#pragma once
#include <stdint.h>

struct Machine;

// 物理メモリ（1MB + HMA 64KB）。0xA0000 未満はただの RAM なので CPU が直接触る。
// その上に拡張メモリ（1MB より上。保護モードのソフトが使う）が続く（全体の大きさは g_ram_top）
#define PC98_RAM_SIZE 0x110000u
#define A20_ON_MASK   0x00FFFFFFu     // A20 を開けたとき（PC-9801 の 386SX 機と同じ 16MB の空間）

extern uint8_t* g_ram;            // 高速パス用（Machine が設定）
extern uint32_t g_ram_top;        // g_ram の大きさ（PC98_RAM_SIZE 以上。拡張メモリの終わり）
extern uint32_t g_addr_mask;      // A20 マスク（0xFFFFF または A20_ON_MASK）
// 「外に影響する（または外から値が変わりうる）アクセス」の回数。メモリへの書き込み・I/O・HLE トラップ・
// VRAM/ROM など RAM 以外の読み出しで増える。CPU の空回り（割込み待ちのループ）を見分けるのに使う
extern uint32_t g_side_fx;
// この番地を CPU が読んだら mem_probe を呼ぶ（BIOS の 045Ch: PC-9821 かどうかを調べるソフトを見つける）。使わないときは 0xFFFFFFFF
extern uint32_t g_probe_addr;
void     mem_probe(Machine* m, uint32_t a);

uint8_t  mem_rb_slow(Machine* m, uint32_t a);
void     mem_wb_slow(Machine* m, uint32_t a, uint8_t v);
uint16_t mem_rw_slow(Machine* m, uint32_t a);          // a は未マスク
void     mem_ww_slow(Machine* m, uint32_t a, uint16_t v);
uint8_t  io_in8(Machine* m, uint16_t port);
void     io_out8(Machine* m, uint16_t port, uint8_t v);
uint16_t io_in16(Machine* m, uint16_t port);
void     io_out16(Machine* m, uint16_t port, uint16_t v);
// F1 xx の HLE トラップ
void     hle_trap(Machine* m, uint8_t n);
// 受け付け可能な割込みがあればベクタ番号、無ければ -1（受け付けたら ISR に立つ）
int      pic_acknowledge(Machine* m);

// A0000h〜FFFFFh を 16KB ごとに見た近道。中身をそのまま読み書きしてよい所（EMS のページフレーム、
// 256 色のパックドピクセルの VRAM の窓、ROM）は、ここに PC 側の先頭を置く（それ以外は nullptr で遅い道へ）。
// 割り当てが変わりうる所（I/O・メモリマップドレジスタへの書き込み・HLE・CPU の実行の区切り）で mem_fast_rebuild が作り直す
extern uint8_t* g_up_rd[24];
extern uint8_t* g_up_wr[24];
void     mem_fast_rebuild(Machine* m);
static inline uint8_t* up_rd(uint32_t a) { return a - 0xA0000u < 0x60000u ? g_up_rd[(a - 0xA0000u) >> 14] : nullptr; }
static inline uint8_t* up_wr(uint32_t a) { return a - 0xA0000u < 0x60000u ? g_up_wr[(a - 0xA0000u) >> 14] : nullptr; }

static inline uint32_t lin_mask(uint32_t a) {
    a &= g_addr_mask;
    return a;
}
static inline bool ext_ram(uint32_t a) { return a >= 0x100000u && a < g_ram_top; }
static inline uint8_t mem_rb(Machine* m, uint32_t a) {
    a = lin_mask(a);
    if (a < 0xA0000u || ext_ram(a)) { if (a == g_probe_addr) mem_probe(m, a); return g_ram[a]; }
    if (uint8_t* p = up_rd(a)) { g_side_fx++; return p[a & 0x3FFF]; }
    return mem_rb_slow(m, a);
}
#ifdef PC98P_WATCH
extern uint32_t g_watch_lo, g_watch_hi;
void watch_hit(Machine* m, uint32_t a, uint32_t v, int w);
#define WATCHCHK(a, v, w) do { if ((a) + (w) > g_watch_lo && (a) < g_watch_hi) watch_hit(m, (a), (v), (w)); } while (0)
#else
#define WATCHCHK(a, v, w) do {} while (0)
#endif
static inline void mem_wb(Machine* m, uint32_t a, uint8_t v) {
    a = lin_mask(a);
    WATCHCHK(a, v, 1);
    if (a < 0xA0000u || ext_ram(a)) { if (g_ram[a] != v) { g_ram[a] = v; g_side_fx++; } return; }   // 同じ値の書き込みは影響なし
    g_side_fx++;
    if (uint8_t* p = up_wr(a)) { p[a & 0x3FFF] = v; return; }
    mem_wb_slow(m, a, v);
}
static inline uint16_t mem_rw(Machine* m, uint32_t a) {
    uint32_t b = lin_mask(a);
    if (b < 0x9FFFFu || (b >= 0x100000u && b + 1 < g_ram_top)) {
        if (b - g_probe_addr + 1 <= 1u) mem_probe(m, g_probe_addr);   // b か b+1 が調べる番地
        return (uint16_t)(g_ram[b] | (g_ram[b + 1] << 8));
    }
    if ((b & 0x3FFF) != 0x3FFF) if (uint8_t* p = up_rd(b)) { g_side_fx++; p += b & 0x3FFF; return (uint16_t)(p[0] | (p[1] << 8)); }
    return mem_rw_slow(m, a);
}
static inline void mem_ww(Machine* m, uint32_t a, uint16_t v) {
    uint32_t b = lin_mask(a);
    WATCHCHK(b, v, 2);
    if (b < 0x9FFFFu || (b >= 0x100000u && b + 1 < g_ram_top)) {
        if (g_ram[b] != (uint8_t)v || g_ram[b + 1] != (uint8_t)(v >> 8)) { g_ram[b] = (uint8_t)v; g_ram[b + 1] = (uint8_t)(v >> 8); g_side_fx++; }
        return;
    }
    g_side_fx++;
    if ((b & 0x3FFF) != 0x3FFF) if (uint8_t* p = up_wr(b)) { p += b & 0x3FFF; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return; }
    mem_ww_slow(m, a, v);
}
// まとめて読み書き（REP MOVS / STOS）してよい所なら、PC 側の場所と、そこから続けて使えるバイト数を返す。
// ふつうの RAM・拡張メモリと、上の近道（g_up_rd / g_up_wr）の 16KB の中だけ。それ以外は nullptr
static inline uint8_t* mem_span(uint32_t a, uint32_t* avail, bool wr) {
    a = lin_mask(a);
    if (a < 0xA0000u) { *avail = 0xA0000u - a; return g_ram + a; }
    if (ext_ram(a)) { uint32_t e = g_ram_top; if (g_addr_mask + 1u != 0 && g_addr_mask + 1u < e) e = g_addr_mask + 1u; *avail = e - a; return g_ram + a; }
    uint8_t* p = wr ? up_wr(a) : up_rd(a);
    if (!p) return nullptr;
    *avail = 0x4000u - (a & 0x3FFFu);
    return p + (a & 0x3FFFu);
}
static inline uint32_t mem_rd(Machine* m, uint32_t a) {
    return (uint32_t)mem_rw(m, a) | ((uint32_t)mem_rw(m, a + 2) << 16);
}
static inline void mem_wd(Machine* m, uint32_t a, uint32_t v) {
    mem_ww(m, a, (uint16_t)v); mem_ww(m, a + 2, (uint16_t)(v >> 16));
}
