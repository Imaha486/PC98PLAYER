// SPDX-License-Identifier: MIT
//  hostfs.h -- ホストのファイルシステムへの薄い層（Windows / POSIX）
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct HostDirEntry {
    std::string name;     // ホスト上の名前（そのまま）
    bool     is_dir;
    uint32_t size;
    uint16_t dos_date, dos_time;
    bool     readonly;
    uint8_t  attr = 0x20;  // DOS の属性（01h 読み取り専用・02h 隠し・04h システム・20h アーカイブ。ディレクトリの 10h は含めない）
};

namespace hostfs {
    // guest_bytes はシフト JIS のバイト列。host には UTF-8（POSIX）または内部表現を返す。
    std::string join(const std::string& a, const std::string& b);
    bool list_dir(const std::string& dir, std::vector<HostDirEntry>& out);
    bool stat(const std::string& path, HostDirEntry& e);
    void* open(const std::string& path, int mode /*0=r,1=w,2=rw*/, bool create, bool trunc);
    void  close(void* h);
    int   read(void* h, void* buf, int n);
    int   write(void* h, const void* buf, int n);
    int64_t seek(void* h, int64_t off, int whence);
    bool  truncate_here(void* h);
    bool  remove(const std::string& path);
    bool  rename(const std::string& a, const std::string& b);
    bool  mkdir(const std::string& path);
    bool  rmdir(const std::string& path);
    bool  set_time(void* h, uint16_t date, uint16_t time);
    bool  get_time(void* h, uint16_t* date, uint16_t* time);
    bool  set_attr(const std::string& path, uint8_t attr);   // DOS の属性（R/H/S/A）を付ける
    std::string from_sjis(const std::string& s);   // ゲストの名前 → ホストの名前
    std::string to_sjis(const std::string& s);     // ホストの名前 → ゲストの名前
}
