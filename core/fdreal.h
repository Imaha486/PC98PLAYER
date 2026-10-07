// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  fdreal.h  --  実機のフロッピードライブ（USB フロッピー / Greaseweazle）
//
//  FloppyImage の「トラックを使うときに読みに行く元（FdSource）」として働く。
//  名前（INI の FloppyImage= や F11 の画面で選ぶもの）:
//    FDD:A  または  \\.\A:      Windows のフロッピードライブ A:（USB フロッピー等）
//    GW                         Greaseweazle（USB の ID で自動的に探す）
//    GW:COM5                    Greaseweazle（ポートを指定）
// -----------------------------------------------------------------------------
#pragma once
#include "floppy.h"
#include <string>
#include <vector>

FdSource*   fdreal_open(const std::string& spec, std::string* err);
std::string fdreal_format_name(const std::string& spec);   // 表示用の形式名（"USB-FDD" 等）
uint32_t    fdreal_now_ms();

namespace fdreal {
    struct Device { std::string spec; std::string label; };
    // つながっている実機（USB フロッピーのドライブ、Greaseweazle のポート）
    std::vector<Device> list_devices();
}
