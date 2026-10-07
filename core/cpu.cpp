// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  cpu.cpp  --  i386 リアルモード命令インタプリタ
//
//  GMPV3 Studio の 8086 コアを出発点に、ゲーム本体を動かすのに要る範囲へ
//  広げたもの。追加したのは次のとおり。
//    ・186 命令（PUSHA/POPA/ENTER/LEAVE/BOUND/INS/OUTS/即値シフト/即値 IMUL）
//    ・386 命令（オペランド/アドレスサイズ接頭辞、32bit レジスタ、FS/GS、
//      MOVZX/MOVSX、SHLD/SHRD、BT 系、BSF/BSR、SETcc、Jcc near、LFS/LGS/LSS）
//    ・486 の BSWAP/XADD/CMPXCHG（使うソフトがまれにあるので）
//    ・F1 xx を HLE トラップとして予約（BIOS/DOS をホスト側で実装するための入口）
//  浮動小数点命令は「FPU なし」として振る舞う（命令は読み飛ばすだけ）。
// -----------------------------------------------------------------------------
#include "cpu.h"
#include "memio.h"
#include <string.h>
#include <stdio.h>
#include <vector>
#include <string>
void plog(const char* fmt, ...);

int g_irq_hint = 0;
int g_trace_int = -1;
unsigned long long g_frame_dbg;
int g_prof_cs = -1; unsigned* g_prof = nullptr;      // Machine が「受け付け待ちの IRQ があるかも」を立てる

static Cpu*     C;
static Machine* M;
static uint8_t  s_parity[256];

// ---- 命令ごとのデコード状態 -------------------------------------------------
static int      d_seg;        // セグメント上書き（-1 = なし）
static bool     d_o32, d_a32;
static int      d_rep;        // 0 / 0xF2 / 0xF3
static uint8_t  d_mod, d_reg, d_rm;
static bool     d_isreg;
static uint32_t d_ea;         // 線形アドレス
static uint32_t d_off;        // セグメント内オフセット（LEA 用）
static int      d_cyc;
static uint8_t  s_op, s_op0f;   // 実行した命令（比較の記録用）
static int      s_vsz;
static uint32_t s_moffs;        // MOV AL/AX,[moffs] の番地

#define MASK(sz) ((sz) == 32 ? 0xFFFFFFFFu : ((1u << (sz)) - 1u))
#define SIGN(sz) (1u << ((sz) - 1))

static inline uint32_t sbase(int s) { return (uint32_t)C->sr[s] << 4; }

// ---- フェッチ ---------------------------------------------------------------
static inline uint8_t f8() {
    uint8_t v = mem_rb(M, sbase(CS_) + C->ip);
    C->ip = (uint16_t)(C->ip + 1);
    return v;
}
static inline uint16_t f16() { uint16_t lo = f8(); return (uint16_t)(lo | (f8() << 8)); }
static inline uint32_t f32() { uint32_t lo = f16(); return lo | ((uint32_t)f16() << 16); }
static inline uint32_t fimm(int sz) { return sz == 8 ? f8() : sz == 16 ? f16() : f32(); }

// ---- レジスタ ---------------------------------------------------------------
static inline uint32_t getr(int sz, int i) {
    if (sz == 8) return i < 4 ? (C->r[i] & 0xFF) : ((C->r[i - 4] >> 8) & 0xFF);
    if (sz == 16) return C->r[i] & 0xFFFF;
    return C->r[i];
}
static inline void setr(int sz, int i, uint32_t v) {
    if (sz == 8) {
        if (i < 4) C->r[i] = (C->r[i] & ~0xFFu) | (v & 0xFF);
        else       C->r[i - 4] = (C->r[i - 4] & ~0xFF00u) | ((v & 0xFF) << 8);
    } else if (sz == 16) C->r[i] = (C->r[i] & 0xFFFF0000u) | (v & 0xFFFF);
    else C->r[i] = v;
}

// ---- メモリ ------------------------------------------------------------------
static inline uint32_t rdm(int sz, uint32_t a) {
    return sz == 8 ? mem_rb(M, a) : sz == 16 ? mem_rw(M, a) : mem_rd(M, a);
}
static inline void wrm(int sz, uint32_t a, uint32_t v) {
    if (sz == 8) mem_wb(M, a, (uint8_t)v);
    else if (sz == 16) mem_ww(M, a, (uint16_t)v);
    else mem_wd(M, a, v);
}

// ---- ModR/M -----------------------------------------------------------------
static void modrm() {
    uint8_t b = f8();
    d_mod = b >> 6; d_reg = (b >> 3) & 7; d_rm = b & 7;
    if (d_mod == 3) { d_isreg = true; return; }
    d_isreg = false;
    d_cyc += 2;
    int ds = DS_;
    uint32_t off = 0;
    if (!d_a32) {
        switch (d_rm) {
        case 0: off = C->r[EBX] + C->r[ESI]; break;
        case 1: off = C->r[EBX] + C->r[EDI]; break;
        case 2: off = C->r[EBP] + C->r[ESI]; ds = SS_; break;
        case 3: off = C->r[EBP] + C->r[EDI]; ds = SS_; break;
        case 4: off = C->r[ESI]; break;
        case 5: off = C->r[EDI]; break;
        case 6: if (d_mod == 0) off = f16(); else { off = C->r[EBP]; ds = SS_; } break;
        case 7: off = C->r[EBX]; break;
        }
        if (d_mod == 1) off += (uint32_t)(int32_t)(int8_t)f8();
        else if (d_mod == 2) off += f16();
        off &= 0xFFFF;
    } else {
        int rm = d_rm;
        if (rm == 4) {
            uint8_t sib = f8();
            int ss = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
            if (base == 5 && d_mod == 0) off = f32();
            else { off = C->r[base]; if (base == ESP || base == EBP) ds = SS_; }
            if (idx != 4) off += C->r[idx] << ss;
        } else if (rm == 5 && d_mod == 0) {
            off = f32();
        } else {
            off = C->r[rm];
            if (rm == EBP) ds = SS_;
        }
        if (d_mod == 1) off += (uint32_t)(int32_t)(int8_t)f8();
        else if (d_mod == 2) off += f32();
    }
    d_off = off;
    d_ea = sbase(d_seg >= 0 ? d_seg : ds) + off;
}
static inline uint32_t rmr(int sz) { return d_isreg ? getr(sz, d_rm) : rdm(sz, d_ea); }
static inline void rmw(int sz, uint32_t v) { if (d_isreg) setr(sz, d_rm, v); else wrm(sz, d_ea, v); }

// ---- フラグ ------------------------------------------------------------------
#define ARITH (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF)
static inline uint32_t szp(uint32_t v, int sz) {
    uint32_t f = 0;
    v &= MASK(sz);
    if (!v) f |= FL_ZF;
    if (v & SIGN(sz)) f |= FL_SF;
    f |= s_parity[v & 0xFF];
    return f;
}
static inline void setfl(uint32_t f) { C->fl = (C->fl & ~ARITH) | f; }

static uint32_t s_alu_a, s_alu_b;   // 直前の ALU 演算の 2 つの値（比較の記録用）
static uint32_t alu(int op, uint32_t a, uint32_t b, int sz) {
    uint32_t mask = MASK(sz), sign = SIGN(sz);
    a &= mask; b &= mask;
    s_alu_a = a; s_alu_b = b;
    uint32_t r = 0, f = 0;
    uint32_t c = C->fl & FL_CF;
    switch (op) {
    case 0: case 2: {                       // ADD / ADC
        uint64_t t = (uint64_t)a + b + (op == 2 ? c : 0);
        r = (uint32_t)t & mask;
        if (t > mask) f |= FL_CF;
        if ((a ^ r) & (b ^ r) & sign) f |= FL_OF;
        if ((a ^ b ^ r) & 0x10) f |= FL_AF;
        break; }
    case 3: case 5: case 7: {               // SBB / SUB / CMP
        uint64_t sub = (uint64_t)b + (op == 3 ? c : 0);
        r = (uint32_t)((uint64_t)a - sub) & mask;
        if ((uint64_t)a < sub) f |= FL_CF;
        if ((a ^ b) & (a ^ r) & sign) f |= FL_OF;
        if ((a ^ b ^ r) & 0x10) f |= FL_AF;
        break; }
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    case 6: r = a ^ b; break;
    }
    setfl(f | szp(r, sz));
    return r;
}
static uint32_t incdec(uint32_t a, int sz, bool dec) {
    uint32_t mask = MASK(sz), sign = SIGN(sz);
    a &= mask;
    uint32_t r = (dec ? a - 1 : a + 1) & mask;
    uint32_t f = C->fl & FL_CF;
    if (!dec && r == sign) f |= FL_OF;
    if (dec && a == sign) f |= FL_OF;
    if ((a ^ 1 ^ r) & 0x10) f |= FL_AF;
    setfl(f | szp(r, sz));
    return r;
}

static uint32_t shift(int op, uint32_t a, int cnt, int sz) {
    uint32_t mask = MASK(sz), sign = SIGN(sz);
    a &= mask;
    cnt &= 0x1F;
    if (!cnt) return a;
    d_cyc += cnt;
    uint32_t r = a, f = C->fl & ARITH;
    uint32_t cf = f & FL_CF;
    switch (op) {
    case 0: {   // ROL
        int n = cnt % sz;
        r = n ? ((a << n) | (a >> (sz - n))) & mask : a;
        cf = r & 1;
        f = (f & ~(FL_CF | FL_OF)) | cf;
        if (((r & sign) ? 1 : 0) ^ cf) f |= FL_OF;
        C->fl = (C->fl & ~ARITH) | f; return r; }
    case 1: {   // ROR
        int n = cnt % sz;
        r = n ? ((a >> n) | (a << (sz - n))) & mask : a;
        cf = (r & sign) ? 1 : 0;
        f = (f & ~(FL_CF | FL_OF)) | cf;
        if (((r >> (sz - 1)) ^ (r >> (sz - 2))) & 1) f |= FL_OF;
        C->fl = (C->fl & ~ARITH) | f; return r; }
    case 2: {   // RCL
        int n = cnt % (sz + 1);
        for (int i = 0; i < n; i++) {
            uint32_t nc = (r & sign) ? 1 : 0;
            r = ((r << 1) | cf) & mask; cf = nc;
        }
        f = (f & ~(FL_CF | FL_OF)) | cf;
        if (((r & sign) ? 1 : 0) ^ cf) f |= FL_OF;
        C->fl = (C->fl & ~ARITH) | f; return r; }
    case 3: {   // RCR
        int n = cnt % (sz + 1);
        uint32_t of = (((a & sign) ? 1 : 0) ^ cf);
        for (int i = 0; i < n; i++) {
            uint32_t nc = r & 1;
            r = (r >> 1) | (cf ? sign : 0); cf = nc;
        }
        f = (f & ~(FL_CF | FL_OF)) | cf | (of ? FL_OF : 0);
        C->fl = (C->fl & ~ARITH) | f; return r; }
    case 4: case 6: {  // SHL/SAL
        uint64_t t = (uint64_t)a << cnt;
        r = (uint32_t)t & mask;
        cf = (uint32_t)((t >> sz) & 1);
        if (cnt > sz) cf = 0;
        f = szp(r, sz) | cf;
        if (((r & sign) ? 1 : 0) ^ cf) f |= FL_OF;
        f |= FL_AF;
        C->fl = (C->fl & ~ARITH) | f; return r; }
    case 5: {   // SHR
        cf = cnt <= sz ? ((a >> (cnt - 1)) & 1) : 0;
        r = cnt >= sz ? 0 : (a >> cnt);
        f = szp(r, sz) | cf;
        if (cnt == 1 && (a & sign)) f |= FL_OF;
        C->fl = (C->fl & ~ARITH) | f; return r; }
    case 7: {   // SAR
        int32_t sa = (int32_t)(a << (32 - sz)) >> (32 - sz);
        if (cnt >= sz) { r = (a & sign) ? mask : 0; cf = (a & sign) ? 1 : 0; }
        else { cf = (uint32_t)(sa >> (cnt - 1)) & 1; r = (uint32_t)(sa >> cnt) & mask; }
        f = szp(r, sz) | cf;
        C->fl = (C->fl & ~ARITH) | f; return r; }
    }
    return r;
}

// ---- スタック ----------------------------------------------------------------
static inline void push16(uint16_t v) {
    uint16_t sp = (uint16_t)(C->r[ESP] - 2);
    C->r[ESP] = (C->r[ESP] & 0xFFFF0000u) | sp;
    uint32_t fx = g_side_fx;   // スタックへの積み下ろしは空回りの判定に数えない（戻れば同じ状態）
    mem_ww(M, sbase(SS_) + sp, v);
    g_side_fx = fx;
}
static inline void push32(uint32_t v) {
    uint16_t sp = (uint16_t)(C->r[ESP] - 4);
    C->r[ESP] = (C->r[ESP] & 0xFFFF0000u) | sp;
    uint32_t fx = g_side_fx;
    mem_wd(M, sbase(SS_) + sp, v);
    g_side_fx = fx;
}
static inline uint16_t pop16() {
    uint16_t sp = (uint16_t)C->r[ESP];
    uint16_t v = mem_rw(M, sbase(SS_) + sp);
    C->r[ESP] = (C->r[ESP] & 0xFFFF0000u) | (uint16_t)(sp + 2);
    return v;
}
static inline uint32_t pop32() {
    uint16_t sp = (uint16_t)C->r[ESP];
    uint32_t v = mem_rd(M, sbase(SS_) + sp);
    C->r[ESP] = (C->r[ESP] & 0xFFFF0000u) | (uint16_t)(sp + 4);
    return v;
}
static inline void pushv(uint32_t v) { if (d_o32) push32(v); else push16((uint16_t)v); }
static inline uint32_t popv() { return d_o32 ? pop32() : pop16(); }

// ---- 割込み ------------------------------------------------------------------
static void do_int(uint8_t vec) {
    push16((uint16_t)(C->fl | 0x0002));
    push16(C->sr[CS_]);
    push16(C->ip);
    C->fl &= ~(FL_IF | FL_TF);
    C->ip     = mem_rw(M, vec * 4u);
    C->sr[CS_] = mem_rw(M, vec * 4u + 2);
    d_cyc += 40;
}
void cpu_interrupt(Cpu* c, uint8_t vec) {
    Cpu* sc = C; Machine* sm = M;
    C = c; M = c->m;
    do_int(vec);
    C = sc; M = sm;
    if (!C) { C = c; M = c->m; }
}
static void fault(uint8_t vec) {        // 386 流：戻り先は命令の先頭
    C->ip = C->op_ip;
    C->sr[CS_] = C->op_cs;
    do_int(vec);
}

static void set_flags_word(uint32_t v, bool is32) {
    // 386 リアルモード: IOPL/NT は書ける、bit15 は 0、bit1 は 1。
    uint32_t keep = C->fl & (is32 ? 0xFFFC0000u : 0xFFFF0000u);
    uint32_t nv = (v & (is32 ? 0x0003FFFFu : 0xFFFFu)) & ~0x8000u & ~0x28u;
    nv |= 0x0002;
    nv &= ~0x00030000u;       // VM/RF はリアルモードでは立たない
    C->fl = keep | nv;
}

// ---- 条件 --------------------------------------------------------------------
static inline bool cond(int cc) {
    uint32_t f = C->fl;
    bool r;
    switch (cc >> 1) {
    case 0: r = f & FL_OF; break;
    case 1: r = f & FL_CF; break;
    case 2: r = f & FL_ZF; break;
    case 3: r = (f & FL_CF) || (f & FL_ZF); break;
    case 4: r = f & FL_SF; break;
    case 5: r = f & FL_PF; break;
    case 6: r = ((f & FL_SF) != 0) != ((f & FL_OF) != 0); break;
    default: r = (f & FL_ZF) || (((f & FL_SF) != 0) != ((f & FL_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

// ---- 文字列命令 -------------------------------------------------------------
static inline uint32_t si_() { return d_a32 ? C->r[ESI] : (C->r[ESI] & 0xFFFF); }
static inline uint32_t di_() { return d_a32 ? C->r[EDI] : (C->r[EDI] & 0xFFFF); }
static inline void adv(int reg, int delta) {
    if (d_a32) C->r[reg] += (uint32_t)delta;
    else C->r[reg] = (C->r[reg] & 0xFFFF0000u) | (uint16_t)(C->r[reg] + delta);
}
static inline uint32_t cx_() { return d_a32 ? C->r[ECX] : (C->r[ECX] & 0xFFFF); }
static inline void setcx(uint32_t v) {
    if (d_a32) C->r[ECX] = v; else C->r[ECX] = (C->r[ECX] & 0xFFFF0000u) | (uint16_t)v;
}

static void string_op(uint8_t op, int sz) {
    int bytes = sz / 8;
    int delta = (C->fl & FL_DF) ? -bytes : bytes;
    int sseg = d_seg >= 0 ? d_seg : DS_;
    bool rep = d_rep != 0;
    uint32_t n = rep ? cx_() : 1;
    if (rep && n == 0) return;
    for (;;) {
        switch (op) {
        case 0xA4: case 0xA5: wrm(sz, sbase(ES_) + di_(), rdm(sz, sbase(sseg) + si_())); adv(ESI, delta); adv(EDI, delta); break;
        case 0xAA: case 0xAB: wrm(sz, sbase(ES_) + di_(), getr(sz, EAX)); adv(EDI, delta); break;
        case 0xAC: case 0xAD: setr(sz, EAX, rdm(sz, sbase(sseg) + si_())); adv(ESI, delta); break;
        case 0xA6: case 0xA7: {
            uint32_t a = rdm(sz, sbase(sseg) + si_()), b = rdm(sz, sbase(ES_) + di_());
            alu(7, a, b, sz); adv(ESI, delta); adv(EDI, delta); break; }
        case 0xAE: case 0xAF: {
            uint32_t b = rdm(sz, sbase(ES_) + di_());
            alu(7, getr(sz, EAX), b, sz); adv(EDI, delta); break; }
        case 0x6C: case 0x6D: {
            uint16_t port = (uint16_t)C->r[EDX];
            uint32_t v = sz == 8 ? io_in8(M, port) : io_in16(M, port);
            if (sz == 32) v |= (uint32_t)io_in16(M, port + 2) << 16;
            wrm(sz, sbase(ES_) + di_(), v); adv(EDI, delta); break; }
        case 0x6E: case 0x6F: {
            uint16_t port = (uint16_t)C->r[EDX];
            uint32_t v = rdm(sz, sbase(sseg) + si_());
            if (sz == 8) io_out8(M, port, (uint8_t)v);
            else { io_out16(M, port, (uint16_t)v); if (sz == 32) io_out16(M, port + 2, (uint16_t)(v >> 16)); }
            adv(ESI, delta); break; }
        }
        d_cyc += 3;
        if (!rep) return;
        n--; setcx(n);
        if (n == 0) return;
        if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
            bool z = (C->fl & FL_ZF) != 0;
            if (d_rep == 0xF3 && !z) return;
            if (d_rep == 0xF2 && z) return;
        }
    }
}

// ---- 乗除算 ------------------------------------------------------------------
static void grp3(int sz) {
    uint32_t mask = MASK(sz);
    switch (d_reg) {
    case 0: case 1: { uint32_t v = rmr(sz), i = fimm(sz); alu(4, v, i, sz); return; }
    case 2: rmw(sz, ~rmr(sz) & mask); return;
    case 3: { uint32_t v = rmr(sz); uint32_t r = alu(5, 0, v, sz); rmw(sz, r);
              if (v) C->fl |= FL_CF; else C->fl &= ~FL_CF; return; }
    case 4: case 5: {
        d_cyc += 12;
        uint32_t v = rmr(sz);
        bool sgn = d_reg == 5;
        uint64_t res; bool ovf;
        if (sz == 8) {
            if (sgn) { int16_t t = (int16_t)(int8_t)(C->r[EAX] & 0xFF) * (int8_t)v; res = (uint16_t)t; ovf = t != (int8_t)t; }
            else { uint16_t t = (uint16_t)((C->r[EAX] & 0xFF) * v); res = t; ovf = (t >> 8) != 0; }
            setr(16, EAX, (uint32_t)res);
        } else if (sz == 16) {
            if (sgn) { int32_t t = (int32_t)(int16_t)C->r[EAX] * (int16_t)v; res = (uint32_t)t; ovf = t != (int16_t)t; }
            else { uint32_t t = (C->r[EAX] & 0xFFFF) * (v & 0xFFFF); res = t; ovf = (t >> 16) != 0; }
            setr(16, EAX, (uint32_t)res); setr(16, EDX, (uint32_t)(res >> 16));
        } else {
            if (sgn) { int64_t t = (int64_t)(int32_t)C->r[EAX] * (int32_t)v; res = (uint64_t)t; ovf = t != (int32_t)t; }
            else { uint64_t t = (uint64_t)C->r[EAX] * v; res = t; ovf = (t >> 32) != 0; }
            C->r[EAX] = (uint32_t)res; C->r[EDX] = (uint32_t)(res >> 32);
        }
        uint32_t f = C->fl & ARITH & ~(FL_CF | FL_OF);
        if (ovf) f |= FL_CF | FL_OF;
        C->fl = (C->fl & ~ARITH) | f;
        return; }
    case 6: case 7: {
        d_cyc += 20;
        uint32_t v = rmr(sz) & mask;
        bool sgn = d_reg == 7;
        if (v == 0) { fault(0); return; }
        if (sz == 8) {
            uint16_t num = (uint16_t)C->r[EAX];
            if (sgn) {
                int q = (int16_t)num / (int8_t)v, rm = (int16_t)num % (int8_t)v;
                if (q > 127 || q < -128) { fault(0); return; }
                setr(8, 0, (uint32_t)q); setr(8, 4, (uint32_t)rm);
            } else {
                unsigned q = num / v, rm = num % v;
                if (q > 0xFF) { fault(0); return; }
                setr(8, 0, q); setr(8, 4, rm);
            }
        } else if (sz == 16) {
            uint32_t num = (C->r[EAX] & 0xFFFF) | ((C->r[EDX] & 0xFFFF) << 16);
            if (sgn) {
                int32_t n = (int32_t)num, dv = (int16_t)v;
                int32_t q = n / dv, rm = n % dv;
                if (q > 32767 || q < -32768) { fault(0); return; }
                setr(16, EAX, (uint32_t)q); setr(16, EDX, (uint32_t)rm);
            } else {
                uint32_t q = num / v, rm = num % v;
                if (q > 0xFFFF) { fault(0); return; }
                setr(16, EAX, q); setr(16, EDX, rm);
            }
        } else {
            uint64_t num = (uint64_t)C->r[EAX] | ((uint64_t)C->r[EDX] << 32);
            if (sgn) {
                int64_t n = (int64_t)num, dv = (int32_t)v;
                int64_t q = n / dv, rm = n % dv;
                if (q > 0x7FFFFFFFLL || q < -0x80000000LL) { fault(0); return; }
                C->r[EAX] = (uint32_t)q; C->r[EDX] = (uint32_t)rm;
            } else {
                uint64_t q = num / v, rm = num % v;
                if (q > 0xFFFFFFFFull) { fault(0); return; }
                C->r[EAX] = (uint32_t)q; C->r[EDX] = (uint32_t)rm;
            }
        }
        return; }
    }
}

static uint32_t imul3(uint32_t a, uint32_t b, int sz) {
    d_cyc += 12;
    int64_t t;
    bool ovf;
    uint32_t r;
    if (sz == 16) { t = (int64_t)(int16_t)a * (int16_t)b; r = (uint32_t)t & 0xFFFF; ovf = t != (int16_t)t; }
    else { t = (int64_t)(int32_t)a * (int32_t)b; r = (uint32_t)t; ovf = t != (int32_t)t; }
    uint32_t f = C->fl & ARITH & ~(FL_CF | FL_OF);
    if (ovf) f |= FL_CF | FL_OF;
    C->fl = (C->fl & ~ARITH) | f;
    return r;
}

static void load_far(int sreg, int sz) {   // LES/LDS/LSS/LFS/LGS
    modrm();
    if (d_isreg) { fault(6); return; }
    uint32_t off = rdm(sz, d_ea);
    uint16_t seg = mem_rw(M, d_ea + sz / 8);
    setr(sz, d_reg, off);
    C->sr[sreg] = seg;
    if (sreg == SS_) C->inhibit_irq = 1;
}

static void undefined(uint8_t op, uint8_t op2) {
    C->undef_count++;
    if (C->undef_count < 20)
        fprintf(stderr, "[cpu] 未定義命令 %02X %02X at %04X:%04X\n", op, op2, C->op_cs, C->op_ip);
    fault(6);
}

// ---- 0F xx ------------------------------------------------------------------
static void op0f() {
    uint8_t op = f8();
    s_op0f = op;
    int sz = d_o32 ? 32 : 16;
    if (op >= 0x80 && op <= 0x8F) {
        int32_t disp = d_o32 ? (int32_t)f32() : (int16_t)f16();
        if (cond(op & 15)) { C->ip = (uint16_t)(C->ip + disp); d_cyc += 4; }
        return;
    }
    if (op >= 0x90 && op <= 0x9F) { modrm(); rmw(8, cond(op & 15) ? 1 : 0); return; }
    if (op >= 0xC8 && op <= 0xCF) {
        uint32_t v = C->r[op & 7];
        C->r[op & 7] = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
        return;
    }
    switch (op) {
    case 0x01: {
        modrm();
        switch (d_reg) {
        case 4: rmw(16, C->cr0 & 0xFFFF); return;                       // SMSW
        case 6: { uint16_t v = (uint16_t)rmr(16);                         // LMSW
                  if (v & 1) fprintf(stderr, "[cpu] 保護モードへの移行は未対応です (LMSW)\n");
                  C->cr0 = (C->cr0 & ~0xEu) | (v & 0xE); return; }
        default: return;   // LGDT/LIDT/SGDT/SIDT: 記録不要
        } }
    case 0x06: C->cr0 &= ~8u; return;                                     // CLTS
    case 0x20: { modrm(); C->r[d_rm] = d_reg == 0 ? C->cr0 : 0; return; }   // MOV r32,CRn
    case 0x22: { modrm();
        if (d_reg == 0) {
            if (C->r[d_rm] & 1) fprintf(stderr, "[cpu] 保護モードへの移行は未対応です (MOV CR0)\n");
            C->cr0 = (C->r[d_rm] & ~1u) | 0x10;
        }
        return; }
    case 0x21: case 0x23: modrm(); if (op == 0x21) C->r[d_rm] = 0; return;   // DRn
    case 0xA0: pushv(C->sr[FS_]); return;
    case 0xA1: C->sr[FS_] = (uint16_t)popv(); return;
    case 0xA8: pushv(C->sr[GS_]); return;
    case 0xA9: C->sr[GS_] = (uint16_t)popv(); return;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {                          // BT/BTS/BTR/BTC r
        modrm();
        int32_t bit = (int32_t)getr(sz, d_reg);
        uint32_t v;
        uint32_t addr = 0;
        if (d_isreg) { bit &= sz - 1; v = getr(sz, d_rm); }
        else {
            int32_t b2 = sz == 16 ? (int16_t)bit : bit;
            addr = d_ea + (uint32_t)((b2 >> (sz == 16 ? 4 : 5)) * (sz / 8));
            bit = b2 & (sz - 1);
            v = rdm(sz, addr);
        }
        if ((v >> bit) & 1) C->fl |= FL_CF; else C->fl &= ~FL_CF;
        uint32_t nv = v;
        if (op == 0xAB) nv |= 1u << bit;
        else if (op == 0xB3) nv &= ~(1u << bit);
        else if (op == 0xBB) nv ^= 1u << bit;
        if (op != 0xA3) { if (d_isreg) setr(sz, d_rm, nv); else wrm(sz, addr, nv); }
        return; }
    case 0xBA: {                                                             // BT 系 imm8
        modrm();
        int bit = f8() & (sz - 1);
        uint32_t v = rmr(sz);
        if ((v >> bit) & 1) C->fl |= FL_CF; else C->fl &= ~FL_CF;
        if (d_reg == 5) rmw(sz, v | (1u << bit));
        else if (d_reg == 6) rmw(sz, v & ~(1u << bit));
        else if (d_reg == 7) rmw(sz, v ^ (1u << bit));
        return; }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {                             // SHLD/SHRD
        modrm();
        int cnt = ((op & 1) ? (int)(C->r[ECX] & 0xFF) : f8()) & 31;
        if (!cnt) return;
        uint32_t dst = rmr(sz), src = getr(sz, d_reg);
        uint32_t r, cf;
        if (cnt > sz) cnt = sz;
        if (op < 0xA8) {  // SHLD
            r = (uint32_t)((((uint64_t)dst << cnt) | ((uint64_t)src >> (sz - cnt))) & MASK(sz));
            cf = (dst >> (sz - cnt)) & 1;
        } else {          // SHRD
            r = (uint32_t)((((uint64_t)dst >> cnt) | ((uint64_t)src << (sz - cnt))) & MASK(sz));
            cf = (dst >> (cnt - 1)) & 1;
        }
        rmw(sz, (uint32_t)r);
        uint32_t f = szp((uint32_t)r, sz) | (cf ? FL_CF : 0);
        if (((r ^ dst) & SIGN(sz))) f |= FL_OF;
        C->fl = (C->fl & ~ARITH) | f;
        return; }
    case 0xAF: { modrm(); setr(sz, d_reg, imul3(getr(sz, d_reg), rmr(sz), sz)); return; }
    case 0xB0: case 0xB1: {                                                  // CMPXCHG
        int s2 = op == 0xB0 ? 8 : sz;
        modrm();
        uint32_t v = rmr(s2);
        alu(7, getr(s2, EAX), v, s2);
        if (C->fl & FL_ZF) rmw(s2, getr(s2, d_reg)); else setr(s2, EAX, v);
        return; }
    case 0xC0: case 0xC1: {                                                  // XADD
        int s2 = op == 0xC0 ? 8 : sz;
        modrm();
        uint32_t v = rmr(s2), rg = getr(s2, d_reg);
        uint32_t r = alu(0, v, rg, s2);
        setr(s2, d_reg, v); rmw(s2, r);
        return; }
    case 0xB2: load_far(SS_, sz); return;
    case 0xB4: load_far(FS_, sz); return;
    case 0xB5: load_far(GS_, sz); return;
    case 0xB6: modrm(); setr(sz, d_reg, rmr(8)); return;
    case 0xB7: modrm(); setr(sz, d_reg, rmr(16)); return;
    case 0xBE: modrm(); setr(sz, d_reg, (uint32_t)(int32_t)(int8_t)rmr(8)); return;
    case 0xBF: modrm(); setr(sz, d_reg, (uint32_t)(int32_t)(int16_t)rmr(16)); return;
    case 0xBC: case 0xBD: {
        modrm();
        uint32_t v = rmr(sz);
        if (!v) { C->fl |= FL_ZF; return; }
        C->fl &= ~FL_ZF;
        int i;
        if (op == 0xBC) { for (i = 0; !((v >> i) & 1); i++); }
        else { for (i = sz - 1; !((v >> i) & 1); i--); }
        setr(sz, d_reg, (uint32_t)i);
        return; }
    case 0x08: case 0x09: return;   // INVD/WBINVD
    }
    undefined(0x0F, op);
}

// ---- 1 命令 ------------------------------------------------------------------
static uint16_t s_tr_cs, s_tr_ip; static int s_tr_on;
// デバッグ用: 直近に実行した命令の位置（ITRACE_STOP の CS に入ったら一度だけ出力）
uint32_t g_itr[256]; unsigned g_itr_pos; int g_itr_stop_cs = -1;
static void itr_dump() {
    fprintf(stderr, "[itrace] last instructions:\n");
    for (unsigned i = 0; i < 256; i++) { uint32_t v = g_itr[(g_itr_pos + i) & 255]; fprintf(stderr, " %04X:%04X", v >> 16, v & 0xFFFF); if ((i & 7) == 7) fprintf(stderr, "\n"); }
}
static void step() {
    if (g_itr_stop_cs >= 0) {
        if (C->sr[CS_] == (uint16_t)g_itr_stop_cs) { itr_dump(); g_itr_stop_cs = -1; }
        else g_itr[g_itr_pos++ & 255] = ((uint32_t)C->sr[CS_] << 16) | C->ip;
    }
    if (s_tr_on && C->ip == s_tr_ip && C->sr[CS_] == s_tr_cs) { s_tr_on = 0; fprintf(stderr, "   -> AX=%04X BX=%04X CX=%04X DX=%04X FL=%04X\n", C->r[EAX] & 0xFFFF, C->r[EBX] & 0xFFFF, C->r[ECX] & 0xFFFF, C->r[EDX] & 0xFFFF, C->fl & 0xFFFF); }
    C->op_ip = C->ip; C->op_cs = C->sr[CS_];
    if (g_prof) { if (g_prof_cs == -2) g_prof[C->sr[CS_]]++; else if (C->sr[CS_] == g_prof_cs) g_prof[C->ip]++; }
    d_seg = -1; d_o32 = false; d_a32 = false; d_rep = 0;
    d_cyc = 2;
    uint8_t op;
    for (;;) {
        op = f8();
        switch (op) {
        case 0x26: d_seg = ES_; continue;
        case 0x2E: d_seg = CS_; continue;
        case 0x36: d_seg = SS_; continue;
        case 0x3E: d_seg = DS_; continue;
        case 0x64: d_seg = FS_; continue;
        case 0x65: d_seg = GS_; continue;
        case 0x66: d_o32 = true; continue;
        case 0x67: d_a32 = true; continue;
        case 0xF0: continue;
        case 0xF2: case 0xF3: d_rep = op; continue;
        }
        break;
    }
    int vsz = d_o32 ? 32 : 16;
    s_op = op; s_vsz = vsz;

    // ALU 0x00-0x3F の規則的な部分
    if (op < 0x40 && (op & 7) < 6) {
        int aop = op >> 3;
        int sz = (op & 1) ? vsz : 8;
        switch (op & 7) {
        case 0: case 1: { modrm(); uint32_t r = alu(aop, rmr(sz), getr(sz, d_reg), sz); if (aop != 7) rmw(sz, r); return; }
        case 2: case 3: { modrm(); uint32_t r = alu(aop, getr(sz, d_reg), rmr(sz), sz); if (aop != 7) setr(sz, d_reg, r); return; }
        case 4: case 5: { uint32_t i = fimm(sz); uint32_t r = alu(aop, getr(sz, EAX), i, sz); if (aop != 7) setr(sz, EAX, r); return; }
        }
    }

    switch (op) {
    case 0x06: pushv(C->sr[ES_]); return;
    case 0x07: C->sr[ES_] = (uint16_t)popv(); return;
    case 0x0E: pushv(C->sr[CS_]); return;
    case 0x0F: op0f(); return;
    case 0x16: pushv(C->sr[SS_]); return;
    case 0x17: C->sr[SS_] = (uint16_t)popv(); C->inhibit_irq = 1; return;
    case 0x1E: pushv(C->sr[DS_]); return;
    case 0x1F: C->sr[DS_] = (uint16_t)popv(); return;
    case 0x27: case 0x2F: {   // DAA / DAS
        uint8_t al = (uint8_t)C->r[EAX], old = al;
        bool cf = C->fl & FL_CF, af = C->fl & FL_AF;
        uint32_t f = 0;
        if ((al & 0x0F) > 9 || af) {
            al = op == 0x27 ? (uint8_t)(al + 6) : (uint8_t)(al - 6);
            f |= FL_AF;
        }
        if (old > 0x99 || cf) { al = op == 0x27 ? (uint8_t)(al + 0x60) : (uint8_t)(al - 0x60); f |= FL_CF; }
        setr(8, 0, al);
        C->fl = (C->fl & ~ARITH) | f | szp(al, 8);
        return; }
    case 0x37: case 0x3F: {   // AAA / AAS
        uint32_t f = C->fl & ~(FL_AF | FL_CF);
        if (((C->r[EAX] & 0x0F) > 9) || (C->fl & FL_AF)) {
            uint16_t ax = (uint16_t)C->r[EAX];
            if (op == 0x37) { ax = (uint16_t)(ax + 0x106); } else { ax = (uint16_t)(ax - 6); ax = (uint16_t)(ax - 0x100); }
            setr(16, EAX, ax);
            f |= FL_AF | FL_CF;
        }
        C->fl = f;
        setr(8, 0, C->r[EAX] & 0x0F);
        return; }
    }
    if (op >= 0x40 && op <= 0x4F) { int i = op & 7; setr(vsz, i, incdec(getr(vsz, i), vsz, op >= 0x48)); return; }
    if (op >= 0x50 && op <= 0x57) {
        uint32_t v = getr(vsz, op & 7);
        pushv(v); return;
    }
    if (op >= 0x58 && op <= 0x5F) { uint32_t v = popv(); setr(vsz, op & 7, v); return; }
    if (op >= 0x70 && op <= 0x7F) { int8_t d = (int8_t)f8(); if (cond(op & 15)) { C->ip = (uint16_t)(C->ip + d); d_cyc += 4; } return; }
    if (op >= 0x91 && op <= 0x97) { uint32_t a = getr(vsz, EAX); setr(vsz, EAX, getr(vsz, op & 7)); setr(vsz, op & 7, a); return; }
    if (op >= 0xB0 && op <= 0xB7) { setr(8, op & 7, f8()); return; }
    if (op >= 0xB8 && op <= 0xBF) { setr(vsz, op & 7, fimm(vsz)); return; }
    if (op >= 0xD8 && op <= 0xDF) { modrm(); return; }   // FPU なし

    switch (op) {
    case 0x60: {  // PUSHA
        uint32_t sp = getr(vsz, ESP);
        for (int i = 0; i < 8; i++) pushv(i == ESP ? sp : getr(vsz, i));
        d_cyc += 16; return; }
    case 0x61: {  // POPA
        for (int i = 7; i >= 0; i--) { uint32_t v = popv(); if (i != ESP) setr(vsz, i, v); }
        d_cyc += 16; return; }
    case 0x62: {  // BOUND
        modrm();
        int32_t idx = vsz == 16 ? (int16_t)getr(16, d_reg) : (int32_t)C->r[d_reg];
        int32_t lo = vsz == 16 ? (int16_t)rdm(16, d_ea) : (int32_t)rdm(32, d_ea);
        int32_t hi = vsz == 16 ? (int16_t)rdm(16, d_ea + 2) : (int32_t)rdm(32, d_ea + 4);
        if (idx < lo || idx > hi) fault(5);
        return; }
    case 0x68: pushv(fimm(vsz)); return;
    case 0x6A: pushv((uint32_t)(int32_t)(int8_t)f8()); return;
    case 0x69: { modrm(); uint32_t i = fimm(vsz); setr(vsz, d_reg, imul3(rmr(vsz), i, vsz)); return; }
    case 0x6B: { modrm(); uint32_t i = (uint32_t)(int32_t)(int8_t)f8(); setr(vsz, d_reg, imul3(rmr(vsz), i, vsz)); return; }
    case 0x6C: string_op(op, 8); return;
    case 0x6D: string_op(op, vsz); return;
    case 0x6E: string_op(op, 8); return;
    case 0x6F: string_op(op, vsz); return;
    case 0x80: case 0x82: { modrm(); uint32_t i = f8(); uint32_t r = alu(d_reg, rmr(8), i, 8); if (d_reg != 7) rmw(8, r); return; }
    case 0x81: { modrm(); uint32_t i = fimm(vsz); uint32_t r = alu(d_reg, rmr(vsz), i, vsz); if (d_reg != 7) rmw(vsz, r); return; }
    case 0x83: { modrm(); uint32_t i = (uint32_t)(int32_t)(int8_t)f8(); uint32_t r = alu(d_reg, rmr(vsz), i, vsz); if (d_reg != 7) rmw(vsz, r); return; }
    case 0x84: { modrm(); alu(4, rmr(8), getr(8, d_reg), 8); return; }
    case 0x85: { modrm(); alu(4, rmr(vsz), getr(vsz, d_reg), vsz); return; }
    case 0x86: { modrm(); uint32_t a = rmr(8); rmw(8, getr(8, d_reg)); setr(8, d_reg, a); return; }
    case 0x87: { modrm(); uint32_t a = rmr(vsz); rmw(vsz, getr(vsz, d_reg)); setr(vsz, d_reg, a); return; }
    case 0x88: modrm(); rmw(8, getr(8, d_reg)); return;
    case 0x89: modrm(); rmw(vsz, getr(vsz, d_reg)); return;
    case 0x8A: modrm(); setr(8, d_reg, rmr(8)); return;
    case 0x8B: modrm(); setr(vsz, d_reg, rmr(vsz)); return;
    case 0x8C: modrm(); if (d_isreg) setr(vsz, d_rm, C->sr[d_reg % 6]); else wrm(16, d_ea, C->sr[d_reg % 6]); return;
    case 0x8D: modrm(); setr(vsz, d_reg, d_a32 ? d_off : (d_off & 0xFFFF)); return;
    case 0x8E: { modrm(); int s = d_reg % 6; if (s == CS_) { undefined(op, 0x8E); return; }
                 C->sr[s] = (uint16_t)rmr(16); if (s == SS_) C->inhibit_irq = 1; return; }
    case 0x8F: { // POP rm: ESP を先に進めてからアドレス計算する（実機どおり）
        uint32_t v = popv();
        modrm(); rmw(vsz, v); return; }
    case 0x90: return;
    case 0x98: if (d_o32) C->r[EAX] = (uint32_t)(int32_t)(int16_t)C->r[EAX]; else setr(16, EAX, (uint32_t)(int16_t)(int8_t)C->r[EAX]); return;
    case 0x99: if (d_o32) C->r[EDX] = (C->r[EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0; else setr(16, EDX, (C->r[EAX] & 0x8000) ? 0xFFFF : 0); return;
    case 0x9A: {
        uint32_t off = fimm(vsz); uint16_t seg = f16();
        pushv(C->sr[CS_]); pushv(C->ip);
        C->sr[CS_] = seg; C->ip = (uint16_t)off; d_cyc += 10; return; }
    case 0x9B: return;
    case 0x9C: if (d_o32) push32(C->fl & 0x00FCFFFFu); else push16((uint16_t)C->fl); return;
    case 0x9D: { uint32_t v = popv(); set_flags_word(v, d_o32); return; }
    case 0x9E: C->fl = (C->fl & ~0xD5u) | ((C->r[EAX] >> 8) & 0xD5); return;
    case 0x9F: setr(8, 4, (C->fl & 0xD5) | 0x02); return;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        uint32_t off = d_a32 ? f32() : f16();
        uint32_t a = sbase(d_seg >= 0 ? d_seg : DS_) + off;
        s_moffs = a;
        int sz = (op & 1) ? vsz : 8;
        if (op < 0xA2) setr(sz, EAX, rdm(sz, a)); else wrm(sz, a, getr(sz, EAX));
        return; }
    case 0xA4: case 0xA6: case 0xAA: case 0xAC: case 0xAE: string_op(op, 8); return;
    case 0xA5: case 0xA7: case 0xAB: case 0xAD: case 0xAF: string_op(op, vsz); return;
    case 0xA8: alu(4, C->r[EAX], f8(), 8); return;
    case 0xA9: alu(4, C->r[EAX], fimm(vsz), vsz); return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int sz = (op & 1) ? vsz : 8;
        modrm();
        int cnt = (op <= 0xC1) ? f8() : (op <= 0xD1 ? 1 : (int)(C->r[ECX] & 0xFF));
        uint32_t v = rmr(sz);
        uint32_t r = shift(d_reg, v, cnt, sz);
        if (cnt & 0x1F) rmw(sz, r);
        return; }
    case 0xC2: { uint16_t n = f16(); C->ip = (uint16_t)popv(); C->r[ESP] = (C->r[ESP] & 0xFFFF0000u) | (uint16_t)(C->r[ESP] + n); d_cyc += 8; return; }
    case 0xC3: C->ip = (uint16_t)popv(); d_cyc += 8; return;
    case 0xC4: load_far(ES_, vsz); return;
    case 0xC5: load_far(DS_, vsz); return;
    case 0xC6: modrm(); rmw(8, f8()); return;
    case 0xC7: modrm(); rmw(vsz, fimm(vsz)); return;
    case 0xC8: {  // ENTER
        uint16_t size = f16(); uint8_t lvl = f8() & 31;
        uint32_t fp;
        pushv(getr(vsz, EBP));
        fp = C->r[ESP] & 0xFFFF;
        if (lvl) {
            for (int i = 1; i < lvl; i++) {
                uint16_t bp = (uint16_t)(C->r[EBP] - (vsz / 8) * i);
                pushv(rdm(vsz, sbase(SS_) + bp));
            }
            pushv(fp);
        }
        setr(vsz, EBP, fp);
        C->r[ESP] = (C->r[ESP] & 0xFFFF0000u) | (uint16_t)(C->r[ESP] - size);
        d_cyc += 10; return; }
    case 0xC9: C->r[ESP] = (C->r[ESP] & 0xFFFF0000u) | (C->r[EBP] & 0xFFFF); setr(vsz, EBP, popv()); return;
    case 0xCA: { uint16_t n = f16(); C->ip = (uint16_t)popv(); C->sr[CS_] = (uint16_t)popv();
                 C->r[ESP] = (C->r[ESP] & 0xFFFF0000u) | (uint16_t)(C->r[ESP] + n); d_cyc += 12; return; }
    case 0xCB: C->ip = (uint16_t)popv(); C->sr[CS_] = (uint16_t)popv(); d_cyc += 12; return;
    case 0xCC: do_int(3); return;
    case 0xCD: { uint8_t v = f8();
        if (v == g_trace_int) { s_tr_cs = C->sr[CS_]; s_tr_ip = C->ip; s_tr_on = 1; }
        if (v == g_trace_int) fprintf(stderr, "[f%llu int %02X] AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X from %04X:%04X\n", (unsigned long long)g_frame_dbg, v, C->r[EAX] & 0xFFFF, C->r[EBX] & 0xFFFF, C->r[ECX] & 0xFFFF, C->r[EDX] & 0xFFFF, C->sr[DS_], C->sr[ES_], C->op_cs, C->op_ip);
        do_int(v); return; }
    case 0xCE: if (C->fl & FL_OF) do_int(4); return;
    case 0xCF: {
        if (d_o32) { C->ip = (uint16_t)pop32(); C->sr[CS_] = (uint16_t)pop32(); set_flags_word(pop32(), true); }
        else { C->ip = pop16(); C->sr[CS_] = pop16(); set_flags_word(pop16(), false); }
        d_cyc += 20; return; }
    case 0xD4: {  // AAM
        uint8_t base = f8();
        if (!base) { fault(0); return; }
        uint8_t al = (uint8_t)C->r[EAX];
        setr(8, 4, al / base); setr(8, 0, al % base);
        C->fl = (C->fl & ~ARITH) | szp(al % base, 8);
        return; }
    case 0xD5: {  // AAD
        uint8_t base = f8();
        uint8_t al = (uint8_t)(((C->r[EAX] >> 8) & 0xFF) * base + (C->r[EAX] & 0xFF));
        setr(16, EAX, al);
        C->fl = (C->fl & ~ARITH) | szp(al, 8);
        return; }
    case 0xD6: setr(8, 0, (C->fl & FL_CF) ? 0xFF : 0); return;   // SALC
    case 0xD7: {
        uint32_t off = d_a32 ? C->r[EBX] + (C->r[EAX] & 0xFF) : (uint16_t)((C->r[EBX] & 0xFFFF) + (C->r[EAX] & 0xFF));
        setr(8, 0, mem_rb(M, sbase(d_seg >= 0 ? d_seg : DS_) + off)); return; }
    case 0xE0: case 0xE1: case 0xE2: {
        int8_t d = (int8_t)f8();
        uint32_t n = cx_() - 1; setcx(n);
        bool go = n != 0;
        if (op == 0xE0) go = go && !(C->fl & FL_ZF);
        if (op == 0xE1) go = go && (C->fl & FL_ZF);
        if (go) { C->ip = (uint16_t)(C->ip + d); d_cyc += 4; }
        return; }
    case 0xE3: { int8_t d = (int8_t)f8(); if (cx_() == 0) C->ip = (uint16_t)(C->ip + d); return; }
    case 0xE4: setr(8, 0, io_in8(M, f8())); d_cyc += 8; return;
    case 0xE5: { uint8_t p = f8(); setr(vsz, EAX, vsz == 16 ? io_in16(M, p) : (io_in16(M, p) | ((uint32_t)io_in16(M, p + 2) << 16))); d_cyc += 8; return; }
    case 0xE6: io_out8(M, f8(), (uint8_t)C->r[EAX]); d_cyc += 8; return;
    case 0xE7: { uint8_t p = f8(); io_out16(M, p, (uint16_t)C->r[EAX]); if (d_o32) io_out16(M, p + 2, (uint16_t)(C->r[EAX] >> 16)); d_cyc += 8; return; }
    case 0xE8: { int32_t d = d_o32 ? (int32_t)f32() : (int16_t)f16(); pushv(C->ip); C->ip = (uint16_t)(C->ip + d); d_cyc += 6; return; }
    case 0xE9: { int32_t d = d_o32 ? (int32_t)f32() : (int16_t)f16(); C->ip = (uint16_t)(C->ip + d); d_cyc += 6; return; }
    case 0xEA: { uint32_t off = fimm(vsz); uint16_t seg = f16(); C->ip = (uint16_t)off; C->sr[CS_] = seg; d_cyc += 8; return; }
    case 0xEB: { int8_t d = (int8_t)f8(); C->ip = (uint16_t)(C->ip + d); d_cyc += 4; return; }
    case 0xEC: setr(8, 0, io_in8(M, (uint16_t)C->r[EDX])); d_cyc += 8; return;
    case 0xED: { uint16_t p = (uint16_t)C->r[EDX]; setr(vsz, EAX, vsz == 16 ? io_in16(M, p) : (io_in16(M, p) | ((uint32_t)io_in16(M, p + 2) << 16))); d_cyc += 8; return; }
    case 0xEE: io_out8(M, (uint16_t)C->r[EDX], (uint8_t)C->r[EAX]); d_cyc += 8; return;
    case 0xEF: { uint16_t p = (uint16_t)C->r[EDX]; io_out16(M, p, (uint16_t)C->r[EAX]); if (d_o32) io_out16(M, p + 2, (uint16_t)(C->r[EAX] >> 16)); d_cyc += 8; return; }
    case 0xF1: { uint8_t n = f8(); hle_trap(M, n); d_cyc += 20; return; }
    case 0xF4: C->halted = 1; return;
    case 0xF5: C->fl ^= FL_CF; return;
    case 0xF6: modrm(); grp3(8); return;
    case 0xF7: modrm(); grp3(vsz); return;
    case 0xF8: C->fl &= ~FL_CF; return;
    case 0xF9: C->fl |= FL_CF; return;
    case 0xFA: C->fl &= ~FL_IF; return;
    case 0xFB: if (!(C->fl & FL_IF)) C->inhibit_irq = 1; C->fl |= FL_IF; return;
    case 0xFC: C->fl &= ~FL_DF; return;
    case 0xFD: C->fl |= FL_DF; return;
    case 0xFE: {
        modrm();
        if (d_reg == 0 || d_reg == 1) { rmw(8, incdec(rmr(8), 8, d_reg == 1)); return; }
        undefined(op, d_reg); return; }
    case 0xFF: {
        modrm();
        switch (d_reg) {
        case 0: case 1: rmw(vsz, incdec(rmr(vsz), vsz, d_reg == 1)); return;
        case 2: { uint32_t t = rmr(vsz); pushv(C->ip); C->ip = (uint16_t)t; d_cyc += 6; return; }
        case 3: { if (d_isreg) { undefined(op, 3); return; }
                  uint32_t off = rdm(vsz, d_ea); uint16_t seg = mem_rw(M, d_ea + vsz / 8);
                  pushv(C->sr[CS_]); pushv(C->ip); C->sr[CS_] = seg; C->ip = (uint16_t)off; d_cyc += 12; return; }
        case 4: C->ip = (uint16_t)rmr(vsz); d_cyc += 6; return;
        case 5: { if (d_isreg) { undefined(op, 5); return; }
                  uint32_t off = rdm(vsz, d_ea); uint16_t seg = mem_rw(M, d_ea + vsz / 8);
                  C->sr[CS_] = seg; C->ip = (uint16_t)off; d_cyc += 10; return; }
        case 6: pushv(rmr(vsz)); return;
        }
        undefined(op, 7); return; }
    }
    undefined(op, 0);
}

// ---- 公開 --------------------------------------------------------------------
void cpu_reset(Cpu* c, Machine* m) {
    for (int i = 0; i < 256; i++) {
        int p = 0; for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        s_parity[i] = p ? 0 : FL_PF;
    }
    memset(c, 0, sizeof(*c));
    c->m = m;
    c->fl = 0x0002;
    c->cr0 = 0x10;
    c->sr[CS_] = 0xF000; c->ip = 0xFFF0;
}

// ---- 空回りの見分け ------------------------------------------------------------
//  「割込みが来るまで同じところを回るだけ」のループ（タイマ割込みで増えるカウンタを待つ など）を見つけたら、
//  HLT と同じく割込みが来るまで命令を実行しない（早送り・CPU 負荷の軽減）。
//  短い後ろ向きの分岐で同じ場所に戻ってきたとき、前回からレジスタ・フラグ・セグメントが同じで、
//  その間にメモリへの書き込み・I/O・HLE・RAM 以外の読み出しが一度も無ければ、次の周回も必ず同じになる
//  （変わりうるのは割込みだけ）ので、止めても結果は変わらない。I/O を読むループ（VSYNC 待ちなど）は対象外。
int g_idle_skip = 1;
unsigned g_spin_hits = 0;
struct SpinSlot {
    bool valid; uint16_t cs, ip; uint32_t fx;
    uint32_t r[8]; uint16_t sr[6]; uint32_t fl;
    uint16_t stk[8];   // スタックの先頭（戻り先）。積み下ろしは副作用に数えないので、ここで見分ける
};
// スタックの先頭 8 ワード（副作用の無い読み出し。メインメモリの外は 0 とみなす）
static inline uint16_t spin_stk(int i) {
    uint32_t a = ((uint32_t)C->sr[SS_] << 4) + (uint16_t)(C->r[ESP] + i * 2);
    return a + 1 < 0xA0000u ? (uint16_t)(g_ram[a] | (g_ram[a + 1] << 8)) : 0;
}
static SpinSlot s_spin[8];   // ループの戻り先ごと（入れ子のループでも外側を見分けられるように）
static inline bool spin_same(const SpinSlot& s) {
    if (s.fl != C->fl) return false;
    for (int i = 0; i < 8; i++) if (s.r[i] != C->r[i]) return false;
    for (int i = 0; i < 6; i++) if (s.sr[i] != C->sr[i]) return false;
    // 同じ関数を別の場所から同じ引数で 2 回呼んだ（32 ビット除算の商と余り など）だけのときは、
    // レジスタが同じでも戻り先が違う。スタックの先頭まで同じときだけ空回りとみなす
    for (int i = 0; i < 8; i++) if (s.stk[i] != spin_stk(i)) return false;
    return true;
}
static inline void spin_record(SpinSlot& s) {
    s.valid = true; s.cs = C->sr[CS_]; s.ip = C->ip; s.fx = g_side_fx;
    for (int i = 0; i < 8; i++) s.r[i] = C->r[i];
    for (int i = 0; i < 6; i++) s.sr[i] = C->sr[i];
    s.fl = C->fl;
    for (int i = 0; i < 8; i++) s.stk[i] = spin_stk(i);
}
static inline void spin_clear() { for (auto& s : s_spin) s.valid = false; }

// ---- ディスクの読み込み結果を調べる比較の記録 -----------------------------------
//  INT 1Bh の後しばらく、①読んだバッファ（とそこから MOVS で写した先）の中身、②そこから読み込んだレジスタ、
//  ③INT 1Bh の結果（AH）や READ ID の C/H/R/N を「どの命令で何と比べたか」を PC98PLAYER.LOG に残す。
//  プロテクトの確認（読んだセクタの内容や結果コードを決まった値と比べる）を見つけるため。
struct DwRange { uint32_t start, len, base_off; };   // base_off: 元のバッファの何バイト目にあたるか
struct DwTaint { bool on; uint8_t val; char label[40]; };
static struct {
    bool active = false;
    int64_t left = 0;             // 残りの命令数
    int lines = 0;                // このディスク読み込みで書いた行数
    std::vector<DwRange> ranges;
    DwTaint t[8];                 // 8 ビットレジスタ AL,CL,DL,BL,AH,CH,DH,BH
} s_dw;
static int s_dw_total = 0;
static const int DW_INSNS = 2000000, DW_LINES = 64, DW_TOTAL = 4000;

void cpu_dw_begin(uint32_t buf, uint32_t len, const char* what) {
    if (s_dw_total >= DW_TOTAL) return;
    s_dw.active = true; s_dw.left = DW_INSNS; s_dw.lines = 0;
    s_dw.ranges.clear();
    for (auto& t : s_dw.t) t.on = false;
    if (len) s_dw.ranges.push_back({buf, len, 0});
    (void)what;
}
void cpu_dw_taint8(int r8, uint8_t val, const char* label) {
    if (r8 < 0 || r8 > 7) return;
    DwTaint& t = s_dw.t[r8];
    t.on = true; t.val = val; snprintf(t.label, sizeof(t.label), "%s", label);
}
static int dw_find(uint32_t a, uint32_t* off) {
    for (size_t i = 0; i < s_dw.ranges.size(); i++) {
        const DwRange& r = s_dw.ranges[i];
        if (a >= r.start && a < r.start + r.len) { if (off) *off = r.base_off + (a - r.start); return (int)i; }
    }
    return -1;
}
static const char* r8name(int r) { static const char* n[] = {"AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH"}; return n[r & 7]; }
static const char* r16name(int r) { static const char* n[] = {"AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"}; return n[r & 7]; }
static const char* r32name(int r) { static const char* n[] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"}; return n[r & 7]; }
// 8 ビットレジスタ番号 → 32 ビットレジスタの何バイト目か
static inline int r8_of(int reg, int byte) { return byte == 0 ? reg : (reg < 4 ? reg + 4 : -1); }
// レジスタの値の出どころ（印が付いていて、値が今も同じなら説明を返す）
static bool dw_reg_src(int sz, int reg, uint32_t v, char* out, size_t n) {
    if (sz == 8) {
        const DwTaint& t = s_dw.t[reg & 7];
        if (t.on && t.val == (uint8_t)v) { snprintf(out, n, "%s", t.label); return true; }
        return false;
    }
    if (reg >= 4) return false;
    const DwTaint& lo = s_dw.t[reg], &hi = s_dw.t[reg + 4];
    bool l = lo.on && lo.val == (uint8_t)v, h = hi.on && hi.val == (uint8_t)(v >> 8);
    if (l && h) snprintf(out, n, "%s・%s", lo.label, hi.label);
    else if (l) snprintf(out, n, "下位が %s", lo.label);
    else if (h) snprintf(out, n, "上位が %s", hi.label);
    else return false;
    return true;
}
static void dw_set_reg(int sz, int reg, uint32_t lin_src, bool from_mem) {
    // MOV などで reg に値が入った: 見張っているメモリからなら印を付け、そうでなければ消す
    int bytes = sz / 8; if (bytes > 2) bytes = 2;
    for (int b = 0; b < bytes; b++) {
        int r8 = sz == 8 ? (reg & 7) : r8_of(reg, b);
        if (r8 < 0) continue;
        uint32_t off;
        DwTaint& t = s_dw.t[r8];
        if (from_mem && dw_find(lin_src + (uint32_t)b, &off) >= 0) {
            t.on = true; t.val = mem_rb(M, lin_src + (uint32_t)b);
            snprintf(t.label, sizeof(t.label), "読んだデータ+%03Xh", off);
        } else t.on = false;
    }
}
static void dw_copy_reg(int sz, int dst, int src) {
    if (sz == 8) { s_dw.t[dst & 7] = s_dw.t[src & 7]; return; }
    for (int b = 0; b < 2; b++) {
        int d = r8_of(dst, b), s2 = r8_of(src, b);
        if (d < 0) continue;
        if (s2 < 0) s_dw.t[d].on = false; else s_dw.t[d] = s_dw.t[s2];
    }
}
static void dw_line(const char* kind, const std::string& a, const std::string& b, int sz) {
    uint32_t va = s_alu_a, vb = s_alu_b;
    const char* res;
    if (!strcmp(kind, "TEST")) res = (va & vb) ? "どれかのビットが立っている（非 0）" : "共通のビットなし（0）";
    else res = va == vb ? "等しい" : va < vb ? "小さい（符号なし）" : "大きい（符号なし）";
    plog("[fdchk] %04X:%04X %s %s と %s（%d ビット）→ %s\n", C->op_cs, C->op_ip, kind, a.c_str(), b.c_str(), sz, res);
    s_dw.lines++; s_dw_total++;
    if (s_dw.lines >= DW_LINES || s_dw_total >= DW_TOTAL) {
        plog("[fdchk] （記録はここまで: %s）\n", s_dw_total >= DW_TOTAL ? "全体の上限" : "この読み込みについての上限");
        s_dw.active = false;
    }
}
// 比べた値の 1 つを言葉にする（rm のメモリ／レジスタ）
static bool dw_desc_rm(int sz, uint32_t v, std::string* out) {
    char b[160], src[64];
    unsigned mask = sz == 8 ? 0xFF : sz == 16 ? 0xFFFF : 0xFFFFFFFFu;
    if (!d_isreg) {
        uint32_t off;
        bool hit = dw_find(d_ea, &off) >= 0;
        snprintf(b, sizeof(b), "[%05X]=%0*Xh%s", d_ea & 0xFFFFF, sz / 4, v & mask, "");
        *out = b;
        if (hit) { snprintf(b, sizeof(b), "（読んだデータ+%03Xh）", off); *out += b; }
        return hit;
    }
    const char* nm = sz == 8 ? r8name(d_rm) : sz == 16 ? r16name(d_rm) : r32name(d_rm);
    bool hit = dw_reg_src(sz, d_rm, v, src, sizeof(src));
    snprintf(b, sizeof(b), "%s=%0*Xh", nm, sz / 4, v & mask);
    *out = b;
    if (hit) { *out += "（"; *out += src; *out += "）"; }
    return hit;
}
static bool dw_desc_reg(int sz, int reg, uint32_t v, std::string* out) {
    char b[96], src[64];
    unsigned mask = sz == 8 ? 0xFF : sz == 16 ? 0xFFFF : 0xFFFFFFFFu;
    const char* nm = sz == 8 ? r8name(reg) : sz == 16 ? r16name(reg) : r32name(reg);
    bool hit = dw_reg_src(sz, reg, v, src, sizeof(src));
    snprintf(b, sizeof(b), "%s=%0*Xh", nm, sz / 4, v & mask);
    *out = b;
    if (hit) { *out += "（"; *out += src; *out += "）"; }
    return hit;
}
static std::string dw_imm(int sz, uint32_t v) {
    char b[32]; unsigned mask = sz == 8 ? 0xFF : sz == 16 ? 0xFFFF : 0xFFFFFFFFu;
    snprintf(b, sizeof(b), "%0*Xh", sz / 4, v & mask); return b;
}
struct DwPre { uint16_t si, di, cx; int seg; };
#if defined(__GNUC__)
#define DW_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define DW_UNLIKELY(x) (x)
#endif
static void dw_after(const DwPre& pre) {
    uint8_t op = s_op;
    int vsz = s_vsz;
    std::string a, b;
    switch (op) {
    case 0x38: case 0x39: case 0x3A: case 0x3B: {
        int sz = (op & 1) ? vsz : 8;
        bool rmfirst = op <= 0x39;
        uint32_t vrm = rmfirst ? s_alu_a : s_alu_b, vrg = rmfirst ? s_alu_b : s_alu_a;
        bool h1 = dw_desc_rm(sz, vrm, &a), h2 = dw_desc_reg(sz, d_reg, vrg, &b);
        if (h1 || h2) { if (rmfirst) dw_line("CMP", a, b, sz); else dw_line("CMP", b, a, sz); }
        return; }
    case 0x3C: case 0x3D: case 0xA8: case 0xA9: {
        int sz = (op & 1) ? vsz : 8;
        if (dw_desc_reg(sz, EAX, s_alu_a, &a)) dw_line(op >= 0xA8 ? "TEST" : "CMP", a, dw_imm(sz, s_alu_b), sz);
        return; }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        if (d_reg != 7) { if (d_isreg) { int sz = op == 0x80 || op == 0x82 ? 8 : vsz; dw_set_reg(sz, d_rm, 0, false); } return; }
        int sz = (op == 0x80 || op == 0x82) ? 8 : vsz;
        if (dw_desc_rm(sz, s_alu_a, &a)) dw_line("CMP", a, dw_imm(sz, s_alu_b), sz);
        return; }
    case 0x84: case 0x85: {
        int sz = (op & 1) ? vsz : 8;
        bool h1 = dw_desc_rm(sz, s_alu_a, &a), h2 = dw_desc_reg(sz, d_reg, s_alu_b, &b);
        if (h1 || h2) dw_line("TEST", a, b, sz);
        return; }
    case 0xF6: case 0xF7: {
        if (d_reg > 1) return;
        int sz = (op & 1) ? vsz : 8;
        if (dw_desc_rm(sz, s_alu_a, &a)) dw_line("TEST", a, dw_imm(sz, s_alu_b), sz);
        return; }
    case 0x8A: case 0x8B: {
        int sz = (op & 1) ? vsz : 8;
        if (d_isreg) dw_copy_reg(sz, d_reg, d_rm); else dw_set_reg(sz, d_reg, d_ea, true);
        return; }
    case 0xA0: case 0xA1: dw_set_reg((op & 1) ? vsz : 8, EAX, s_moffs, true); return;
    case 0xAC: case 0xAD: {
        if (d_rep) return;
        dw_set_reg((op & 1) ? vsz : 8, EAX, ((uint32_t)C->sr[pre.seg] << 4) + pre.si, true);
        return; }
    case 0xA4: case 0xA5: {   // MOVS: 見張っている所から写したら、写し先も見張る
        uint32_t src = ((uint32_t)C->sr[pre.seg] << 4) + pre.si, dst = ((uint32_t)C->sr[ES_] << 4) + pre.di;
        uint32_t n = (d_rep ? pre.cx : 1) * (uint32_t)((op & 1) ? vsz / 8 : 1);
        uint32_t off;
        if (n && !(C->fl & FL_DF) && dw_find(src, &off) >= 0 && dw_find(dst, nullptr) < 0 && s_dw.ranges.size() < 16) {
            s_dw.ranges.push_back({dst, n, off});
            plog("[fdchk] %04X:%04X 読んだデータ+%03Xh から %u バイトを [%05X] へ写した（写し先も見張る）\n", C->op_cs, C->op_ip, off, n, dst);
        }
        return; }
    case 0xA6: case 0xA7: case 0xAE: case 0xAF: {   // CMPS / SCAS
        int unit = (op & 1) ? vsz / 8 : 1;
        uint32_t src = ((uint32_t)C->sr[pre.seg] << 4) + pre.si, dst = ((uint32_t)C->sr[ES_] << 4) + pre.di;
        uint32_t done = d_rep ? (uint32_t)(pre.cx - (uint16_t)C->r[ECX]) : 1;
        uint32_t o1, o2;
        bool cmps = op <= 0xA7;
        bool h1 = cmps && dw_find(src, &o1) >= 0, h2 = dw_find(dst, &o2) >= 0;
        if (!h1 && !h2) return;
        char line[400]; int k = 0;
        k += snprintf(line + k, sizeof(line) - k, "[fdchk] %04X:%04X %s%s ", C->op_cs, C->op_ip, d_rep == 0xF3 ? "REPE " : d_rep == 0xF2 ? "REPNE " : "", cmps ? "CMPS" : "SCAS");
        if (cmps) {
            k += snprintf(line + k, sizeof(line) - k, "[%05X]%s と [%05X]%s を %u 単位比べた:", src, h1 ? "（読んだデータ）" : "", dst, h2 ? "（読んだデータ）" : "", done);
            for (uint32_t i = 0; i < done * unit && i < 12 && k < 330; i++) k += snprintf(line + k, sizeof(line) - k, " %02X/%02X", mem_rb(M, src + i), mem_rb(M, dst + i));
        } else {
            k += snprintf(line + k, sizeof(line) - k, "%s=%Xh を [%05X]（読んだデータ+%03Xh）から %u 単位探した", unit == 1 ? "AL" : "AX", unit == 1 ? (C->r[EAX] & 0xFF) : (C->r[EAX] & 0xFFFF), dst, o2, done);
        }
        snprintf(line + k, sizeof(line) - k, " → %s\n", (C->fl & FL_ZF) ? "最後は等しい" : "最後は違う");
        plog("%s", line);
        if (++s_dw.lines >= DW_LINES) s_dw.active = false;
        s_dw_total++;
        return; }
    case 0x0F:
        if (s_op0f == 0xB6 || s_op0f == 0xB7) { if (!d_isreg) dw_set_reg(s_op0f == 0xB6 ? 8 : 16, d_reg, d_ea, true); }
        return;
    }
    if (op >= 0xB0 && op <= 0xB7) { s_dw.t[op & 7].on = false; return; }
    if (op >= 0xB8 && op <= 0xBF) { dw_set_reg(16, op & 7, 0, false); return; }
}

// 比較を記録している間の 1 命令（ふだんの経路を重くしないよう別の関数にする）
#if defined(__GNUC__)
__attribute__((noinline))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
static void step_dw() {
    DwPre pre = {(uint16_t)C->r[ESI], (uint16_t)C->r[EDI], (uint16_t)C->r[ECX], DS_};
    step();
    if (d_seg >= 0) pre.seg = d_seg;
    dw_after(pre);
    if (--s_dw.left <= 0) s_dw.active = false;
}

int cpu_run(Cpu* c, int budget) {
    C = c; M = c->m;
    int used = 0;
    while (used < budget) {
        if (g_irq_hint && (C->fl & FL_IF) && !C->inhibit_irq) {
            int v = pic_acknowledge(M);
            if (v >= 0) { C->halted = 0; d_cyc = 0; do_int((uint8_t)v); used += d_cyc; spin_clear(); }
        }
        C->inhibit_irq = 0;
        if (C->halted) { used = budget; break; }
        uint16_t cs0 = C->sr[CS_], ip0 = C->ip;
        if (DW_UNLIKELY(s_dw.active)) step_dw(); else step();
        used += d_cyc;
        // 短い後ろ向きの分岐（ループの戻り）
        if (C->sr[CS_] == cs0 && C->ip < ip0 && (uint16_t)(ip0 - C->ip) <= 64 && g_idle_skip) {
            SpinSlot& sp = s_spin[(C->ip ^ (C->ip >> 3)) & 7];
            if (sp.valid && sp.cs == cs0 && sp.ip == C->ip && sp.fx == g_side_fx && spin_same(sp)) {
                if (C->fl & FL_IF) { C->halted = 1; spin_clear(); used = budget; g_spin_hits++; break; }
            } else spin_record(sp);
        }
    }
    C->cycles += used;
    return used;
}
