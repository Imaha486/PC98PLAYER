// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  isofs.h  --  CD-ROM のイメージ（ISO 9660）を読み出し専用のドライブとして見せる
//
//  hostfs の各関数は、パスが「\x02」で始まるか、ハンドルがここで作ったものなら
//  こちらへ回す（fatfs.cpp の hostfs:: の定義）。DOS 側は CD-ROM のドライブ
//  （CDDrive=、既定 Q:）をフロッピーと同じ形で扱える。
//
//  ・.ISO（2048 バイト/セクタ）と、.BIN（2352 バイト/セクタの生のデータトラック）を読む
//  ・名前は ISO 9660 の 8.3（";1" と末尾の "." は外す）。シフト JIS のまま
//  ・書き込みはすべて失敗にする（CD-ROM なので）
// -----------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct HostDirEntry;
namespace isofs {
    // イメージを差し込む（host_path はホストのパス）。前のものは外す
    // sec_size が 0 なら形（.ISO / .BIN）を自動で見分ける。CUE から入れるときは形とトラックの先頭の位置を渡す
    bool mount(const std::string& host_path, std::string* err, uint32_t sec_size = 0, uint32_t sec_off = 0, int64_t base = 0);
    void unmount();
    bool mounted();
    const std::string& image_path();
    std::string volume_label();          // 主ボリューム記述子のボリューム名（後ろの空白は除く）
    uint32_t volume_sectors();           // ボリュームの大きさ（2048 バイトのセクタ数）
    uint32_t image_sectors();            // イメージのファイルに入っているセクタの数（トラックの長さ）
    // 2048 バイトのセクタを読む（MSCDEX の絶対読み出し・VTOC の読み出し用）。範囲外は false
    bool read_sector(uint32_t lba, uint8_t* out);

    // 仮想パス: "\x02" + ("\\" 区切りの要素。名前はホストの形（UTF-8）)
    bool is_vpath(const std::string& p);
    std::string root_path();
    bool is_handle(void* h);
    bool list_dir(const std::string& dir, std::vector<HostDirEntry>& out);
    bool stat(const std::string& path, HostDirEntry& e);
    void* open(const std::string& path, int mode, bool create, bool trunc);   // 読み出しだけ
    void  close(void* h);
    int   read(void* h, void* buf, int n);
    int64_t seek(void* h, int64_t off, int whence);
    bool  get_time(void* h, uint16_t* d, uint16_t* t);

    // フォルダを CD-ROM として入れたとき、そのフォルダ（ホストのパス）の下への書き込みを止める
    void set_readonly_folder(const std::string& host_dir);   // 空で解除
    bool in_readonly_folder(const std::string& host_path);
}
