// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  hdimage.cpp  --  ハードディスクイメージの中身をフォルダへ展開する
//
//  形式（ヘッダ）
//   ・HDI（Anex86）: +8 ヘッダ長, +12 データ長, +16 セクタ長, +20 セクタ数, +24 ヘッド数, +28 シリンダ数
//   ・NHD（T98-Next）: "T98HDDIMAGE.R0", +110h ヘッダ長, +114h シリンダ数, +118h ヘッド数, +11Ah セクタ数, +11Ch セクタ長
//   ・THD（T98）: +0 シリンダ数（256 バイトのヘッダ。8 ヘッド・33 セクタ・256 バイト）
//   ・ベタ（.HDD 等）: 区画表の位置から 256 / 512 バイトセクタを見分ける
//
//  PC-98 の区画表は物理セクタ 1 から 32 バイト x 16 個:
//   +0 起動の種類 +1 システムの種類（MS-DOS は 20h/21h 等。最上位ビット = 起動できる）
//   +8 開始セクタ +9 開始ヘッド +10 開始シリンダ(2) ... +16 名前(16)
//  区画の先頭は MS-DOS の起動セクタで、BPB（+0Bh からセクタ長・クラスタ長…）がある。
// -----------------------------------------------------------------------------
#include "hdimage.h"
#include "hostfs.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <set>

namespace hdimage {

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static std::string upper_ext(const std::string& p) {
    size_t d = p.find_last_of('.'), s = p.find_last_of("\\/");
    if (d == std::string::npos || (s != std::string::npos && d < s)) return "";
    std::string e = p.substr(d + 1);
    for (auto& c : e) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return e;
}

// イメージのファイル（読み込みだけ）
struct Img {
    void* h = nullptr;
    int64_t size = 0;
    ~Img() { if (h) hostfs::close(h); }
    bool open(const std::string& p) {
        h = hostfs::open(p, 0, false, false);
        if (!h) return false;
        size = hostfs::seek(h, 0, 2);
        return size > 0;
    }
    bool read(int64_t off, void* buf, size_t n) {
        if (off < 0 || off + (int64_t)n > size) return false;
        if (hostfs::seek(h, off, 0) != off) return false;
        uint8_t* p = (uint8_t*)buf;
        while (n) {
            int k = (int)std::min<size_t>(n, 1 << 20);
            if (hostfs::read(h, p, k) != k) return false;
            p += k; n -= (size_t)k;
        }
        return true;
    }
};

// 区画の先頭の起動セクタに、使える BPB があるか
static bool bpb_ok(const uint8_t* b, std::string* fs, uint32_t* bytes) {
    uint16_t bps = rd16(b + 11);
    uint8_t spc = b[13];
    uint16_t rsv = rd16(b + 14);
    uint8_t nf = b[16];
    uint16_t rootn = rd16(b + 17);
    uint32_t tot = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    uint16_t spf = rd16(b + 22);
    if (bps != 256 && bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return false;
    if (!spc || (spc & (spc - 1)) || !rsv || nf < 1 || nf > 2 || !rootn || !spf || !tot) return false;
    uint32_t root_secs = ((uint32_t)rootn * 32 + bps - 1) / bps;
    uint32_t data = rsv + nf * spf + root_secs;
    if (data >= tot) return false;
    uint32_t clus = (tot - data) / spc;
    if (fs) *fs = clus < 4085 ? "FAT12" : "FAT16";
    if (bytes) *bytes = (uint32_t)std::min<uint64_t>(0xFFFFFFFFu, (uint64_t)tot * bps);
    return true;
}

bool is_hd_image(const std::string& path) {
    std::string e = upper_ext(path);
    return e == "HDI" || e == "NHD" || e == "THD" || e == "HDD" || e == "HDM2";
}

bool probe(const std::string& path, Info* info, std::string* err) {
    Img im;
    if (!im.open(path)) { if (err) *err = "イメージを開けません"; return false; }
    uint8_t h[0x200] = {};
    im.read(0, h, (size_t)std::min<int64_t>(sizeof(h), im.size));
    Info in;
    std::string e = upper_ext(path);
    if (!memcmp(h, "T98HDDIMAGE.R0", 14)) {
        in.format = "NHD";
        in.data_off = rd32(h + 0x110); in.cyls = (int)rd32(h + 0x114); in.heads = rd16(h + 0x118);
        in.spt = rd16(h + 0x11A); in.ssize = rd16(h + 0x11C);
    } else if (e == "HDI" || (rd32(h) == 0 && (int64_t)rd32(h + 8) + rd32(h + 12) == im.size)) {
        in.format = "HDI";
        in.data_off = rd32(h + 8); in.ssize = (int)rd32(h + 16); in.spt = (int)rd32(h + 20);
        in.heads = (int)rd32(h + 24); in.cyls = (int)rd32(h + 28);
    } else if (e == "THD") {
        in.format = "THD";
        in.data_off = 256; in.cyls = rd16(h); in.heads = 8; in.spt = 33; in.ssize = 256;
    } else {
        in.format = "ベタ";
        in.data_off = 0; in.ssize = 0;
    }
    if (in.data_off < 0 || in.data_off >= im.size) { if (err) *err = "ヘッダが正しくありません"; return false; }
    // ベタ: 区画表（物理セクタ 1）の位置で 256 / 512 を見分ける。形は区画表の終わりの値から推す
    std::vector<int> sizes;
    if (in.ssize == 256 || in.ssize == 512 || in.ssize == 1024) sizes.push_back(in.ssize);
    else { sizes.push_back(512); sizes.push_back(256); }
    for (int ss : sizes) {
        uint8_t pt[512];
        if (!im.read(in.data_off + ss, pt, sizeof(pt))) continue;
        std::vector<Info::Part> parts;
        // 形が分からないベタは、よくある形を順に試す
        struct G { int heads, spt; };
        std::vector<G> geos;
        if (in.heads > 0 && in.spt > 0) geos.push_back({in.heads, in.spt});
        else { geos = {{8, 17}, {8, 33}, {4, 17}, {6, 17}, {8, 32}, {16, 63}, {15, 17}}; }
        for (const auto& g : geos) {
            parts.clear();
            for (int k = 0; k < 16; k++) {
                const uint8_t* p = pt + k * 32;
                if (p[0] == 0 && p[1] == 0) continue;
                uint32_t sc = rd16(p + 10), sh = p[9], ss_ = p[8];
                int64_t lba = ((int64_t)sc * g.heads + sh) * g.spt + ss_;
                int64_t off = in.data_off + lba * ss;
                uint8_t bs[64];
                if (!im.read(off, bs, sizeof(bs))) continue;
                std::string fs; uint32_t bytes = 0;
                if (!bpb_ok(bs, &fs, &bytes)) continue;
                std::string name((const char*)p + 16, 16);
                while (!name.empty() && (name.back() == ' ' || name.back() == 0)) name.pop_back();
                parts.push_back({off, name, fs, bytes});
            }
            if (!parts.empty()) { in.heads = g.heads; in.spt = g.spt; break; }
        }
        if (!parts.empty()) { in.ssize = ss; in.parts = parts; break; }
    }
    // 区画表が無い（ディスク全体が 1 つの FAT）
    if (in.parts.empty()) {
        uint8_t bs[64];
        std::string fs; uint32_t bytes = 0;
        if (im.read(in.data_off, bs, sizeof(bs)) && bpb_ok(bs, &fs, &bytes)) in.parts.push_back({in.data_off, "", fs, bytes});
    }
    if (in.parts.empty()) { if (err) *err = "MS-DOS の区画が見つかりません"; return false; }
    if (info) *info = in;
    return true;
}

// ---- FAT の区画を展開する ---------------------------------------------------------
struct Fat {
    Img* im = nullptr;
    int64_t base = 0;
    uint32_t bps = 0, spc = 0, rsv = 0, nf = 0, rootn = 0, spf = 0, tot = 0;
    uint32_t root_sec = 0, data_sec = 0, nclus = 0;
    bool fat12 = false;
    std::vector<uint8_t> fat;
    Result* res = nullptr;

    bool init(Img* i, int64_t off) {
        im = i; base = off;
        uint8_t b[64];
        if (!im->read(off, b, sizeof(b)) || !bpb_ok(b, nullptr, nullptr)) return false;
        bps = rd16(b + 11); spc = b[13]; rsv = rd16(b + 14); nf = b[16]; rootn = rd16(b + 17);
        tot = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32); spf = rd16(b + 22);
        root_sec = rsv + nf * spf;
        data_sec = root_sec + (rootn * 32 + bps - 1) / bps;
        nclus = (tot - data_sec) / spc;
        fat12 = nclus < 4085;
        fat.resize((size_t)spf * bps);
        return im->read(base + (int64_t)rsv * bps, fat.data(), fat.size());
    }
    uint32_t next(uint32_t c) const {
        if (fat12) {
            size_t o = c + c / 2;
            if (o + 1 >= fat.size()) return 0xFFFF;
            uint16_t v = rd16(&fat[o]);
            v = (c & 1) ? (uint16_t)(v >> 4) : (uint16_t)(v & 0xFFF);
            return v >= 0xFF8 ? 0xFFFF : v;
        }
        size_t o = (size_t)c * 2;
        if (o + 1 >= fat.size()) return 0xFFFF;
        uint16_t v = rd16(&fat[o]);
        return v >= 0xFFF8 ? 0xFFFF : v;
    }
    int64_t clus_off(uint32_t c) const { return base + ((int64_t)data_sec + (int64_t)(c - 2) * spc) * bps; }
    // クラスタの鎖を読む（ループ・範囲外は打ち切る）
    bool read_chain(uint32_t c, uint32_t limit, std::vector<uint8_t>& out) {
        out.clear();
        uint32_t cs = spc * bps;
        std::set<uint32_t> seen;
        while (c >= 2 && c < nclus + 2 && out.size() < limit) {
            if (!seen.insert(c).second) break;
            size_t o = out.size();
            out.resize(o + cs);
            if (!im->read(clus_off(c), &out[o], cs)) return false;
            c = next(c);
            if (c == 0xFFFF) break;
        }
        if (out.size() > limit) out.resize(limit);
        return true;
    }
    void note(const std::string& s) { if (res->notes.size() < 40) res->notes.push_back(s); }

    // ディレクトリの中身（32 バイトの並び）を展開する
    void walk(const std::vector<uint8_t>& dir, const std::string& host_dir, const std::string& guest_dir, int depth) {
        if (depth > 32) return;
        for (size_t o = 0; o + 32 <= dir.size(); o += 32) {
            const uint8_t* e = &dir[o];
            if (e[0] == 0x00) break;
            if (e[0] == 0xE5) continue;
            uint8_t attr = e[11];
            if (attr == 0x0F || (attr & 0x08)) continue;   // 長い名前・ボリュームラベル
            char nm[9], ex[4];
            memcpy(nm, e, 8); nm[8] = 0; memcpy(ex, e + 8, 3); ex[3] = 0;
            if (nm[0] == 0x05) nm[0] = (char)0xE5;
            std::string n(nm), x(ex);
            while (!n.empty() && n.back() == ' ') n.pop_back();
            while (!x.empty() && x.back() == ' ') x.pop_back();
            if (n == "." || n == ".." || n.empty()) continue;
            std::string gname = x.empty() ? n : n + "." + x;
            // Windows で使えない文字は _ に（DOS の名前にはまず無い）
            for (size_t i = 0; i < gname.size(); i++) {
                unsigned char ch = (unsigned char)gname[i];
                if ((ch >= 0x81 && ch <= 0x9F) || (ch >= 0xE0 && ch <= 0xFC)) { i++; continue; }   // 全角の 2 バイト目は触らない
                if (ch < 0x20 || strchr("\\/:*?\"<>|", ch)) gname[i] = '_';
            }
            std::string hname = hostfs::from_sjis(gname);
            std::string hpath = hostfs::join(host_dir, hname);
            std::string gpath = guest_dir + "\\" + gname;
            uint32_t clus = rd16(e + 26);
            uint16_t tm = rd16(e + 22), dt = rd16(e + 24);
            if (attr & 0x10) {
                HostDirEntry st;
                if (!hostfs::stat(hpath, st)) {
                    if (!hostfs::mkdir(hpath)) { res->errors++; note("フォルダを作れません: " + hostfs::from_sjis(gpath)); continue; }
                    res->dirs++;
                }
                std::vector<uint8_t> sub;
                if (!read_chain(clus, 8u << 20, sub)) { res->errors++; continue; }
                walk(sub, hpath, gpath, depth + 1);
                continue;
            }
            uint32_t size = rd32(e + 28);
            HostDirEntry st;
            if (hostfs::stat(hpath, st)) {   // 既にある: 上書きしない
                res->skipped++;
                note("既にあるので飛ばしました: " + hostfs::from_sjis(gpath));
                continue;
            }
            std::vector<uint8_t> data;
            bool ok = size == 0 || read_chain(clus, size, data);
            if (ok && data.size() < size) { note("途中までしか読めません: " + hostfs::from_sjis(gpath)); res->errors++; }
            void* h = hostfs::open(hpath, 1, true, true);
            if (!h) { res->errors++; note("書き出せません: " + hostfs::from_sjis(gpath)); continue; }
            if (!data.empty() && hostfs::write(h, data.data(), (int)data.size()) != (int)data.size()) { res->errors++; note("書き出しに失敗: " + hostfs::from_sjis(gpath)); }
            hostfs::set_time(h, dt, tm);
            hostfs::close(h);
            res->files++;
            res->bytes += data.size();
        }
    }
    bool extract(const std::string& dest) {
        std::vector<uint8_t> root((size_t)(data_sec - root_sec) * bps);
        if (!im->read(base + (int64_t)root_sec * bps, root.data(), root.size())) return false;
        walk(root, dest, "", 0);
        return true;
    }
};

bool extract(const std::string& path, const std::string& dest, Result* res, std::string* err) {
    Info in;
    if (!probe(path, &in, err)) return false;
    Img im;
    if (!im.open(path)) { if (err) *err = "イメージを開けません"; return false; }
    Result r;
    for (size_t k = 0; k < in.parts.size(); k++) {
        std::string d = dest;
        if (k > 0) {   // 2 つ目からの区画は下のフォルダへ
            char sub[16]; snprintf(sub, sizeof(sub), "DRIVE_%c", (char)('A' + k));
            d = hostfs::join(dest, sub);
            HostDirEntry st;
            if (!hostfs::stat(d, st)) hostfs::mkdir(d);
        }
        Fat f; f.res = &r;
        if (!f.init(&im, in.parts[k].off) || !f.extract(d)) { r.errors++; f.note("区画を読めません: " + in.parts[k].name); continue; }
        std::string label = hostfs::from_sjis(in.parts[k].name);
        r.notes.insert(r.notes.begin() + std::min<size_t>(k, r.notes.size()),
                       "区画 " + std::to_string(k + 1) + (label.empty() ? "" : "「" + label + "」") + "（" + in.parts[k].fs + "）→ " +
                       (k == 0 ? std::string("展開先のフォルダ") : d));
    }
    if (res) *res = r;
    return true;
}

} // namespace hdimage
