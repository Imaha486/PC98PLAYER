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

static inline uint32_t lin_mask(uint32_t a) {
    a &= g_addr_mask;
    return a;
}
static inline bool ext_ram(uint32_t a) { return a >= 0x100000u && a < g_ram_top; }
static inline uint8_t mem_rb(Machine* m, uint32_t a) {
    a = lin_mask(a);
    if (a < 0xA0000u || ext_ram(a)) return g_ram[a];
    return mem_rb_slow(m, a);
}
static inline void mem_wb(Machine* m, uint32_t a, uint8_t v) {
    a = lin_mask(a);
    if (a < 0xA0000u || ext_ram(a)) { if (g_ram[a] != v) { g_ram[a] = v; g_side_fx++; } return; }   // 同じ値の書き込みは影響なし
    g_side_fx++;
    mem_wb_slow(m, a, v);
}
static inline uint16_t mem_rw(Machine* m, uint32_t a) {
    uint32_t b = lin_mask(a);
    if (b < 0x9FFFFu || (b >= 0x100000u && b + 1 < g_ram_top)) return (uint16_t)(g_ram[b] | (g_ram[b + 1] << 8));
    return mem_rw_slow(m, a);
}
static inline void mem_ww(Machine* m, uint32_t a, uint16_t v) {
    uint32_t b = lin_mask(a);
    if (b < 0x9FFFFu || (b >= 0x100000u && b + 1 < g_ram_top)) {
        if (g_ram[b] != (uint8_t)v || g_ram[b + 1] != (uint8_t)(v >> 8)) { g_ram[b] = (uint8_t)v; g_ram[b + 1] = (uint8_t)(v >> 8); g_side_fx++; }
        return;
    }
    g_side_fx++;
    mem_ww_slow(m, a, v);
}
static inline uint32_t mem_rd(Machine* m, uint32_t a) {
    return (uint32_t)mem_rw(m, a) | ((uint32_t)mem_rw(m, a + 2) << 16);
}
static inline void mem_wd(Machine* m, uint32_t a, uint32_t v) {
    mem_ww(m, a, (uint16_t)v); mem_ww(m, a + 2, (uint16_t)(v >> 16));
}
