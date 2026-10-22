// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  debugdump.cpp  --  Trace=2 の「終わる直前のようす」の記録（設計は debugdump.h）
// -----------------------------------------------------------------------------
#include "debugdump.h"
#include "machine.h"
#include "cpu.h"
#include "hostfs.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <vector>
#include <string>
#include <unordered_map>

bool g_dbg_on = false;

namespace {
struct Insn {   // 命令を実行する前のレジスタ
    uint32_t frame;
    uint16_t cs, ip, ax, bx, cx, dx, si, di, bp, sp, ds, es, ss, fl;
};
struct IntEv {
    uint32_t frame; uint8_t vec, kind;   // kind: 0 = ハードウェア, 1 = INT 命令（例外を含む）, 2 = INT 21h の戻り
    uint16_t cs, ip, ax, bx, cx, dx, si, di, ds, es, ss, sp, fl;
    uint16_t stk[4];
    uint32_t count;   // まとめた回数（長い記録のほう）
    uint32_t last_frame;
};
struct IoEv { uint32_t frame; uint16_t port, val, cs, ip; uint8_t out, w16; };
struct Snap {
    uint32_t frame;
    uint16_t cs, ip, ax, bx, cx, dx, si, di, bp, sp, ds, es, ss, fl;
    uint16_t stk[32];
    uint8_t imr0, irr0, isr0, imr1, irr1, isr1;
    uint32_t ints[256];   // この 1 秒に入った割込みの回数
};
struct Mark { uint32_t frame; std::string text; };

const unsigned NI = 16384, NE = 8192, NL = 4096, NO = 4096, NS = 10, NM = 64;
std::vector<Insn> s_insn; unsigned s_insn_pos = 0;
std::vector<IntEv> s_ev; unsigned s_ev_pos = 0;
// 長い記録: 同じ種類（番号・AH・呼んだ場所）の割込みが 0.5 秒以内に続いたら、最初の 1 件に回数を足す
std::vector<IntEv> s_lev; unsigned s_lev_pos = 0;
std::unordered_map<uint64_t, unsigned> s_lev_last;   // 種類 → s_lev の通し番号
std::vector<IoEv> s_io; unsigned s_io_pos = 0;
// 画面の設定の I/O（GDC・モードF/F・表示バンク・画面モード）だけの長い記録
const unsigned NG = 4096;
std::vector<IoEv> s_gio; unsigned s_gio_pos = 0;
inline bool is_video_port(uint16_t p) {
    return p == 0x60 || p == 0x62 || p == 0xA0 || p == 0xA2 || p == 0x68 || p == 0x6A || p == 0xA4 || p == 0x9A0 || p == 0x9A2 || p == 0x9A8 || p == 0x70 || p == 0x72 || p == 0x74 || p == 0x76 || p == 0x78 || p == 0x7A;
}
std::vector<Snap> s_snap; unsigned s_snap_pos = 0;
std::vector<Mark> s_mark; unsigned s_mark_pos = 0;
uint32_t s_cur_ints[256];
uint32_t s_ivt0[256];
uint64_t s_ints_total[256];
bool s_ivt_ok = false;

inline uint16_t R(const Cpu& c, int i) { return (uint16_t)c.r[i]; }
inline uint16_t rw(Machine* m, uint32_t a) { a &= 0xFFFFF; return (uint16_t)(m->ram[a] | (m->ram[(a + 1) & 0xFFFFF] << 8)); }
uint32_t frame_of(Machine* m) { return (uint32_t)m->frame_count; }
}

// 命令ごと（cpu.cpp から。Trace=2 のときだけ）
void dbg_note_insn(Machine* m) {
    const Cpu& c = m->cpu;
    if (s_insn.empty()) s_insn.resize(NI);
    Insn& e = s_insn[s_insn_pos++ % NI];
    e.frame = frame_of(m); e.cs = c.sr[CS_]; e.ip = (uint16_t)c.ip;
    e.ax = R(c, EAX); e.bx = R(c, EBX); e.cx = R(c, ECX); e.dx = R(c, EDX);
    e.si = R(c, ESI); e.di = R(c, EDI); e.bp = R(c, EBP); e.sp = R(c, ESP);
    e.ds = c.sr[DS_]; e.es = c.sr[ES_]; e.ss = c.sr[SS_]; e.fl = (uint16_t)c.fl;
}

static void fill_int(Machine* m, IntEv& e, uint8_t vec, int kind) {
    const Cpu& c = m->cpu;
    e.frame = frame_of(m); e.vec = vec; e.kind = (uint8_t)kind;
    if (kind == 0) { e.cs = c.sr[CS_]; e.ip = (uint16_t)c.ip; } else { e.cs = c.op_cs; e.ip = (uint16_t)c.op_ip; }
    e.ax = R(c, EAX); e.bx = R(c, EBX); e.cx = R(c, ECX); e.dx = R(c, EDX); e.si = R(c, ESI); e.di = R(c, EDI);
    e.ds = c.sr[DS_]; e.es = c.sr[ES_]; e.ss = c.sr[SS_]; e.sp = R(c, ESP); e.fl = (uint16_t)c.fl;
    for (int i = 0; i < 4; i++) e.stk[i] = rw(m, ((uint32_t)e.ss << 4) + (uint16_t)(e.sp + i * 2));
}
static void long_note(const IntEv& e) {
    if (s_lev.empty()) s_lev.resize(NL);
    // ハードウェア割込みは番号だけで（割り込まれた場所はいろいろなので）、INT は番号・AH・呼んだ場所でまとめる
    uint64_t key = e.kind == 0 ? ((uint64_t)e.vec << 56)
                 : ((uint64_t)e.vec << 56) | ((uint64_t)e.kind << 48) | ((uint64_t)(e.ax >> 8) << 40) | ((uint64_t)e.cs << 16) | e.ip;
    auto it = s_lev_last.find(key);
    if (it != s_lev_last.end() && s_lev_pos - it->second < NL) {
        IntEv& l = s_lev[it->second % NL];
        if (e.frame - l.last_frame < 28) { l.count++; l.last_frame = e.frame; return; }
    }
    unsigned idx = s_lev_pos++;
    IntEv& n = s_lev[idx % NL]; n = e; n.count = 1; n.last_frame = e.frame;
    s_lev_last[key] = idx;
    if (s_lev_last.size() > 65536) s_lev_last.clear();
}
void dbg_note_int(Machine* m, uint8_t vec, bool hw) {
    if (!g_dbg_on) return;
    if (s_ev.empty()) s_ev.resize(NE);
    IntEv& e = s_ev[s_ev_pos++ % NE];
    fill_int(m, e, vec, hw ? 0 : 1);
    long_note(e);
    s_cur_ints[vec]++; s_ints_total[vec]++;
}
void dbg_note_int21_ret(Machine* m) {
    if (!g_dbg_on) return;
    if (s_ev.empty()) s_ev.resize(NE);
    IntEv& e = s_ev[s_ev_pos++ % NE];
    fill_int(m, e, 0x21, 2);
    e.ip = e.stk[0]; e.cs = e.stk[1];   // IRET で戻る先（呼んだ命令の次）
    // 戻りの FLAGS はスタック（IRET が拾う）にある
    e.fl = rw(m, ((uint32_t)m->cpu.sr[SS_] << 4) + (uint16_t)(R(m->cpu, ESP) + 4));
    long_note(e);
}
void dbg_note_io(Machine* m, uint16_t port, uint16_t v, bool out, bool w16) {
    if (!g_dbg_on) return;
    if (s_io.empty()) s_io.resize(NO);
    IoEv& e = s_io[s_io_pos++ % NO];
    e.frame = frame_of(m); e.port = port; e.val = v; e.out = out; e.w16 = w16; e.cs = m->cpu.op_cs; e.ip = (uint16_t)m->cpu.op_ip;
    if (out && is_video_port(port)) {
        if (s_gio.empty()) s_gio.resize(NG);
        s_gio[s_gio_pos++ % NG] = e;
    }
}
void dbg_note_text(const char* s) {
    if (!g_dbg_on) return;
    if (s_mark.empty()) s_mark.resize(NM);
    Mark& k = s_mark[s_mark_pos++ % NM];
    k.frame = 0; k.text = s;
}

void dbg_begin(Machine* m) {
    s_insn_pos = s_ev_pos = s_io_pos = s_snap_pos = s_mark_pos = s_lev_pos = s_gio_pos = 0; s_lev_last.clear();
    memset(s_cur_ints, 0, sizeof s_cur_ints); memset(s_ints_total, 0, sizeof s_ints_total);
    for (int i = 0; i < 256; i++) s_ivt0[i] = (uint32_t)rw(m, i * 4) | ((uint32_t)rw(m, i * 4 + 2) << 16);
    s_ivt_ok = true;
}

void dbg_frame(Machine* m) {
    if (!g_dbg_on) return;
    if (m->frame_count % 56 != 0) return;   // 約 1 秒ごと
    if (s_snap.empty()) s_snap.resize(NS);
    Snap& s = s_snap[s_snap_pos++ % NS];
    const Cpu& c = m->cpu;
    s.frame = frame_of(m); s.cs = c.sr[CS_]; s.ip = (uint16_t)c.ip;
    s.ax = R(c, EAX); s.bx = R(c, EBX); s.cx = R(c, ECX); s.dx = R(c, EDX); s.si = R(c, ESI); s.di = R(c, EDI); s.bp = R(c, EBP); s.sp = R(c, ESP);
    s.ds = c.sr[DS_]; s.es = c.sr[ES_]; s.ss = c.sr[SS_]; s.fl = (uint16_t)c.fl;
    for (int i = 0; i < 32; i++) s.stk[i] = rw(m, ((uint32_t)s.ss << 4) + (uint16_t)(s.sp + i * 2));
    s.imr0 = m->pic[0].imr; s.irr0 = m->pic[0].irr; s.isr0 = m->pic[0].isr;
    s.imr1 = m->pic[1].imr; s.irr1 = m->pic[1].irr; s.isr1 = m->pic[1].isr;
    memcpy(s.ints, s_cur_ints, sizeof s.ints); memset(s_cur_ints, 0, sizeof s_cur_ints);
}

// ---- 書き出し ----------------------------------------------------------------
static std::string s_out;
static void ap(const char* fmt, ...) {
    char b[1024];
    va_list v; va_start(v, fmt); int n = vsnprintf(b, sizeof b, fmt, v); va_end(v);
    if (n < 0) return;
    if (n < (int)sizeof b) { s_out.append(b, n); return; }
    std::vector<char> big((size_t)n + 1);
    va_start(v, fmt); vsnprintf(big.data(), big.size(), fmt, v); va_end(v);
    s_out.append(big.data(), n);
}
static bool put_file(const std::string& path, const void* d, size_t n) {
    void* h = hostfs::open(path, 1, true, true);
    if (!h) return false;
    const char* p = (const char*)d; bool ok = true;
    while (n > 0) { int k = (int)(n > 0x100000 ? 0x100000 : n); if (hostfs::write(h, p, k) != k) { ok = false; break; } p += k; n -= (size_t)k; }
    hostfs::close(h);
    return ok;
}
static std::string flags_str(uint16_t f) {
    std::string s;
    s += (f & FL_OF) ? "O" : "o"; s += (f & FL_DF) ? "D" : "d"; s += (f & FL_IF) ? "I" : "i"; s += (f & FL_TF) ? "T" : "t";
    s += (f & FL_SF) ? "S" : "s"; s += (f & FL_ZF) ? "Z" : "z"; s += (f & FL_AF) ? "A" : "a"; s += (f & FL_PF) ? "P" : "p"; s += (f & FL_CF) ? "C" : "c";
    return s;
}
static void hexline(Machine* m, uint32_t a, int n) {
    for (int i = 0; i < n; i++) ap(" %02X", m->ram[(a + i) & 0xFFFFF]);
}
static const char* when(Machine* m, uint32_t frame, char* buf, size_t n) {
    double sec = ((double)m->frame_count - frame) / 56.42;
    snprintf(buf, n, "f%u (-%.2fs)", frame, sec);
    return buf;
}

static void gdc_text(const char* name, const Gdc& g) {
    ap("%s: 表示 %s  ZOOM=%02X  PITCH=%02X  CSRFORM=%02X %02X %02X\n", name, g.display ? "ON" : "OFF", g.zoom, g.pitch, g.csrform[0], g.csrform[1], g.csrform[2]);
    ap("    SYNC:"); for (int i = 0; i < 8; i++) ap(" %02X", g.sync[i]);
    ap("\n    PRAM（SCROLL）:"); for (int i = 0; i < 16; i++) ap(" %02X", g.pram[i]);
    // 領域 1・2: 先頭（SAD、ワード）と行数（LEN）
    for (int a = 0; a < 2; a++) {
        const uint8_t* p = g.pram + a * 4;
        uint32_t sad = p[0] | (p[1] << 8) | ((p[2] & 3) << 16); uint32_t len = ((p[2] >> 4) | (p[3] << 4)) & 0x3FF;
        ap("\n    領域 %d: SAD=%05X（ワード） LEN=%u 行%s", a + 1, sad, len, (p[3] & 0x40) ? " IM" : "");
    }
    ap("\n    コマンド %02X  パラメータ %d 個  EAD=%05X\n", g.cmd, g.pcount, g.ead);
}
static void write_bmp(const std::string& path, const uint32_t* fb) {
    const int W = 640, H = 400; std::vector<uint8_t> b(54 + W * H * 3);
    auto w32 = [&](int o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); b[o + 2] = (uint8_t)(v >> 16); b[o + 3] = (uint8_t)(v >> 24); };
    b[0] = 'B'; b[1] = 'M'; w32(2, (uint32_t)b.size()); w32(10, 54); w32(14, 40); w32(18, W); w32(22, H); b[26] = 1; b[28] = 24; w32(34, W * H * 3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint32_t c = fb[(H - 1 - y) * W + x]; uint8_t* d = &b[54 + (y * W + x) * 3];
        d[0] = (uint8_t)c; d[1] = (uint8_t)(c >> 8); d[2] = (uint8_t)(c >> 16);
    }
    put_file(path, b.data(), b.size());
}

bool dbg_write(Machine* m, const std::string& dir, std::string* err, const uint32_t* fb) {
    if (!m) return false;
    std::string tp = hostfs::join(dir, "PC98PLAYER.DEBUG"), mp = hostfs::join(dir, "PC98PLAYER.MEM");
    s_out.clear(); s_out.reserve(4 << 20);
    char tb[64];
    const Cpu& c = m->cpu;
    ap("# PC98PLAYER.DEBUG  (Trace=2)\n");
    ap("# 時刻はフレーム番号（1 秒 ≒ 56.42 フレーム）と、終わったときから何秒前か。数はすべて 16 進。\n");
    ap("# 同じフォルダの PC98PLAYER.MEM は 1MB のメモリの中身（ファイルの位置 = リニアアドレス）。\n\n");

    ap("=== 終わったときの CPU（フレーム %llu）===\n", (unsigned long long)m->frame_count);
    ap("CS:IP=%04X:%04X  SS:SP=%04X:%04X  BP=%04X\n", c.sr[CS_], (unsigned)(c.ip & 0xFFFF), c.sr[SS_], R(c, ESP), R(c, EBP));
    ap("AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X DS=%04X ES=%04X\n", R(c, EAX), R(c, EBX), R(c, ECX), R(c, EDX), R(c, ESI), R(c, EDI), c.sr[DS_], c.sr[ES_]);
    ap("FL=%04X [%s]  halted=%d  CR0=%08X  32 ビットのレジスタ: EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESP=%08X\n",
            (unsigned)(c.fl & 0xFFFF), flags_str((uint16_t)c.fl).c_str(), c.halted, c.cr0, c.r[EAX], c.r[EBX], c.r[ECX], c.r[EDX], c.r[ESP]);
    if (c.cr0 & 1) ap("（プロテクトモード。下のリニアアドレスはリアルモードとして計算しているので、正しくない）\n");
    uint32_t cip = ((uint32_t)c.sr[CS_] << 4) + (c.ip & 0xFFFF);
    ap("コード %04X:%04X の前 16 バイト:", c.sr[CS_], (unsigned)((c.ip - 16) & 0xFFFF)); hexline(m, cip - 16, 16);
    ap("\nコード %04X:%04X から 32 バイト:", c.sr[CS_], (unsigned)(c.ip & 0xFFFF)); hexline(m, cip, 32); ap("\n");
    uint32_t sa = ((uint32_t)c.sr[SS_] << 4);
    ap("スタック（SS:SP から 64 語。[SP+n]）:\n");
    for (int i = 0; i < 64; i++) {
        if (i % 8 == 0) ap("  +%03X:", i * 2);
        ap(" %04X", rw(m, sa + (uint16_t)(R(c, ESP) + i * 2)));
        if (i % 8 == 7) ap("\n");
    }
    ap("BP の鎖（[BP] 古い BP / [BP+2] 戻り先 IP / [BP+4] CS かもしれない）:\n");
    { uint16_t bp = R(c, EBP); for (int i = 0; i < 12 && bp; i++) {
        uint16_t nb = rw(m, sa + bp), r1 = rw(m, sa + (uint16_t)(bp + 2)), r2 = rw(m, sa + (uint16_t)(bp + 4));
        ap("  BP=%04X → 古い BP=%04X 戻り %04X（%04X:%04X）引数 %04X %04X %04X\n", bp, nb, r1, r2, r1,
                rw(m, sa + (uint16_t)(bp + 6)), rw(m, sa + (uint16_t)(bp + 8)), rw(m, sa + (uint16_t)(bp + 10)));
        if (nb <= bp) break; bp = nb; } }

    ap("\n=== 画面 ===\n");
    gdc_text("文字の GDC（マスタ）", m->gdcm);
    gdc_text("グラフィックの GDC（スレーブ）", m->gdcs);
    ap("グラフィックの GDC のクロック: %s\n", m->gdc_clk5 ? "5MHz（PITCH はバイト単位）" : "2.5MHz（PITCH はワード単位）");
    ap("200 ライン: %d（下半分: %d）  カラー: %d  表示バンク %d  描画バンク %d\n", m->gfx_200, m->gfx_200_lower, m->gfx_color, m->disp_bank, m->draw_bank);
    ap("モードF/F（68h）:"); for (int i = 0; i < 8; i++) ap(" %d", m->modeff[i]);
    ap("\nモードF/F2（6Ah）:"); for (int i = 0; i < 8; i++) ap(" %d", m->modeff2[i]);
    ap("\nGRCG: モード %02X タイル %02X %02X %02X %02X   EGC: 使える %d  ACCESS=%04X FGBG=%04X OPE=%04X FG=%04X MASK=%04X BG=%04X SFT=%04X LENG=%04X\n",
       m->grcg_mode, m->grcg_tile[0], m->grcg_tile[1], m->grcg_tile[2], m->grcg_tile[3], m->egc_enabled,
       m->egc.access, m->egc.fgbg, m->egc.ope, m->egc.fg, m->egc.mask, m->egc.bg, m->egc.sft, m->egc.leng);
    ap("パレット（G R B）:"); for (int i = 0; i < 16; i++) ap(" %X:%X%X%X", i, m->pal[i][0], m->pal[i][1], m->pal[i][2]);
    ap("\n（PC98PLAYER.VRAM: 0x00000 からテキスト VRAM 16KB、0x04000 からグラフィック VRAM バンク 0 のプレーン B/R/G/I（各 32KB）、続いてバンク 1）\n");

    ap("\n=== PIC / PIT ===\n");
    for (int i = 0; i < 2; i++) ap("PIC%d（%s）: IMR=%02X IRR=%02X ISR=%02X ベース=%02X\n", i, i ? "スレーブ" : "マスタ", m->pic[i].imr, m->pic[i].irr, m->pic[i].isr, m->pic[i].base);
    for (int i = 0; i < 3; i++) ap("PIT%d: モード %d 初期値 %04X 今 %04X\n", i, m->pit[i].mode, m->pit[i].reload, m->pit[i].counter);

    ap("\n=== 割込みベクタ（起動の直後から変わったもの）===\n");
    for (int i = 0; i < 256; i++) {
        uint32_t v = (uint32_t)rw(m, i * 4) | ((uint32_t)rw(m, i * 4 + 2) << 16);
        if (s_ivt_ok && v == s_ivt0[i]) continue;
        ap("INT %02X → %04X:%04X（起動時 %04X:%04X）  入った回数 %llu\n", i, v >> 16, v & 0xFFFF, s_ivt0[i] >> 16, s_ivt0[i] & 0xFFFF, (unsigned long long)s_ints_total[i]);
    }
    ap("（よく使われた割込み:");
    for (int i = 0; i < 256; i++) if (s_ints_total[i] > 0) ap(" %02X=%llu", i, (unsigned long long)s_ints_total[i]);
    ap("）\n");

    ap("\n=== MCB の鎖 ===\n%s", dos_debug_mcb(m).c_str());

    if (!s_mark.empty() && s_mark_pos) {
        ap("\n=== 目印 ===\n");
        unsigned n = s_mark_pos < NM ? s_mark_pos : NM;
        for (unsigned k = 0; k < n; k++) ap("%s\n", s_mark[(s_mark_pos - n + k) % NM].text.c_str());
    }

    if (!s_snap.empty() && s_snap_pos) {
        ap("\n=== 1 秒ごとの様子（古い順）===\n");
        unsigned n = s_snap_pos < NS ? s_snap_pos : NS;
        for (unsigned k = 0; k < n; k++) {
            const Snap& s = s_snap[(s_snap_pos - n + k) % NS];
            ap("[%s] CS:IP=%04X:%04X SS:SP=%04X:%04X BP=%04X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X DS=%04X ES=%04X FL=%04X[%s]\n",
                    when(m, s.frame, tb, sizeof tb), s.cs, s.ip, s.ss, s.sp, s.bp, s.ax, s.bx, s.cx, s.dx, s.si, s.di, s.ds, s.es, s.fl, flags_str(s.fl).c_str());
            ap("    PIC IMR=%02X/%02X IRR=%02X/%02X ISR=%02X/%02X  スタック:", s.imr0, s.imr1, s.irr0, s.irr1, s.isr0, s.isr1);
            for (int i = 0; i < 16; i++) ap(" %04X", s.stk[i]);
            ap("\n    この 1 秒の割込み:");
            for (int i = 0; i < 256; i++) if (s.ints[i]) ap(" %02X=%u", i, s.ints[i]);
            ap("\n");
        }
    }

    if (!s_lev.empty() && s_lev_pos) {
        unsigned n = s_lev_pos < NL ? s_lev_pos : NL;
        ap("\n=== 割込みの長い記録（直近 %u 件、古い順。同じ割込み（ハードウェアは番号、INT は番号・AH・呼んだ場所が同じ）が 0.5 秒以内に続いたものは 1 件にまとめて回数を書く）===\n", n);
        for (unsigned k = 0; k < n; k++) {
            const IntEv& e = s_lev[(s_lev_pos - n + k) % NL];
            const char* kd = e.kind == 0 ? "HW   " : e.kind == 1 ? "INT  " : "RET21";
            ap("[%s] %s %02X %s %04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X SS:SP=%04X:%04X%s",
               when(m, e.frame, tb, sizeof tb), kd, e.vec, e.kind == 2 ? "戻り先" : "場所", e.cs, e.ip, e.ax, e.bx, e.cx, e.dx, e.ds, e.es, e.ss, e.sp,
               e.kind == 2 ? ((e.fl & FL_CF) ? " CF=1" : " CF=0") : "");
            if (e.count > 1) ap("  ×%u（最後 %s）", e.count, when(m, e.last_frame, tb, sizeof tb));
            ap("\n");
        }
    }

    if (!s_ev.empty() && s_ev_pos) {
        unsigned n = s_ev_pos < NE ? s_ev_pos : NE;
        ap("\n=== 割込み（直近 %u 回、古い順。HW = ハードウェア、INT = 命令、RET21 = INT 21h の結果）===\n", n);
        for (unsigned k = 0; k < n; k++) {
            const IntEv& e = s_ev[(s_ev_pos - n + k) % NE];
            const char* kd = e.kind == 0 ? "HW   " : e.kind == 1 ? "INT  " : "RET21";
            ap("[%s] %s %02X %s %04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X DS=%04X ES=%04X SS:SP=%04X:%04X FL=%04X%s [SP] %04X %04X %04X %04X\n",
                    when(m, e.frame, tb, sizeof tb), kd, e.vec, e.kind == 2 ? "戻り先" : "場所", e.cs, e.ip, e.ax, e.bx, e.cx, e.dx, e.si, e.di, e.ds, e.es, e.ss, e.sp, e.fl,
                    e.kind == 2 ? ((e.fl & FL_CF) ? " CF=1（エラー）" : " CF=0") : "", e.stk[0], e.stk[1], e.stk[2], e.stk[3]);
        }
    }

    if (!s_gio.empty() && s_gio_pos) {
        unsigned n = s_gio_pos < NG ? s_gio_pos : NG;
        ap("\n=== 画面の設定の OUT（直近 %u 回、古い順。GDC 60/62・A0/A2、モードF/F 68/6A、表示バンク A4、CRTC 70～7A、9A0/9A2/9A8）===\n", n);
        for (unsigned k = 0; k < n; k++) {
            const IoEv& e = s_gio[(s_gio_pos - n + k) % NG];
            const char* what = "";
            if (e.port == 0xA2 || e.port == 0x62) {
                switch (e.val & 0xFF) {
                case 0x0C: case 0x0D: what = "（START/STOP）"; break; case 0x0E: case 0x0F: what = "（SYNC）"; break;
                case 0x47: what = "（PITCH）"; break; case 0x4B: what = "（CSRFORM）"; break; case 0x46: what = "（ZOOM）"; break;
                case 0x49: what = "（CSRW）"; break; case 0x4A: what = "（MASK）"; break; case 0x4C: what = "（VECTW）"; break;
                case 0x6B: what = "（START）"; break; case 0x6F: what = "（VSYNC）"; break;
                default: if ((e.val & 0xF0) == 0x70) what = "（SCROLL / PRAM）"; break;
                }
            }
            ap("[%s] OUT %04X <- %02X%s  @%04X:%04X\n", when(m, e.frame, tb, sizeof tb), e.port, e.val & 0xFF, what, e.cs, e.ip);
        }
    }

    if (!s_io.empty() && s_io_pos) {
        unsigned n = s_io_pos < NO ? s_io_pos : NO;
        ap("\n=== I/O ポート（直近 %u 回、古い順）===\n", n);
        for (unsigned k = 0; k < n; k++) {
            const IoEv& e = s_io[(s_io_pos - n + k) % NO];
            if (e.w16) ap("[%s] %s %04X %s %04X  @%04X:%04X\n", when(m, e.frame, tb, sizeof tb), e.out ? "OUT" : "IN ", e.port, e.out ? "<-" : "->", e.val, e.cs, e.ip);
            else ap("[%s] %s %04X %s %02X  @%04X:%04X\n", when(m, e.frame, tb, sizeof tb), e.out ? "OUT" : "IN ", e.port, e.out ? "<-" : "->", e.val & 0xFF, e.cs, e.ip);
        }
    }

    if (!s_insn.empty() && s_insn_pos) {
        unsigned n = s_insn_pos < NI ? s_insn_pos : NI;
        ap("\n=== 命令（直近 %u 個、古い順。その命令を実行する前のレジスタ）===\n", n);
        uint32_t last_f = 0xFFFFFFFF;
        for (unsigned k = 0; k < n; k++) {
            const Insn& e = s_insn[(s_insn_pos - n + k) % NI];
            if (e.frame != last_f) { ap("-- %s\n", when(m, e.frame, tb, sizeof tb)); last_f = e.frame; }
            uint32_t a = ((uint32_t)e.cs << 4) + e.ip;
            ap("%04X:%04X %02X%02X%02X%02X  AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X SS=%04X %s\n",
                    e.cs, e.ip, m->ram[a & 0xFFFFF], m->ram[(a + 1) & 0xFFFFF], m->ram[(a + 2) & 0xFFFFF], m->ram[(a + 3) & 0xFFFFF],
                    e.ax, e.bx, e.cx, e.dx, e.si, e.di, e.bp, e.sp, e.ds, e.es, e.ss, flags_str(e.fl).c_str());
        }
        ap("（命令の 4 バイトは「終わったときの」メモリの中身。実行したあとで書き換わったコードは違って見える）\n");
    }
    bool ok = put_file(tp, s_out.data(), s_out.size());
    s_out.clear(); s_out.shrink_to_fit();
    if (!ok) { if (err) *err = "書けません: " + tp; return false; }
    if (!put_file(mp, m->ram, 0x100000)) { if (err) *err = "書けません: " + mp; return false; }
    {   // VRAM
        std::vector<uint8_t> v(sizeof m->tvram + sizeof m->gvram);
        memcpy(v.data(), m->tvram, sizeof m->tvram); memcpy(v.data() + sizeof m->tvram, m->gvram, sizeof m->gvram);
        put_file(hostfs::join(dir, "PC98PLAYER.VRAM"), v.data(), v.size());
    }
    if (fb) write_bmp(hostfs::join(dir, "PC98PLAYER.BMP"), fb);
    return true;
}
