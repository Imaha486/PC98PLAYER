// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  cdvirt.h  --  フォルダの CD-ROM を、ISO 9660 のセクタの並びとしても見せる
//
//  CDROM= にフォルダを書いたとき、ファイルとしての読み書きはホストのファイルをそのまま使うが、
//  MSCDEX の絶対セクタの読み出し（INT 2Fh AX=1508h）や、CD-ROM の装置への読み出しの要求、
//  ディレクトリ項目の問い合わせ（AX=150Fh）でセクタを読むゲームのために、差し込んだときに
//  ボリューム記述子・パステーブル・ディレクトリを組み立て、ファイルの中身をセクタに並べる。
//   16: 主ボリューム記述子  17: 終わり  18-: パステーブル（L・M）  その後: ディレクトリ  その後: ファイル
//  名前は大文字のシフト JIS、ファイルには ";1" を付ける（ISO 9660 と同じ形）。
// -----------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <string>

namespace cdvirt {
    bool build(const std::string& host_dir, const std::string& label);   // 前のものは捨てる
    void clear();
    bool active();
    uint32_t sectors();                                // ボリュームの大きさ（セクタ）
    bool read_sector(uint32_t lba, uint8_t* out);      // 2048 バイト
}
