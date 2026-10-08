// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  cpu.h  --  i386 命令インタプリタ（8086 / V30 / 286 の上位互換）
//
//  PC-9801 のゲームのほとんどはリアルモードの DOS プログラム。1995 年以降のソフトは
//  386 命令（32bit レジスタ・MOVZX・SHLD など）をリアルモードで普通に使う。
//  一部のゲームは自前で保護モードに切り替える（素の DOS で 1MB より上のメモリを使う）ので、
//  保護モードのセグメント（GDT/LDT/IDT・32 ビットのコードとスタック）も扱う。
//  ページング・仮想 86 モード・タスク切り替え・特権の検査は扱わない。
// -----------------------------------------------------------------------------
#pragma once
#include <stdint.h>

enum { EAX=0, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
enum { ES_=0, CS_, SS_, DS_, FS_, GS_ };

#define FL_CF 0x0001u
#define FL_PF 0x0004u
#define FL_AF 0x0010u
#define FL_ZF 0x0040u
#define FL_SF 0x0080u
#define FL_TF 0x0100u
#define FL_IF 0x0200u
#define FL_DF 0x0400u
#define FL_OF 0x0800u

struct Machine;

struct Cpu {
    uint32_t r[8];
    uint16_t sr[6];           // セレクタ（リアルモードではセグメント値）
    uint32_t ip;              // EIP（16 ビットのコードでは下位 16 ビットだけ使う）
    uint32_t fl;
    uint32_t cr0;
    uint8_t  halted;
    uint8_t  inhibit_irq;     // MOV SS / POP SS / STI の直後 1 命令
    uint32_t op_ip;           // 実行中命令の先頭（例外の戻り先）
    uint16_t op_cs;
    int64_t  cycles;          // 累計
    Machine* m;

    // 診断
    uint32_t undef_count;

    // セグメントの記述子キャッシュ。csel[i] == sr[i] のときだけ有効（HLE 側がリアルモードで
    // sr[] を直接書き換えたら、その値 *16 をベースとして使う）
    uint16_t csel[6];
    uint32_t cbase[6];
    uint32_t climit[6];
    uint8_t  cbig[6];         // CS: D ビット（32 ビットのコード）/ SS: B ビット（ESP を使う）
    // 保護モードの表
    uint32_t gdt_base, idt_base, ldt_base, tr_base;
    uint16_t gdt_limit, idt_limit;
    uint32_t ldt_limit, tr_limit;
    uint16_t ldtr, tr;
    uint32_t cr2, cr3;
};

// 旧形式（ver 261008 まで）のステートの CPU（読み込み用）
struct CpuV1 {
    uint32_t r[8];
    uint16_t sr[6];
    uint16_t ip;
    uint32_t fl;
    uint32_t cr0;
    uint8_t  halted;
    uint8_t  inhibit_irq;
    uint16_t op_ip;
    uint16_t op_cs;
    int64_t  cycles;
    Machine* m;
    uint32_t undef_count;
};

void cpu_reset(Cpu* c, Machine* m);
// 最低 budget サイクル走らせて戻る。実際に使ったサイクル数を返す。
int  cpu_run(Cpu* c, int budget);
// ディスクの読み込み結果を調べる比較の記録（Trace=1 のとき、INT 1Bh の後に使う）
//  begin: 読み込んだバッファ（線形アドレスと長さ。len=0 はバッファなし）を見張り始める
//  taint8: 8 ビットレジスタ（AL,CL,DL,BL,AH,CH,DH,BH = 0..7）に「どこから来た値か」の印を付ける
void cpu_dw_begin(uint32_t buf, uint32_t len, const char* what);
void cpu_dw_taint8(int r8, uint8_t val, const char* label);
// ソフトウェア割込み（HLE 側から呼び出す用）
void cpu_interrupt(Cpu* c, uint8_t vec);

// HLE 側（リアルモード）からセグメントレジスタを書き換える
static inline void cpu_setsr(Cpu* c, int s, uint16_t v) {
    c->sr[s] = v; c->csel[s] = v; c->cbase[s] = (uint32_t)v << 4;
    if (s == CS_ || s == SS_) { c->cbig[s] = 0; c->climit[s] = 0xFFFF; }
}
static inline uint16_t cpu_ax(Cpu* c) { return (uint16_t)c->r[EAX]; }
