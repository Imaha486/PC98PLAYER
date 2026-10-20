// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  isofs.cpp  --  CD-ROM のイメージ（ISO 9660）を読み出し専用のドライブとして見せる
//  設計は isofs.h に。ディレクトリの木は差し込んだときに全部読んでおく（CD は変わらないので）。
// -----------------------------------------------------------------------------
#include "isofs.h"
#include "hostfs.h"
#include <string.h>
#include <set>

namespace hostfs_native {
    void* open(const std::string& path, int mode, bool create, bool trunc);
    void  close(void* h);
    int   read(void* h, void* buf, int n);
    int64_t seek(void* h, int64_t off, int whence);
    std::string from_sjis(const std::string& s);
    std::string to_sjis(const std::string& s);
}

namespace isofs {

struct Node {
    std::string name;      // シフト JIS（大文字）
    bool is_dir = false;
    uint32_t lba = 0, size = 0;
    uint16_t date = 0x21, time = 0;
    int parent = -1;
    std::vector<int> kids;
};
struct Fh { uint32_t magic = 0x49534F46; int node = 0; uint32_t pos = 0; };

static void* s_img = nullptr;
static std::string s_path;
static uint32_t s_secsize = 2048, s_secoff = 0;   // イメージの 1 セクタの大きさと、その中のデータの位置
static uint32_t s_volsecs = 0;
static int64_t s_imgsize = 0;
static int64_t s_base = 0;    // データトラックの先頭のファイル内の位置（CUE のとき）
static std::string s_label;
static std::vector<Node> s_nodes;
static std::set<void*> s_handles;

static inline uint32_t rd32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

bool read_sector(uint32_t lba, uint8_t* out) {
    if (!s_img) return false;
    int64_t off = s_base + (int64_t)lba * s_secsize + s_secoff;
    if (off + 2048 > s_imgsize) return false;
    if (hostfs_native::seek(s_img, off, 0) != off) return false;
    return hostfs_native::read(s_img, out, 2048) == 2048;
}

static uint16_t dos_date_of(const uint8_t* t) {   // ISO の日時（1900 年からの年, 月, 日, 時, 分, 秒）
    int y = t[0] + 1900; if (y < 1980) y = 1980; if (y > 2107) y = 2107;
    int mo = t[1] ? t[1] : 1, d = t[2] ? t[2] : 1;
    return (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
}
static uint16_t dos_time_of(const uint8_t* t) { return (uint16_t)((t[3] << 11) | (t[4] << 5) | (t[5] / 2)); }

static void read_dir(int idx, int depth) {
    if (depth > 16) return;
    uint32_t lba = s_nodes[idx].lba, len = s_nodes[idx].size;
    std::vector<uint8_t> sec(2048);
    for (uint32_t done = 0; done < len; done += 2048, lba++) {
        if (!read_sector(lba, sec.data())) return;
        for (uint32_t o = 0; o < 2048;) {
            uint8_t rl = sec[o];
            if (rl == 0 || o + rl > 2048 || rl < 34) break;   // このセクタの残りは使わない
            const uint8_t* r = &sec[o];
            uint8_t nl = r[32];
            std::string n((const char*)r + 33, nl < (uint8_t)(rl - 33) ? nl : (uint8_t)(rl - 33));
            o += rl;
            if (nl == 1 && (n[0] == 0 || n[0] == 1)) continue;   // . と ..
            if (r[25] & 0x01) continue;                          // 隠し（関連ファイル）
            size_t sc = n.find(';'); if (sc != std::string::npos) n = n.substr(0, sc);
            if (!n.empty() && n.back() == '.') n.pop_back();
            for (auto& c : n) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (n.empty()) continue;
            Node k;
            k.name = n; k.is_dir = (r[25] & 0x02) != 0;
            k.lba = rd32(r + 2); k.size = rd32(r + 10);
            k.date = dos_date_of(r + 18); k.time = dos_time_of(r + 18);
            k.parent = idx;
            bool dup = false;
            for (int j : s_nodes[idx].kids) if (s_nodes[j].name == k.name) { dup = true; break; }
            if (dup) continue;
            s_nodes.push_back(k);
            int ki = (int)s_nodes.size() - 1;
            s_nodes[idx].kids.push_back(ki);
            if (k.is_dir) read_dir(ki, depth + 1);
        }
    }
}

void unmount() {
    for (void* h : s_handles) delete (Fh*)h;
    s_handles.clear();
    if (s_img) hostfs_native::close(s_img);
    s_img = nullptr; s_path.clear(); s_nodes.clear(); s_label.clear(); s_volsecs = 0; s_base = 0;
}
bool mounted() { return s_img != nullptr; }
const std::string& image_path() { return s_path; }
std::string volume_label() { return s_label; }
uint32_t volume_sectors() { return s_volsecs; }
uint32_t image_sectors() { return s_img && s_secsize ? (uint32_t)((s_imgsize - s_base) / s_secsize) : 0; }

bool mount(const std::string& host_path, std::string* err, uint32_t sec_size, uint32_t sec_off, int64_t base) {
    unmount();
    void* h = hostfs_native::open(host_path, 0, false, false);
    if (!h) { if (err) *err = "開けません"; return false; }
    s_img = h;
    s_imgsize = hostfs_native::seek(h, 0, 2);
    // 2048 バイト/セクタの .ISO か、2352 バイト/セクタの生のトラック（.BIN。モード 1 は 16 バイト目から、
    // モード 2 の XA は 24 バイト目から）かを、セクタ 16 の主ボリューム記述子で見分ける
    static const struct { uint32_t size, off; } kinds[] = { {2048, 0}, {2352, 16}, {2352, 24}, {2336, 8} };
    bool found = false;
    uint8_t pvd[2048];
    s_base = base;
    for (auto& k : kinds) {
        s_secsize = sec_size ? sec_size : k.size; s_secoff = sec_size ? sec_off : k.off;
        if (read_sector(16, pvd) && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5)) { found = true; break; }
    }
    if (!found) { unmount(); if (err) *err = "ISO 9660 のイメージではありません"; return false; }
    s_path = host_path;
    s_volsecs = rd32(pvd + 80);
    s_label.assign((const char*)pvd + 40, 32);
    while (!s_label.empty() && (s_label.back() == ' ' || s_label.back() == 0)) s_label.pop_back();
    const uint8_t* root = pvd + 156;
    Node r; r.name = ""; r.is_dir = true; r.lba = rd32(root + 2); r.size = rd32(root + 10);
    r.date = dos_date_of(root + 18); r.time = dos_time_of(root + 18);
    s_nodes.push_back(r);
    read_dir(0, 0);
    return true;
}

bool is_vpath(const std::string& p) { return !p.empty() && p[0] == '\x02'; }
std::string root_path() { return std::string("\x02"); }

// 仮想パス → 節の番号（-1 = 無い）
static int lookup(const std::string& p) {
    if (!mounted() || !is_vpath(p)) return -1;
    int cur = 0;
    std::string c;
    for (size_t i = 1; i <= p.size(); i++) {
        char ch = i < p.size() ? p[i] : '\\';
        if (ch == '\\' || ch == '/') {
            if (!c.empty()) {
                std::string g = hostfs_native::to_sjis(c);
                for (auto& x : g) if (x >= 'a' && x <= 'z') x = (char)(x - 32);
                int hit = -1;
                if (!s_nodes[cur].is_dir) return -1;
                for (int k : s_nodes[cur].kids) if (s_nodes[k].name == g) { hit = k; break; }
                if (hit < 0) return -1;
                cur = hit;
            }
            c.clear();
        } else c.push_back(ch);
    }
    return cur;
}
static void fill(const Node& n, HostDirEntry& e) {
    e.name = hostfs_native::from_sjis(n.name);
    e.is_dir = n.is_dir; e.size = n.is_dir ? 0 : n.size;
    e.readonly = true; e.attr = n.is_dir ? 0x01 : 0x21;   // CD のファイルは読み取り専用
    e.dos_date = n.date; e.dos_time = n.time;
}
bool list_dir(const std::string& dir, std::vector<HostDirEntry>& out) {
    int i = lookup(dir);
    if (i < 0 || !s_nodes[i].is_dir) return false;
    for (int k : s_nodes[i].kids) { HostDirEntry e; fill(s_nodes[k], e); out.push_back(e); }
    return true;
}
bool stat(const std::string& path, HostDirEntry& e) {
    int i = lookup(path);
    if (i < 0) return false;
    fill(s_nodes[i], e);
    return true;
}
bool is_handle(void* h) { return h && s_handles.count(h) != 0; }
void* open(const std::string& path, int mode, bool create, bool trunc) {
    if (mode != 0 || create || trunc) return nullptr;
    int i = lookup(path);
    if (i < 0 || s_nodes[i].is_dir) return nullptr;
    Fh* f = new Fh(); f->node = i;
    s_handles.insert(f);
    return f;
}
void close(void* h) { if (is_handle(h)) { s_handles.erase(h); delete (Fh*)h; } }
int read(void* h, void* buf, int n) {
    if (!is_handle(h) || n < 0) return -1;
    Fh* f = (Fh*)h; const Node& nd = s_nodes[f->node];
    if (f->pos >= nd.size) return 0;
    uint32_t want = (uint32_t)n; if (want > nd.size - f->pos) want = nd.size - f->pos;
    uint8_t sec[2048]; uint32_t got = 0;
    while (got < want) {
        uint32_t p = f->pos + got, so = p % 2048, take = 2048 - so;
        if (take > want - got) take = want - got;
        if (!read_sector(nd.lba + p / 2048, sec)) break;
        memcpy((uint8_t*)buf + got, sec + so, take);
        got += take;
    }
    f->pos += got;
    return (int)got;
}
int64_t seek(void* h, int64_t off, int whence) {
    if (!is_handle(h)) return -1;
    Fh* f = (Fh*)h;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->pos : (int64_t)s_nodes[f->node].size;
    int64_t np = base + off;
    if (np < 0) return -1;
    f->pos = (uint32_t)np;
    return np;
}
bool get_time(void* h, uint16_t* d, uint16_t* t) {
    if (!is_handle(h)) return false;
    const Node& n = s_nodes[((Fh*)h)->node];
    *d = n.date; *t = n.time;
    return true;
}

static std::string s_ro_folder;
void set_readonly_folder(const std::string& d) { s_ro_folder = d; while (s_ro_folder.size() > 1 && (s_ro_folder.back() == '\\' || s_ro_folder.back() == '/')) s_ro_folder.pop_back(); }
bool in_readonly_folder(const std::string& p) {
    if (s_ro_folder.empty() || p.size() < s_ro_folder.size()) return false;
    for (size_t i = 0; i < s_ro_folder.size(); i++) {
        char a = p[i], b = s_ro_folder[i];
        if (a == '/') a = '\\'; if (b == '/') b = '\\';
#ifdef _WIN32
        if (a >= 'a' && a <= 'z') a = (char)(a - 32); if (b >= 'a' && b <= 'z') b = (char)(b - 32);
#endif
        if (a != b) return false;
    }
    return p.size() == s_ro_folder.size() || p[s_ro_folder.size()] == '\\' || p[s_ro_folder.size()] == '/';
}

} // namespace isofs
