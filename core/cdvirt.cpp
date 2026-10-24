// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  cdvirt.cpp  --  フォルダの CD-ROM を ISO 9660 のセクタとして見せる（設計は cdvirt.h）
// -----------------------------------------------------------------------------
#include "cdvirt.h"
#include "hostfs.h"
#include <string.h>
#include <vector>
#include <algorithm>

namespace hostfs_native {
    void* open(const std::string& path, int mode, bool create, bool trunc);
    void  close(void* h);
    int   read(void* h, void* buf, int n);
    int64_t seek(void* h, int64_t off, int whence);
}

namespace cdvirt {

struct Node {
    std::string name;       // ISO の名前（大文字のシフト JIS。ファイルは ";1" 付き）
    std::string host;       // ホストのパス
    bool dir = false;
    uint32_t size = 0;      // ファイルの大きさ / ディレクトリの記録の大きさ（セクタの倍数）
    uint32_t lba = 0;
    uint16_t date = 0x21, time = 0;
    int parent = 0;
    std::vector<int> kids;
    int ptnum = 0;          // パステーブルの番号（1 から）
};

static bool s_on = false;
static std::vector<Node> s_n;
static std::vector<uint8_t> s_meta;     // LBA 16 から、ファイルの手前までの中身
static uint32_t s_meta_lba = 16, s_total = 0;
static std::vector<int> s_files;        // LBA の順
static void* s_fh = nullptr; static int s_fh_node = -1;

void clear() {
    if (s_fh) hostfs_native::close(s_fh);
    s_fh = nullptr; s_fh_node = -1;
    s_on = false; s_n.clear(); s_meta.clear(); s_files.clear(); s_total = 0;
}
bool active() { return s_on; }
uint32_t sectors() { return s_on ? s_total : 0; }

static std::string iso_name(const std::string& host_name, bool dir) {
    std::string s = hostfs::to_sjis(host_name);
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < s.size()) { o += s[i]; o += s[i + 1]; i++; continue; }
        o += (char)toupper(c);
    }
    if (!dir) { if (o.find('.') == std::string::npos) o += '.'; o += ";1"; }
    return o;
}

static void scan(int idx, int depth) {
    std::vector<HostDirEntry> l;
    if (depth > 8 || !hostfs::list_dir(s_n[idx].host, l)) return;
    std::vector<int> kids;
    for (auto& e : l) {
        if (e.name == "." || e.name == "..") continue;
        Node k;
        k.dir = e.is_dir; k.name = iso_name(e.name, e.is_dir); k.host = hostfs::join(s_n[idx].host, e.name);
        k.size = e.is_dir ? 0 : e.size; k.date = e.dos_date; k.time = e.dos_time; k.parent = idx;
        s_n.push_back(k);
        kids.push_back((int)s_n.size() - 1);
    }
    std::sort(kids.begin(), kids.end(), [](int a, int b) { return s_n[a].name < s_n[b].name; });
    s_n[idx].kids = kids;
    for (int k : kids) if (s_n[k].dir) scan(k, depth + 1);
}

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8)); }
static void put32b(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - i * 8)); }
static void both16(uint8_t* p, uint16_t v) { put16(p, v); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void both32(uint8_t* p, uint32_t v) { put32(p, v); put32b(p + 4, v); }

static void rec_time(uint8_t* t, uint16_t d, uint16_t tm) {
    t[0] = (uint8_t)(((d >> 9) & 0x7F) + 80); t[1] = (uint8_t)((d >> 5) & 15); t[2] = (uint8_t)(d & 31);
    if (!t[1]) t[1] = 1;
    if (!t[2]) t[2] = 1;
    t[3] = (uint8_t)(tm >> 11); t[4] = (uint8_t)((tm >> 5) & 63); t[5] = (uint8_t)((tm & 31) * 2); t[6] = 36;   // +9 時間
}
// ディレクトリの記録を 1 つ作る（名前の長さ nl、名前 nm）
static int make_rec(uint8_t* p, const Node& n, const char* nm, int nl) {
    int len = 33 + nl; if (len & 1) len++;
    memset(p, 0, (size_t)len);
    p[0] = (uint8_t)len;
    both32(p + 2, n.lba);
    both32(p + 10, n.size);
    rec_time(p + 18, n.date, n.time);
    p[25] = n.dir ? 0x02 : 0x00;
    both16(p + 28, 1);
    p[32] = (uint8_t)nl;
    memcpy(p + 33, nm, (size_t)nl);
    return len;
}
static int rec_len(int nl) { int len = 33 + nl; return (len & 1) ? len + 1 : len; }

// ディレクトリの記録の大きさ（セクタをまたがないように詰める）
static uint32_t dir_bytes(const Node& d) {
    uint32_t pos = 0;
    auto add = [&](int l) { if ((pos % 2048) + (uint32_t)l > 2048) pos = (pos / 2048 + 1) * 2048; pos += (uint32_t)l; };
    add(34); add(34);
    for (int k : d.kids) add(rec_len((int)s_n[k].name.size()));
    return (pos + 2047) / 2048 * 2048;
}

bool build(const std::string& host_dir, const std::string& label) {
    clear();
    Node root; root.dir = true; root.host = host_dir; root.parent = 0;
    s_n.push_back(root);
    scan(0, 0);
    // パステーブル（幅優先）
    std::vector<int> order{0};
    for (size_t i = 0; i < order.size(); i++) for (int k : s_n[order[i]].kids) if (s_n[k].dir) order.push_back(k);
    uint32_t pt_bytes = 0;
    for (size_t i = 0; i < order.size(); i++) {
        s_n[order[i]].ptnum = (int)i + 1;
        int nl = i == 0 ? 1 : (int)s_n[order[i]].name.size();
        pt_bytes += (uint32_t)(8 + nl + (nl & 1));
    }
    uint32_t pt_secs = (pt_bytes + 2047) / 2048;
    uint32_t lba = 18;
    uint32_t pt_l = lba; lba += pt_secs;
    uint32_t pt_m = lba; lba += pt_secs;
    for (int d : order) { s_n[d].size = dir_bytes(s_n[d]); s_n[d].lba = lba; lba += s_n[d].size / 2048; }
    uint32_t meta_end = lba;
    // ファイル（ディレクトリの順）
    for (int d : order) for (int k : s_n[d].kids) if (!s_n[k].dir) {
        s_n[k].lba = lba; lba += (s_n[k].size + 2047) / 2048;
        if (s_n[k].size == 0) s_n[k].lba = 0;
        else s_files.push_back(k);
    }
    s_total = lba + 150;   // 終わりに少し余白（実機の CD のリードアウトの手前）
    s_meta.assign((size_t)(meta_end - 16) * 2048, 0);
    auto sec = [&](uint32_t l) { return s_meta.data() + (size_t)(l - 16) * 2048; };
    // 主ボリューム記述子
    {
        uint8_t* p = sec(16);
        p[0] = 1; memcpy(p + 1, "CD001", 5); p[6] = 1;
        memset(p + 8, ' ', 32); memset(p + 40, ' ', 32);
        std::string lb = label.substr(0, 32);
        for (size_t i = 0; i < lb.size(); i++) p[40 + i] = (uint8_t)toupper((unsigned char)lb[i]);
        both32(p + 80, s_total);
        both16(p + 120, 1); both16(p + 124, 1); both16(p + 128, 2048);
        both32(p + 132, pt_bytes);
        put32(p + 140, pt_l); put32b(p + 148, pt_m);
        make_rec(p + 156, s_n[0], "\0", 1);
        memset(p + 190, ' ', 128 * 4 + 37 * 3);
        for (int i = 0; i < 4; i++) { memset(p + 813 + i * 17, '0', 16); p[813 + i * 17 + 16] = 0; }
        memcpy(p + 813, "1994010100000000", 16);
        memcpy(p + 830, "1994010100000000", 16);
        p[881] = 1;
    }
    { uint8_t* p = sec(17); p[0] = 255; memcpy(p + 1, "CD001", 5); p[6] = 1; }
    // パステーブル
    for (int big = 0; big < 2; big++) {
        uint8_t* p = sec(big ? pt_m : pt_l);
        uint32_t o = 0;
        for (size_t i = 0; i < order.size(); i++) {
            const Node& n = s_n[order[i]];
            int nl = i == 0 ? 1 : (int)n.name.size();
            p[o] = (uint8_t)nl; p[o + 1] = 0;
            uint16_t par = (uint16_t)s_n[n.parent].ptnum;
            if (big) { put32b(p + o + 2, n.lba); p[o + 6] = (uint8_t)(par >> 8); p[o + 7] = (uint8_t)par; }
            else { put32(p + o + 2, n.lba); put16(p + o + 6, par); }
            if (i == 0) p[o + 8] = 0; else memcpy(p + o + 8, n.name.data(), (size_t)nl);
            o += (uint32_t)(8 + nl + (nl & 1));
        }
    }
    // ディレクトリ
    for (int d : order) {
        const Node& n = s_n[d];
        uint8_t* base = sec(n.lba);
        uint32_t pos = 0;
        auto put = [&](const Node& r, const char* nm, int nl) {
            int l = rec_len(nl);
            if ((pos % 2048) + (uint32_t)l > 2048) pos = (pos / 2048 + 1) * 2048;
            make_rec(base + pos, r, nm, nl); pos += (uint32_t)l;
        };
        put(n, "\0", 1);
        put(s_n[n.parent], "\1", 1);
        for (int k : n.kids) put(s_n[k], s_n[k].name.data(), (int)s_n[k].name.size());
    }
    s_meta_lba = 16;
    s_on = true;
    return true;
}

bool read_sector(uint32_t lba, uint8_t* out) {
    if (!s_on || lba >= s_total) return false;
    memset(out, 0, 2048);
    uint32_t meta_end = 16 + (uint32_t)(s_meta.size() / 2048);
    if (lba < 16) return true;
    if (lba < meta_end) { memcpy(out, s_meta.data() + (size_t)(lba - 16) * 2048, 2048); return true; }
    // ファイル（LBA の順に並んでいるので二分探索）
    int lo = 0, hi = (int)s_files.size() - 1, hit = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const Node& n = s_n[s_files[mid]];
        uint32_t secs = (n.size + 2047) / 2048;
        if (lba < n.lba) hi = mid - 1;
        else if (lba >= n.lba + secs) lo = mid + 1;
        else { hit = s_files[mid]; break; }
    }
    if (hit < 0) return true;   // すき間（0 で埋める）
    const Node& n = s_n[hit];
    if (s_fh_node != hit) {
        if (s_fh) hostfs_native::close(s_fh);
        s_fh = hostfs_native::open(n.host, 0, false, false); s_fh_node = s_fh ? hit : -1;
    }
    if (!s_fh) return true;
    int64_t off = (int64_t)(lba - n.lba) * 2048;
    if (hostfs_native::seek(s_fh, off, 0) != off) return true;
    int want = (int)std::min<int64_t>(2048, (int64_t)n.size - off);
    if (want > 0) hostfs_native::read(s_fh, out, want);
    return true;
}

} // namespace cdvirt
