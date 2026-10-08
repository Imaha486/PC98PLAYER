// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  dos.cpp  --  MS-DOS の HLE とバッチ実行（COMMAND.COM の代わり）
//
//  ・ドライブ（既定 A:）はホストの「ゲームのフォルダ」そのもの
//  ・メモリ管理は本物と同じ MCB の鎖をゲスト側のメモリに作る（ゲームが MCB を
//    直接たどることがあるため）
//  ・EXEC / 常駐終了（TSR）を扱う。サウンドドライバを常駐させてからゲームを
//    起動する、いつもの PC-98 のバッチがそのまま動く
// -----------------------------------------------------------------------------
#include "machine.h"
#include "hostfs.h"
#include "floppy.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <map>
#include <algorithm>

void bios_hook_vec(Machine* m, int vec, uint8_t n);
uint16_t bios_hle_stub(Machine* m, uint8_t n);

#define ROMSEG 0xF000
static const uint16_t DOSSEG      = 0x0080;   // DOS のデータ（0x800〜。0x600-0x7FF は PC-98 の DOS ワークエリア）
// 先頭 MCB の位置。実機の MS-DOS では本体・バッファ・デバイスドライバの後ろになるので
// 0x0100 のような低い位置にはまず来ない。自分のロード位置から領域の大きさを計算する
// ソフト（銀河英雄伝説IV EX の G4XSTART.EXE など）が破綻しないよう、少し上げておく。
static const uint16_t FIRST_MCB_DEFAULT = 0x0200;   // INI の FirstMCB= で変更できる
static uint16_t first_mcb_cfg(Machine* m) { int v = m->cfg.first_mcb; return (v >= 0x0100 && v <= 0x4000) ? (uint16_t)v : FIRST_MCB_DEFAULT; }
static const uint16_t MEM_TOP     = 0xA000;   // 640KB

// DOS データ領域内のオフセット（DOSSEG 基準）
static const uint16_t D_INDOS     = 0x0000;
static const uint16_t D_DBCS      = 0x0010;
static const uint16_t D_LOL       = 0x0300;   // LoL の手前 2 バイトが先頭 MCB（LoL は 0x66 バイトほど）
static const uint16_t D_SFT       = 0x0380;   // SFT の先頭ブロック（DOS 4+ 形式 3Bh バイト x 16）
static const int      SFT_GUEST_N = 16;
static const uint16_t D_SWITCHAR  = 0x0080;
static const uint16_t D_UPCASE    = 0x0090;   // 大文字化関数（far ret）
static const uint16_t D_COUNTRY   = 0x00A0;
static const uint16_t D_NAMEBUF   = 0x0100;
static const uint16_t D_DPB       = 0x0740;   // ゲームのドライブの DPB（21h バイト、DOS 4+ 形式）
static const uint16_t D_BLKDEV    = 0x0770;   // その DPB が指すブロック装置のヘッダ（実体は無い）

static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }
static inline uint16_t rw(Machine* m, uint32_t a) { return mem_rw(m, a); }
static inline void ww(Machine* m, uint32_t a, uint16_t v) { mem_ww(m, a, v); }

#define R(m) (m)->cpu.r
static inline uint16_t AX(Machine* m) { return (uint16_t)R(m)[EAX]; }
static inline uint16_t BX(Machine* m) { return (uint16_t)R(m)[EBX]; }
static inline uint16_t CX(Machine* m) { return (uint16_t)R(m)[ECX]; }
static inline uint16_t DX(Machine* m) { return (uint16_t)R(m)[EDX]; }
static inline uint16_t SI(Machine* m) { return (uint16_t)R(m)[ESI]; }
static inline uint16_t DI(Machine* m) { return (uint16_t)R(m)[EDI]; }
static inline uint8_t AH(Machine* m) { return (uint8_t)(R(m)[EAX] >> 8); }
static inline uint8_t AL(Machine* m) { return (uint8_t)R(m)[EAX]; }
static inline uint8_t DL(Machine* m) { return (uint8_t)R(m)[EDX]; }
static inline void SETAX(Machine* m, uint16_t v) { R(m)[EAX] = (R(m)[EAX] & 0xFFFF0000u) | v; }
static inline void SETBX(Machine* m, uint16_t v) { R(m)[EBX] = (R(m)[EBX] & 0xFFFF0000u) | v; }
static inline void SETCX(Machine* m, uint16_t v) { R(m)[ECX] = (R(m)[ECX] & 0xFFFF0000u) | v; }
static inline void SETDX(Machine* m, uint16_t v) { R(m)[EDX] = (R(m)[EDX] & 0xFFFF0000u) | v; }
static inline void SETSI(Machine* m, uint16_t v) { R(m)[ESI] = (R(m)[ESI] & 0xFFFF0000u) | v; }
static inline void SETDI(Machine* m, uint16_t v) { R(m)[EDI] = (R(m)[EDI] & 0xFFFF0000u) | v; }
static inline void SETAL(Machine* m, uint8_t v) { R(m)[EAX] = (R(m)[EAX] & 0xFFFFFF00u) | v; }
static inline void SETAH(Machine* m, uint8_t v) { R(m)[EAX] = (R(m)[EAX] & 0xFFFF00FFu) | ((uint32_t)v << 8); }
static inline uint16_t DS(Machine* m) { return m->cpu.sr[DS_]; }
static inline uint16_t ES(Machine* m) { return m->cpu.sr[ES_]; }

// ---- 状態 --------------------------------------------------------------------
struct Sft {
    int refs = 0;
    int kind = 0;           // 0=空き 1=ファイル 2=CON 3=NUL 4=AUX/PRN
    void* h = nullptr;
    std::string host;
    std::string name;
    int mode = 0;
    uint16_t owner = 0;
};
struct ExecFrame {
    uint32_t r[8];
    uint16_t sr[6];
    uint16_t ip, cs;
    uint32_t fl;
    uint16_t parent_psp, child_psp;
    uint16_t dta_seg, dta_off;
};
struct Search {
    std::vector<HostDirEntry> list;
    size_t pos = 0;
    uint8_t attr = 0;
    std::string pattern;
    bool used = false;
};

static std::vector<Sft> s_sft;
static std::vector<ExecFrame> s_frames;
static std::vector<Search> s_search;
static uint16_t s_psp, s_shell_psp;
static uint16_t s_dta_seg, s_dta_off;
static uint16_t s_retcode;
static uint16_t s_last_err;
static std::string s_cwd;      // ゲスト側のカレント（先頭の \ なし、大文字）。ゲームのドライブ
static std::string s_fdcwd;    // フロッピーのドライブのカレント
static std::string s_fdcwd2;   // 2 台目のフロッピーのドライブのカレント
// SUBST で作ったドライブ（ドライブ名 → ホストのパス）と、そのカレント
static std::map<char, std::string> s_subst;
static std::map<char, std::string> s_subst_cwd;
static char s_curdrv;          // カレントドライブ（0 = ゲームのドライブ）
static char s_sg_drive;        // 直前に split_guest が解いたパスのドライブ
static uint32_t s_fd_change = 0xFFFFFFFFu;
static uint16_t s_stub_iret;    // INT 21h の戻り（IRET）の位置
static uint16_t s_int21_off;
static uint16_t s_shell_entry;
static std::map<std::string, std::vector<HostDirEntry>> s_dircache;
static uint8_t s_break_flag;
static uint8_t s_verify;
static std::string s_pending_input;   // ファンクションキー等の展開待ち
static uint8_t s_alloc_strategy;

// ---- バッチ ------------------------------------------------------------------
struct BatchCtx {
    std::vector<std::string> lines;
    size_t pc = 0;
    std::vector<std::string> args;
};
static std::vector<BatchCtx> s_batch;
static std::map<std::string, std::string> s_env;
static bool s_shell_running;
static uint16_t s_retcode_last;
static uint16_t s_retcode_last_get() { return s_retcode_last & 0xFF; }
static int  s_shell_wait;        // 1=PAUSE のキー待ち

static void dos_error(Machine* m, uint16_t code) {
    if (m->cfg.trace) plog("[dos] AH=%02X -> error %d\n", (unsigned)(uint8_t)(m->cpu.r[EAX] >> 8), code);
    s_last_err = code;
    SETAX(m, code);
    set_cf(m, true);
}
static void dos_ok(Machine* m) { set_cf(m, false); }

// ---- 文字列 ------------------------------------------------------------------
static inline bool is_lead(uint8_t c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }
static std::string read_asciiz(Machine* m, uint16_t seg, uint16_t off, int max = 256) {
    std::string s;
    for (int i = 0; i < max; i++) {
        uint8_t c = mem_rb(m, lin(seg, (uint16_t)(off + i)));
        if (!c) break;
        s.push_back((char)c);
    }
    return s;
}
static void write_asciiz(Machine* m, uint16_t seg, uint16_t off, const std::string& s) {
    for (size_t i = 0; i < s.size(); i++) mem_wb(m, lin(seg, (uint16_t)(off + i)), (uint8_t)s[i]);
    mem_wb(m, lin(seg, (uint16_t)(off + s.size())), 0);
}
static std::string upper_dbcs(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); i++) {
        uint8_t c = (uint8_t)r[i];
        if (is_lead(c)) { i++; continue; }
        if (c >= 'a' && c <= 'z') r[i] = (char)(c - 0x20);
    }
    return r;
}
// 8.3 に丸める
static std::string to83(const std::string& comp) {
    if (comp == "." || comp == "..") return comp;
    std::string base, ext;
    size_t dot = comp.find('.');
    if (dot == std::string::npos) base = comp; else { base = comp.substr(0, dot); ext = comp.substr(dot + 1); }
    auto cut = [](const std::string& s, size_t n) {
        std::string r;
        for (size_t i = 0; i < s.size();) {
            size_t w = is_lead((uint8_t)s[i]) ? 2 : 1;
            if (r.size() + w > n) break;
            r += s.substr(i, w); i += w;
        }
        return r;
    };
    base = cut(base, 8); ext = cut(ext, 3);
    // 「GAMEKWD .TOJ」のように 8 文字ぶん空白で埋めた名前は、空白を除いた名前と同じ（DOS は名前を 8+3 の空白埋めで持つ）
    while (!base.empty() && base.back() == ' ') base.pop_back();
    while (!ext.empty() && ext.back() == ' ') ext.pop_back();
    return ext.empty() ? base : base + "." + ext;
}
static bool valid83(const std::string& name) {
    size_t dot = name.find('.');
    std::string base = dot == std::string::npos ? name : name.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3 || ext.find('.') != std::string::npos) return false;
    for (char ch : name) { if (ch == ' ') return false; }
    return true;
}

// ---- パス --------------------------------------------------------------------
static uint16_t hdd_free_clusters(Machine* m) { return (uint16_t)(m->cfg.free_mb * 64); }
static uint16_t hdd_total_clusters(Machine* m) { uint32_t t = (uint32_t)m->cfg.free_mb * 64 + 2048; if (t < 8192) t = 8192; if (t > 0xFFF0) t = 0xFFF0; return (uint16_t)t; }
static char cur_drive(Machine* m) { return s_curdrv ? s_curdrv : m->cfg.drive; }
static bool drive_valid(Machine* m, char d) { return d == m->cfg.drive || floppy::unit_of_letter(d) >= 0 || s_subst.count(d); }
static std::string& cwd_of(Machine* m, char d) {
    if (d == m->cfg.drive) return s_cwd;
    if (s_subst.count(d)) return s_subst_cwd[d];
    return floppy::unit_of_letter(d) == 1 ? s_fdcwd2 : s_fdcwd;
}
static const std::vector<HostDirEntry>& dir_list(const std::string& hostdir) {
    if (s_fd_change != floppy::change_count()) { s_fd_change = floppy::change_count(); s_dircache.clear(); }
    auto it = s_dircache.find(hostdir);
    if (it != s_dircache.end()) return it->second;
    std::vector<HostDirEntry> v;
    hostfs::list_dir(hostdir, v);
    return s_dircache[hostdir] = v;
}
static void dir_invalidate() { s_dircache.clear(); }

static bool split_guest(const std::string& in, std::vector<std::string>& comps, bool* is_dev) {
    std::string s = in;
    // 末尾の空白を除く
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t p = 0;
    char drv = cur_drive(g_m);
    if (s.size() >= 2 && s[1] == ':') {
        p = 2;
        char d = s[0];
        if (d >= 'a' && d <= 'z') d = (char)(d - 32);
        if (d >= 'A' && d <= 'Z') drv = d;
    }
    s_sg_drive = drv;
    const std::string& cwd = cwd_of(g_m, drv);
    bool abs = p < s.size() && (s[p] == '\\' || s[p] == '/');
    comps.clear();
    if (!abs && !cwd.empty()) {
        std::string c;
        for (char ch : cwd) { if (ch == '\\') { if (!c.empty()) comps.push_back(c); c.clear(); } else c.push_back(ch); }
        if (!c.empty()) comps.push_back(c);
    }
    std::string cur;
    for (size_t i = p; i <= s.size(); i++) {
        uint8_t c = i < s.size() ? (uint8_t)s[i] : 0;
        if (c && is_lead(c) && i + 1 < s.size()) { cur.push_back((char)c); cur.push_back(s[++i]); continue; }
        if (c == '\\' || c == '/' || c == 0) {
            if (cur == "..") { if (!comps.empty()) comps.pop_back(); }
            else if (cur == "." || cur.empty()) {}
            else comps.push_back(to83(upper_dbcs(cur)));
            cur.clear();
            continue;
        }
        cur.push_back((char)c);
    }
    if (is_dev) {
        *is_dev = false;
        if (!comps.empty()) {
            std::string b = comps.back();
            size_t dot = b.find('.');
            if (dot != std::string::npos) b = b.substr(0, dot);
            if (b == "CON" || b == "NUL" || b == "AUX" || b == "PRN" || b == "CLOCK$" || b == "LPT1" || b == "COM1" || (b == "EMMXXXX0" && ems_enabled(g_m)))
                *is_dev = true;
        }
    }
    return true;
}
// ゲストのパスをホストのパスへ。存在しない最後の要素は大文字のまま付ける。
static std::string drive_root(Machine* m, char d) {
    if (d == m->cfg.drive || !d) return m->cfg.root;
    { auto it = s_subst.find(d); if (it != s_subst.end()) return it->second; }   // SUBST で作ったドライブ
    if (d == floppy::drive_letter() && !floppy::folder().empty()) return floppy::folder();   // フォルダを入れたフロッピー
    return fatfs::root_path(d);   // フロッピー（それ以外のドライブ名は何も無い扱い）
}
static std::string host_of(Machine* m, const std::vector<std::string>& comps, size_t upto, bool* all_exist) {
    std::string h = drive_root(m, s_sg_drive);
    bool ok = true;
    for (size_t i = 0; i < upto && i < comps.size(); i++) {
        const auto& lst = dir_list(h);
        const HostDirEntry* hit = nullptr;
        for (const auto& e : lst) {
            std::string g = upper_dbcs(hostfs::to_sjis(e.name));
            if (g == comps[i]) { hit = &e; break; }
        }
        if (hit) h = hostfs::join(h, hit->name);
        else { h = hostfs::join(h, hostfs::from_sjis(comps[i])); ok = false; }
    }
    if (all_exist) *all_exist = ok;
    return h;
}
static std::string guest_to_host(Machine* m, const std::string& g, bool* exists, bool* is_dev, std::string* canon) {
    std::vector<std::string> c;
    split_guest(g, c, is_dev);
    if (canon) {
        std::string s;
        for (size_t i = 0; i < c.size(); i++) { if (i) s += "\\"; s += c[i]; }
        *canon = s;
    }
    return host_of(m, c, c.size(), exists);
}
static std::string dev_name(const std::string& g) {
    std::vector<std::string> c; bool dev;
    split_guest(g, c, &dev);
    if (!dev || c.empty()) return "";
    std::string b = c.back();
    size_t dot = b.find('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

// ---- MCB ---------------------------------------------------------------------
// 鎖の先頭は List of Lists の手前（RAM 上）から読む（古いステートでも正しく辿れるように）
static uint16_t first_mcb(Machine* m) { return rw(m, lin(DOSSEG, D_LOL - 2)); }
static inline uint8_t mcb_type(Machine* m, uint16_t s) { return m->ram[lin(s, 0)]; }
static inline uint16_t mcb_owner(Machine* m, uint16_t s) { return rw(m, lin(s, 1)); }
static inline uint16_t mcb_size(Machine* m, uint16_t s) { return rw(m, lin(s, 3)); }
static void mcb_set(Machine* m, uint16_t s, uint8_t t, uint16_t owner, uint16_t size) {
    m->ram[lin(s, 0)] = t; ww(m, lin(s, 1), owner); ww(m, lin(s, 3), size);
}
static void mcb_name(Machine* m, uint16_t s, const std::string& n) {
    for (int i = 0; i < 8; i++) m->ram[lin(s, 8 + i)] = (uint8_t)(i < (int)n.size() ? n[i] : 0);
}
static void mcb_merge(Machine* m) {
    uint16_t s = first_mcb(m);
    for (int guard = 0; guard < 4096; guard++) {
        uint8_t t = mcb_type(m, s);
        if (t != 'M' && t != 'Z') return;
        if (t == 'Z') return;
        uint16_t nx = (uint16_t)(s + 1 + mcb_size(m, s));
        if (mcb_owner(m, s) == 0 && mcb_owner(m, nx) == 0 && (mcb_type(m, nx) == 'M' || mcb_type(m, nx) == 'Z')) {
            uint8_t nt = mcb_type(m, nx);
            mcb_set(m, s, nt, 0, (uint16_t)(mcb_size(m, s) + 1 + mcb_size(m, nx)));
            continue;
        }
        s = nx;
    }
}
static int mem_alloc(Machine* m, uint16_t paras, uint16_t owner, uint16_t* seg, uint16_t* largest) {
    mcb_merge(m);
    uint16_t s = first_mcb(m), best = 0, bestsz = 0;
    uint16_t big = 0;
    for (int guard = 0; guard < 4096; guard++) {
        uint8_t t = mcb_type(m, s);
        if (t != 'M' && t != 'Z') return 7;
        uint16_t sz = mcb_size(m, s);
        if (mcb_owner(m, s) == 0) {
            if (sz > big) big = sz;
            if (sz >= paras) {
                bool take = false;
                if (!best) take = true;
                else if ((s_alloc_strategy & 3) == 1 && sz < bestsz) take = true;   // best fit
                else if ((s_alloc_strategy & 3) == 2) take = true;                   // last fit
                if (take) { best = s; bestsz = sz; }
                if ((s_alloc_strategy & 3) == 0) break;
            }
        }
        if (t == 'Z') break;
        s = (uint16_t)(s + 1 + sz);
    }
    if (largest) *largest = big;
    if (!best) return 8;
    uint8_t t = mcb_type(m, best);
    if (bestsz > paras) {
        if ((s_alloc_strategy & 3) == 2) {   // 上から取る
            uint16_t newblk = (uint16_t)(best + bestsz - paras);
            mcb_set(m, best, 'M', 0, (uint16_t)(bestsz - paras - 1));
            mcb_set(m, newblk, t, owner, paras);
            *seg = (uint16_t)(newblk + 1);
            return 0;
        }
        uint16_t rest = (uint16_t)(best + 1 + paras);
        mcb_set(m, rest, t, 0, (uint16_t)(bestsz - paras - 1));
        mcb_set(m, best, 'M', owner, paras);
    } else mcb_set(m, best, t, owner, bestsz);
    *seg = (uint16_t)(best + 1);
    return 0;
}
static int mem_free(Machine* m, uint16_t seg) {
    uint16_t s = (uint16_t)(seg - 1);
    uint8_t t = mcb_type(m, s);
    if (t != 'M' && t != 'Z') return 9;
    ww(m, lin(s, 1), 0);
    return 0;
}
static int mem_resize(Machine* m, uint16_t seg, uint16_t paras, uint16_t* maxp) {
    uint16_t s = (uint16_t)(seg - 1);
    uint8_t t = mcb_type(m, s);
    if (t != 'M' && t != 'Z') return 9;
    uint16_t sz = mcb_size(m, s);
    uint16_t owner = mcb_owner(m, s);
    if (paras <= sz) {
        if (paras < sz) {
            uint16_t rest = (uint16_t)(s + 1 + paras);
            mcb_set(m, rest, t, 0, (uint16_t)(sz - paras - 1));
            mcb_set(m, s, 'M', owner, paras);
            mcb_merge(m);
        }
        return 0;
    }
    // 伸ばす: 後ろの空きを吸収
    mcb_merge(m);
    uint32_t avail = sz;
    if (t == 'M') {
        uint16_t nx = (uint16_t)(s + 1 + sz);
        if (mcb_owner(m, nx) == 0) avail = (uint32_t)sz + 1 + mcb_size(m, nx);
        if (avail >= paras) {
            uint8_t nt = mcb_type(m, nx);
            uint32_t left = avail - paras;
            if (left >= 1) {
                uint16_t rest = (uint16_t)(s + 1 + paras);
                mcb_set(m, rest, nt, 0, (uint16_t)(left - 1));
                mcb_set(m, s, 'M', owner, paras);
            } else mcb_set(m, s, nt, owner, (uint16_t)avail);
            return 0;
        }
    }
    if (maxp) *maxp = (uint16_t)(avail > 0xFFFF ? 0xFFFF : avail);
    return 8;
}
static void mem_free_owner(Machine* m, uint16_t psp) {
    uint16_t s = first_mcb(m);
    for (int guard = 0; guard < 4096; guard++) {
        uint8_t t = mcb_type(m, s);
        if (t != 'M' && t != 'Z') break;
        if (mcb_owner(m, s) == psp) ww(m, lin(s, 1), 0);
        if (t == 'Z') break;
        s = (uint16_t)(s + 1 + mcb_size(m, s));
    }
    mcb_merge(m);
}

// ---- ハンドル ----------------------------------------------------------------
static uint32_t jft_addr(Machine* m, uint16_t psp, int* size) {
    *size = rw(m, lin(psp, 0x32));
    uint16_t off = rw(m, lin(psp, 0x34)), seg = rw(m, lin(psp, 0x36));
    return lin(seg, off);
}
static int sft_of(Machine* m, uint16_t h) {
    int size;
    uint32_t a = jft_addr(m, s_psp, &size);
    if (h >= size) return -1;
    uint8_t v = mem_rb(m, a + h);
    if (v == 0xFF || v >= s_sft.size() || !s_sft[v].kind) return -1;
    return v;
}
static int new_handle(Machine* m, int sft) {
    int size;
    uint32_t a = jft_addr(m, s_psp, &size);
    for (int i = 0; i < size; i++) {
        if (mem_rb(m, a + i) == 0xFF) { mem_wb(m, a + i, (uint8_t)sft); return i; }
    }
    return -1;
}
static int new_sft() {
    for (size_t i = 5; i < s_sft.size(); i++) if (!s_sft[i].kind) return (int)i;
    if (s_sft.size() >= 250) return -1;
    s_sft.push_back(Sft());
    return (int)s_sft.size() - 1;
}
static void sft_release(int i) {
    if (i < 0 || i >= (int)s_sft.size()) return;
    Sft& f = s_sft[i];
    if (--f.refs > 0) return;
    if (f.kind == 1 && f.h) hostfs::close(f.h);
    f = Sft();
}
static void close_handle(Machine* m, uint16_t h) {
    int size;
    uint32_t a = jft_addr(m, s_psp, &size);
    if (h >= size) return;
    uint8_t v = mem_rb(m, a + h);
    if (v == 0xFF) return;
    mem_wb(m, a + h, 0xFF);
    sft_release(v);
}

// ---- キーの割り当て（INT DCh CL=0Ch/0Dh） -------------------------------------------
//  MS-DOS はファンクションキー（F1-F10, SHIFT+F1-F10: 16 バイトずつ）と編集キー（ROLL UP・ROLL DOWN・INS・DEL・
//  ↑・←・→・↓・HOME/CLR・HELP・SHIFT+HOME/CLR: 6 バイトずつ）に文字列を割り当て、コンソール入力で展開する。
//  ファイラーなどは起動時に自分用の割り当てに変える（例: DFX はカーソルキーを自分の文字に変える）。
//  ファンクションキーの 16 バイトは、先頭が FEh なら続く 5 バイトが表示名で、文字列は 6 バイト目から。
static uint8_t s_fkey[20][16];
static uint8_t s_ekey[11][6];
static bool s_keytab_init = false;
static void keytab_default() {
    memset(s_fkey, 0, sizeof(s_fkey)); memset(s_ekey, 0, sizeof(s_ekey));
    static const char fk[10] = {'S','T','U','V','W','E','J','P','Q','Z'};
    for (int i = 0; i < 10; i++) { s_fkey[i][0] = 0x1B; s_fkey[i][1] = (uint8_t)fk[i]; }
    static const uint8_t ek[11][3] = {
        {0x1B, 'R', 0}, {0x1B, 'Q', 0}, {0x1B, 'P', 0}, {0x7F, 0, 0},       // ROLL UP / ROLL DOWN / INS / DEL
        {0x0B, 0, 0}, {0x08, 0, 0}, {0x0C, 0, 0}, {0x0A, 0, 0},             // ↑ ← → ↓
        {0x1A, 0, 0}, {0, 0, 0}, {0x1E, 0, 0} };                            // HOME/CLR / HELP / SHIFT+HOME/CLR
    for (int i = 0; i < 11; i++) memcpy(s_ekey[i], ek[i], 3);
    s_keytab_init = true;
}
static uint8_t* keytab_entry(uint16_t ax, int* len) {
    if (!s_keytab_init) keytab_default();
    if (ax >= 0x01 && ax <= 0x14) { *len = 16; return s_fkey[ax - 1]; }
    if (ax >= 0x15 && ax <= 0x1F) { *len = 6; return s_ekey[ax - 0x15]; }
    *len = 0; return nullptr;
}
void dos_game_blocks(Machine* m, std::vector<DosMemBlock>& out) {
    out.clear();
    if (m->cfg.boot_fd) return;   // ブートモード: MS-DOS が無いので、ゲームのブロックは分からない（全体を対象にする）
    // 実行中のプログラムから親をたどる（シェルの手前まで）
    std::vector<uint16_t> chain;
    uint16_t p = s_psp;
    for (int i = 0; i < 16 && p && p != s_shell_psp; i++) {
        if (std::find(chain.begin(), chain.end(), p) != chain.end()) break;
        chain.push_back(p);
        p = rw(m, lin(p, 0x16));
    }
    if (chain.empty()) return;
    uint16_t s = first_mcb(m);
    for (int n = 0; n < 4096; n++) {
        uint8_t t = mcb_type(m, s);
        if (t != 'M' && t != 'Z') break;
        uint16_t own = mcb_owner(m, s), sz = mcb_size(m, s);
        if (own && std::find(chain.begin(), chain.end(), own) != chain.end() && sz) {
            uint32_t st = ((uint32_t)s + 1) << 4, len = (uint32_t)sz << 4;
            // 名前: そのブロックの持ち主（PSP の直前の MCB）の名前
            std::string nm;
            for (int i = 0; i < 8; i++) { uint8_t c = m->ram[lin((uint16_t)(own - 1), 8 + i)]; if (!c) break; nm.push_back((char)c); }
            if (!out.empty() && out.back().start + out.back().len == st && out.back().name == nm) out.back().len += len;
            else out.push_back({st, len, nm});
        }
        if (t == 'Z') break;
        uint32_t next = (uint32_t)s + sz + 1;
        if (next >= 0xA000) break;
        s = (uint16_t)next;
    }
}

void dos_keytab_get(Machine* m, uint16_t ax, uint32_t a) {
    if (!s_keytab_init) keytab_default();
    if (ax == 0x0000 || ax == 0x00FF) {   // 全部（386 バイト）
        for (int i = 0; i < 20; i++) for (int k = 0; k < 16; k++) mem_wb(m, a + i * 16 + k, s_fkey[i][k]);
        for (int i = 0; i < 11; i++) for (int k = 0; k < 6; k++) mem_wb(m, a + 320 + i * 6 + k, s_ekey[i][k]);
        return;
    }
    int len; uint8_t* e = keytab_entry(ax, &len);
    if (e) for (int k = 0; k < len; k++) mem_wb(m, a + k, e[k]);
}
void dos_keytab_set(Machine* m, uint16_t ax, uint32_t a) {
    if (!s_keytab_init) keytab_default();
    if (ax == 0x0000 || ax == 0x00FF) {
        for (int i = 0; i < 20; i++) for (int k = 0; k < 16; k++) s_fkey[i][k] = mem_rb(m, a + i * 16 + k);
        for (int i = 0; i < 11; i++) for (int k = 0; k < 6; k++) s_ekey[i][k] = mem_rb(m, a + 320 + i * 6 + k);
        return;
    }
    int len; uint8_t* e = keytab_entry(ax, &len);
    if (e) for (int k = 0; k < len; k++) e[k] = mem_rb(m, a + k);
}
// 割り当てた文字列を入力待ちへ積む
static void keytab_push(const uint8_t* e, int len) {
    int st = 0;
    if (len == 16 && e[0] == 0xFE) st = 6;   // 表示名つき
    for (int k = st; k < len && e[k]; k++) s_pending_input.push_back((char)e[k]);
}

// ---- コンソール入力 ------------------------------------------------------------
static bool con_have_char(Machine* m) {
    if (!s_keytab_init) keytab_default();
    if (!s_pending_input.empty()) return true;
    while (bios_key_available(m)) {
        uint16_t k = bios_key_read(m, false);
        uint8_t ch = (uint8_t)k, sc = (uint8_t)(k >> 8);
        // ファンクションキー（SHIFT つきは BIOS のコード 82h-8Bh）と編集キーは、割り当てた文字列に置き換える
        int idx = -1; bool fkey = false;
        if (sc >= 0x62 && sc <= 0x6B) { idx = sc - 0x62; fkey = true; }
        else if (sc >= 0x82 && sc <= 0x8B) { idx = 10 + sc - 0x82; fkey = true; }
        else if (sc >= 0x36 && sc <= 0x3F) {
            idx = sc - 0x36;
            if (sc == 0x3E && (m->kb_down[0x70] || m->kb_down[0x7D])) idx = 10;   // SHIFT+HOME/CLR
        } else if (sc == 0xAE) idx = 10;
        if (idx >= 0) {
            bios_key_read(m, true);
            if (fkey) keytab_push(s_fkey[idx], 16); else keytab_push(s_ekey[idx], 6);
            if (!s_pending_input.empty()) return true;
            continue;
        }
        if (ch) return true;
        bios_key_read(m, true);
    }
    return false;
}
static uint8_t con_get_char(Machine* m) {
    if (!s_pending_input.empty()) { uint8_t c = (uint8_t)s_pending_input[0]; s_pending_input.erase(0, 1); return c; }
    return (uint8_t)bios_key_read(m, true);
}
static void wait_retry(Machine* m) {
    m->cpu.ip = (uint16_t)(m->cpu.ip - 2);
    m->cpu.fl |= FL_IF;
    m->cpu.halted = 1;
}

// ---- 環境変数とプログラムの読み込み ----------------------------------------------
static std::vector<uint8_t> build_env(Machine* m, uint16_t src_env, const std::string& progpath) {
    std::vector<uint8_t> e;
    if (src_env) {
        uint32_t a = lin(src_env, 0);
        for (int i = 0; i < 32768; i++) {
            uint8_t c = mem_rb(m, a + i);
            e.push_back(c);
            if (c == 0 && (i == 0 || mem_rb(m, a + i - 1) == 0)) break;
        }
        if (e.size() == 1) e.push_back(0);
    } else {
        for (auto& kv : s_env) {
            std::string s = kv.first + "=" + kv.second;
            e.insert(e.end(), s.begin(), s.end());
            e.push_back(0);
        }
        e.push_back(0);
        if (e.size() == 1) e.push_back(0);
    }
    e.push_back(1); e.push_back(0);
    e.insert(e.end(), progpath.begin(), progpath.end());
    e.push_back(0);
    return e;
}

static void parse_fcb(Machine* m, uint32_t fcb, const std::string& arg) {
    mem_wb(m, fcb, 0);
    for (int i = 0; i < 11; i++) mem_wb(m, fcb + 1 + i, ' ');
    std::string s = arg;
    size_t p = 0;
    if (s.size() >= 2 && s[1] == ':') { mem_wb(m, fcb, (uint8_t)(toupper((unsigned char)s[0]) - 'A' + 1)); p = 2; }
    int i = 0;
    for (; p < s.size() && s[p] != '.' && i < 8; p++, i++) mem_wb(m, fcb + 1 + i, (uint8_t)toupper((unsigned char)s[p]));
    while (p < s.size() && s[p] != '.') p++;
    if (p < s.size() && s[p] == '.') {
        p++;
        for (i = 0; p < s.size() && i < 3; p++, i++) mem_wb(m, fcb + 9 + i, (uint8_t)toupper((unsigned char)s[p]));
    }
}

static std::vector<std::string> split_args(const std::string& tail) {
    std::vector<std::string> a;
    std::string cur;
    for (char c : tail) {
        if (c == ' ' || c == '\t' || c == ',' || c == ';' || c == '=') { if (!cur.empty()) a.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) a.push_back(cur);
    return a;
}

static void build_psp(Machine* m, uint16_t psp, uint16_t top, uint16_t env, uint16_t parent, const std::string& tail) {
    uint32_t p = lin(psp, 0);
    for (int i = 0; i < 256; i++) m->ram[p + i] = 0;
    m->ram[p] = 0xCD; m->ram[p + 1] = 0x20;
    ww(m, p + 2, top);
    m->ram[p + 5] = 0x9A;       // CALL 5 互換（使われない前提で INT 21h 入口を指す）
    ww(m, p + 6, s_int21_off); ww(m, p + 8, ROMSEG);
    // INT 22h/23h/24h
    for (int i = 0; i < 3; i++) {
        ww(m, p + 0x0A + i * 4, rw(m, (0x22 + i) * 4));
        ww(m, p + 0x0C + i * 4, rw(m, (0x22 + i) * 4 + 2));
    }
    ww(m, p + 0x16, parent);
    for (int i = 0; i < 20; i++) m->ram[p + 0x18 + i] = 0xFF;
    ww(m, p + 0x2C, env);
    ww(m, p + 0x32, 20);
    ww(m, p + 0x34, 0x18); ww(m, p + 0x36, psp);
    ww(m, p + 0x38, 0xFFFF); ww(m, p + 0x3A, 0xFFFF);
    m->ram[p + 0x40] = 5; m->ram[p + 0x41] = 0;   // DOS バージョン
    m->ram[p + 0x50] = 0xCD; m->ram[p + 0x51] = 0x21; m->ram[p + 0x52] = 0xCB;
    std::vector<std::string> a = split_args(tail);
    parse_fcb(m, p + 0x5C, a.size() > 0 ? a[0] : "");
    parse_fcb(m, p + 0x6C, a.size() > 1 ? a[1] : "");
    std::string t = tail;
    if (t.size() > 126) t.resize(126);
    m->ram[p + 0x80] = (uint8_t)t.size();
    for (size_t i = 0; i < t.size(); i++) m->ram[p + 0x81 + i] = (uint8_t)t[i];
    m->ram[p + 0x81 + t.size()] = 0x0D;
}

// 親の JFT を引き継ぐ
static void inherit_handles(Machine* m, uint16_t parent, uint16_t child) {
    int psize, csize;
    uint32_t pa = jft_addr(m, parent, &psize);
    uint32_t ca = jft_addr(m, child, &csize);
    for (int i = 0; i < 20 && i < psize && i < csize; i++) {
        uint8_t v = mem_rb(m, pa + i);
        if (v != 0xFF && v < s_sft.size() && s_sft[v].kind) {
            // 継承しない指定（オープンモード bit7）は飛ばす
            if (s_sft[v].mode & 0x80) { mem_wb(m, ca + i, 0xFF); continue; }
            s_sft[v].refs++;
            mem_wb(m, ca + i, v);
        } else mem_wb(m, ca + i, 0xFF);
    }
}

static bool read_host_file(const std::string& host, std::vector<uint8_t>& data) {
    void* h = hostfs::open(host, 0, false, false);
    if (!h) return false;
    int64_t sz = hostfs::seek(h, 0, 2);
    hostfs::seek(h, 0, 0);
    if (sz < 0 || sz > 0x100000) { hostfs::close(h); return false; }
    data.resize((size_t)sz);
    int r = sz ? hostfs::read(h, data.data(), (int)sz) : 0;
    hostfs::close(h);
    return r == (int)sz;
}

struct LoadResult { uint16_t psp, cs, ip, ss, sp; };

// 戻り値: DOS エラーコード（0 = 成功）
static int load_program(Machine* m, const std::string& guest, const std::string& tail, uint16_t env_src,
                        uint16_t parent, LoadResult* out, const char* envname = nullptr) {
    bool exists = false, dev = false;
    std::string canon;
    std::string host = guest_to_host(m, guest, &exists, &dev, &canon);
    std::vector<uint8_t> data;
    if (!exists || dev || !read_host_file(host, data)) return 2;
    std::string full = std::string(1, s_sg_drive ? s_sg_drive : m->cfg.drive) + ":\\" + canon;
    std::string base = canon.substr(canon.rfind('\\') == std::string::npos ? 0 : canon.rfind('\\') + 1);
    std::string mcbname = base.substr(0, base.find('.'));

    // 環境の後ろのプログラム名（C の argv[0]）: 実機の MS-DOS の EXEC は、呼んだプログラムが渡した名前を
    // そのまま写す（"game.exe" と呼べば "game.exe"）。COMMAND.COM から起動したときは見つけた完全な名前
    std::vector<uint8_t> env = build_env(m, env_src, envname ? std::string(envname) : full);
    uint16_t env_paras = (uint16_t)((env.size() + 15) / 16);
    uint16_t env_seg, largest;
    if (mem_alloc(m, env_paras, 0xFFFF, &env_seg, &largest)) return 8;
    for (size_t i = 0; i < env.size(); i++) m->ram[lin(env_seg, 0) + i] = env[i];

    bool is_exe = data.size() >= 0x1C && ((data[0] == 'M' && data[1] == 'Z') || (data[0] == 'Z' && data[1] == 'M'));
    uint16_t psp;
    uint16_t block_paras;
    // 空きの最大を調べる
    uint16_t dummy;
    mem_alloc(m, 0xFFFF, 0, &dummy, &largest);
    if (!is_exe) {
        uint32_t need = (uint32_t)((data.size() + 0x100 + 15) / 16) + 1;
        if (largest < need || data.size() > 0xFF00) { mem_free(m, env_seg); return 8; }
        block_paras = largest;
        if (mem_alloc(m, block_paras, 0xFFFF, &psp, nullptr)) { mem_free(m, env_seg); return 8; }
        for (size_t i = 0; i < data.size(); i++) m->ram[lin(psp, 0x100) + i] = data[i];
        out->cs = psp; out->ip = 0x100; out->ss = psp;
        uint32_t top = (uint32_t)block_paras * 16;
        out->sp = top >= 0x10000 ? 0xFFFE : (uint16_t)((top - 2) & 0xFFFE);
        ww(m, lin(psp, out->sp), 0);
    } else {
        uint16_t cblp = (uint16_t)(data[2] | (data[3] << 8));
        uint16_t cp = (uint16_t)(data[4] | (data[5] << 8));
        uint16_t crlc = (uint16_t)(data[6] | (data[7] << 8));
        uint16_t hdr = (uint16_t)(data[8] | (data[9] << 8));
        uint16_t minalloc = (uint16_t)(data[10] | (data[11] << 8));
        uint16_t maxalloc = (uint16_t)(data[12] | (data[13] << 8));
        uint16_t e_ss = (uint16_t)(data[14] | (data[15] << 8));
        uint16_t e_sp = (uint16_t)(data[16] | (data[17] << 8));
        uint16_t e_ip = (uint16_t)(data[20] | (data[21] << 8));
        uint16_t e_cs = (uint16_t)(data[22] | (data[23] << 8));
        uint16_t lfarlc = (uint16_t)(data[24] | (data[25] << 8));
        uint32_t imgsize = (uint32_t)cp * 512 - (cblp ? (512 - cblp) : 0);
        uint32_t hdrbytes = (uint32_t)hdr * 16;
        if (imgsize < hdrbytes) imgsize = hdrbytes;
        imgsize -= hdrbytes;
        if (hdrbytes + imgsize > data.size()) imgsize = (uint32_t)data.size() - hdrbytes;
        uint32_t img_paras = (imgsize + 15) / 16;
        uint32_t minp = 0x10 + img_paras + minalloc;
        uint32_t maxp = 0x10 + img_paras + (uint32_t)maxalloc;
        if (maxp > 0xFFFF) maxp = 0xFFFF;
        if (minp > largest) {
            plog("[dos] %s: メモリ不足（必要 %u KB / 空き %u KB）\n", base.c_str(), minp / 64, largest / 64);
            mem_free(m, env_seg); return 8;
        }
        block_paras = (uint16_t)(maxp < largest ? maxp : largest);
        if (block_paras < minp) block_paras = (uint16_t)minp;
        if (mem_alloc(m, block_paras, 0xFFFF, &psp, nullptr)) { mem_free(m, env_seg); return 8; }
        {
            // EXEPACK で縮めたプログラムは、最初の 64KB（セグメント 1000h 未満）に読み込まれると
            // 展開に失敗して「Packed file is corrupt」で終わる（MS-DOS 5 の LOADFIX と同じ問題）。
            // 1000h より下に置かれそうなら、下に詰め物のブロックを置いて 64KB より上へ読み込む
            static const char sig[] = "Packed file is corrupt";
            bool exepack = std::search(data.begin(), data.end(), sig, sig + sizeof(sig) - 1) != data.end();
            if (exepack && psp < 0x1000) {
                mem_free(m, psp);
                uint16_t fill = 0, lg2 = 0;
                bool ok = !mem_alloc(m, (uint16_t)(0x1000 - psp), 0xFFFF, &fill, nullptr);
                if (ok) {
                    mem_alloc(m, 0xFFFF, 0, &dummy, &lg2);
                    uint16_t bp2 = (uint16_t)(maxp < lg2 ? maxp : lg2);
                    if (bp2 >= minp && !mem_alloc(m, bp2, 0xFFFF, &psp, nullptr)) {
                        block_paras = bp2;
                        if (m->cfg.trace) plog("[dos] %s は EXEPACK なので 64KB より上（%04X）へ読み込みます\n", base.c_str(), psp);
                        mem_free(m, fill);
                    } else { mem_free(m, fill); ok = false; }
                }
                if (!ok && mem_alloc(m, block_paras, 0xFFFF, &psp, nullptr)) { mem_free(m, env_seg); return 8; }
            }
        }
        uint16_t load = (uint16_t)(psp + 0x10);
        for (uint32_t i = 0; i < imgsize; i++) m->ram[(lin(load, 0) + i) & 0xFFFFF] = data[hdrbytes + i];
        for (uint32_t i = 0; i < crlc; i++) {
            uint32_t r = lfarlc + i * 4;
            if (r + 4 > data.size()) break;
            uint16_t off = (uint16_t)(data[r] | (data[r + 1] << 8));
            uint16_t seg = (uint16_t)(data[r + 2] | (data[r + 3] << 8));
            uint32_t a = lin((uint16_t)(seg + load), off);
            ww(m, a, (uint16_t)(rw(m, a) + load));
        }
        out->cs = (uint16_t)(e_cs + load); out->ip = e_ip;
        out->ss = (uint16_t)(e_ss + load); out->sp = e_sp;
    }
    ww(m, lin((uint16_t)(env_seg - 1), 1), psp);
    ww(m, lin((uint16_t)(psp - 1), 1), psp);
    mcb_name(m, (uint16_t)(env_seg - 1), mcbname);
    mcb_name(m, (uint16_t)(psp - 1), mcbname);
    build_psp(m, psp, (uint16_t)(psp + block_paras), env_seg, parent, tail);
    inherit_handles(m, parent, psp);
    out->psp = psp;
    if (m->cfg.trace) {
        uint16_t dmy, rest = 0; mem_alloc(m, 0xFFFF, 0, &dmy, &rest);   // 残りの空きの最大（確保はしない）
        plog("[dos] 起動 %s %s  PSP=%04X  CS:IP=%04X:%04X  %u KB（残りの空きの最大 %u KB）\n", full.c_str(), tail.c_str(), psp, out->cs, out->ip, block_paras / 64, rest / 64);
    }
    return 0;
}

static void start_program(Machine* m, const LoadResult& lr) {
    Cpu* c = &m->cpu;
    s_psp = lr.psp;
    s_dta_seg = lr.psp; s_dta_off = 0x80;
    cpu_setsr(c, CS_, lr.cs); c->ip = lr.ip;
    cpu_setsr(c, SS_, lr.ss); c->r[ESP] = lr.sp;
    cpu_setsr(c, DS_, lr.psp); cpu_setsr(c, ES_, lr.psp);
    c->r[EAX] = 0; c->r[EBX] = 0; c->r[ECX] = 0xFF; c->r[EDX] = lr.psp;
    c->r[ESI] = lr.ip; c->r[EDI] = lr.sp; c->r[EBP] = 0x091C;
    c->fl = 0x0202;
    c->halted = 0;
}

// ---- 終了 --------------------------------------------------------------------
static void shell_resume(Machine* m);

static void terminate(Machine* m, uint8_t code, int type, uint16_t keep) {
    uint16_t psp = s_psp;
    if (psp == s_shell_psp) { m->quit = 1; return; }
    uint16_t parent = rw(m, lin(psp, 0x16));
    // INT 22h/23h/24h を戻す
    for (int i = 0; i < 3; i++) {
        ww(m, (0x22 + i) * 4, rw(m, lin(psp, (uint16_t)(0x0A + i * 4))));
        ww(m, (0x22 + i) * 4 + 2, rw(m, lin(psp, (uint16_t)(0x0C + i * 4))));
    }
    if (type == 0) {
        // 開いているファイルを閉じる
        int size;
        uint32_t a = jft_addr(m, psp, &size);
        for (int i = 0; i < size; i++) {
            uint8_t v = mem_rb(m, a + i);
            if (v != 0xFF) { mem_wb(m, a + i, 0xFF); sft_release(v); }
        }
        mem_free_owner(m, psp);
    } else {
        if (keep < 6) keep = 6;
        mem_resize(m, psp, keep, nullptr);
        if (m->cfg.trace) plog("[dos] 常駐終了 PSP=%04X  %u バイト\n", psp, keep * 16);
    }
    s_retcode = (uint16_t)(code | (type << 8));
    s_psp = parent;
    // EXEC した子そのもの、または子が AH=55h で作った「身代わり」の PSP（親が同じ）が終わったら、EXEC の呼び出し元へ戻る
    //（CSTMOUSE.BIN などの、自分を読み込み直して高い位置へ移すローダ）
    if (!s_frames.empty() && (s_frames.back().child_psp == psp || s_frames.back().parent_psp == parent)) {
        ExecFrame f = s_frames.back();
        s_frames.pop_back();
        Cpu* c = &m->cpu;
        for (int i = 0; i < 8; i++) c->r[i] = f.r[i];
        for (int i = 0; i < 6; i++) cpu_setsr(c, i, f.sr[i]);
        c->ip = f.ip; c->fl = f.fl;
        s_dta_seg = f.dta_seg; s_dta_off = f.dta_off;
        c->halted = 0;
        set_cf(m, false);
        return;
    }
    // シェルへ戻る
    shell_resume(m);
}

// ---- FindFirst / FindNext ----------------------------------------------------
static bool wild_match(const std::string& pat, const std::string& name) {
    auto split = [](const std::string& s, std::string& b, std::string& e) {
        size_t d = s.find('.');
        if (d == std::string::npos) { b = s; e = ""; } else { b = s.substr(0, d); e = s.substr(d + 1); }
    };
    auto expand = [](const std::string& p, size_t n) {
        std::string r;
        for (size_t i = 0; i < p.size() && r.size() < n; i++) {
            if (p[i] == '*') { while (r.size() < n) r.push_back('?'); break; }
            r.push_back(p[i]);
        }
        while (r.size() < n) r.push_back(' ');
        return r;
    };
    std::string pb, pe, nb, ne;
    split(pat, pb, pe); split(name, nb, ne);
    if (pat.find('.') == std::string::npos && pb.find('*') != std::string::npos) pe = "*";
    std::string P = expand(pb, 8) + expand(pe, 3);
    std::string N = expand(nb, 8) + expand(ne, 3);
    for (int i = 0; i < 11; i++) {
        if (P[i] == '?') continue;
        if (P[i] != N[i]) return false;
    }
    return true;
}
static void fill_dta(Machine* m, const HostDirEntry& e, const std::string& gname, int sid, int pos) {
    uint32_t d = lin(s_dta_seg, s_dta_off);
    mem_wb(m, d + 0, 0xA5); mem_wb(m, d + 1, 0x5A);
    ww(m, d + 2, (uint16_t)sid); ww(m, d + 4, (uint16_t)pos);
    uint8_t attr = (uint8_t)((e.is_dir ? 0x10 : 0x20) | (e.readonly ? 1 : 0));
    mem_wb(m, d + 0x15, attr);
    ww(m, d + 0x16, e.dos_time); ww(m, d + 0x18, machine_file_date(m, e.dos_date));
    ww(m, d + 0x1A, (uint16_t)e.size); ww(m, d + 0x1C, (uint16_t)(e.size >> 16));
    for (int i = 0; i < 13; i++) mem_wb(m, d + 0x1E + i, 0);
    for (size_t i = 0; i < gname.size() && i < 12; i++) mem_wb(m, d + 0x1E + (uint32_t)i, (uint8_t)gname[i]);
}
static bool find_next_in(Machine* m, int sid) {
    Search& s = s_search[sid];
    while (s.pos < s.list.size()) {
        const HostDirEntry& e = s.list[s.pos++];
        std::string g = e.name == "." || e.name == ".." ? e.name : upper_dbcs(hostfs::to_sjis(e.name));
        if (e.name != "." && e.name != ".." && !valid83(g)) continue;
        if (e.is_dir && !(s.attr & 0x10)) continue;
        if (!wild_match(s.pattern, g) && !(g == "." || g == "..")) continue;
        if ((g == "." || g == "..") && !wild_match(s.pattern, g)) continue;
        fill_dta(m, e, g, sid, (int)s.pos);
        return true;
    }
    s.used = false;
    return false;
}
static void dos_find_first(Machine* m) {
    std::string spec = read_asciiz(m, DS(m), DX(m));
    uint8_t attr = (uint8_t)CX(m);
    std::vector<std::string> comps;
    split_guest(spec, comps, nullptr);
    if (attr == 0x08) {
        // ボリュームラベル（属性が 08h だけのとき）: フロッピーのドライブなら円盤のラベルを返す（キーディスクの確認に使うゲームがある）
        std::string lab; uint16_t ld = 0, lt = 0;
        if (s_sg_drive && floppy::unit_of_letter(s_sg_drive) >= 0 && fatfs::volume_label_drive(s_sg_drive, &lab, &ld, &lt)) {
            uint32_t d = lin(s_dta_seg, s_dta_off);
            mem_wb(m, d + 0, 0xA5); mem_wb(m, d + 1, 0x5A);
            ww(m, d + 2, 0xFFFF); ww(m, d + 4, 0);
            mem_wb(m, d + 0x15, 0x08);
            ww(m, d + 0x16, lt); ww(m, d + 0x18, machine_file_date(m, ld));
            ww(m, d + 0x1A, 0); ww(m, d + 0x1C, 0);
            for (int i = 0; i < 13; i++) mem_wb(m, d + 0x1E + i, 0);
            for (size_t i = 0; i < lab.size() && i < 12; i++) mem_wb(m, d + 0x1E + (uint32_t)i, (uint8_t)lab[i]);
            dos_ok(m); return;
        }
        dos_error(m, 0x12); return;
    }
    std::string pat = comps.empty() ? "*.*" : comps.back();
    bool exists;
    std::string dir = host_of(m, comps, comps.empty() ? 0 : comps.size() - 1, &exists);
    if (!exists) { dos_error(m, 3); return; }
    int sid = -1;
    for (size_t i = 0; i < s_search.size(); i++) if (!s_search[i].used) { sid = (int)i; break; }
    if (sid < 0) {
        if (s_search.size() >= 64) { s_search.erase(s_search.begin()); }
        s_search.push_back(Search()); sid = (int)s_search.size() - 1;
    }
    Search& s = s_search[sid];
    s = Search();
    s.used = true; s.attr = attr; s.pattern = pat;
    s.list = dir_list(dir);
    std::sort(s.list.begin(), s.list.end(), [](const HostDirEntry& a, const HostDirEntry& b) { return a.name < b.name; });
    if (comps.size() > 1 && (attr & 0x10)) {
        HostDirEntry dot; dot.is_dir = true; dot.size = 0; dot.dos_date = 0x2821; dot.dos_time = 0; dot.readonly = false;
        dot.name = ".."; s.list.insert(s.list.begin(), dot);
        dot.name = "."; s.list.insert(s.list.begin(), dot);
    }
    if (find_next_in(m, sid)) dos_ok(m); else dos_error(m, 0x12);
}
static void dos_find_next(Machine* m) {
    uint32_t d = lin(s_dta_seg, s_dta_off);
    if (mem_rb(m, d) != 0xA5 || mem_rb(m, d + 1) != 0x5A) { dos_error(m, 0x12); return; }
    int sid = rw(m, d + 2), pos = rw(m, d + 4);
    if (sid >= (int)s_search.size()) { dos_error(m, 0x12); return; }
    s_search[sid].pos = (size_t)pos;
    s_search[sid].used = true;
    if (find_next_in(m, sid)) dos_ok(m); else dos_error(m, 0x12);
}


// ---- FCB（DOS 1.x 系のファイル操作）--------------------------------------------
//  開いた FCB の予約域 18h に SFT 番号、19h に目印 'F' を置く。
static uint32_t fcb_base(Machine* m, uint32_t* xattr_out) {
    uint32_t p = lin(DS(m), DX(m));
    if (mem_rb(m, p) == 0xFF) { if (xattr_out) *xattr_out = mem_rb(m, p + 6); return p + 7; }
    if (xattr_out) *xattr_out = 0;
    return p;
}
static std::string fcb_name_at(Machine* m, uint32_t f) {
    std::string b, e;
    for (int i = 0; i < 8; i++) b.push_back((char)mem_rb(m, f + 1 + i));
    for (int i = 0; i < 3; i++) e.push_back((char)mem_rb(m, f + 9 + i));
    while (!b.empty() && b.back() == ' ') b.pop_back();
    while (!e.empty() && e.back() == ' ') e.pop_back();
    std::string n = b;
    if (!e.empty()) n += "." + e;
    uint8_t drv = mem_rb(m, f);
    if (drv) { std::string d(1, (char)('A' + drv - 1)); n = d + ":" + n; }
    return n;
}
static int fcb_sft(Machine* m, uint32_t f) {
    if (mem_rb(m, f + 0x19) != 'F') return -1;
    int i = mem_rb(m, f + 0x18);
    if (i >= (int)s_sft.size() || s_sft[i].kind != 1) return -1;
    return i;
}
static void fcb_open(Machine* m, bool create) {
    uint32_t f = fcb_base(m, nullptr);
    std::string n = fcb_name_at(m, f);
    if (n.find('?') != std::string::npos) { SETAL(m, 0xFF); return; }
    bool ex; std::string host = guest_to_host(m, n, &ex, nullptr, nullptr);
    HostDirEntry st;
    bool is_file = hostfs::stat(host, st) && !st.is_dir;
    if (!create && !is_file) { SETAL(m, 0xFF); return; }
    int sft = new_sft();
    if (sft < 0 || sft > 255) { SETAL(m, 0xFF); return; }
    void* h = hostfs::open(host, 2, create, create);
    if (!h) { SETAL(m, 0xFF); return; }
    if (create) dir_invalidate();
    Sft& s = s_sft[sft];
    s.kind = 1; s.h = h; s.host = host; s.name = n; s.refs = 1; s.mode = 2;
    int64_t size = hostfs::seek(h, 0, 2); hostfs::seek(h, 0, 0);
    if (!mem_rb(m, f)) mem_wb(m, f, (uint8_t)(m->cfg.drive - 'A' + 1));
    ww(m, f + 0x0C, 0); ww(m, f + 0x0E, 128);
    ww(m, f + 0x10, (uint16_t)size); ww(m, f + 0x12, (uint16_t)(size >> 16));
    uint16_t d = 0x2821, t = 0; hostfs::get_time(h, &d, &t); d = machine_file_date(m, d);
    ww(m, f + 0x14, d); ww(m, f + 0x16, t);
    mem_wb(m, f + 0x18, (uint8_t)sft); mem_wb(m, f + 0x19, 'F');
    mem_wb(m, f + 0x20, 0);
    SETAL(m, 0);
}
static uint16_t fcb_recsize(Machine* m, uint32_t f) { uint16_t r = rw(m, f + 0x0E); if (!r) { r = 128; ww(m, f + 0x0E, r); } return r; }
static uint32_t fcb_random(Machine* m, uint32_t f, uint16_t rs) {
    uint32_t v = rw(m, f + 0x21) | ((uint32_t)mem_rb(m, f + 0x23) << 16);
    if (rs < 64) v |= (uint32_t)mem_rb(m, f + 0x24) << 24;
    return v;
}
static void fcb_set_random(Machine* m, uint32_t f, uint32_t v, uint16_t rs) {
    ww(m, f + 0x21, (uint16_t)v); mem_wb(m, f + 0x23, (uint8_t)(v >> 16));
    if (rs < 64) mem_wb(m, f + 0x24, (uint8_t)(v >> 24));
}
static void fcb_set_seq(Machine* m, uint32_t f, uint32_t rec) { ww(m, f + 0x0C, (uint16_t)(rec / 128)); mem_wb(m, f + 0x20, (uint8_t)(rec % 128)); }
static uint32_t fcb_seq(Machine* m, uint32_t f) { return (uint32_t)rw(m, f + 0x0C) * 128 + mem_rb(m, f + 0x20); }
static void fcb_update_size(Machine* m, uint32_t f, int sft) {
    int64_t cur = hostfs::seek(s_sft[sft].h, 0, 1), end = hostfs::seek(s_sft[sft].h, 0, 2);
    hostfs::seek(s_sft[sft].h, cur, 0);
    ww(m, f + 0x10, (uint16_t)end); ww(m, f + 0x12, (uint16_t)(end >> 16));
}
// 1 レコード読み: 0=成功 1=データなし 3=途中まで
static uint8_t fcb_read_rec(Machine* m, uint32_t f, int sft, uint32_t rec, uint32_t dta) {
    uint16_t rs = fcb_recsize(m, f);
    hostfs::seek(s_sft[sft].h, (int64_t)rec * rs, 0);
    std::vector<uint8_t> b(rs, 0);
    int r = hostfs::read(s_sft[sft].h, b.data(), rs);
    if (r <= 0) return 1;
    for (int i = 0; i < rs; i++) mem_wb(m, dta + i, i < r ? b[i] : 0);
    return r < rs ? 3 : 0;
}
static uint8_t fcb_write_rec(Machine* m, uint32_t f, int sft, uint32_t rec, uint32_t dta, uint16_t n) {
    uint16_t rs = fcb_recsize(m, f);
    hostfs::seek(s_sft[sft].h, (int64_t)rec * rs, 0);
    std::vector<uint8_t> b(n);
    for (int i = 0; i < n; i++) b[i] = mem_rb(m, dta + i);
    int w = n ? hostfs::write(s_sft[sft].h, b.data(), n) : 0;
    fcb_update_size(m, f, sft);
    return w == n ? 0 : 1;
}
static bool fcb_find(Machine* m, bool first) {
    uint32_t xattr = 0;
    uint32_t p = lin(DS(m), DX(m));
    bool ext = mem_rb(m, p) == 0xFF;
    uint32_t f = fcb_base(m, &xattr);
    int sid;
    if (first) {
        std::string pat;
        for (int i = 0; i < 8; i++) pat.push_back((char)mem_rb(m, f + 1 + i));
        while (!pat.empty() && pat.back() == ' ') pat.pop_back();
        std::string e; for (int i = 0; i < 3; i++) e.push_back((char)mem_rb(m, f + 9 + i));
        while (!e.empty() && e.back() == ' ') e.pop_back();
        pat += "." + e;
        std::vector<std::string> comps;
        bool exists;
        std::string dir = host_of(m, comps, 0, &exists);
        sid = -1;
        for (size_t i = 0; i < s_search.size(); i++) if (!s_search[i].used) { sid = (int)i; break; }
        if (sid < 0) { if (s_search.size() >= 64) s_search.erase(s_search.begin()); s_search.push_back(Search()); sid = (int)s_search.size() - 1; }
        Search& s = s_search[sid];
        s = Search(); s.used = true; s.attr = (uint8_t)xattr; s.pattern = pat;
        s.list = dir_list(dir);
        std::sort(s.list.begin(), s.list.end(), [](const HostDirEntry& a, const HostDirEntry& b) { return a.name < b.name; });
        ww(m, f + 0x0C, (uint16_t)sid); ww(m, f + 0x0E, 0);
    } else {
        sid = rw(m, f + 0x0C);
        if (sid >= (int)s_search.size()) return false;
        s_search[sid].pos = rw(m, f + 0x0E);
    }
    Search& s = s_search[sid];
    while (s.pos < s.list.size()) {
        const HostDirEntry& e = s.list[s.pos++];
        std::string g = upper_dbcs(hostfs::to_sjis(e.name));
        if (!valid83(g)) continue;
        if (e.is_dir && !(s.attr & 0x10)) continue;
        if (!wild_match(s.pattern, g)) continue;
        ww(m, f + 0x0E, (uint16_t)s.pos);
        uint32_t d = lin(s_dta_seg, s_dta_off);
        if (ext) { mem_wb(m, d, 0xFF); for (int i = 1; i < 6; i++) mem_wb(m, d + i, 0); mem_wb(m, d + 6, (uint8_t)(e.is_dir ? 0x10 : 0x20)); d += 7; }
        mem_wb(m, d, (uint8_t)(m->cfg.drive - 'A' + 1));
        std::string b = g, x;
        size_t dot = g.find('.');
        if (dot != std::string::npos) { b = g.substr(0, dot); x = g.substr(dot + 1); }
        for (int i = 0; i < 8; i++) mem_wb(m, d + 1 + i, i < (int)b.size() ? (uint8_t)b[i] : ' ');
        for (int i = 0; i < 3; i++) mem_wb(m, d + 9 + i, i < (int)x.size() ? (uint8_t)x[i] : ' ');
        mem_wb(m, d + 0x0C, (uint8_t)(e.is_dir ? 0x10 : 0x20));
        for (int i = 0x0D; i < 0x16; i++) mem_wb(m, d + i, 0);
        ww(m, d + 0x16, e.dos_time); ww(m, d + 0x18, machine_file_date(m, e.dos_date));
        ww(m, d + 0x1A, 0);
        ww(m, d + 0x1C, (uint16_t)e.size); ww(m, d + 0x1E, (uint16_t)(e.size >> 16));
        return true;
    }
    s.used = false;
    return false;
}
static bool fcb_call(Machine* m, uint8_t ah) {
    uint32_t dta = lin(s_dta_seg, s_dta_off);
    switch (ah) {
    case 0x0F: fcb_open(m, false); return true;
    case 0x16: fcb_open(m, true); return true;
    case 0x10: {
        uint32_t f = fcb_base(m, nullptr);
        int sft = fcb_sft(m, f);
        if (sft < 0) { SETAL(m, 0); return true; }   // 既に閉じている: 成功扱い
        sft_release(sft);
        mem_wb(m, f + 0x19, 0);
        SETAL(m, 0); return true; }
    case 0x11: case 0x12: SETAL(m, fcb_find(m, ah == 0x11) ? 0 : 0xFF); return true;
    case 0x13: {
        uint32_t f = fcb_base(m, nullptr);
        std::string n = fcb_name_at(m, f);
        bool any = false;
        if (n.find('?') == std::string::npos) {
            bool ex; std::string h = guest_to_host(m, n, &ex, nullptr, nullptr);
            if (ex && hostfs::remove(h)) any = true;
        } else {
            std::string pat = n.size() > 2 && n[1] == ':' ? n.substr(2) : n;
            std::vector<std::string> comps; bool exists;
            std::string dir = host_of(m, comps, 0, &exists);
            std::vector<HostDirEntry> lst = dir_list(dir);
            for (auto& e : lst) {
                if (e.is_dir) continue;
                std::string g = upper_dbcs(hostfs::to_sjis(e.name));
                if (valid83(g) && wild_match(pat, g) && hostfs::remove(hostfs::join(dir, e.name))) any = true;
            }
        }
        if (any) dir_invalidate();
        SETAL(m, any ? 0 : 0xFF); return true; }
    case 0x14: case 0x15: {
        uint32_t f = fcb_base(m, nullptr);
        int sft = fcb_sft(m, f);
        if (sft < 0) { SETAL(m, 1); return true; }
        uint32_t rec = fcb_seq(m, f);
        uint8_t r = ah == 0x14 ? fcb_read_rec(m, f, sft, rec, dta) : fcb_write_rec(m, f, sft, rec, dta, fcb_recsize(m, f));
        if (r != 1) fcb_set_seq(m, f, rec + 1);
        SETAL(m, r); return true; }
    case 0x17: {
        uint32_t f = fcb_base(m, nullptr);
        std::string a = fcb_name_at(m, f);
        std::string b;
        for (int i = 0; i < 8; i++) b.push_back((char)mem_rb(m, f + 0x11 + i));
        while (!b.empty() && b.back() == ' ') b.pop_back();
        std::string x; for (int i = 0; i < 3; i++) x.push_back((char)mem_rb(m, f + 0x19 + i));
        while (!x.empty() && x.back() == ' ') x.pop_back();
        if (!x.empty()) b += "." + x;
        bool e1, e2;
        std::string ha = guest_to_host(m, a, &e1, nullptr, nullptr), hb = guest_to_host(m, b, &e2, nullptr, nullptr);
        bool ok = e1 && !e2 && a.find('?') == std::string::npos && hostfs::rename(ha, hb);
        if (ok) dir_invalidate();
        SETAL(m, ok ? 0 : 0xFF); return true; }
    case 0x21: case 0x22: {
        uint32_t f = fcb_base(m, nullptr);
        int sft = fcb_sft(m, f);
        if (sft < 0) { SETAL(m, 1); return true; }
        uint16_t rs = fcb_recsize(m, f);
        uint32_t rec = fcb_random(m, f, rs);
        fcb_set_seq(m, f, rec);
        SETAL(m, ah == 0x21 ? fcb_read_rec(m, f, sft, rec, dta) : fcb_write_rec(m, f, sft, rec, dta, rs));
        return true; }
    case 0x23: {
        uint32_t f = fcb_base(m, nullptr);
        std::string n = fcb_name_at(m, f);
        bool ex; std::string h = guest_to_host(m, n, &ex, nullptr, nullptr);
        HostDirEntry st;
        if (!ex || !hostfs::stat(h, st) || st.is_dir) { SETAL(m, 0xFF); return true; }
        uint16_t rs = fcb_recsize(m, f);
        fcb_set_random(m, f, (uint32_t)((st.size + rs - 1) / rs), rs);
        SETAL(m, 0); return true; }
    case 0x24: {
        uint32_t f = fcb_base(m, nullptr);
        fcb_set_random(m, f, fcb_seq(m, f), fcb_recsize(m, f));
        return true; }
    case 0x27: case 0x28: {
        uint32_t f = fcb_base(m, nullptr);
        int sft = fcb_sft(m, f);
        if (sft < 0) { SETAL(m, 1); SETCX(m, 0); return true; }
        uint16_t rs = fcb_recsize(m, f);
        uint32_t rec = fcb_random(m, f, rs);
        uint16_t n = CX(m), done = 0;
        uint8_t r = 0;
        if (ah == 0x28 && n == 0) {
            hostfs::seek(s_sft[sft].h, (int64_t)rec * rs, 0);
            hostfs::truncate_here(s_sft[sft].h);
            fcb_update_size(m, f, sft);
            SETAL(m, 0); return true;
        }
        for (; done < n; done++) {
            r = ah == 0x27 ? fcb_read_rec(m, f, sft, rec + done, dta + (uint32_t)done * rs) : fcb_write_rec(m, f, sft, rec + done, dta + (uint32_t)done * rs, rs);
            if (r == 1) break;
            if (r == 3) { done++; break; }
        }
        fcb_set_random(m, f, rec + done, rs);
        fcb_set_seq(m, f, rec + done);
        SETCX(m, done); SETAL(m, r); return true; }
    }
    return false;
}

// ---- ファイル操作 ------------------------------------------------------------
static void dos_open(Machine* m, const std::string& name, int mode, bool create, bool trunc, bool excl) {
    if (m->cfg.trace) plog("[dos] open %s mode=%d%s\n", name.c_str(), mode, create ? " create" : "");
    std::string dn = dev_name(name);
    int sft = new_sft();
    if (sft < 0) { dos_error(m, 4); return; }
    Sft& f = s_sft[sft];
    if (!dn.empty()) {
        if (dn == "CON") f.kind = 2; else if (dn == "NUL") f.kind = 3; else f.kind = 4;
    } else {
        bool exists;
        std::string canon;
        std::string host = guest_to_host(m, name, &exists, nullptr, &canon);
        HostDirEntry st;
        bool is_file = hostfs::stat(host, st);
        if (is_file && st.is_dir) { f = Sft(); dos_error(m, 5); return; }
        if (!create && !is_file) {
            // 親ディレクトリがあるかでエラーコードを分ける
            std::vector<std::string> c; split_guest(name, c, nullptr);
            bool pe = true;
            if (c.size() > 1) host_of(m, c, c.size() - 1, &pe);
            f = Sft(); dos_error(m, pe ? 2 : 3); return;
        }
        if (excl && is_file) { f = Sft(); dos_error(m, 0x50); return; }
        void* h = hostfs::open(host, (mode & 3) > 2 ? 2 : (mode & 3), create, trunc);
        if (!h) { f = Sft(); dos_error(m, create ? 5 : 2); return; }
        if (create) dir_invalidate();
        f.kind = 1; f.h = h; f.host = host; f.name = canon;
    }
    f.refs = 1;
    f.mode = mode;
    int hnd = new_handle(m, sft);
    if (hnd < 0) { sft_release(sft); dos_error(m, 4); return; }
    SETAX(m, (uint16_t)hnd);
    dos_ok(m);
}

static void dos_read(Machine* m) {
    int sft = sft_of(m, BX(m));
    if (sft < 0) { dos_error(m, 6); return; }
    Sft& f = s_sft[sft];
    uint16_t n = CX(m);
    uint32_t buf = lin(DS(m), DX(m));
    if (f.kind == 1) {
        std::vector<uint8_t> tmp(n);
        int r = n ? hostfs::read(f.h, tmp.data(), n) : 0;
        if (r < 0) { dos_error(m, 5); return; }
        for (int i = 0; i < r; i++) mem_wb(m, buf + (uint32_t)i, tmp[i]);
        SETAX(m, (uint16_t)r); dos_ok(m); return;
    }
    if (f.kind == 2) {   // CON: 1 行入力（簡易）
        if (!con_have_char(m)) { wait_retry(m); return; }
        uint8_t c = con_get_char(m);
        if (c == 0x0D) { if (n >= 2) { mem_wb(m, buf, 0x0D); mem_wb(m, buf + 1, 0x0A); SETAX(m, 2); } else { mem_wb(m, buf, 0x0D); SETAX(m, 1); } console_putc(m, 0x0D); console_putc(m, 0x0A); }
        else { mem_wb(m, buf, c); SETAX(m, 1); console_putc(m, c); }
        dos_ok(m); return;
    }
    SETAX(m, 0); dos_ok(m);
}
static void dos_write(Machine* m) {
    int sft = sft_of(m, BX(m));
    if (sft < 0) { dos_error(m, 6); return; }
    Sft& f = s_sft[sft];
    uint16_t n = CX(m);
    uint32_t buf = lin(DS(m), DX(m));
    if (f.kind == 1) {
        if (n == 0) { hostfs::truncate_here(f.h); SETAX(m, 0); dos_ok(m); return; }
        std::vector<uint8_t> tmp(n);
        for (int i = 0; i < n; i++) tmp[i] = mem_rb(m, buf + (uint32_t)i);
        int r = hostfs::write(f.h, tmp.data(), n);
        if (r < 0) { dos_error(m, 5); return; }
        SETAX(m, (uint16_t)r); dos_ok(m); return;
    }
    if (f.kind == 2) for (int i = 0; i < n; i++) console_putc(m, mem_rb(m, buf + (uint32_t)i));
    SETAX(m, n); dos_ok(m);
}

static int dos_resolve_exec(Machine* m, const std::string& name, std::string* found);

// ---- EXEC --------------------------------------------------------------------
static bool run_command(Machine* m, const std::string& cmdline);
static void dos_exec(Machine* m) {
    uint8_t al = AL(m);
    std::string name = read_asciiz(m, DS(m), DX(m));
    uint32_t pb = lin(ES(m), BX(m));
    if (al == 3) {   // オーバーレイ読み込み
        uint16_t seg = rw(m, pb), reloc = rw(m, pb + 2);
        bool exists; std::string host = guest_to_host(m, name, &exists, nullptr, nullptr);
        std::vector<uint8_t> data;
        if (!exists || !read_host_file(host, data)) { dos_error(m, 2); return; }
        if (m->cfg.trace) plog("[dos] オーバーレイ読み込み %s → %04X（再配置 %04X）\n", name.c_str(), seg, reloc);
        if (data.size() >= 0x1C && data[0] == 'M' && data[1] == 'Z') {
            uint16_t cblp = (uint16_t)(data[2] | (data[3] << 8)), cp = (uint16_t)(data[4] | (data[5] << 8));
            uint16_t crlc = (uint16_t)(data[6] | (data[7] << 8)), hdr = (uint16_t)(data[8] | (data[9] << 8));
            uint16_t lfarlc = (uint16_t)(data[24] | (data[25] << 8));
            uint32_t img = (uint32_t)cp * 512 - (cblp ? (512 - cblp) : 0) - (uint32_t)hdr * 16;
            if ((uint32_t)hdr * 16 + img > data.size()) img = (uint32_t)data.size() - (uint32_t)hdr * 16;
            for (uint32_t i = 0; i < img; i++) m->ram[(lin(seg, 0) + i) & 0xFFFFF] = data[(uint32_t)hdr * 16 + i];
            for (uint32_t i = 0; i < crlc; i++) {
                uint32_t r = lfarlc + i * 4;
                uint16_t off = (uint16_t)(data[r] | (data[r + 1] << 8)), sg = (uint16_t)(data[r + 2] | (data[r + 3] << 8));
                uint32_t a = lin((uint16_t)(sg + seg), off);
                ww(m, a, (uint16_t)(rw(m, a) + reloc));
            }
        } else {
            for (size_t i = 0; i < data.size(); i++) m->ram[(lin(seg, 0) + i) & 0xFFFFF] = data[i];
        }
        dos_ok(m); return;
    }
    uint16_t env = rw(m, pb);
    uint16_t tail_off = rw(m, pb + 2), tail_seg = rw(m, pb + 4);
    uint8_t tlen = mem_rb(m, lin(tail_seg, tail_off));
    std::string tail;
    for (int i = 0; i < tlen && i < 127; i++) {
        uint8_t c = mem_rb(m, lin(tail_seg, (uint16_t)(tail_off + 1 + i)));
        if (c == 0x0D) break;
        tail.push_back((char)c);
    }
    bool as_given = true;   // 環境に呼び出し側の名前をそのまま書く
    // COMMAND.COM /C xxx は xxx を直接起動する
    std::string base = name;
    size_t sl = base.find_last_of("\\/:");
    if (sl != std::string::npos) base = base.substr(sl + 1);
    base = upper_dbcs(base);
    if (base == "COMMAND.COM") {
        std::string t = tail;
        size_t p = upper_dbcs(t).find("/C");
        if (p == std::string::npos) { s_retcode = 0; dos_ok(m); return; }
        t = t.substr(p + 2);
        while (!t.empty() && t[0] == ' ') t.erase(0, 1);
        size_t sp = t.find(' ');
        std::string prog = sp == std::string::npos ? t : t.substr(0, sp);
        tail = sp == std::string::npos ? "" : t.substr(sp);
        // 内部コマンド（COPY / MD / DEL など）は、シェルの実装でその場で実行して戻る
        // （インストーラが COMMAND.COM /C COPY B:\*.AIC A: > NUL のように使う）
        {
            std::string up = upper_dbcs(prog);
            static const char* const internal[] = {"COPY", "XCOPY", "DEL", "ERASE", "MD", "MKDIR", "RD", "RMDIR", "REN", "RENAME",
                                                   "TYPE", "ECHO", "CD", "CHDIR", "CLS", "SET", "PATH", "DIR", "VER", "VOL", "REM", "ATTRIB", "SUBST"};
            bool is_int = up.size() == 2 && up[1] == ':';
            for (const char* k : internal) if (up == k) is_int = true;
            if (is_int) {
                if (m->cfg.trace) plog("[dos] COMMAND /C %s\n", t.c_str());
                run_command(m, t);
                s_retcode = 0; dos_ok(m); return;
            }
        }
        std::string found;
        if (dos_resolve_exec(m, prog, &found) != 0) { s_retcode = 1; dos_ok(m); return; }
        name = found;
        as_given = false;
    }
    LoadResult lr;
    int err = load_program(m, name, tail, env ? env : rw(m, lin(s_psp, 0x2C)), s_psp, &lr, as_given ? name.c_str() : nullptr);
    if (err) { dos_error(m, (uint16_t)err); return; }
    if (al == 1) {
        ww(m, pb + 0x0E, lr.sp); ww(m, pb + 0x10, lr.ss);
        ww(m, pb + 0x12, lr.ip); ww(m, pb + 0x14, lr.cs);
        s_psp = lr.psp;
        dos_ok(m); return;
    }
    ExecFrame f;
    Cpu* c = &m->cpu;
    for (int i = 0; i < 8; i++) f.r[i] = c->r[i];
    for (int i = 0; i < 6; i++) f.sr[i] = c->sr[i];
    f.ip = c->ip; f.cs = c->sr[CS_]; f.fl = c->fl;
    f.parent_psp = s_psp; f.child_psp = lr.psp;
    f.dta_seg = s_dta_seg; f.dta_off = s_dta_off;
    ww(m, lin(s_psp, 0x2E), (uint16_t)c->r[ESP]); ww(m, lin(s_psp, 0x30), c->sr[SS_]);
    s_frames.push_back(f);
    start_program(m, lr);
}

// ---- INT 21h -----------------------------------------------------------------
static void int21(Machine* m) {
    uint8_t ah = AH(m);
    Cpu* c = &m->cpu;
    if (m->cfg.trace) {
        // どの機能を使っているかの記録（機能ごとに最初の 4 回、メモリの確保・解放は 64 回。44h は AL ごと）
        static uint8_t seen[256][256];
        uint8_t sub = (ah == 0x44 || ah == 0x33 || ah == 0x58 || ah == 0x65) ? AL(m) : 0;
        int lim = (ah == 0x48 || ah == 0x49 || ah == 0x4A || ah == 0x58) ? 64 : 4;   // メモリの確保・解放は多めに残す
        if (seen[ah][sub] < lim && ah != 0x3F && ah != 0x40 && ah != 0x42) {
            seen[ah][sub]++;
            plog("[dos] INT21 AX=%04X BX=%04X CX=%04X DX=%04X\n", AX(m), BX(m), CX(m), DX(m));
        }
    }
    switch (ah) {
    case 0x00: terminate(m, 0, 0, 0); return;
    case 0x01: case 0x07: case 0x08: {
        if (!con_have_char(m)) { wait_retry(m); return; }
        uint8_t ch = con_get_char(m);
        if (ah == 0x01) console_putc(m, ch);
        SETAL(m, ch); return; }
    case 0x02: console_putc(m, DL(m)); return;
    case 0x03: SETAL(m, 0); return;
    case 0x04: case 0x05: return;
    case 0x06:
        if (DL(m) == 0xFF) {
            if (con_have_char(m)) { SETAL(m, con_get_char(m)); set_zf(m, false); }
            else { SETAL(m, 0); set_zf(m, true); }
        } else console_putc(m, DL(m));
        return;
    case 0x09: {
        uint32_t a = lin(DS(m), DX(m));
        for (int i = 0; i < 65536; i++) { uint8_t ch = mem_rb(m, a + (uint32_t)i); if (ch == '$') break; console_putc(m, ch); }
        return; }
    case 0x0A: {   // バッファ付き入力
        uint32_t a = lin(DS(m), DX(m));
        uint8_t max = mem_rb(m, a);
        static std::string line;
        static bool active = false;
        if (!active) { line.clear(); active = true; }
        while (con_have_char(m)) {
            uint8_t ch = con_get_char(m);
            if (ch == 0x0D) {
                for (size_t i = 0; i < line.size(); i++) mem_wb(m, a + 2 + (uint32_t)i, (uint8_t)line[i]);
                mem_wb(m, a + 2 + (uint32_t)line.size(), 0x0D);
                mem_wb(m, a + 1, (uint8_t)line.size());
                console_putc(m, 0x0D);
                active = false;
                return;
            }
            if (ch == 0x08) { if (!line.empty()) { line.pop_back(); console_putc(m, 8); console_putc(m, ' '); console_putc(m, 8); } continue; }
            if (line.size() + 1 < max) { line.push_back((char)ch); console_putc(m, ch); }
        }
        wait_retry(m); return; }
    case 0x0B: SETAL(m, con_have_char(m) ? 0xFF : 0x00); return;
    case 0x0C: {
        while (bios_key_available(m)) bios_key_read(m, true);
        s_pending_input.clear();
        uint8_t sub = AL(m);
        if (sub == 0x01 || sub == 0x06 || sub == 0x07 || sub == 0x08 || sub == 0x0A) {
            c->r[EAX] = (c->r[EAX] & 0xFFFF00FFu) | ((uint32_t)sub << 8);
            int21(m);
        }
        return; }
    case 0x0D: return;
    case 0x0E: { char d = (char)('A' + DL(m)); if (drive_valid(m, d)) s_curdrv = d == m->cfg.drive ? 0 : d; SETAL(m, 26); return; }
    case 0x19: SETAL(m, (uint8_t)(cur_drive(m) - 'A')); return;
    case 0x1A: s_dta_seg = DS(m); s_dta_off = DX(m); return;
    case 0x1B: case 0x1C: SETAL(m, 16); SETCX(m, 1024); SETDX(m, hdd_total_clusters(m)); return;
    case 0x25: { uint8_t v = AL(m); ww(m, v * 4u, DX(m)); ww(m, v * 4u + 2, DS(m)); return; }
    case 0x2A: {
        PcTime lt; machine_now(m, &lt);
        SETCX(m, (uint16_t)lt.year);
        SETDX(m, (uint16_t)((lt.month << 8) | lt.day));
        SETAL(m, (uint8_t)lt.wday); return; }
    case 0x2B: SETAL(m, 0); return;
    case 0x2C: {
        PcTime lt; machine_now(m, &lt);
        uint32_t cs100 = (uint32_t)((m->ticks / (MASTER_CLOCK / 100)) % 100);
        SETCX(m, (uint16_t)((lt.hour << 8) | lt.min));
        SETDX(m, (uint16_t)((lt.sec << 8) | cs100)); return; }
    case 0x2D: SETAL(m, 0); return;
    case 0x2E: s_verify = AL(m) & 1; return;
    case 0x2F: cpu_setsr(c, ES_, s_dta_seg); SETBX(m, s_dta_off); return;
    case 0x30: SETAX(m, (uint16_t)(((m->cfg.dos_version & 0xFF) << 8) | ((m->cfg.dos_version >> 8) & 0xFF)) );
               SETBX(m, 0xFF00); SETCX(m, 0); return;
    case 0x31: terminate(m, AL(m), 3, DX(m)); return;
    case 0x1F: case 0x32: {   // DPB の取得（ゲームのドライブだけ持っている）
        int dr = ah == 0x1F || DL(m) == 0 ? (m->cfg.drive - 'A') : DL(m) - 1;
        if (dr != m->cfg.drive - 'A') { SETAL(m, 0xFF); return; }
        cpu_setsr(c, DS_, DOSSEG); SETBX(m, D_DPB); SETAL(m, 0); return; }
    case 0x33: {
        uint8_t al = AL(m);
        if (al == 0) R(m)[EDX] = (R(m)[EDX] & ~0xFFu) | s_break_flag;
        else if (al == 1) s_break_flag = DL(m) & 1;
        else if (al == 5) R(m)[EDX] = (R(m)[EDX] & ~0xFFu) | 1;   // 起動ドライブ
        else if (al == 6) { SETBX(m, 0x0005); SETDX(m, 0); }
        return; }
    case 0x34: cpu_setsr(c, ES_, DOSSEG); SETBX(m, D_INDOS); return;
    case 0x35: { uint8_t v = AL(m); SETBX(m, rw(m, v * 4u)); cpu_setsr(c, ES_, rw(m, v * 4u + 2)); return; }
    case 0x36: {
        char d = DL(m) ? (char)('A' + DL(m) - 1) : cur_drive(m);
        if (!drive_valid(m, d)) { SETAX(m, 0xFFFF); return; }
        if (d == floppy::drive_letter() && !floppy::folder().empty()) {   // フォルダのフロッピー: 1.25MB の 2HD らしく見せる
            SETAX(m, 1); SETBX(m, 600); SETCX(m, 1024); SETDX(m, 1221); return;
        }
        if (d != m->cfg.drive && !s_subst.count(d)) {   // SUBST のドライブはゲームのドライブと同じ大きさに見せる
            uint32_t spc, bps, fr, tot;
            if (!fatfs::free_space_drive(d, &spc, &bps, &fr, &tot)) { SETAX(m, 0xFFFF); return; }
            SETAX(m, (uint16_t)spc); SETBX(m, (uint16_t)fr); SETCX(m, (uint16_t)bps); SETDX(m, (uint16_t)tot); return;
        }
        // ゲームのドライブ: 実際のディスクの大きさではなく、当時のハードディスクらしい大きさを見せる
        // （今の何百 GB をそのまま返すと、16/32 ビットの計算があふれて「空きが足りない」になるソフトがある）。
        // 16KB クラスタ（1024 バイト x 16）で、空きは FreeSpaceMB（既定 96MB）、全体は 128MB 以上
        SETAX(m, 16); SETBX(m, hdd_free_clusters(m)); SETCX(m, 1024); SETDX(m, hdd_total_clusters(m)); return; }
    case 0x37:
        if (AL(m) == 0) R(m)[EDX] = (R(m)[EDX] & ~0xFFu) | '/';
        SETAL(m, 0); return;
    case 0x38: {
        uint32_t a = lin(DS(m), DX(m));
        uint8_t info[34] = {2,0, '\\',0,0,0,0, ',',0, '.',0, '-',0, ':',0, 0, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0,0,0};
        info[0x12] = 0; info[0x13] = 0; // 大文字化関数は後で
        for (int i = 0; i < 34; i++) mem_wb(m, a + i, info[i]);
        ww(m, a + 0x12, D_UPCASE); ww(m, a + 0x14, DOSSEG);
        SETBX(m, 81); dos_ok(m); return; }
    case 0x39: case 0x3A: {
        std::string n = read_asciiz(m, DS(m), DX(m));
        bool ex; std::string h = guest_to_host(m, n, &ex, nullptr, nullptr);
        bool ok = ah == 0x39 ? hostfs::mkdir(h) : hostfs::rmdir(h);
        dir_invalidate();
        if (ok) dos_ok(m); else dos_error(m, ah == 0x39 ? 5 : 3);
        return; }
    case 0x3B: {
        std::string n = read_asciiz(m, DS(m), DX(m));
        std::vector<std::string> comps; split_guest(n, comps, nullptr);
        bool ex; std::string h = host_of(m, comps, comps.size(), &ex);
        HostDirEntry st;
        if (!ex || !hostfs::stat(h, st) || !st.is_dir) { dos_error(m, 3); return; }
        std::string s;
        for (size_t i = 0; i < comps.size(); i++) { if (i) s += "\\"; s += comps[i]; }
        cwd_of(m, s_sg_drive) = s;
        dos_ok(m); return; }
    case 0x3C: dos_open(m, read_asciiz(m, DS(m), DX(m)), 2, true, true, false); return;
    case 0x3D: dos_open(m, read_asciiz(m, DS(m), DX(m)), AL(m), false, false, false); return;
    case 0x3E: { int s = sft_of(m, BX(m)); if (s < 0) { dos_error(m, 6); return; } close_handle(m, BX(m)); dos_ok(m); return; }
    case 0x3F: dos_read(m); return;
    case 0x40: dos_write(m); return;
    case 0x41: {
        std::string n = read_asciiz(m, DS(m), DX(m));
        bool ex; std::string h = guest_to_host(m, n, &ex, nullptr, nullptr);
        if (!ex) { dos_error(m, 2); return; }
        if (hostfs::remove(h)) { dir_invalidate(); dos_ok(m); } else dos_error(m, 5);
        return; }
    case 0x42: {
        int s = sft_of(m, BX(m));
        if (s < 0) { dos_error(m, 6); return; }
        Sft& f = s_sft[s];
        if (f.kind != 1) { SETAX(m, 0); SETDX(m, 0); dos_ok(m); return; }
        int32_t off = (int32_t)(((uint32_t)CX(m) << 16) | DX(m));
        int64_t r = hostfs::seek(f.h, off, AL(m) > 2 ? 0 : AL(m));
        if (r < 0) { dos_error(m, 0x19); return; }
        SETAX(m, (uint16_t)r); SETDX(m, (uint16_t)(r >> 16)); dos_ok(m); return; }
    case 0x43: {
        std::string n = read_asciiz(m, DS(m), DX(m));
        bool ex; std::string h = guest_to_host(m, n, &ex, nullptr, nullptr);
        HostDirEntry st;
        if (m->cfg.trace) { static int cnt = 0; if (cnt++ < 40) plog("[dos] attr %s\n", n.c_str()); }
        if (!ex || !hostfs::stat(h, st)) { dos_error(m, 2); return; }
        if (AL(m) == 0) { SETCX(m, (uint16_t)((st.is_dir ? 0x10 : 0x20) | (st.readonly ? 1 : 0))); }
        dos_ok(m); return; }
    case 0x44: {
        uint8_t al = AL(m);
        if (al == 0x00 || al == 0x01 || al == 0x06 || al == 0x07 || al == 0x0A) {
            int s = sft_of(m, BX(m));
            if (s < 0) { dos_error(m, 6); return; }
            Sft& f = s_sft[s];
            if (al == 0x00) {
                uint16_t info = f.kind == 1 ? (uint16_t)(m->cfg.drive - 'A') : f.kind == 2 ? 0x80D3 : f.kind == 3 ? 0x8084 : 0x80C0;
                SETDX(m, info); SETAX(m, info);
            } else if (al == 0x06) {
                if (f.kind == 1) {
                    int64_t cur = hostfs::seek(f.h, 0, 1), end = hostfs::seek(f.h, 0, 2);
                    hostfs::seek(f.h, cur, 0);
                    SETAL(m, cur < end ? 0xFF : 0x00);
                } else if (f.kind == 2) SETAL(m, con_have_char(m) ? 0xFF : 0x00);
                else SETAL(m, 0x00);
            } else if (al == 0x07) SETAL(m, 0xFF);
            else if (al == 0x0A) SETDX(m, 0);
            dos_ok(m); return;
        }
        if (al == 0x08) { SETAX(m, 1); dos_ok(m); return; }
        if (al == 0x09) { SETDX(m, 0); dos_ok(m); return; }
        if (al == 0x0E) { SETAL(m, 0); dos_ok(m); return; }
        if (al == 0x0B) { dos_ok(m); return; }
        dos_error(m, 1); return; }
    case 0x45: {
        int s = sft_of(m, BX(m));
        if (s < 0) { dos_error(m, 6); return; }
        int h = new_handle(m, s);
        if (h < 0) { dos_error(m, 4); return; }
        s_sft[s].refs++;
        SETAX(m, (uint16_t)h); dos_ok(m); return; }
    case 0x46: {
        int s = sft_of(m, BX(m));
        if (s < 0) { dos_error(m, 6); return; }
        uint16_t nh = CX(m);
        int size; uint32_t a = jft_addr(m, s_psp, &size);
        if (nh >= size) { dos_error(m, 6); return; }
        if (mem_rb(m, a + nh) != 0xFF) close_handle(m, nh);
        mem_wb(m, a + nh, (uint8_t)s); s_sft[s].refs++;
        dos_ok(m); return; }
    case 0x47: {
        char d = DL(m) ? (char)('A' + DL(m) - 1) : cur_drive(m);
        if (!drive_valid(m, d)) { dos_error(m, 15); return; }
        std::string s = cwd_of(m, d);
        write_asciiz(m, DS(m), SI(m), s);
        SETAX(m, 0x0100); dos_ok(m); return; }
    case 0x48: {
        uint16_t seg, largest = 0, want = BX(m);
        int e = mem_alloc(m, want, s_psp, &seg, &largest);
        if (e) { SETBX(m, largest); dos_error(m, (uint16_t)e); if (m->cfg.trace) plog("[dos]   確保 %u 段落 → 失敗（最大 %u 段落）\n", (unsigned)want, (unsigned)largest); return; }
        if (m->cfg.trace) { static int n = 0; if (n++ < 64) plog("[dos]   確保 → %04X（%u 段落、持ち主 %04X）\n", seg, (unsigned)want, s_psp); }
        SETAX(m, seg); dos_ok(m); return; }
    case 0x49: { int e = mem_free(m, ES(m)); mcb_merge(m); if (e) dos_error(m, (uint16_t)e); else dos_ok(m); return; }
    case 0x4A: {
        uint16_t maxp = 0;
        int e = mem_resize(m, ES(m), BX(m), &maxp);
        if (e) { SETBX(m, maxp); dos_error(m, (uint16_t)e); return; }
        // 実機の DOS は PSP の「メモリの終わり」（PSP:2）を書き換えない（ここを見て子に渡すローダがある）
        dos_ok(m); return; }
    case 0x4B: dos_exec(m); return;
    case 0x4C: terminate(m, AL(m), 0, 0); return;
    case 0x4D: SETAX(m, s_retcode); s_retcode = 0; dos_ok(m); return;
    case 0x4E: dos_find_first(m); return;
    case 0x4F: dos_find_next(m); return;
    case 0x50: s_psp = BX(m); return;
    case 0x26:     // 新しい PSP を作る（今の PSP を写す。ハンドルは複製しない。今の PSP は変えない）
    case 0x55: {   // 子の PSP を作る（DX=セグメント, SI=メモリの終わり。ハンドルを継承し、今の PSP を DX にする）
        uint16_t np = DX(m);
        uint32_t src = lin(s_psp, 0), dst = lin(np, 0);
        if (src != dst) for (int i = 0; i < 0x100; i++) mem_wb(m, dst + i, mem_rb(m, src + i));
        if (AH(m) == 0x26) {
            ww(m, dst + 2, rw(m, lin(s_psp, 2)));
        } else {
            ww(m, dst + 2, SI(m));
            ww(m, dst + 0x16, s_psp);
            for (int i = 0; i < 20; i++) mem_wb(m, dst + 0x18 + i, 0xFF);
            ww(m, dst + 0x32, 20); ww(m, dst + 0x34, 0x18); ww(m, dst + 0x36, np);
            ww(m, dst + 0x38, 0xFFFF); ww(m, dst + 0x3A, 0xFFFF);
            inherit_handles(m, s_psp, np);
        }
        for (int i = 0; i < 3; i++) {   // INT 22h/23h/24h の今のベクタ
            ww(m, dst + 0x0A + i * 4, rw(m, (0x22 + i) * 4));
            ww(m, dst + 0x0C + i * 4, rw(m, (0x22 + i) * 4 + 2));
        }
        if (m->cfg.trace) plog("[dos] PSP を作りました: %04X（元 %04X, AH=%02X, 終わり %04X）\n", np, s_psp, AH(m), rw(m, dst + 2));
        if (AH(m) == 0x55) { s_psp = np; SETAL(m, 0xF0); }
        return; }
    case 0x51: case 0x62: SETBX(m, s_psp); return;
    case 0x52: cpu_setsr(c, ES_, DOSSEG); SETBX(m, D_LOL); return;
    case 0x54: SETAL(m, s_verify); return;
    case 0x56: {
        std::string a = read_asciiz(m, DS(m), DX(m)), b = read_asciiz(m, ES(m), DI(m));
        bool ex1, ex2;
        std::string ha = guest_to_host(m, a, &ex1, nullptr, nullptr), hb = guest_to_host(m, b, &ex2, nullptr, nullptr);
        if (!ex1) { dos_error(m, 2); return; }
        if (ex2) { dos_error(m, 5); return; }
        if (hostfs::rename(ha, hb)) { dir_invalidate(); dos_ok(m); } else dos_error(m, 5);
        return; }
    case 0x57: {
        int s = sft_of(m, BX(m));
        if (s < 0) { dos_error(m, 6); return; }
        Sft& f = s_sft[s];
        if (AL(m) == 0) {
            uint16_t d = 0x2821, t = 0;
            if (f.kind == 1) { hostfs::get_time(f.h, &d, &t); d = machine_file_date(m, d); }
            SETCX(m, t); SETDX(m, d);
        } else if (f.kind == 1) hostfs::set_time(f.h, DX(m), CX(m));
        dos_ok(m); return; }
    case 0x58:
        if (AL(m) == 0) { SETAX(m, s_alloc_strategy); dos_ok(m); }
        else if (AL(m) == 1) { s_alloc_strategy = (uint8_t)BX(m); dos_ok(m); }
        else if (AL(m) == 2) { SETAL(m, 0); dos_ok(m); }
        else dos_ok(m);
        return;
    case 0x59: SETAX(m, s_last_err); R(m)[EBX] = (R(m)[EBX] & 0xFFFF0000u) | 0x0104; R(m)[ECX] = (R(m)[ECX] & 0xFFFF00FFu) | 0x0100; return;
    case 0x5A: {
        std::string dir = read_asciiz(m, DS(m), DX(m));
        char nm[16];
        for (int i = 0; i < 1000; i++) {
            snprintf(nm, sizeof(nm), "TMP%05u.$$$", (unsigned)(rand() % 100000));
            std::string full = dir;
            if (!full.empty() && full.back() != '\\') full += "\\";
            full += nm;
            bool ex; guest_to_host(m, full, &ex, nullptr, nullptr);
            if (ex) continue;
            write_asciiz(m, DS(m), DX(m), full);
            dos_open(m, full, 2, true, true, false);
            return;
        }
        dos_error(m, 5); return; }
    case 0x5B: dos_open(m, read_asciiz(m, DS(m), DX(m)), 2, true, true, true); return;
    case 0x5C: dos_ok(m); return;
    case 0x5D: case 0x5E: case 0x5F: dos_error(m, 1); return;
    case 0x60: {
        std::string n = read_asciiz(m, DS(m), SI(m));
        std::string canon;
        guest_to_host(m, n, nullptr, nullptr, &canon);
        write_asciiz(m, ES(m), DI(m), std::string(1, s_sg_drive ? s_sg_drive : m->cfg.drive) + ":\\" + canon);
        dos_ok(m); return; }
    case 0x63:
        if (AL(m) == 0) { cpu_setsr(c, DS_, DOSSEG); SETSI(m, D_DBCS); SETAL(m, 0); }
        else SETAL(m, 0);
        dos_ok(m); return;
    case 0x65: {
        uint8_t al = AL(m);
        if (al == 7) {
            uint32_t a = lin(ES(m), DI(m));
            mem_wb(m, a, 7); ww(m, a + 1, D_DBCS - 2); ww(m, a + 3, DOSSEG);
            SETCX(m, 5); dos_ok(m); return;
        }
        if (al == 1) {
            uint32_t a = lin(ES(m), DI(m));
            uint8_t info[41] = {1, 38, 0, 81, 0, 0xB5, 0x03};
            for (int i = 0; i < 41; i++) mem_wb(m, a + i, info[i]);
            SETCX(m, 41); dos_ok(m); return;
        }
        if (al == 0x20) { uint8_t ch = DL(m); if (ch >= 'a' && ch <= 'z') ch -= 0x20; R(m)[EDX] = (R(m)[EDX] & ~0xFFu) | ch; dos_ok(m); return; }
        dos_error(m, 1); return; }
    case 0x66: SETBX(m, 932); SETDX(m, 932); dos_ok(m); return;
    case 0x67: dos_ok(m); return;
    case 0x68: case 0x6A: dos_ok(m); return;
    case 0x6C: {
        std::string n = read_asciiz(m, DS(m), SI(m));
        uint16_t mode = BX(m), act = DX(m);
        bool ex; std::string h = guest_to_host(m, n, &ex, nullptr, nullptr);
        HostDirEntry st;
        bool is = hostfs::stat(h, st);
        if (is) {
            if ((act & 0x0F) == 0) { dos_error(m, 0x50); return; }
            bool tr = (act & 0x0F) == 2;
            dos_open(m, n, mode & 0xFF, false, false, false);
            if (tr && !(c->fl & FL_CF)) { int s = sft_of(m, AX(m)); if (s >= 0) hostfs::truncate_here(s_sft[s].h); }
            if (!(c->fl & FL_CF)) SETCX(m, tr ? 3 : 1);
        } else {
            if (!(act & 0x10)) { dos_error(m, 2); return; }
            dos_open(m, n, mode & 0xFF, true, true, false);
            if (!(c->fl & FL_CF)) SETCX(m, 2);
        }
        return; }
    case 0x29: {   // ファイル名を FCB へ
        uint32_t src = lin(DS(m), SI(m));
        uint32_t fcb = lin(ES(m), DI(m));
        std::string s;
        uint32_t p = src;
        uint8_t ch;
        while ((ch = mem_rb(m, p)) == ' ' || ch == '\t') p++;
        if ((AL(m) & 1) && (ch == ':' || ch == ';' || ch == '.' || ch == ',' || ch == '=' || ch == '+')) { p++; while (mem_rb(m, p) == ' ') p++; }
        uint32_t start = p;
        for (;;) {
            ch = mem_rb(m, p);
            if (ch <= ' ' || ch == '/' || ch == '"' || ch == '[' || ch == ']' || ch == '+' || ch == '=' || ch == ';' || ch == ',' || ch == '|' || ch == '<' || ch == '>') break;
            s.push_back((char)ch); p++;
        }
        (void)start;
        parse_fcb(m, fcb, s);
        bool wild = s.find('*') != std::string::npos || s.find('?') != std::string::npos;
        if (wild) {
            // * を ? で埋める
            for (int i = 0; i < 11; i++) {
                if (mem_rb(m, fcb + 1 + i) == '*') {
                    int end = i < 8 ? 8 : 11;
                    for (int k = i; k < end; k++) mem_wb(m, fcb + 1 + k, '?');
                }
            }
        }
        SETAL(m, wild ? 1 : 0);
        SETSI(m, (uint16_t)(p - lin(DS(m), 0)));
        return; }
    default:
        if (fcb_call(m, ah)) return;
        if (m->cfg.trace) plog("[dos] INT 21h AH=%02Xh 未対応\n", ah);
        SETAL(m, 0);
        dos_error(m, 1);
        return;
    }
}

// ---- シェル（バッチ）----------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n' || s[b - 1] == 0x1A)) b--;
    return s.substr(a, b - a);
}
static void shell_print(Machine* m, const std::string& s) {
    for (char c : s) console_putc(m, (uint8_t)c);
    console_putc(m, 0x0D); console_putc(m, 0x0A);
}

static int dos_resolve_exec(Machine* m, const std::string& name, std::string* found) {
    static const char* exts[] = {".COM", ".EXE", ".BAT"};
    std::string up = upper_dbcs(name);
    bool has_ext = up.find('.', up.find_last_of("\\/:") == std::string::npos ? 0 : up.find_last_of("\\/:")) != std::string::npos;
    std::vector<std::string> dirs;
    dirs.push_back("");
    if (up.find_first_of("\\/:") == std::string::npos) {
        std::string path = s_env.count("PATH") ? s_env["PATH"] : "";
        std::string cur;
        for (char c : path + ";") { if (c == ';') { if (!cur.empty()) dirs.push_back(cur); cur.clear(); } else cur.push_back(c); }
        dirs.push_back("\\");
    }
    for (const std::string& d : dirs) {
        std::string base = d.empty() ? up : (d.back() == '\\' ? d + up : d + "\\" + up);
        if (has_ext) {
            bool ex; std::string h = guest_to_host(m, base, &ex, nullptr, nullptr);
            HostDirEntry st;
            if (ex && hostfs::stat(h, st) && !st.is_dir) { *found = base; return 0; }
        } else {
            for (const char* e : exts) {
                bool ex; std::string h = guest_to_host(m, base + e, &ex, nullptr, nullptr);
                HostDirEntry st;
                if (ex && hostfs::stat(h, st) && !st.is_dir) { *found = base + e; return 0; }
            }
        }
    }
    return 2;
}

static bool load_batch(Machine* m, const std::string& guest, const std::vector<std::string>& args, bool call) {
    bool ex; std::string h = guest_to_host(m, guest, &ex, nullptr, nullptr);
    std::vector<uint8_t> data;
    if (!ex || !read_host_file(h, data)) return false;
    BatchCtx b;
    std::string cur;
    for (uint8_t c : data) {
        if (c == 0x1A) break;
        if (c == '\n') { b.lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back((char)c);
    }
    if (!cur.empty()) b.lines.push_back(cur);
    b.args = args;
    if (!call && !s_batch.empty()) s_batch.pop_back();
    s_batch.push_back(b);
    return true;
}

static std::string expand_vars(const BatchCtx& b, const std::string& line) {
    std::string r;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '%' && i + 1 < line.size()) {
            char d = line[i + 1];
            if (d >= '0' && d <= '9') { size_t k = (size_t)(d - '0'); if (k < b.args.size()) r += b.args[k]; i++; continue; }
            if (d == '%') { r.push_back('%'); i++; continue; }
            size_t e = line.find('%', i + 1);
            if (e != std::string::npos) {
                std::string v = upper_dbcs(line.substr(i + 1, e - i - 1));
                auto it = s_env.find(v);
                if (it != s_env.end()) r += it->second;
                i = e; continue;
            }
        }
        r.push_back(c);
    }
    return r;
}

// 1 コマンドを実行。プログラムを起動したら true（CPU はそちらへ移っている）。
static bool shell_exec_line(Machine* m, std::string line);

static bool run_command(Machine* m, const std::string& cmdline) {
    std::string line = trim(cmdline);
    if (line.empty()) return false;
    // リダイレクトを取り除く
    {
        std::string r; bool inq = false;
        for (size_t i = 0; i < line.size(); i++) {
            char c = line[i];
            if (c == '"') inq = !inq;
            if (!inq && (c == '>' || c == '<' || c == '|')) {
                if (c == '|') break;
                i++;
                if (i < line.size() && line[i] == '>') i++;
                while (i < line.size() && line[i] == ' ') i++;
                while (i < line.size() && line[i] != ' ' && line[i] != '>' && line[i] != '<') i++;
                i--;
                continue;
            }
            r.push_back(c);
        }
        line = trim(r);
    }
    if (line.empty()) return false;
    size_t sp = line.find_first_of(" \t/=");   // 「path=a:\」のように = で続ける書き方もある
    std::string cmd = upper_dbcs(sp == std::string::npos ? line : line.substr(0, sp));
    std::string rest = sp == std::string::npos ? "" : line.substr(sp);
    std::string arg = trim(rest);
    std::string uarg = upper_dbcs(arg);

    // 「A::」のように : が重なっても（バッチの %2: に A: が入ったとき）COMMAND.COM はドライブ変更として受け付ける
    while (cmd.size() > 2 && cmd[1] == ':' && cmd.back() == ':' && arg.empty()) cmd.pop_back();
    if (cmd.size() == 2 && cmd[1] == ':') {                               // ドライブ変更
        char d = cmd[0];
        if (drive_valid(m, d)) s_curdrv = d == m->cfg.drive ? 0 : d;
        else shell_print(m, "Invalid drive specification");
        return false;
    }
    if (cmd == "REM" || cmd == "BREAK" || cmd == "VERIFY" || cmd == "PROMPT" || cmd == "TITLE" ||
        cmd == "MODE" || cmd == "KEYB" || cmd == "CHCP" || cmd == "VER" || cmd == "VOL" || cmd == "LOADFIX") {
        if (cmd == "LOADFIX" && !arg.empty()) return run_command(m, arg);
        return false;
    }
    if (cmd == "LH" || cmd == "LOADHIGH") return run_command(m, arg);
    if (cmd == "SUBST") {
        // SUBST X: パス   … パス（ディレクトリ）を X: として見せる
        // SUBST X: /D     … 取り消す
        // SUBST           … 一覧
        std::vector<std::string> w;
        { std::string cur; for (char c : arg) { if (c == ' ' || c == '\t') { if (!cur.empty()) w.push_back(cur); cur.clear(); } else cur.push_back(c); } if (!cur.empty()) w.push_back(cur); }
        if (w.empty()) {
            for (auto& kv : s_subst) shell_print(m, std::string(1, kv.first) + ": => " + hostfs::to_sjis(kv.second));
            return false;
        }
        std::string dl = upper_dbcs(w[0]);
        char d = dl.size() == 2 && dl[1] == ':' ? dl[0] : 0;
        if (d < 'A' || d > 'Z') { shell_print(m, "Invalid parameter"); return false; }
        if (w.size() >= 2 && upper_dbcs(w[1]) == "/D") {
            if (!s_subst.erase(d)) shell_print(m, "Invalid parameter");
            s_subst_cwd.erase(d);
            if (s_curdrv == d) s_curdrv = 0;
            dir_invalidate();
            if (m->cfg.trace) plog("[dos] SUBST %c: /D\n", d);
            return false;
        }
        if (w.size() < 2) { shell_print(m, "Invalid parameter"); return false; }
        // ゲームのドライブ・フロッピーのドライブには重ねられない
        if (d == m->cfg.drive || floppy::unit_of_letter(d) >= 0) { shell_print(m, "Invalid parameter"); return false; }
        std::vector<std::string> comps; split_guest(w[1], comps, nullptr);
        bool ex = false;
        std::string host = host_of(m, comps, comps.size(), &ex);
        HostDirEntry he;
        if (!ex || !hostfs::stat(host, he) || !he.is_dir) { shell_print(m, "Path not found - " + w[1]); return false; }
        s_subst[d] = host;
        s_subst_cwd[d].clear();
        dir_invalidate();
        if (m->cfg.trace) plog("[dos] SUBST %c: %s → %s\n", d, w[1].c_str(), host.c_str());
        return false;
    }
    if (cmd == "ECHO" || cmd.rfind("ECHO.", 0) == 0) {
        if (cmd != "ECHO") { shell_print(m, cmd.size() > 5 ? line.substr(5) : ""); return false; }
        if (uarg == "OFF" || uarg == "ON") return false;
        if (arg.empty()) return false;
        shell_print(m, rest.size() > 0 ? rest.substr(1) : ""); return false;
    }
    if (cmd == "CLS") { const char* s = "\x1B[2J"; for (const char* p = s; *p; p++) console_putc(m, (uint8_t)*p); return false; }
    if (cmd == "PAUSE") {
        if (line.find('>') == std::string::npos) shell_print(m, "Hit any key when ready . . .");
        s_shell_wait = 1; return false;
    }
    if (cmd == "SET") {
        size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            std::string k = upper_dbcs(trim(arg.substr(0, eq))), v = arg.substr(eq + 1);
            if (v.empty()) s_env.erase(k); else s_env[k] = v;
        }
        return false;
    }
    if (cmd == "PATH") { if (!arg.empty()) s_env["PATH"] = arg.substr(arg[0] == '=' ? 1 : 0); return false; }
    if (cmd == "CD" || cmd == "CHDIR") {
        if (arg.empty()) return false;
        std::vector<std::string> comps; split_guest(arg, comps, nullptr);
        bool ex; host_of(m, comps, comps.size(), &ex);
        if (ex) { std::string s; for (size_t i = 0; i < comps.size(); i++) { if (i) s += "\\"; s += comps[i]; } cwd_of(m, s_sg_drive) = s; }
        return false;
    }
    if (cmd == "GOTO") {
        // ラベルは区切り文字（空白・タブ・, ; =）までを取り出し、MS-DOS 6.x までと同じく先頭 8 文字だけで比べる。
        // （GOTO LABEL の後ろに余計な語があっても、長いラベルの 9 文字目以降が違っても一致する）
        auto label_token = [](std::string t) {
            size_t a = 0;
            while (a < t.size() && (t[a] == ' ' || t[a] == '\t' || t[a] == ':')) a++;
            t = t.substr(a);
            size_t e = t.find_first_of(" \t,;=");
            if (e != std::string::npos) t = t.substr(0, e);
            if (t.size() > 8) t = t.substr(0, 8);
            return upper_dbcs(t);
        };
        if (s_batch.empty()) return false;
        std::string lbl = label_token(arg);
        BatchCtx& b = s_batch.back();
        for (size_t i = 0; i < b.lines.size(); i++) {
            std::string l = trim(b.lines[i]);
            while (!l.empty() && l[0] == '@') l = trim(l.substr(1));
            if (!l.empty() && l[0] == ':') {
                if (!lbl.empty() && label_token(l.substr(1)) == lbl) { b.pc = i + 1; return false; }
            }
        }
        plog("[shell] ラベルが見つかりません: %s\n", lbl.c_str());
        b.pc = b.lines.size();
        return false;
    }
    if (cmd == "SHIFT") { if (!s_batch.empty() && !s_batch.back().args.empty()) s_batch.back().args.erase(s_batch.back().args.begin()); return false; }
    if (cmd == "IF") {
        std::string a = arg;
        bool neg = false;
        auto word = [&](std::string& s) { s = trim(s); size_t e = s.find_first_of(" \t"); std::string w = e == std::string::npos ? s : s.substr(0, e); s = e == std::string::npos ? "" : s.substr(e); return w; };
        std::string w = a; std::string first = word(w);
        if (upper_dbcs(first) == "NOT") { neg = true; a = w; w = a; first = word(w); }
        // COMMAND.COM は "=" も区切りとして扱う（IF ERRORLEVEL==5 / IF EXIST=FILE など）
        {
            std::string uf = upper_dbcs(first);
            for (const char* kw : {"ERRORLEVEL", "EXIST"}) {
                size_t kl = strlen(kw);
                if (uf.size() > kl && uf.compare(0, kl, kw) == 0 && uf[kl] == '=') {
                    size_t q = kl; while (q < first.size() && first[q] == '=') q++;
                    w = " " + first.substr(q) + w; first = first.substr(0, kl);
                }
            }
            if (upper_dbcs(first) == "ERRORLEVEL" || upper_dbcs(first) == "EXIST") {
                std::string t = trim(w);
                size_t q = 0; while (q < t.size() && t[q] == '=') q++;
                w = " " + t.substr(q);
            }
        }
        bool cond = false;
        std::string after;
        if (upper_dbcs(first) == "ERRORLEVEL") {
            std::string num = word(w);
            cond = (int)(s_retcode_last_get()) >= atoi(num.c_str());
            after = w;
        } else if (upper_dbcs(first) == "EXIST") {
            std::string f = word(w);
            bool ex; std::string h = guest_to_host(m, f, &ex, nullptr, nullptr);
            HostDirEntry st;
            if (upper_dbcs(f).size() >= 4 && upper_dbcs(f).substr(upper_dbcs(f).size() - 4) == "\\NUL") {
                std::string d = f.substr(0, f.size() - 4);
                guest_to_host(m, d, &ex, nullptr, nullptr);
                cond = ex;
            } else cond = ex && hostfs::stat(h, st);
            after = w;
        } else {
            size_t eq = a.find("==");
            if (eq == std::string::npos) return false;
            std::string l = trim(a.substr(0, eq));
            std::string r = trim(a.substr(eq + 2));
            size_t e = r.find_first_of(" \t");
            std::string rv = e == std::string::npos ? r : r.substr(0, e);
            after = e == std::string::npos ? "" : r.substr(e);
            cond = l == rv;
        }
        if (neg) cond = !cond;
        if (cond) return shell_exec_line(m, after);
        return false;
    }
    if (cmd == "CALL") {
        std::string found;
        std::vector<std::string> args = split_args(arg);
        if (args.empty()) return false;
        if (dos_resolve_exec(m, args[0], &found) == 0 && upper_dbcs(found).size() > 4 && upper_dbcs(found).substr(upper_dbcs(found).size() - 4) == ".BAT") {
            load_batch(m, found, args, true);
            return false;
        }
        return run_command(m, arg);
    }
    if (cmd == "EXIT") { s_batch.clear(); return false; }
    if (cmd == "COMMAND") {
        // COMMAND /C xxx は xxx を実行、それ以外（常駐シェルの起動）は何もしない
        std::string a = trim(arg);
        std::string ua = upper_dbcs(a);
        size_t c = ua.find("/C");
        if (c != std::string::npos) return run_command(m, trim(a.substr(c + 2)));
        return false;
    }
    if (cmd == "FOR") {
        // FOR %V IN (集合) DO コマンド: 展開した行をバッチの次の行として差し込む
        std::string a = trim(arg);
        std::string ua = upper_dbcs(a);
        size_t in = ua.find(" IN "), lp = a.find('('), rp = a.find(')'), dw = ua.find(" DO ", rp == std::string::npos ? 0 : rp);
        if (in == std::string::npos) in = ua.find(" IN(");   // 「FOR %%A IN( \ZAVAS2\*.*) DO …」のように IN と ( がくっついていても受け付ける
        if (a.size() < 2 || a[0] != '%' || in == std::string::npos || lp == std::string::npos || rp == std::string::npos || dw == std::string::npos) return false;
        std::string var = trim(a.substr(0, in));
        std::string set = a.substr(lp + 1, rp - lp - 1);
        std::string body = trim(a.substr(dw + 4));
        std::vector<std::string> items;
        for (const std::string& it : split_args(set)) {
            if (it.find('*') != std::string::npos || it.find('?') != std::string::npos) {
                std::vector<std::string> comps; split_guest(it, comps, nullptr);
                bool ex; std::string dir = host_of(m, comps, comps.empty() ? 0 : comps.size() - 1, &ex);
                std::string prefix; size_t bs = it.find_last_of("\\:");
                if (bs != std::string::npos) prefix = it.substr(0, bs + 1);
                std::vector<HostDirEntry> lst = dir_list(dir);
                std::sort(lst.begin(), lst.end(), [](const HostDirEntry& x, const HostDirEntry& y) { return x.name < y.name; });
                for (auto& e : lst) {
                    if (e.is_dir) continue;
                    std::string g = upper_dbcs(hostfs::to_sjis(e.name));
                    if (valid83(g) && !comps.empty() && wild_match(upper_dbcs(comps.back()), g)) items.push_back(prefix + g);
                }
            } else items.push_back(it);
        }
        std::vector<std::string> lines;
        for (auto& it : items) {
            std::string l = body, r;
            for (size_t i = 0; i < l.size();) {
                if (l.compare(i, var.size(), var) == 0) { r += it; i += var.size(); } else r.push_back(l[i++]);
            }
            // バッチの行としてもう一度 % 展開されるので、残った % を守る
            std::string esc; for (char ch : r) { esc.push_back(ch); if (ch == '%') esc.push_back('%'); }
            lines.push_back(esc);
        }
        if (lines.empty()) return false;
        if (s_batch.empty()) { BatchCtx b; b.lines = lines; s_batch.push_back(b); s_shell_running = true; }
        else { BatchCtx& b = s_batch.back(); b.lines.insert(b.lines.begin() + (long)b.pc, lines.begin(), lines.end()); }
        return false;
    }
    if (cmd == "MD" || cmd == "MKDIR" || cmd == "RD" || cmd == "RMDIR") {
        if (arg.empty()) return false;
        bool ex; std::string h = guest_to_host(m, arg, &ex, nullptr, nullptr);
        if (cmd[0] == 'M') { if (!ex) hostfs::mkdir(h); } else if (ex) hostfs::rmdir(h);
        dir_invalidate();
        return false;
    }
    if (cmd == "DEL" || cmd == "ERASE" || cmd == "TYPE" || cmd == "COPY" || cmd == "XCOPY" || cmd == "DIR" || cmd == "REN" || cmd == "RENAME" || cmd == "ATTRIB") {
        // 引数（/ で始まるスイッチは捨てる）
        std::vector<std::string> args;
        for (auto& x : split_args(arg)) if (x[0] != '/') {
            size_t sl = x.find('/'); args.push_back(sl == std::string::npos ? x : x.substr(0, sl));
        }
        auto expand = [&](const std::string& spec, std::vector<std::pair<std::string,std::string>>& out) {
            // spec に合うファイル（ゲスト名, ホストのパス）。ワイルドカード無しは存在するものだけ
            std::vector<std::string> comps; split_guest(spec, comps, nullptr);
            if (comps.empty()) return;
            bool ex; std::string dir = host_of(m, comps, comps.size() - 1, &ex);
            if (!ex) return;
            std::string pat = upper_dbcs(comps.back());
            std::vector<HostDirEntry> lst = dir_list(dir);
            std::sort(lst.begin(), lst.end(), [](const HostDirEntry& x, const HostDirEntry& y) { return x.name < y.name; });
            for (auto& e : lst) {
                if (e.is_dir) continue;
                std::string g = upper_dbcs(hostfs::to_sjis(e.name));
                if (valid83(g) && wild_match(pat, g)) out.push_back({g, hostfs::join(dir, e.name)});
            }
        };
        if (cmd == "DIR" || cmd == "ATTRIB") return false;
        if (cmd == "DEL" || cmd == "ERASE") {
            for (auto& a1 : args) { std::vector<std::pair<std::string,std::string>> f; expand(a1, f); for (auto& x : f) hostfs::remove(x.second); }
            dir_invalidate(); return false;
        }
        if (cmd == "REN" || cmd == "RENAME") {
            if (args.size() < 2) return false;
            bool e1, e2; std::string ha = guest_to_host(m, args[0], &e1, nullptr, nullptr);
            std::vector<std::string> comps; split_guest(args[0], comps, nullptr);
            bool ex; std::string dir = host_of(m, comps, comps.empty() ? 0 : comps.size() - 1, &ex);
            std::string hb = hostfs::join(dir, hostfs::from_sjis(upper_dbcs(args[1])));
            guest_to_host(m, args[1], &e2, nullptr, nullptr);
            if (e1) hostfs::rename(ha, hb);
            dir_invalidate(); return false;
        }
        if (cmd == "TYPE") {
            if (args.empty()) return false;
            bool ex; std::string h = guest_to_host(m, args[0], &ex, nullptr, nullptr);
            void* fh = ex ? hostfs::open(h, 0, false, false) : nullptr;
            if (!fh) return false;
            uint8_t buf[512]; int n;
            while ((n = hostfs::read(fh, buf, sizeof(buf))) > 0) { for (int i = 0; i < n; i++) { if (buf[i] == 0x1A) { n = -1; break; } console_putc(m, buf[i]); } if (n < 0) break; }
            hostfs::close(fh);
            return false;
        }
        // COPY / XCOPY: 元（+ で連結可）→ 先（ファイルかディレクトリ。省略時はカレント）
        if (args.empty()) return false;
        std::string src = args[0], dst = args.size() > 1 ? args[1] : ".";
        std::vector<std::string> parts;
        { std::string cur; for (char ch : src) { if (ch == '+') { if (!cur.empty()) parts.push_back(cur); cur.clear(); } else cur.push_back(ch); } if (!cur.empty()) parts.push_back(cur); }
        std::vector<std::pair<std::string,std::string>> files;
        for (auto& pp : parts) expand(pp, files);
        if (files.empty()) { shell_print(m, "File not found"); return false; }
        bool dex; std::string dh = guest_to_host(m, dst, &dex, nullptr, nullptr);
        HostDirEntry dst_st;
        bool dst_is_dir = dst == "." || (dex && hostfs::stat(dh, dst_st) && dst_st.is_dir) || (!dst.empty() && (dst.back() == '\\' || dst.back() == ':'));
        auto copy_one = [&](const std::string& from, const std::string& to, bool append) {
            void* a1 = hostfs::open(from, 0, false, false);
            if (!a1) return;
            void* b1 = hostfs::open(to, 1, true, !append);
            if (b1 && append) hostfs::seek(b1, 0, 2);
            if (b1) { std::vector<uint8_t> buf(65536); int n; while ((n = hostfs::read(a1, buf.data(), (int)buf.size())) > 0) hostfs::write(b1, buf.data(), n); hostfs::close(b1); }
            hostfs::close(a1);
        };
        bool concat = parts.size() > 1 && !dst_is_dir;
        for (size_t i = 0; i < files.size(); i++) {
            std::string to;
            if (dst_is_dir) {
                std::string d = dst == "." ? "" : dst;
                if (!d.empty() && d.back() != '\\' && d.back() != ':') d += "\\";
                bool e2; to = guest_to_host(m, d + files[i].first, &e2, nullptr, nullptr);
            } else if (dst.find('*') != std::string::npos || dst.find('?') != std::string::npos) {
                // *.BAK のような先: 名前の部分を置き換える
                std::string pat = upper_dbcs(dst), nm = files[i].first, out;
                size_t pd = pat.find('.'), nd = nm.find('.');
                std::string pb = pat.substr(0, pd), pe = pd == std::string::npos ? "" : pat.substr(pd + 1);
                std::string nb = nm.substr(0, nd), ne = nd == std::string::npos ? "" : nm.substr(nd + 1);
                out = (pb == "*" ? nb : pb);
                std::string ee = (pe == "*" ? ne : pe);
                if (!ee.empty()) out += "." + ee;
                bool e2; to = guest_to_host(m, out, &e2, nullptr, nullptr);
            } else to = dh;
            if (to == files[i].second) continue;
            copy_one(files[i].second, to, concat && i > 0);
        }
        dir_invalidate();
        return false;
    }

    // プログラム
    std::string prog = sp == std::string::npos ? line : line.substr(0, sp);
    std::string found;
    if (dos_resolve_exec(m, prog, &found) != 0) {
        shell_print(m, "Bad command or file name");
        plog("[shell] 見つかりません: %s\n", prog.c_str());
        return false;
    }
    std::string ufound = upper_dbcs(found);
    if (ufound.size() > 4 && ufound.substr(ufound.size() - 4) == ".BAT") {
        std::vector<std::string> args = split_args(arg);
        args.insert(args.begin(), prog);
        load_batch(m, found, args, false);
        return false;
    }
    std::string tail = rest;
    if (!tail.empty() && tail[0] != ' ' && tail[0] != '\t') tail = " " + tail;
    LoadResult lr;
    int err = load_program(m, found, tail, 0, s_shell_psp, &lr);
    if (err) {
        shell_print(m, err == 8 ? "Not enough memory" : "Bad command or file name");
        plog("[shell] 起動できません: %s (エラー %d)\n", found.c_str(), err);
        s_retcode = 0xFF;
        return false;
    }
    start_program(m, lr);
    return true;
}

static bool shell_exec_line(Machine* m, std::string line) {
    line = trim(line);
    while (!line.empty() && line[0] == '@') line = trim(line.substr(1));
    if (line.empty() || line[0] == ':') return false;
    return run_command(m, line);
}

static void shell_step(Machine* m) {
    Cpu* c = &m->cpu;
    if (s_shell_wait) {
        if (!bios_key_available(m)) return;   // スタブ側で HLT して待つ
        bios_key_read(m, true);
        s_shell_wait = 0;
    }
    for (int guard = 0; guard < 2000; guard++) {
        if (s_batch.empty()) {
            if (s_shell_running) {
                s_shell_running = false;
                m->quit = 1;
                plog("[shell] すべてのコマンドが終わりました\n");
            }
            return;
        }
        BatchCtx& b = s_batch.back();
        if (b.pc >= b.lines.size()) { s_batch.pop_back(); continue; }
        std::string raw = b.lines[b.pc++];
        std::string line = expand_vars(b, raw);
        if (shell_exec_line(m, line)) return;
        if (s_shell_wait) return;
    }
    // 延々と続くときは一度 CPU に戻す（次の命令で再びここへ来る）
    c->ip = s_shell_entry;
}

static void shell_resume(Machine* m) {
    Cpu* c = &m->cpu;
    s_retcode_last = s_retcode;
    s_psp = s_shell_psp;
    s_dta_seg = s_shell_psp; s_dta_off = 0x80;
    cpu_setsr(c, CS_, ROMSEG); c->ip = s_shell_entry;
    cpu_setsr(c, SS_, s_shell_psp); c->r[ESP] = 0x0FFE;
    cpu_setsr(c, DS_, s_shell_psp); cpu_setsr(c, ES_, s_shell_psp);
    c->fl = 0x0202;
    c->halted = 0;
}

void shell_start(Machine* m, const std::string& cmdline) {
    // Start=B:\INSTALL.BAT のようにドライブ付きなら、そのドライブをカレントにして始める
    // CurrentDrive= があればそれを優先（例: フロッピーの B:\INST.EXE を、ハードディスク A: をカレントにして動かす）
    if (cmdline.size() >= 2 && cmdline[1] == ':') {
        char d = cmdline[0]; if (d >= 'a' && d <= 'z') d = (char)(d - 32);
        if (drive_valid(m, d)) s_curdrv = d == m->cfg.drive ? 0 : d;
    }
    if (m->cfg.current_drive && drive_valid(m, m->cfg.current_drive))
        s_curdrv = m->cfg.current_drive == m->cfg.drive ? 0 : m->cfg.current_drive;
    // Start=GAME\GAME.EXE のようにゲームのフォルダの下のファイルなら、そのフォルダをカレントにして始める
    // （フォルダの中で起動される前提のゲームが多い）。名前が英数字だけのときに限る
    std::string cl = cmdline;
    {
        size_t sp = cl.find(' ');
        std::string prog = cl.substr(0, sp);
        size_t bs = prog.find_last_of("\\/");
        bool ascii = true;
        for (char ch : prog) if ((unsigned char)ch >= 0x80) ascii = false;
        if (ascii && bs != std::string::npos && bs > 0 && !(prog.size() >= 2 && prog[1] == ':') && prog[0] != '\\' && prog[0] != '/') {
            std::string d = prog.substr(0, bs);
            for (auto& ch : d) { if (ch == '/') ch = '\\'; if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32); }
            s_cwd = d;
            cl = "\\" + cl;
        }
    }
    // CurrentDirectory=A:\NANPA\ : 起動時のカレントディレクトリ。「\NANPA から実行して下さい」という
    // インストーラ向け。ドライブが付いていればカレントドライブにもする。ゲームのドライブなら無いフォルダは作る
    if (!m->cfg.current_dir.empty()) {
        std::string d = m->cfg.current_dir;
        for (auto& ch : d) if (ch == '/') ch = '\\';
        char dv = 0;
        if (d.size() >= 2 && d[1] == ':') {
            dv = d[0]; if (dv >= 'a' && dv <= 'z') dv = (char)(dv - 32);
            d = d.substr(2);
            if (drive_valid(m, dv)) s_curdrv = dv == m->cfg.drive ? 0 : dv;
        }
        if (d.empty() || d[0] != '\\') d = "\\" + d;
        std::vector<std::string> comps; split_guest(d, comps, nullptr);
        if (s_sg_drive == m->cfg.drive) {
            for (size_t i = 1; i <= comps.size(); i++) {
                bool ex; std::string h = host_of(m, comps, i, &ex);
                if (!ex) { hostfs::mkdir(h); dir_invalidate(); }
            }
        }
        bool ex; std::string h = host_of(m, comps, comps.size(), &ex);
        HostDirEntry st;
        if (ex && hostfs::stat(h, st) && st.is_dir) {
            std::string s;
            for (size_t i = 0; i < comps.size(); i++) { if (i) s += "\\"; s += comps[i]; }
            cwd_of(m, s_sg_drive) = s;
        }
    }
    BatchCtx b;
    b.lines.push_back(cl);
    s_batch.clear();
    s_batch.push_back(b);
    s_shell_running = true;
    shell_resume(m);
}

// ゲストから見える SFT（LoL+4 から辿れる表）を中身に合わせる。
// PMD などは自分の名前をこの表から探すので、鎖が正しく終わっていることが大事。
static void sft_sync_guest(Machine* m) {
    uint32_t base = lin(DOSSEG, D_SFT) + 6;
    for (int i = 0; i < SFT_GUEST_N; i++) {
        uint32_t e = base + (uint32_t)i * 0x3B;
        const Sft* f = i < (int)s_sft.size() ? &s_sft[i] : nullptr;
        uint16_t refs = (f && f->kind) ? (uint16_t)f->refs : 0;
        for (int k = 0; k < 0x3B; k++) m->ram[e + k] = 0;
        ww(m, e, refs);
        if (!refs) continue;
        ww(m, e + 2, (uint16_t)(f->mode & 0xFF));
        ww(m, e + 5, f->kind == 1 ? (uint16_t)(m->cfg.drive - 'A') : f->kind == 2 ? 0x80D3 : 0x8084);
        std::string n = f->kind == 1 ? f->name : f->name.empty() ? std::string("CON") : f->name;
        size_t bs = n.find_last_of("\\:"); if (bs != std::string::npos) n = n.substr(bs + 1);
        std::string b = n, x; size_t d = n.find('.');
        if (d != std::string::npos) { b = n.substr(0, d); x = n.substr(d + 1); }
        for (int k = 0; k < 8; k++) m->ram[e + 0x20 + k] = k < (int)b.size() ? (uint8_t)toupper((unsigned char)b[k]) : ' ';
        for (int k = 0; k < 3; k++) m->ram[e + 0x28 + k] = k < (int)x.size() ? (uint8_t)toupper((unsigned char)x[k]) : ' ';
        ww(m, e + 0x31, f->owner);
    }
}

// ---- 振り分け ----------------------------------------------------------------
void dos_hle(Machine* m, uint8_t n) {
    m->ram[lin(DOSSEG, D_INDOS)] = 1;
    switch (n) {
    case HLE_INT21: int21(m); sft_sync_guest(m); break;
    case HLE_INT20: terminate(m, 0, 0, 0); break;
    case HLE_INT27: terminate(m, 0, 3, (uint16_t)((DX(m) + 15) >> 4)); break;
    case HLE_INT28: break;
    case HLE_INT29: console_putc(m, AL(m)); break;
    case HLE_INT25: case HLE_INT26: {
        // 絶対ディスク読み書き。フロッピーのドライブだけイメージへ（AL=ドライブ 0=A:）
        char d = (char)('A' + AL(m));
        int fu = floppy::unit_of_letter(d);
        FloppyImage* im = fu >= 0 ? floppy::image_unit(fu) : nullptr;
        if (!im) { SETAX(m, 0x8002); set_cf(m, true); break; }
        uint32_t start = DX(m), count = CX(m), buf = lin(DS(m), BX(m));
        if (count == 0xFFFF) {   // 32bit 版: DS:BX にパケット
            uint32_t pk = buf;
            start = rw(m, pk) | ((uint32_t)rw(m, pk + 2) << 16);
            count = rw(m, pk + 4);
            buf = lin(rw(m, pk + 8), rw(m, pk + 6));
        }
        floppy::poll_change();
        std::vector<uint8_t> sec((size_t)im->lsec_size);
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; i++) {
            uint32_t a = buf + i * (uint32_t)im->lsec_size;
            if (n == HLE_INT25) {
                ok = im->read_lba(start + i, sec.data());
                for (int k = 0; k < im->lsec_size; k++) mem_wb(m, a + k, sec[k]);
            } else {
                for (int k = 0; k < im->lsec_size; k++) sec[k] = mem_rb(m, a + k);
                ok = !im->wprot && im->write_lba(start + i, sec.data());
            }
        }
        if (n == HLE_INT26) { fatfs::reset(); dir_invalidate(); }
        if (ok) { SETAX(m, 0); set_cf(m, false); } else { SETAX(m, im->wprot ? 0x0300 : 0x0408); set_cf(m, true); }
        if (m->cfg.trace) {
            plog("[fd] INT %02Xh 論理セクタ %u から %u 個 → %s\n", n == HLE_INT25 ? 0x25 : 0x26, start, count, ok ? "成功" : "失敗");
            if (n == HLE_INT25) {   // 読んだ内容を何と比べるかを記録する（cpu.cpp）
                cpu_dw_begin(buf, count * (uint32_t)im->lsec_size, "");
                cpu_dw_taint8(4, (uint8_t)(AX(m) >> 8), "INT25 の結果(AH)");
                cpu_dw_taint8(0, (uint8_t)AX(m), "INT25 の結果(AL)");
                plog("[fdchk] ↓ここから、読んだ %u バイト（[%05X]〜）と結果 AX を比べる命令を記録\n", count * (uint32_t)im->lsec_size, buf & 0xFFFFF);
            }
        }
        break; }
    case HLE_INT2F: {
        uint16_t ax = AX(m);
        if (xms_int2f(m)) {}
        else if (ax == 0x4300) SETAL(m, 0x00);     // XMS なし
        else if (ax == 0x1600) SETAL(m, 0x00);     // Windows なし
        else if (ax == 0x1680) SETAL(m, 0x00);
        else if ((ax & 0xFF00) == 0x1A00) SETAL(m, 0xFF);   // ANSI あり
        else if (ax == 0x1687) {}                   // DPMI なし（AX そのまま）
        else if ((ax & 0x00FF) == 0x00) SETAL(m, 0x00);
        break; }
    case HLE_SHELL: shell_step(m); break;
    case HLE_XMS: case HLE_INT67: xmsems_hle(m, n); break;
    default: break;
    }
    m->ram[lin(DOSSEG, D_INDOS)] = 0;
}

// ---- 初期化 ------------------------------------------------------------------
void dos_init(Machine* m) {
    uint8_t* r = m->ram;
    for (auto& f : s_sft) if (f.kind == 1 && f.h) hostfs::close(f.h);   // 再起動のとき: 開いたままのファイルを閉じる
    s_sft.clear(); s_sft.resize(5);
    s_sft[0].kind = 2; s_sft[0].refs = 1; s_sft[0].name = "CON";
    s_sft[1].kind = 4; s_sft[1].refs = 1; s_sft[1].name = "AUX";
    s_sft[2].kind = 4; s_sft[2].refs = 1; s_sft[2].name = "PRN";
    s_sft[3].kind = 3; s_sft[3].refs = 1; s_sft[3].name = "NUL";
    s_sft[4].kind = 2; s_sft[4].refs = 1;
    s_frames.clear(); s_search.clear(); s_batch.clear(); s_env.clear();
    s_cwd.clear(); s_dircache.clear(); s_fdcwd.clear(); s_fdcwd2.clear(); s_curdrv = 0; s_sg_drive = 0;
    s_subst.clear(); s_subst_cwd.clear();
    keytab_default(); s_pending_input.clear();
    s_retcode = 0; s_retcode_last = 0; s_shell_wait = 0; s_shell_running = false;
    s_alloc_strategy = 0;

    // DOS データ
    uint32_t d = lin(DOSSEG, 0);
    for (int i = 0; i < 0x1000 - 0x800; i++) r[d + i] = 0;
    static const uint8_t dbcs[] = {0x81, 0x9F, 0xE0, 0xFC, 0, 0};
    ww(m, lin(DOSSEG, D_DBCS - 2), 6);
    for (int i = 0; i < 6; i++) r[lin(DOSSEG, D_DBCS) + i] = dbcs[i];
    const uint16_t FIRST_MCB = first_mcb_cfg(m);
    ww(m, lin(DOSSEG, D_LOL - 2), FIRST_MCB);
    {
        // List of Lists の主な項目
        uint32_t L = lin(DOSSEG, D_LOL);
        ww(m, L + 0x00, D_DPB);  ww(m, L + 0x02, DOSSEG);            // DPB（ゲームのドライブ 1 つ）
        ww(m, L + 0x04, D_SFT);  ww(m, L + 0x06, DOSSEG);            // SFT
        ww(m, L + 0x08, 0xFFFF); ww(m, L + 0x0A, 0xFFFF);            // CLOCK$
        ww(m, L + 0x0C, 0xFFFF); ww(m, L + 0x0E, 0xFFFF);            // CON
        ww(m, L + 0x10, 512);                                         // 最大セクタ長
        ww(m, L + 0x16, 0xFFFF); ww(m, L + 0x18, 0xFFFF);            // CDS
        ww(m, L + 0x1A, 0xFFFF); ww(m, L + 0x1C, 0xFFFF);            // FCB SFT
        r[L + 0x20] = 1; r[L + 0x21] = 26;                           // ブロック装置数 / LASTDRIVE
        // NUL 装置ヘッダ（装置の鎖の先頭）。EMS があれば EMMXXXX0 へつなぐ
        uint32_t N = L + 0x22;
        ww(m, N + 0, 0xFFFF); ww(m, N + 2, 0xFFFF);
        ww(m, N + 4, 0x8004); ww(m, N + 6, 0); ww(m, N + 8, 0);
        memcpy(r + N + 0x0A, "NUL     ", 8);
        r[L + 0x34] = 0;                                              // JOIN 数
        // SFT の先頭ブロック: 次へのポインタ（無し）と項目数
        uint32_t S = lin(DOSSEG, D_SFT);
        ww(m, S + 0, 0xFFFF); ww(m, S + 2, 0xFFFF); ww(m, S + 4, SFT_GUEST_N);
    }
    {
        // ゲームのドライブの DPB。ファイルの読み書きは HLE なので中身は形だけだが、
        // LoL から DPB の鎖を辿るソフト（ディスクキャッシュ MACACHE など）が迷わないように、
        // INT 21h 36h と同じ値（1024 バイト/セクタ、16 セクタ/クラスタ）で終端つきの鎖を作る。
        uint32_t D = lin(DOSSEG, D_DPB);
        r[D + 0x00] = (uint8_t)(m->cfg.drive - 'A'); r[D + 0x01] = 0;
        ww(m, D + 0x02, 1024); r[D + 0x04] = 15; r[D + 0x05] = 4;
        ww(m, D + 0x06, 1); r[D + 0x08] = 2; ww(m, D + 0x09, 512);
        ww(m, D + 0x0B, 0x40); ww(m, D + 0x0D, (uint16_t)(hdd_total_clusters(m) + 1)); ww(m, D + 0x0F, 0x20); ww(m, D + 0x11, 0x3F);
        ww(m, D + 0x13, D_BLKDEV); ww(m, D + 0x15, DOSSEG);
        r[D + 0x17] = 0xF8; r[D + 0x18] = 0;
        ww(m, D + 0x19, 0xFFFF); ww(m, D + 0x1B, 0xFFFF);            // 次の DPB（無し）
        ww(m, D + 0x1D, 0xFFFF); ww(m, D + 0x1F, hdd_free_clusters(m));
        uint32_t B = lin(DOSSEG, D_BLKDEV);
        ww(m, B + 0, 0xFFFF); ww(m, B + 2, 0xFFFF); ww(m, B + 4, 0x0800);
        ww(m, B + 6, D_BLKDEV + 0x12); ww(m, B + 8, D_BLKDEV + 0x12); r[B + 0x0A] = 1;
        r[B + 0x12] = 0xCB;                                           // 戦略/割込みルーチン = RETF
    }
    r[lin(DOSSEG, D_UPCASE)] = 0xCB;   // RETF
    r[lin(DOSSEG, D_SWITCHAR)] = '/';
    // PC-98 の MS-DOS のワークエリア 0060:006Ch: ドライブ A:〜P: の DA/UA。
    // ゲームのドライブはハードディスク（80h）、フロッピーのドライブは 1MB FDD のユニット 0（90h）。
    // キーディスクを探すゲーム（ファルコムの英雄伝説など）はこの表でフロッピーのドライブを見つける。
    for (int i = 0; i < 16; i++) r[0x66C + i] = 0;
    if (m->cfg.drive >= 'A' && m->cfg.drive <= 'P') r[0x66C + (m->cfg.drive - 'A')] = 0x80;
    if (floppy::drive_letter() >= 'A' && floppy::drive_letter() <= 'P') r[0x66C + (floppy::drive_letter() - 'A')] = 0x90;
    {   // 2 台目のフロッピー（FloppyDrive2=）はユニット 1（91h）
        char l2 = floppy::drive_letter_unit(1);
        if (l2 >= 'A' && l2 <= 'P') r[0x66C + (l2 - 'A')] = 0x91;
    }

    // 割込みベクタ
    uint8_t code21[3] = {0xF1, HLE_INT21, 0xCF};
    (void)code21;
    uint16_t o21 = bios_hle_stub(m, HLE_INT21);
    s_int21_off = o21;
    s_stub_iret = (uint16_t)(o21 + 2);
    ww(m, 0x21 * 4, o21); ww(m, 0x21 * 4 + 2, ROMSEG);
    bios_hook_vec(m, 0x20, HLE_INT20);
    bios_hook_vec(m, 0x25, HLE_INT25);
    bios_hook_vec(m, 0x26, HLE_INT26);
    // INT 25h/26h は元のフラグをスタックに残して戻る（呼んだ側が POPF する）ので IRET ではなく RETF
    for (int v = 0x25; v <= 0x26; v++) r[lin(ROMSEG, (uint16_t)(rw(m, v * 4u) + 2))] = 0xCB;
    bios_hook_vec(m, 0x27, HLE_INT27);
    bios_hook_vec(m, 0x28, HLE_INT28);
    bios_hook_vec(m, 0x29, HLE_INT29);
    bios_hook_vec(m, 0x2F, HLE_INT2F);
    // シェルの入口: F1 FD / STI / HLT / JMP -6
    {
        uint16_t o = bios_hle_stub(m, HLE_SHELL);   // F1 FD CF
        // CF を上書きしてループにする
        r[lin(ROMSEG, (uint16_t)(o + 2))] = 0xFB;
        uint8_t tail[3] = {0xF4, 0xEB, 0xFA};
        uint16_t o2 = bios_hle_stub(m, HLE_NOP);    // 3 バイト確保
        for (int i = 0; i < 3; i++) r[lin(ROMSEG, (uint16_t)(o2 + i))] = tail[i];
        s_shell_entry = o;
    }

    // メモリの鎖: 先頭 MCB から 640KB まで
    uint16_t top = (uint16_t)(m->cfg.memory_kb * 64);
    if (top > MEM_TOP || top < 0x2000) top = MEM_TOP;
    mcb_set(m, FIRST_MCB, 'Z', 0, (uint16_t)(top - FIRST_MCB - 1));

    // 環境
    s_env["COMSPEC"] = std::string(1, m->cfg.drive) + ":\\COMMAND.COM";
    s_env["PATH"] = std::string(1, m->cfg.drive) + ":\\";
    s_env["PROMPT"] = "$P$G";

    // シェル（COMMAND.COM の代わり）のプロセス
    uint16_t envseg, psp, lg;
    std::vector<uint8_t> env = build_env(m, 0, std::string(1, m->cfg.drive) + ":\\COMMAND.COM");
    mem_alloc(m, 0x20, 0xFFFF, &envseg, &lg);
    for (size_t i = 0; i < env.size() && i < 0x200; i++) r[lin(envseg, 0) + i] = env[i];
    mem_alloc(m, 0x100, 0xFFFF, &psp, &lg);
    ww(m, lin((uint16_t)(envseg - 1), 1), psp);
    ww(m, lin((uint16_t)(psp - 1), 1), psp);
    mcb_name(m, (uint16_t)(psp - 1), "COMMAND");
    s_shell_psp = psp;
    s_psp = psp;
    build_psp(m, psp, (uint16_t)(psp + 0x100), envseg, psp, "");
    uint32_t jft = lin(psp, 0x18);
    r[jft + 0] = 0; r[jft + 1] = 0; r[jft + 2] = 0; r[jft + 3] = 1; r[jft + 4] = 2;
    s_sft[0].refs = 3;
    s_dta_seg = psp; s_dta_off = 0x80;

    xmsems_init(m);
    if (m->cfg.ems) {   // NUL -> EMMXXXX0 -> 終わり
        uint32_t N = lin(DOSSEG, D_LOL) + 0x22;
        ww(m, N + 0, 0x0000); ww(m, N + 2, 0xF7F0);
    }
    sft_sync_guest(m);

    // 使われていない割込みベクタは、実機の MS-DOS と同じく 0060:xxxx の IRET を指すようにする。
    // 常駐ドライバには「ベクタのセグメントが 0060h なら未登録」で常駐済みかを見分けるものがある
    //（Dante98 の MUSIC.COM: INT 48h が 0060h でなければ、INT 0Ah と同じセグメントかで判断する。
    //  どちらも BIOS の ROM を指していると「常駐済み」と誤解して、解放して終わってしまう）
    {
        const uint16_t DOS_IRET = 0x02F0;   // 0060:02F0 = DOSSEG:00F0（国別情報と名前バッファの間の空き）
        r[lin(0x0060, DOS_IRET)] = 0xCF;
        uint16_t def_off = rw(m, 0x48 * 4), def_seg = rw(m, 0x48 * 4 + 2);   // INT 48h はどこも使わない＝既定の IRET
        for (int v = 0x20; v < 0x100; v++) {
            if (v >= 0xA0 && v <= 0xAF) continue;   // LIO（ROM の中にあるもの）はそのまま
            if (rw(m, v * 4) == def_off && rw(m, v * 4 + 2) == def_seg) { ww(m, v * 4, DOS_IRET); ww(m, v * 4 + 2, 0x0060); }
        }
    }

    // PIC の初期マスク（キーボードとスレーブ連結、それと FDC の 640KB/1MB（スレーブの IR2・IR3 = INT 41h・42h）を開ける。
    // 実機の BIOS はフロッピーを割込みで動かすので FDC は開いたまま。ディスクの入れ替えの割込みを見るソフトがある）
    m->pic[0].imr = 0x7D;
    m->pic[1].imr = 0xF3;
    pic_update_hint(m);

    shell_resume(m);
}

// ---- ステートセーブ ----------------------------------------------------------
//  開いているファイルはホストのパスと位置を記録し、ロード時に開き直す。
#include "state.h"
void dos_state_save(Machine* m, StateW& w) {
    (void)m;
    w.tag("DOS ");
    // フロッピー（開いているファイルを開き直す前に入れ直せるよう先頭に置く）
    w.tag("DRV1");
    w.pod(s_curdrv); w.str(s_fdcwd);
    { char fl = floppy::drive_letter(); w.pod(fl); }
    w.str(floppy::current_path());
    w.tag("DRV2");   // 2 台目のフロッピー（開いているファイルを開き直す前に入れ直す）
    { char l2 = floppy::drive_letter_unit(1); w.pod(l2); }
    w.str(floppy::current_path_unit(1)); w.str(s_fdcwd2);
    w.tag("SUBS");   // SUBST のドライブ
    w.u32((uint32_t)s_subst.size());
    for (auto& kv : s_subst) { w.pod(kv.first); w.str(kv.second); w.str(s_subst_cwd[kv.first]); }
    if (!s_keytab_init) keytab_default();
    w.tag("KEY1");
    for (int i = 0; i < 20; i++) for (int k = 0; k < 16; k++) w.pod(s_fkey[i][k]);
    for (int i = 0; i < 11; i++) for (int k = 0; k < 6; k++) w.pod(s_ekey[i][k]);
    w.u32((uint32_t)s_sft.size());
    for (auto& f : s_sft) {
        w.pod(f.refs); w.pod(f.kind); w.str(f.host); w.str(f.name); w.pod(f.mode); w.pod(f.owner);
        int64_t pos = (f.kind == 1 && f.h) ? hostfs::seek(f.h, 0, 1) : 0;
        w.pod(pos);
    }
    w.u32((uint32_t)s_frames.size());
    for (auto& fr : s_frames) w.pod(fr);
    w.pod(s_psp); w.pod(s_shell_psp); w.pod(s_dta_seg); w.pod(s_dta_off);
    w.pod(s_retcode); w.pod(s_last_err); w.str(s_cwd);
    w.pod(s_stub_iret); w.pod(s_int21_off); w.pod(s_shell_entry);
    w.pod(s_break_flag); w.pod(s_verify); w.str(s_pending_input); w.pod(s_alloc_strategy);
    w.u32((uint32_t)s_batch.size());
    for (auto& b : s_batch) {
        w.u32((uint32_t)b.lines.size()); for (auto& l : b.lines) w.str(l);
        uint64_t pc = b.pc; w.pod(pc);
        w.u32((uint32_t)b.args.size()); for (auto& a : b.args) w.str(a);
    }
    w.u32((uint32_t)s_env.size());
    for (auto& kv : s_env) { w.str(kv.first); w.str(kv.second); }
    w.pod(s_shell_running); w.pod(s_retcode_last); w.pod(s_shell_wait);
}

void dos_state_load(Machine* m, StateR& r) {
    (void)m;
    r.tag("DOS ");
    if (r.peek_tag("DRV1")) {
        r.tag("DRV1");
        r.pod(s_curdrv); s_fdcwd = r.str();
        char fl = 0; r.pod(fl);
        std::string fp = r.str();
        floppy::set_drive_letter(fl);
        std::string cur = floppy::current_path();
        if (fp.empty()) { if (!cur.empty()) floppy::eject(); }
        else if (cur != fp) {
            std::string e;
            if (!floppy::insert(fp, &e)) plog("[state] フロッピーを入れ直せません: %s (%s)\n", fp.c_str(), e.c_str());
        }
    } else { s_curdrv = 0; s_fdcwd.clear(); }
    s_fdcwd2.clear();
    if (r.peek_tag("DRV2")) {
        r.tag("DRV2");
        char l2 = 0; r.pod(l2);
        std::string fp = r.str(); s_fdcwd2 = r.str();
        floppy::set_drive_letter_unit(1, l2);
        std::string cur = floppy::current_path_unit(1);
        if (fp.empty()) { if (!cur.empty()) floppy::eject_unit(1); }
        else if (cur != fp) {
            std::string e;
            if (!floppy::insert_unit(1, fp, &e)) plog("[state] 2 台目のフロッピーを入れ直せません: %s (%s)\n", fp.c_str(), e.c_str());
        }
    }
    s_subst.clear(); s_subst_cwd.clear();
    if (r.peek_tag("SUBS")) {
        r.tag("SUBS");
        uint32_t n = r.u32();
        for (uint32_t i = 0; i < n && i < 26 && r.ok; i++) {
            char d = 0; r.pod(d);
            std::string h = r.str(), c = r.str();
            s_subst[d] = h; s_subst_cwd[d] = c;
        }
    }
    keytab_default();
    if (r.peek_tag("KEY1")) {
        r.tag("KEY1");
        for (int i = 0; i < 20; i++) for (int k = 0; k < 16; k++) r.pod(s_fkey[i][k]);
        for (int i = 0; i < 11; i++) for (int k = 0; k < 6; k++) r.pod(s_ekey[i][k]);
    }
    s_pending_input.clear();
    // 今開いているホストのファイルを閉じる
    for (auto& f : s_sft) if (f.kind == 1 && f.h) hostfs::close(f.h);
    s_sft.clear();
    uint32_t n = r.u32(); if (!r.ok || n > 1000) { r.ok = false; return; }
    s_sft.resize(n);
    for (auto& f : s_sft) {
        r.pod(f.refs); r.pod(f.kind); f.host = r.str(); f.name = r.str(); r.pod(f.mode); r.pod(f.owner);
        int64_t pos = 0; r.pod(pos);
        f.h = nullptr;
        if (r.ok && f.kind == 1) {
            f.h = hostfs::open(f.host, (f.mode & 3) ? 2 : 0, false, false);
            if (f.h) hostfs::seek(f.h, pos, 0);
            else { plog("[state] ファイルを開き直せません: %s\n", f.host.c_str()); f.kind = 3; }
        }
    }
    n = r.u32(); if (!r.ok || n > 64) { r.ok = false; return; }
    s_frames.resize(n);
    for (auto& fr : s_frames) r.pod(fr);
    r.pod(s_psp); r.pod(s_shell_psp); r.pod(s_dta_seg); r.pod(s_dta_off);
    r.pod(s_retcode); r.pod(s_last_err); s_cwd = r.str();
    r.pod(s_stub_iret); r.pod(s_int21_off); r.pod(s_shell_entry);
    r.pod(s_break_flag); r.pod(s_verify); s_pending_input = r.str(); r.pod(s_alloc_strategy);
    n = r.u32(); if (!r.ok || n > 64) { r.ok = false; return; }
    s_batch.assign(n, BatchCtx());
    for (auto& b : s_batch) {
        uint32_t k = r.u32(); if (!r.ok || k > 100000) { r.ok = false; return; }
        b.lines.resize(k); for (auto& l : b.lines) l = r.str();
        uint64_t pc = 0; r.pod(pc); b.pc = (size_t)pc;
        k = r.u32(); if (!r.ok || k > 1000) { r.ok = false; return; }
        b.args.resize(k); for (auto& a : b.args) a = r.str();
    }
    n = r.u32(); if (!r.ok || n > 10000) { r.ok = false; return; }
    s_env.clear();
    for (uint32_t i = 0; i < n; i++) { std::string k = r.str(); s_env[k] = r.str(); }
    r.pod(s_shell_running); r.pod(s_retcode_last); r.pod(s_shell_wait);
    s_search.clear();
    dir_invalidate();
}
