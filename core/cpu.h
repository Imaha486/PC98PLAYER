// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  cpu.h  --  i386 リアルモード命令インタプリタ（8086 / V30 / 286 の上位互換）
//
//  PC-9801 のゲームはリアルモードの DOS プログラムなので、保護モード・
//  ページングは扱わない。ただし 1995 年以降のソフトは 386 命令（32bit
//  レジスタ・MOVZX・SHLD など）をリアルモードで普通に使うので、それは扱う。
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
    uint16_t sr[6];
    uint16_t ip;
    uint32_t fl;
    uint32_t cr0;
    uint8_t  halted;
    uint8_t  inhibit_irq;     // MOV SS / POP SS / STI の直後 1 命令
    uint16_t op_ip;           // 実行中命令の先頭（例外の戻り先）
    uint16_t op_cs;
    int64_t  cycles;          // 累計
    Machine* m;

    // 診断
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

static inline uint16_t cpu_ax(Cpu* c) { return (uint16_t)c->r[EAX]; }
