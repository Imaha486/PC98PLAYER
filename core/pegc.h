// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  pegc.h  --  PC-9821 の 256 色（PEGC）のプレーンモードのラッチと、窓の読み書きの入口
// -----------------------------------------------------------------------------
#pragma once
#include <stdint.h>
// プレーンモード（E0100h bit0=1）で読んだ 32 ドットぶんの色（転送元・パターン）。状態の保存では "PGCX" に入る
struct PegcPlanar {
    uint8_t source[32] = {}, pattern[32] = {};
    uint8_t selector = 0;      // 予備（以前の形式との互換のため）
};
struct Machine;
bool     pegc_window(Machine* m, uint32_t address);                  // 256 色モードの VRAM の窓（A8000h-B7FFFh）か
uint16_t pegc_read(Machine* m, uint32_t address, int bytes);         // 1 / 2 バイト
void     pegc_write(Machine* m, uint32_t address, uint16_t value, int bytes);
