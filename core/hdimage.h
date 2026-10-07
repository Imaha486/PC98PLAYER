// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  hdimage.h  --  ハードディスクイメージの中身をフォルダへ展開する
//
//  HDI（Anex86）/ NHD（T98-Next）/ THD（T98）/ ヘッダ無しのベタ を読み、PC-98 の区画表から
//  MS-DOS の区画（FAT12/16）を探して、ファイルとフォルダをそのままホストのフォルダへ書き出す。
//  イメージそのものを開いて動かすのではなく、「展開したフォルダを PC98PLAYER で遊ぶ」ための道具。
// -----------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace hdimage {

struct Info {
    std::string format;              // "HDI" 等
    int ssize = 0, spt = 0, heads = 0, cyls = 0;
    int64_t data_off = 0;            // イメージの中のディスクの先頭
    struct Part { int64_t off; std::string name; std::string fs; uint32_t bytes; };
    std::vector<Part> parts;         // 見つかった MS-DOS の区画
};

struct Result {
    int files = 0, dirs = 0, skipped = 0, errors = 0;
    uint64_t bytes = 0;
    std::vector<std::string> notes;  // 区画ごとの展開先、飛ばしたファイル等（表示用、先頭のいくつか）
};

// ハードディスクイメージか（拡張子と中身で判断）
bool is_hd_image(const std::string& path);
// 形と区画を調べる
bool probe(const std::string& path, Info* info, std::string* err);
// dest へ展開する。既にある同じ名前のファイルは上書きしない（飛ばして数える）。
// 区画が 2 つ以上あるとき、2 つ目からは dest の下の "DRIVE_B" 等へ
bool extract(const std::string& path, const std::string& dest, Result* res, std::string* err);

}
