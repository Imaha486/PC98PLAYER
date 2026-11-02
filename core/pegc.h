// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
// Additional planar latches; kept separate from upstream's serialized Pegc.
struct PegcPlanar {
    uint8_t source[32] = {}, pattern[32] = {};
    uint8_t selector = 0;
};
struct Machine;
bool pegc_window(Machine* m, uint32_t address);
uint16_t pegc_read(Machine* m, uint32_t address, int bytes);
void pegc_write(Machine* m, uint32_t address, uint16_t value, int bytes);
