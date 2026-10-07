// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  egc.cpp  --  EGC（Enhanced Graphic Charger）
//
//  PC-9801 の EGC を、公開資料から読み取れる振る舞い（I/O 4A0h-4AEh の各レジスタ、
//  ビット単位のシフタ、3 項ラスタオペレーション、パターンレジスタ、書き込みマスク）
//  にもとづいて一から書いたもの。
//
//  考え方
//   ・画素の並び: VRAM の 1 ワード（偶数番地 b0 / 奇数番地 b1）を「左の画素が上位」
//     になるよう px = (b0 << 8) | b1 に並べ替えて扱う（以下「画素順」）。
//   ・シフタ: プレーンごとのビット列の待ち行列。昇順転送では画素順の上位から、
//     降順転送（4ACh bit12）では下位から出し入れする。
//       - 入れる  : 転送元の先頭 srcbit ビットを捨ててから積む
//       - 取り出す: 転送先の先頭 dstbit ビットを空けて、残りを埋める。
//                   足りなければ何も書かない（マスク 0）。
//       - 長さ（4AEh+1 ビット）を出し終えたら、同じ設定で最初からやり直す
//   ・入れたら直ちに取り出して「最新の出力」と「出力マスク」に保持する。
//     VRAM を転送元にするとき（4A4h bit10=0）は、読み出しがシフタを進め、
//     書き込みはその保持値を使う。CPU を転送元にするときは書き込みが進める。
// -----------------------------------------------------------------------------
#include "machine.h"
#include "state.h"
#include <string.h>

namespace {

struct Regs {
    uint16_t access;     // 4A0h: bit0-3 = 1 のプレーンは書き込み禁止
    uint16_t fgbg;       // 4A2h: bit8-9 読み出しプレーン / bit13-14 パターンの出どころ
    uint16_t ope;        // 4A4h: bit0-7 ROP / bit8-9 パターンレジスタ読込み / bit10 転送元 / bit11-12 書込みデータ / bit13 読み出し方
    uint16_t fg, bg;     // 4A6h / 4AAh: 下位 4bit がプレーンごとの色
    uint16_t mask;       // 4A8h（画素順）
    uint16_t sft;        // 4ACh: bit0-3 転送元ビット位置 / bit4-7 転送先ビット位置 / bit12 降順
    uint16_t leng;       // 4AEh: 転送ビット数-1
};

struct Shifter {
    uint64_t q[4];       // 待ち行列（昇順: 上位詰め / 降順: 下位詰め）
    int32_t  have;       // 積まれているビット数
    int32_t  skip;       // まだ捨てていない転送元の先頭ビット数
    int32_t  lead;       // まだ空けていない転送先の先頭ビット数
    int32_t  remain;     // 出力の残りビット数
    uint8_t  down;       // 降順
};

struct Egc {
    Regs     r;
    Shifter  s;
    uint16_t out[4];     // シフタの最新出力（画素順、プレーンごと）
    uint16_t outmask;    // その有効ビット（画素順）
    uint16_t pat[4];     // パターンレジスタ（画素順）
    uint16_t last[4];    // 直前に読んだ VRAM（画素順）
};

Egc g;

inline uint16_t plane_color(uint16_t reg, int p) { return (reg & (1 << p)) ? 0xFFFF : 0x0000; }

void shifter_restart() {
    Shifter& s = g.s;
    memset(s.q, 0, sizeof(s.q));
    s.have = 0;
    s.skip = g.r.sft & 0x0F;
    s.lead = (g.r.sft >> 4) & 0x0F;
    s.remain = (int32_t)(g.r.leng & 0x0FFF) + 1;
    s.down = (g.r.sft & 0x1000) ? 1 : 0;
}

// 「幅 w ビットの区切り」の中での位置 half（0 = 画素順の上位 8bit / 1 = 下位 8bit）
inline uint16_t place(uint16_t v, int w, int half) { return w == 16 ? v : (uint16_t)(half ? (v & 0xFF) : (v << 8)); }
inline uint16_t lane_mask(int w, int half) { return w == 16 ? 0xFFFF : (half ? 0x00FF : 0xFF00); }

// シフタから w ビットの区切り 1 つぶんを取り出して out / outmask の該当部分へ置く
void shifter_emit(int w, int half) {
    Shifter& s = g.s;
    uint16_t lm = lane_mask(w, half);
    g.outmask &= (uint16_t)~lm;
    if (s.lead >= w) { s.lead -= w; return; }             // この区切りは全部「空け」
    int need = w - s.lead;
    if (s.have < need) return;                            // まだ足りない: 何も出さない
    int n = need < s.remain ? need : s.remain;            // 実際に描くビット数
    uint32_t wmask = (1u << w) - 1;
    uint32_t bits_mask = ((1u << n) - 1);
    for (int p = 0; p < 4; p++) {
        uint32_t v;
        if (!s.down) { v = (uint32_t)(s.q[p] >> (64 - need)); s.q[p] = need == 64 ? 0 : (s.q[p] << need); }
        else { v = (uint32_t)(s.q[p] & ((1ull << need) - 1)); s.q[p] >>= need; }
        // 区切りの中での位置: 昇順は先頭 lead ビットの後ろ（= 下位 need ビット）、降順は下位から lead ビット空けた上
        uint32_t lane = s.down ? (v << s.lead) : v;
        g.out[p] = (uint16_t)((g.out[p] & ~lm) | (place((uint16_t)(lane & wmask), w, half) & lm));
    }
    s.have -= need;
    uint32_t mk = s.down ? (bits_mask << s.lead) : (bits_mask << (need - n));
    g.outmask |= (uint16_t)(place((uint16_t)(mk & wmask), w, half) & lm);
    s.remain -= n;
    s.lead = 0;
    if (s.remain <= 0) shifter_restart();
}

// 4 プレーンぶんの w ビット（画素順）をシフタへ入れ、続けて同じ幅を取り出す
void shifter_feed(const uint16_t v[4], int w, int half) {
    Shifter& s = g.s;
    if (s.have <= 16) {
        if (s.skip >= w) s.skip -= w;
        else {
            int take = w - s.skip;
            for (int p = 0; p < 4; p++) {
                uint64_t bits = s.down ? ((uint64_t)v[p] >> s.skip) : ((uint64_t)v[p] & ((1ull << take) - 1));
                bits &= (1ull << take) - 1;
                if (!s.down) s.q[p] |= bits << (64 - s.have - take);
                else s.q[p] |= bits << s.have;
            }
            s.have += take;
            s.skip = 0;
        }
    }
    shifter_emit(w, half);
}

// ---- VRAM（画素順で読み書き）----------------------------------------------------
inline uint8_t* plane_ptr(Machine* m, int p) { return m->gvram[m->draw_bank][p]; }
inline uint16_t vram_get(Machine* m, int p, uint32_t off, int w) {
    const uint8_t* b = plane_ptr(m, p);
    return w == 16 ? (uint16_t)((b[off & 0x7FFF] << 8) | b[(off + 1) & 0x7FFF]) : b[off & 0x7FFF];
}
inline void vram_put(Machine* m, int p, uint32_t off, int w, uint16_t v) {
    uint8_t* b = plane_ptr(m, p);
    if (w == 16) { b[off & 0x7FFF] = (uint8_t)(v >> 8); b[(off + 1) & 0x7FFF] = (uint8_t)v; }
    else b[off & 0x7FFF] = (uint8_t)v;
}

// 区切りの中身（w ビット）を 16bit の画素順の位置から取り出す / 置く
inline uint16_t lane_get(uint16_t v, int w, int half) { return w == 16 ? v : (uint16_t)(half ? (v & 0xFF) : (v >> 8)); }
inline void lane_set(uint16_t& dst, uint16_t v, int w, int half) {
    uint16_t lm = lane_mask(w, half);
    dst = (uint16_t)((dst & ~lm) | (place(v, w, half) & lm));
}

// パターン（ROP の P 入力）の出どころ
uint16_t pattern_for(int p, int w, int half) {
    switch (g.r.fgbg & 0x6000) {
    case 0x2000: return lane_get(plane_color(g.r.bg, p), w, half);
    case 0x4000: return lane_get(plane_color(g.r.fg, p), w, half);
    default:
        // パターンレジスタを「読み出し時に更新」の設定では、シフタの出力をそのまま使う
        if ((g.r.ope & 0x0300) == 0x0100) return lane_get(g.out[p], w, half);
        return lane_get(g.pat[p], w, half);
    }
}

// 3 項（P: パターン, S: シフタ出力, D: VRAM）のラスタオペレーション
uint16_t raster_op(uint8_t code, uint16_t P, uint16_t S, uint16_t D) {
    uint16_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (!(code & (1 << i))) continue;
        // ビット番号 i = (P?1:0) + (D?2:0) + (S?4:0) の組に対応する項
        uint16_t t = 0xFFFF;
        t &= (i & 1) ? P : (uint16_t)~P;
        t &= (i & 2) ? D : (uint16_t)~D;
        t &= (i & 4) ? S : (uint16_t)~S;
        r |= t;
    }
    return r;
}

// ---- 読み書きの本体（w = 8 または 16、off は区切りの先頭番地）----------------------
uint16_t egc_read(Machine* m, int plane, uint32_t off, int w) {
    int half = w == 8 ? (int)(off & 1) : 0;
    uint16_t v[4];
    for (int p = 0; p < 4; p++) { v[p] = vram_get(m, p, off, w); lane_set(g.last[p], v[p], w, half); }
    bool cpu_src = (g.r.ope & 0x0400) != 0;
    if (!cpu_src) shifter_feed(v, w, half);
    if ((g.r.ope & 0x0300) == 0x0100) for (int p = 0; p < 4; p++) lane_set(g.pat[p], v[p], w, half);
    if (!(g.r.ope & 0x2000)) {
        int pl = (g.r.fgbg >> 8) & 3;
        return cpu_src ? v[pl] : lane_get(g.out[pl], w, half);
    }
    return v[plane & 3];
}

void egc_write(Machine* m, uint32_t off, int w, uint16_t value) {
    int half = w == 8 ? (int)(off & 1) : 0;
    uint16_t lm = w == 16 ? 0xFFFF : 0xFF;
    uint16_t mk = lane_get(g.r.mask, w, half);
    bool cpu_src = (g.r.ope & 0x0400) != 0;
    if ((g.r.ope & 0x0300) == 0x0200) for (int p = 0; p < 4; p++) lane_set(g.pat[p], vram_get(m, p, off, w), w, half);
    uint16_t cpu4[4] = {value, value, value, value};
    uint16_t data[4];
    auto use_shifter = [&](bool feed) {
        if (feed) shifter_feed(cpu4, w, half);
        mk &= lane_get(g.outmask, w, half);
    };
    switch (g.r.ope & 0x1800) {
    case 0x0800:                                           // ROP の結果を書く
        use_shifter(cpu_src);
        for (int p = 0; p < 4; p++)
            data[p] = raster_op((uint8_t)g.r.ope, pattern_for(p, w, half), lane_get(g.out[p], w, half), vram_get(m, p, off, w));
        break;
    case 0x1000: {                                         // パターン（前景色 / 背景色 / パターンレジスタ）を書く
        uint16_t sel = g.r.fgbg & 0x6000;
        if (w == 16) use_shifter(true);
        else if (sel != 0x2000 && sel != 0x4000) use_shifter(cpu_src);
        // 4A2h bit13-14 = 00 のときはパターンレジスタ。ただし CPU が転送元で 4A4h bit8-9 = 01
        // （読み出しで更新）のときは、シフタを通った CPU のデータを書く（痕のオープニングの雲:
        // 主記憶の 1 プレーンぶんの絵を、ずらしながら VRAM へ送る）
        for (int p = 0; p < 4; p++)
            data[p] = sel == 0x2000 ? (uint16_t)(plane_color(g.r.bg, p) & lm)
                    : sel == 0x4000 ? (uint16_t)(plane_color(g.r.fg, p) & lm)
                    : (cpu_src && (g.r.ope & 0x0300) == 0x0100) ? lane_get(g.out[p], w, half)
                    : lane_get(g.pat[p], w, half);
        break; }
    default:                                               // CPU のデータをそのまま書く
        if (w == 16) use_shifter(true);
        for (int p = 0; p < 4; p++) data[p] = value;
        break;
    }
    if (!mk) return;
    for (int p = 0; p < 4; p++) {
        if (g.r.access & (1 << p)) continue;
        uint16_t old = vram_get(m, p, off, w);
        vram_put(m, p, off, w, (uint16_t)((old & ~mk) | (data[p] & mk)));
    }
}

inline uint16_t swap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

} // namespace

// ---- 公開: VRAM アクセス（plane: 窓のプレーン、off: プレーン内の番地）------------------
uint8_t egc_read_b(Machine* m, int plane, uint32_t off) { return (uint8_t)egc_read(m, plane, off & 0x7FFF, 8); }
void egc_write_b(Machine* m, int plane, uint32_t off, uint8_t v) { (void)plane; egc_write(m, off & 0x7FFF, 8, v); }

uint16_t egc_read_w(Machine* m, int plane, uint32_t off) {
    off &= 0x7FFF;
    if (!(off & 1)) return swap16(egc_read(m, plane, off, 16));
    // 奇数番地のワードは 2 回のバイトアクセス（転送の向きの順に）
    if (!g.s.down) { uint16_t lo = egc_read_b(m, plane, off); return (uint16_t)(lo | (egc_read_b(m, plane, off + 1) << 8)); }
    uint16_t hi = (uint16_t)(egc_read_b(m, plane, off + 1) << 8);
    return (uint16_t)(hi | egc_read_b(m, plane, off));
}
void egc_write_w(Machine* m, int plane, uint32_t off, uint16_t v) {
    off &= 0x7FFF;
    if (!(off & 1)) { egc_write(m, off, 16, swap16(v)); return; }
    if (!g.s.down) { egc_write_b(m, plane, off, (uint8_t)v); egc_write_b(m, plane, off + 1, (uint8_t)(v >> 8)); }
    else { egc_write_b(m, plane, off + 1, (uint8_t)(v >> 8)); egc_write_b(m, plane, off, (uint8_t)v); }
}

// ---- 公開: I/O 4A0h-4AFh --------------------------------------------------------
static void reg_write(int idx, uint16_t v, uint16_t keep_mask) {
    Regs& r = g.r;
    auto merge = [&](uint16_t& reg) { reg = (uint16_t)((reg & keep_mask) | (v & ~keep_mask)); };
    switch (idx) {
    case 0: merge(r.access); break;
    case 1: merge(r.fgbg); break;
    case 2: merge(r.ope); break;
    case 3: merge(r.fg); break;
    case 4: {
        // マスクは 4A2h bit13-14 が 00 のときだけ書ける。CPU の順（下位=偶数番地）を画素順へ
        if (r.fgbg & 0x6000) break;
        uint16_t cur = swap16(r.mask);
        cur = (uint16_t)((cur & keep_mask) | (v & ~keep_mask));
        r.mask = swap16(cur);
        break; }
    case 5: merge(r.bg); break;
    case 6: merge(r.sft); shifter_restart(); g.outmask = 0xFFFF; break;
    case 7: merge(r.leng); shifter_restart(); g.outmask = 0xFFFF; break;
    }
}
void egc_out(Machine* m, uint16_t port, uint8_t v) {
    if (!m->egc_enabled) return;
    int idx = (port & 0x0F) >> 1;
    if (port & 1) reg_write(idx, (uint16_t)(v << 8), 0x00FF);
    else reg_write(idx, v, 0xFF00);
}
void egc_out16(Machine* m, uint16_t port, uint16_t v) {
    if (!m->egc_enabled) return;
    if (port & 1) { egc_out(m, port, (uint8_t)v); egc_out(m, (uint16_t)(port + 1), (uint8_t)(v >> 8)); return; }
    reg_write((port & 0x0F) >> 1, v, 0x0000);
}

void egc_reset(Machine* m) {
    (void)m;
    memset(&g, 0, sizeof(g));
    g.r.access = 0xFFF0;
    g.r.fgbg = 0x00FF;
    g.r.mask = 0xFFFF;
    g.r.leng = 0x000F;
    shifter_restart();
    g.outmask = 0xFFFF;
}

// 旧版（v1 のステート）の EGC 部分の大きさ。形式が違うので読み飛ばして初期状態にする
static const size_t OLD_EGC_STATE_BYTES = 640;
void egc_state_save(StateW& w) { w.tag("EGC2"); w.pod(g); }
void egc_state_load(StateR& r) {
    if (r.peek_tag("EGC ")) {
        r.tag("EGC ");
        static uint8_t skip[OLD_EGC_STATE_BYTES];
        r.bytes(skip, sizeof(skip));
        egc_reset(nullptr);
        return;
    }
    r.tag("EGC2"); r.pod(g);
}
