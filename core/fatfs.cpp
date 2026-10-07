// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  fatfs.cpp  --  フロッピーイメージの FAT12/16 を、ホストのファイルと同じ形で見せる
//
//  hostfs の各関数は、パスが「\x01 + ドライブ名」で始まるか、ハンドルがここで作った
//  ものならこちらへ回す（下の hostfs:: の定義）。DOS 側（dos.cpp）はそれ以外を変えずに
//  B: などのドライブを扱える。
//
//  ・FAT は差し込み時に読み、変えた所だけすべての FAT へ書き戻す
//  ・ディレクトリは毎回ディスク（イメージ）から読む
//  ・名前は 8.3（シフト JIS）。hostfs へはホストと同じく UTF-8 で渡す
// -----------------------------------------------------------------------------
#include "floppy.h"
#include "hostfs.h"
#include <string.h>
#include <time.h>
#include <set>
#include <algorithm>

namespace hostfs_native {
    std::string join(const std::string& a, const std::string& b);
    bool list_dir(const std::string& dir, std::vector<HostDirEntry>& out);
    bool stat(const std::string& path, HostDirEntry& e);
    void* open(const std::string& path, int mode, bool create, bool trunc);
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
    std::string from_sjis(const std::string& s);
    std::string to_sjis(const std::string& s);
}

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
static inline void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

namespace fatfs {

struct Vol {
    bool ok = false;
    uint32_t change = 0xFFFFFFFFu;
    uint32_t bps = 1024, spc = 1, res = 1, nfat = 2, rootent = 192, total = 1232, spf = 2;
    uint32_t root_lba = 0, root_secs = 0, data_lba = 0, nclus = 0;
    bool fat16 = false;
    std::vector<uint8_t> fat;         // 1 つ目の FAT の中身
    std::set<uint32_t> dirty;         // 書き戻す FAT のセクタ番号（FAT 内の番号）
};
// ドライブ（ユニット）ごとの状態。各入口でパスのドライブ名かハンドルから s_u を選ぶ
static Vol Vs[2];
static int s_u = 0;
#define V (Vs[s_u])

struct Fh {                          // 開いているファイル
    uint32_t magic = 0x46415446;
    uint32_t ent_lba = 0, ent_off = 0;   // ディレクトリ項目の場所
    uint32_t first = 0, size = 0, pos = 0;
    int mode = 0;
    uint32_t change = 0;
    int unit = 0;
};
static std::set<void*> s_handles;

void reset() { Vs[0] = Vol(); Vs[1] = Vol(); }

static FloppyImage* img() { return floppy::image_unit(s_u); }
static int unit_of_vpath(const std::string& p) { return p.size() >= 2 && p[0] == '\x01' ? floppy::unit_of_letter(p[1]) : -1; }

static bool mount() {
    floppy::poll_change();   // 実機のディスクが入れ替わっていれば、ここで変更回数が増えて読み直す
    FloppyImage* im = img();
    if (!im) { V.ok = false; return false; }
    if (V.ok && V.change == floppy::change_count_unit(s_u)) return true;
    V = Vol();
    V.change = floppy::change_count_unit(s_u);
    std::vector<uint8_t> b((size_t)im->lsec_size);
    if (!im->read_lba(0, b.data())) return false;
    uint32_t bps = rd16(&b[11]), spc = b[13], res = rd16(&b[14]), nfat = b[16], rootent = rd16(&b[17]);
    uint32_t total = rd16(&b[19]); if (!total && b.size() >= 36) total = rd32(&b[32]);
    uint32_t spf = rd16(&b[22]);
    bool bpb = bps == (uint32_t)im->lsec_size && spc && !(spc & (spc - 1)) && spc <= 64 && res >= 1 && nfat >= 1 && nfat <= 2 &&
               rootent && spf && total;
    if (!bpb) {
        // BPB が無い古い形式: 大きさから決める
        uint32_t ss = (uint32_t)im->lsec_size, tot = im->lsec_total;
        bps = ss; res = 1; nfat = 2; total = tot;
        if (ss == 1024) { spc = 1; rootent = 192; spf = 2; }
        else if (tot >= 2800) { spc = 1; rootent = 224; spf = 9; }
        else if (tot >= 2300) { spc = 1; rootent = 224; spf = 7; }
        else if (tot >= 1400) { spc = 2; rootent = 112; spf = 3; }
        else { spc = 2; rootent = 112; spf = 2; }
    }
    V.bps = bps; V.spc = spc; V.res = res; V.nfat = nfat; V.rootent = rootent; V.total = total; V.spf = spf;
    V.root_lba = res + nfat * spf;
    V.root_secs = (rootent * 32 + bps - 1) / bps;
    V.data_lba = V.root_lba + V.root_secs;
    if (V.total <= V.data_lba) return false;
    V.nclus = (V.total - V.data_lba) / spc;
    V.fat16 = V.nclus >= 4085;
    V.fat.resize((size_t)spf * bps);
    for (uint32_t i = 0; i < spf; i++) im->read_lba(res + i, &V.fat[(size_t)i * bps]);
    V.ok = true;
    return true;
}

// ---- FAT --------------------------------------------------------------------------
static uint32_t fat_get(uint32_t cl) {
    if (V.fat16) { size_t o = (size_t)cl * 2; return o + 1 < V.fat.size() ? rd16(&V.fat[o]) : 0xFFFF; }
    size_t o = (size_t)cl * 3 / 2;
    if (o + 1 >= V.fat.size()) return 0xFFF;
    uint16_t v = rd16(&V.fat[o]);
    return (cl & 1) ? (v >> 4) : (v & 0xFFF);
}
static void fat_set(uint32_t cl, uint32_t val) {
    if (V.fat16) {
        size_t o = (size_t)cl * 2; if (o + 1 >= V.fat.size()) return;
        wr16(&V.fat[o], (uint16_t)val);
        V.dirty.insert((uint32_t)(o / V.bps)); V.dirty.insert((uint32_t)((o + 1) / V.bps));
        return;
    }
    size_t o = (size_t)cl * 3 / 2; if (o + 1 >= V.fat.size()) return;
    uint16_t v = rd16(&V.fat[o]);
    if (cl & 1) v = (uint16_t)((v & 0x000F) | ((val & 0xFFF) << 4));
    else v = (uint16_t)((v & 0xF000) | (val & 0xFFF));
    wr16(&V.fat[o], v);
    V.dirty.insert((uint32_t)(o / V.bps)); V.dirty.insert((uint32_t)((o + 1) / V.bps));
}
static bool is_eoc(uint32_t v) { return V.fat16 ? v >= 0xFFF8 : v >= 0xFF8; }
static uint32_t eoc() { return V.fat16 ? 0xFFFF : 0xFFF; }
static void fat_flush() {
    FloppyImage* im = img();
    if (!im) return;
    for (uint32_t s : V.dirty) {
        if (s >= V.spf) continue;
        for (uint32_t f = 0; f < V.nfat; f++) im->write_lba(V.res + f * V.spf + s, &V.fat[(size_t)s * V.bps]);
    }
    V.dirty.clear();
}
static uint32_t alloc_cluster(uint32_t prev) {
    for (uint32_t cl = 2; cl < V.nclus + 2; cl++) {
        if (fat_get(cl) == 0) {
            fat_set(cl, eoc());
            if (prev >= 2) fat_set(prev, cl);
            // 中身を 0 で埋める（ディレクトリ用。ファイルは上書きされる）
            std::vector<uint8_t> z((size_t)V.bps, 0);
            for (uint32_t i = 0; i < V.spc; i++) img()->write_lba(V.data_lba + (cl - 2) * V.spc + i, z.data());
            return cl;
        }
    }
    return 0;
}
static void free_chain(uint32_t cl) {
    for (int guard = 0; cl >= 2 && cl < V.nclus + 2 && guard < 70000; guard++) {
        uint32_t nx = fat_get(cl);
        fat_set(cl, 0);
        if (is_eoc(nx) || nx < 2) break;
        cl = nx;
    }
}
static inline uint32_t clus_lba(uint32_t cl) { return V.data_lba + (cl - 2) * V.spc; }

// ---- ディレクトリ --------------------------------------------------------------------
struct Ent {
    uint32_t lba, off;       // 項目の場所
    uint8_t raw[32];
};
// dir_cl = 0 はルート。コールバックが true を返したら止める
template <class F> static bool dir_walk(uint32_t dir_cl, F fn) {
    FloppyImage* im = img();
    std::vector<uint8_t> b((size_t)V.bps);
    auto scan = [&](uint32_t lba) -> int {   // 1=止めた 2=終端 0=続ける
        im->read_lba(lba, b.data());
        for (uint32_t o = 0; o + 32 <= V.bps; o += 32) {
            Ent e; e.lba = lba; e.off = o; memcpy(e.raw, &b[o], 32);
            if (e.raw[0] == 0x00) { if (fn(e, true)) return 1; return 2; }
            if (fn(e, false)) return 1;
        }
        return 0;
    };
    if (dir_cl == 0) {
        for (uint32_t i = 0; i < V.root_secs; i++) { int r = scan(V.root_lba + i); if (r == 1) return true; if (r == 2) return false; }
        return false;
    }
    uint32_t cl = dir_cl;
    for (int guard = 0; cl >= 2 && cl < V.nclus + 2 && guard < 70000; guard++) {
        for (uint32_t i = 0; i < V.spc; i++) { int r = scan(clus_lba(cl) + i); if (r == 1) return true; if (r == 2) return false; }
        uint32_t nx = fat_get(cl);
        if (is_eoc(nx)) break;
        cl = nx;
    }
    return false;
}
static std::string ent_name(const uint8_t* r) {   // "BASE.EXT"（シフト JIS）
    std::string base((const char*)r, 8), ext((const char*)r + 8, 3);
    if ((uint8_t)base[0] == 0x05) base[0] = (char)0xE5;
    while (!base.empty() && base.back() == ' ') base.pop_back();
    while (!ext.empty() && ext.back() == ' ') ext.pop_back();
    return ext.empty() ? base : base + "." + ext;
}
static std::string upper(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); i++) {
        uint8_t c = (uint8_t)r[i];
        if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) { i++; continue; }
        if (c >= 'a' && c <= 'z') r[i] = (char)(c - 32);
    }
    return r;
}
static bool make83(const std::string& sj, uint8_t out[11]) {
    std::string n = upper(sj);
    if (n.empty() || n == "." || n == "..") return false;
    size_t dot = n.find('.');
    std::string base = dot == std::string::npos ? n : n.substr(0, dot), ext = dot == std::string::npos ? "" : n.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3) return false;
    memset(out, ' ', 11);
    memcpy(out, base.data(), base.size());
    memcpy(out + 8, ext.data(), ext.size());
    if (out[0] == 0xE5) out[0] = 0x05;
    return true;
}
static bool usable(const Ent& e) { return e.raw[0] != 0xE5 && e.raw[11] != 0x0F && !(e.raw[11] & 0x08); }

static bool find_in(uint32_t dir_cl, const std::string& sjname, Ent* out) {
    std::string want = upper(sjname);
    return dir_walk(dir_cl, [&](const Ent& e, bool end) {
        if (end || !usable(e)) return false;
        if (upper(ent_name(e.raw)) == want) { *out = e; return true; }
        return false;
    });
}
static uint32_t ent_cluster(const uint8_t* r) { return rd16(r + 26); }
static void ent_write(const Ent& e) {
    std::vector<uint8_t> b((size_t)V.bps);
    img()->read_lba(e.lba, b.data());
    memcpy(&b[e.off], e.raw, 32);
    img()->write_lba(e.lba, b.data());
}
static void now_dos(uint16_t* d, uint16_t* t) {
    time_t tt = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    *d = (uint16_t)(((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
    *t = (uint16_t)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
}
// 空いた項目を探す（無ければサブディレクトリを 1 クラスタ伸ばす）
static bool free_slot(uint32_t dir_cl, Ent* out) {
    bool found = dir_walk(dir_cl, [&](const Ent& e, bool end) {
        if (end || e.raw[0] == 0xE5) { *out = e; return true; }
        return false;
    });
    if (found) return true;
    if (dir_cl == 0) return false;
    uint32_t last = dir_cl;
    for (int g = 0; g < 70000; g++) { uint32_t nx = fat_get(last); if (is_eoc(nx) || nx < 2) break; last = nx; }
    uint32_t nc = alloc_cluster(last);
    if (!nc) return false;
    fat_flush();
    out->lba = clus_lba(nc); out->off = 0; memset(out->raw, 0, 32);
    return true;
}

// ---- パス ------------------------------------------------------------------------
bool is_vpath(const std::string& p) { return p.size() >= 2 && p[0] == '\x01'; }
std::string root_path(char drive) { return std::string("\x01") + drive; }

// 仮想パス → 要素（シフト JIS）。ドライブ名が今のフロッピーでなければ false
static bool split(const std::string& p, std::vector<std::string>& comps) {
    if (!is_vpath(p)) return false;
    char d = p[1];
    int u = floppy::unit_of_letter(d);
    if (u < 0) return false;
    s_u = u;
    if (!mount()) return false;
    comps.clear();
    std::string cur;
    for (size_t i = 2; i <= p.size(); i++) {
        char c = i < p.size() ? p[i] : '\\';
        if (c == '\\' || c == '/') { if (!cur.empty()) comps.push_back(hostfs_native::to_sjis(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    return true;
}
// 要素をたどる。last_parent=true なら最後の要素の親まで。dir_cl にそのディレクトリ
static bool walk(const std::vector<std::string>& comps, size_t upto, uint32_t* dir_cl) {
    uint32_t cl = 0;
    for (size_t i = 0; i < upto; i++) {
        Ent e;
        if (!find_in(cl, comps[i], &e) || !(e.raw[11] & 0x10)) return false;
        cl = ent_cluster(e.raw);
    }
    *dir_cl = cl;
    return true;
}
static bool lookup(const std::string& path, Ent* e, bool* is_root) {
    std::vector<std::string> c;
    if (!split(path, c)) return false;
    if (c.empty()) { if (is_root) *is_root = true; return true; }
    if (is_root) *is_root = false;
    uint32_t dir;
    if (!walk(c, c.size() - 1, &dir)) return false;
    return find_in(dir, c.back(), e);
}

bool list_dir(const std::string& dir, std::vector<HostDirEntry>& out) {
    std::vector<std::string> c;
    if (!split(dir, c)) return false;
    uint32_t cl;
    if (!walk(c, c.size(), &cl)) return false;
    dir_walk(cl, [&](const Ent& e, bool end) {
        if (end) return false;
        if (!usable(e)) return false;
        std::string n = ent_name(e.raw);
        if (n == "." || n == ".." || n.empty()) return false;
        HostDirEntry h;
        h.name = hostfs_native::from_sjis(n);
        h.is_dir = (e.raw[11] & 0x10) != 0;
        h.size = rd32(e.raw + 28);
        h.readonly = (e.raw[11] & 0x01) != 0;
        h.dos_time = rd16(e.raw + 22); h.dos_date = rd16(e.raw + 24);
        out.push_back(h);
        return false;
    });
    return true;
}
bool stat(const std::string& path, HostDirEntry& h) {
    Ent e; bool root = false;
    if (!lookup(path, &e, &root)) return false;
    if (root) { h.name = ""; h.is_dir = true; h.size = 0; h.readonly = false; h.dos_date = 0x21; h.dos_time = 0; return true; }
    h.name = hostfs_native::from_sjis(ent_name(e.raw));
    h.is_dir = (e.raw[11] & 0x10) != 0;
    h.size = rd32(e.raw + 28);
    h.readonly = (e.raw[11] & 0x01) != 0;
    h.dos_time = rd16(e.raw + 22); h.dos_date = rd16(e.raw + 24);
    return true;
}

bool is_handle(void* h) { return h && s_handles.count(h) != 0; }

static bool create_entry(const std::string& path, uint8_t attr, Ent* out) {
    std::vector<std::string> c;
    if (!split(path, c) || c.empty()) return false;
    if (img()->wprot) return false;
    uint32_t dir;
    if (!walk(c, c.size() - 1, &dir)) return false;
    uint8_t nm[11];
    if (!make83(c.back(), nm)) return false;
    Ent e;
    if (!free_slot(dir, &e)) return false;
    memset(e.raw, 0, 32);
    memcpy(e.raw, nm, 11);
    e.raw[11] = attr;
    uint16_t d, t; now_dos(&d, &t);
    wr16(e.raw + 22, t); wr16(e.raw + 24, d);
    ent_write(e);
    *out = e;
    return true;
}

void* open(const std::string& path, int mode, bool create, bool trunc) {
    Ent e; bool root = false;
    bool exists = lookup(path, &e, &root);
    if (root) return nullptr;
    if (exists && (e.raw[11] & 0x10)) return nullptr;
    if (!exists) {
        if (!create) return nullptr;
        if (!create_entry(path, 0x20, &e)) return nullptr;
    } else if (trunc) {
        if (img()->wprot) return nullptr;
        free_chain(ent_cluster(e.raw)); fat_flush();
        wr16(e.raw + 26, 0); wr32(e.raw + 28, 0);
        ent_write(e);
    }
    if (mode != 0 && img()->wprot) mode = 0;
    Fh* f = new Fh();
    f->ent_lba = e.lba; f->ent_off = e.off;
    f->first = ent_cluster(e.raw); f->size = rd32(e.raw + 28); f->pos = 0; f->mode = mode;
    f->change = floppy::change_count_unit(s_u);
    f->unit = s_u;
    s_handles.insert(f);
    return f;
}
void close(void* h) { s_handles.erase(h); delete (Fh*)h; }

static bool fh_ok(Fh* f) { s_u = f->unit; return mount() && f->change == floppy::change_count_unit(s_u); }
// ファイルの k 番目のクラスタ（grow なら足りないぶんを確保）
static uint32_t nth_cluster(Fh* f, uint32_t k, bool grow) {
    if (f->first < 2) {
        if (!grow) return 0;
        uint32_t nc = alloc_cluster(0);
        if (!nc) return 0;
        f->first = nc;
    }
    uint32_t cl = f->first;
    for (uint32_t i = 0; i < k; i++) {
        uint32_t nx = fat_get(cl);
        if (is_eoc(nx) || nx < 2) {
            if (!grow) return 0;
            nx = alloc_cluster(cl);
            if (!nx) return 0;
        }
        cl = nx;
    }
    return cl;
}
static void fh_update_entry(Fh* f) {
    std::vector<uint8_t> b((size_t)V.bps);
    img()->read_lba(f->ent_lba, b.data());
    uint8_t* r = &b[f->ent_off];
    wr16(r + 26, (uint16_t)f->first); wr32(r + 28, f->size);
    uint16_t d, t; now_dos(&d, &t);
    wr16(r + 22, t); wr16(r + 24, d);
    img()->write_lba(f->ent_lba, b.data());
}

int read(void* h, void* buf, int n) {
    Fh* f = (Fh*)h;
    if (!fh_ok(f)) return -1;
    if (f->pos >= f->size || n <= 0) return 0;
    uint32_t left = std::min<uint32_t>((uint32_t)n, f->size - f->pos);
    uint32_t csz = V.bps * V.spc, done = 0;
    std::vector<uint8_t> sec((size_t)V.bps);
    while (left > 0) {
        uint32_t cl = nth_cluster(f, f->pos / csz, false);
        if (!cl) break;
        uint32_t in = f->pos % csz;
        uint32_t lba = clus_lba(cl) + in / V.bps, so = in % V.bps;
        img()->read_lba(lba, sec.data());
        uint32_t k = std::min(left, V.bps - so);
        memcpy((uint8_t*)buf + done, &sec[so], k);
        done += k; left -= k; f->pos += k;
    }
    return (int)done;
}
int write(void* h, const void* buf, int n) {
    Fh* f = (Fh*)h;
    if (!fh_ok(f) || f->mode == 0) return -1;
    if (n <= 0) return 0;
    uint32_t csz = V.bps * V.spc, done = 0, left = (uint32_t)n;
    std::vector<uint8_t> sec((size_t)V.bps);
    while (left > 0) {
        uint32_t cl = nth_cluster(f, f->pos / csz, true);
        if (!cl) break;
        uint32_t in = f->pos % csz;
        uint32_t lba = clus_lba(cl) + in / V.bps, so = in % V.bps;
        uint32_t k = std::min(left, V.bps - so);
        if (k < V.bps) img()->read_lba(lba, sec.data());
        memcpy(&sec[so], (const uint8_t*)buf + done, k);
        img()->write_lba(lba, sec.data());
        done += k; left -= k; f->pos += k;
        if (f->pos > f->size) f->size = f->pos;
    }
    fat_flush();
    fh_update_entry(f);
    return (int)done;
}
int64_t seek(void* h, int64_t off, int whence) {
    Fh* f = (Fh*)h;
    int64_t np = whence == 0 ? off : whence == 1 ? (int64_t)f->pos + off : (int64_t)f->size + off;
    if (np < 0) return -1;
    f->pos = (uint32_t)np;
    return np;
}
bool truncate_here(void* h) {
    Fh* f = (Fh*)h;
    if (!fh_ok(f) || f->mode == 0) return false;
    uint32_t csz = V.bps * V.spc;
    if (f->pos < f->size) {
        uint32_t keep = (f->pos + csz - 1) / csz;
        if (keep == 0) { free_chain(f->first); f->first = 0; }
        else {
            uint32_t last = nth_cluster(f, keep - 1, false);
            if (last) { uint32_t nx = fat_get(last); fat_set(last, eoc()); if (!is_eoc(nx) && nx >= 2) free_chain(nx); }
        }
        f->size = f->pos;
    } else if (f->pos > f->size) {
        // 伸ばす（中身は 0）
        std::vector<uint8_t> z(4096, 0);
        uint32_t target = f->pos; f->pos = f->size;
        while (f->pos < target) { uint32_t k = std::min<uint32_t>(4096, target - f->pos); if (write(h, z.data(), (int)k) != (int)k) break; }
    }
    fat_flush();
    fh_update_entry(f);
    return true;
}
bool remove(const std::string& path) {
    Ent e; bool root = false;
    if (!lookup(path, &e, &root) || root || (e.raw[11] & 0x10) || img()->wprot) return false;
    free_chain(ent_cluster(e.raw)); fat_flush();
    e.raw[0] = 0xE5; ent_write(e);
    return true;
}
bool mkdir(const std::string& path) {
    Ent e; bool root = false;
    if (lookup(path, &e, &root)) return false;
    std::vector<std::string> c;
    if (!split(path, c) || c.empty()) return false;
    uint32_t parent;
    if (!walk(c, c.size() - 1, &parent)) return false;
    uint32_t nc = alloc_cluster(0);
    if (!nc) return false;
    fat_flush();
    if (!create_entry(path, 0x10, &e)) { free_chain(nc); fat_flush(); return false; }
    wr16(e.raw + 26, (uint16_t)nc); ent_write(e);
    std::vector<uint8_t> b((size_t)V.bps, 0);
    memset(&b[0], ' ', 11); b[0] = '.'; b[11] = 0x10; wr16(&b[26], (uint16_t)nc);
    memset(&b[32], ' ', 11); b[32] = '.'; b[33] = '.'; b[43] = 0x10; wr16(&b[58], (uint16_t)parent);
    memcpy(&b[22], e.raw + 22, 4); memcpy(&b[54], e.raw + 22, 4);
    img()->write_lba(clus_lba(nc), b.data());
    return true;
}
bool rmdir(const std::string& path) {
    Ent e; bool root = false;
    if (!lookup(path, &e, &root) || root || !(e.raw[11] & 0x10) || img()->wprot) return false;
    uint32_t cl = ent_cluster(e.raw);
    bool empty = !dir_walk(cl, [&](const Ent& x, bool end) {
        if (end || !usable(x)) return false;
        std::string n = ent_name(x.raw);
        return n != "." && n != "..";
    });
    if (!empty) return false;
    free_chain(cl); fat_flush();
    e.raw[0] = 0xE5; ent_write(e);
    return true;
}
bool rename(const std::string& a, const std::string& b) {
    if (unit_of_vpath(a) != unit_of_vpath(b)) return false;   // ドライブをまたぐ名前の変更はできない
    Ent ea, eb; bool ra = false, rb = false;
    if (!lookup(a, &ea, &ra) || ra || img()->wprot) return false;
    if (lookup(b, &eb, &rb)) return false;
    Ent ne;
    if (!create_entry(b, ea.raw[11], &ne)) return false;
    // 名前以外をそのまま写す（作った項目の場所に、元の項目の中身を置く）
    uint8_t nm[11]; memcpy(nm, ne.raw, 11);
    memcpy(ne.raw, ea.raw, 32); memcpy(ne.raw, nm, 11);
    ent_write(ne);
    // ディレクトリを別の親へ動かしたら ".." を直す
    if (ea.raw[11] & 0x10) {
        std::vector<std::string> c; split(b, c);
        uint32_t parent = 0; walk(c, c.size() - 1, &parent);
        uint32_t cl = ent_cluster(ea.raw);
        dir_walk(cl, [&](const Ent& x, bool end) {
            if (end) return true;
            if (ent_name(x.raw) == "..") { Ent y = x; wr16(y.raw + 26, (uint16_t)parent); ent_write(y); return true; }
            return false;
        });
    }
    ea.raw[0] = 0xE5; ent_write(ea);
    return true;
}
bool set_time(void* h, uint16_t d, uint16_t t) {
    Fh* f = (Fh*)h;
    if (!fh_ok(f) || img()->wprot) return false;
    std::vector<uint8_t> b((size_t)V.bps);
    img()->read_lba(f->ent_lba, b.data());
    wr16(&b[f->ent_off + 22], t); wr16(&b[f->ent_off + 24], d);
    img()->write_lba(f->ent_lba, b.data());
    return true;
}
bool get_time(void* h, uint16_t* d, uint16_t* t) {
    Fh* f = (Fh*)h;
    if (!fh_ok(f)) return false;
    std::vector<uint8_t> b((size_t)V.bps);
    img()->read_lba(f->ent_lba, b.data());
    *t = rd16(&b[f->ent_off + 22]); *d = rd16(&b[f->ent_off + 24]);
    return true;
}
bool volume_label(std::string* name, uint16_t* date, uint16_t* time) { return volume_label_drive(floppy::drive_letter(), name, date, time); }
bool free_space(uint32_t* spc, uint32_t* bps, uint32_t* fr, uint32_t* total) { return free_space_drive(floppy::drive_letter(), spc, bps, fr, total); }
bool volume_label_drive(char drive, std::string* name, uint16_t* date, uint16_t* time) {
    int u = floppy::unit_of_letter(drive);
    if (u < 0) return false;
    s_u = u;
    if (!mount()) return false;
    bool found = false;
    dir_walk(0, [&](const Ent& e, bool end) {
        if (end) return false;
        if (e.raw[0] == 0xE5 || e.raw[11] == 0x0F || !(e.raw[11] & 0x08)) return false;
        std::string n((const char*)e.raw, 11);
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (n.size() > 8) n = n.substr(0, 8) + "." + n.substr(8);   // DOS の検索と同じく 8 文字目の後に "."
        *name = n; *time = rd16(e.raw + 22); *date = rd16(e.raw + 24);
        found = true;
        return true;
    });
    return found;
}
bool free_space_drive(char drive, uint32_t* spc, uint32_t* bps, uint32_t* fr, uint32_t* total) {
    int u = floppy::unit_of_letter(drive);
    if (u < 0) return false;
    s_u = u;
    if (!mount()) return false;
    uint32_t n = 0;
    for (uint32_t cl = 2; cl < V.nclus + 2; cl++) if (fat_get(cl) == 0) n++;
    *spc = V.spc; *bps = V.bps; *fr = n; *total = V.nclus;
    return true;
}
} // namespace fatfs

// ---- hostfs の入口（仮想パス・仮想ハンドルはフロッピーへ） --------------------------------
namespace hostfs {
std::string join(const std::string& a, const std::string& b) {
    if (fatfs::is_vpath(a)) { if (a.back() == '\\' || a.back() == '/') return a + b; return a + "\\" + b; }
    return hostfs_native::join(a, b);
}
bool list_dir(const std::string& d, std::vector<HostDirEntry>& out) { return fatfs::is_vpath(d) ? fatfs::list_dir(d, out) : hostfs_native::list_dir(d, out); }
bool stat(const std::string& p, HostDirEntry& e) { return fatfs::is_vpath(p) ? fatfs::stat(p, e) : hostfs_native::stat(p, e); }
void* open(const std::string& p, int mode, bool create, bool trunc) { return fatfs::is_vpath(p) ? fatfs::open(p, mode, create, trunc) : hostfs_native::open(p, mode, create, trunc); }
void close(void* h) { if (fatfs::is_handle(h)) fatfs::close(h); else hostfs_native::close(h); }
int read(void* h, void* buf, int n) { return fatfs::is_handle(h) ? fatfs::read(h, buf, n) : hostfs_native::read(h, buf, n); }
int write(void* h, const void* buf, int n) { return fatfs::is_handle(h) ? fatfs::write(h, buf, n) : hostfs_native::write(h, buf, n); }
int64_t seek(void* h, int64_t off, int whence) { return fatfs::is_handle(h) ? fatfs::seek(h, off, whence) : hostfs_native::seek(h, off, whence); }
bool truncate_here(void* h) { return fatfs::is_handle(h) ? fatfs::truncate_here(h) : hostfs_native::truncate_here(h); }
bool remove(const std::string& p) { return fatfs::is_vpath(p) ? fatfs::remove(p) : hostfs_native::remove(p); }
bool rename(const std::string& a, const std::string& b) {
    bool va = fatfs::is_vpath(a), vb = fatfs::is_vpath(b);
    if (va != vb) return false;   // ドライブをまたぐ移動はできない（DOS でも同じ）
    return va ? fatfs::rename(a, b) : hostfs_native::rename(a, b);
}
bool mkdir(const std::string& p) { return fatfs::is_vpath(p) ? fatfs::mkdir(p) : hostfs_native::mkdir(p); }
bool rmdir(const std::string& p) { return fatfs::is_vpath(p) ? fatfs::rmdir(p) : hostfs_native::rmdir(p); }
bool set_time(void* h, uint16_t d, uint16_t t) { return fatfs::is_handle(h) ? fatfs::set_time(h, d, t) : hostfs_native::set_time(h, d, t); }
bool get_time(void* h, uint16_t* d, uint16_t* t) { return fatfs::is_handle(h) ? fatfs::get_time(h, d, t) : hostfs_native::get_time(h, d, t); }
std::string from_sjis(const std::string& s) { return hostfs_native::from_sjis(s); }
std::string to_sjis(const std::string& s) { return hostfs_native::to_sjis(s); }
}
