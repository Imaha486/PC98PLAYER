// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  debugdump.h  --  Trace=2 のときの「終わる直前のようす」の記録（PC98PLAYER.DEBUG / PC98PLAYER.MEM）
//
//  Trace=2 で起動すると、動いている間ずっと次のものを輪のように覚えておき（古いものから捨てる）、
//  終わるとき（ウインドウを閉じる・プログラムが終わる・リセット）にゲームのフォルダへ書き出す。
//   ・直近の命令 16384 個（その命令を実行する前のレジスタ）
//   ・直近の割込み 4096 回（ハードウェア / INT 命令、そのときのレジスタとスタック）と、INT 21h の結果
//   ・直近の I/O ポート 4096 回
//   ・1 秒ごとの様子（レジスタ・スタック・PIC）を直近 10 秒ぶん
//   ・終わったときのレジスタ・スタック・コード・PIC / PIT・割込みベクタ・MCB の鎖
//  PC98PLAYER.MEM は 1MB（0000:0000～FFFF:000F）のメモリの中身そのまま。
// -----------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <string>

struct Machine;

extern bool g_dbg_on;   // Trace=2 のとき true（命令ごとの記録をする）

void dbg_begin(Machine* m);                  // 起動の直後（割込みベクタの最初の形を覚える）
void dbg_frame(Machine* m);                  // 1 フレームごと
void dbg_note_int(Machine* m, uint8_t vec, bool hw);            // 割込みに入るとき（cpu.cpp）
void dbg_note_int21_ret(Machine* m);                            // INT 21h の HLE が終わったとき（dos.cpp）
void dbg_note_io(Machine* m, uint16_t port, uint16_t v, bool out, bool w16);   // I/O（machine.cpp）
void dbg_note_text(const char* s);           // 目印（[stall] など）
// 書き出す。fb があれば最後の画面（640x400、0xRRGGBB）も PC98PLAYER.BMP に書く
bool dbg_write(Machine* m, const std::string& dir, std::string* err, const uint32_t* fb = nullptr);

// dos.cpp: MCB の鎖を文字にする
std::string dos_debug_mcb(Machine* m);
