// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  avsfm.cpp  --  内蔵の AVSDRV.SYS の FM 音源の機能（INT DAh）
//
//  プログラムは作業領域（ES）を渡し、パートごとの曲のデータ（バイト列）をリングバッファに置く。
//  拍の 1/48 ごとに各パートのデータを読み、音符（高さ・長さ）と命令（音色・音量・定位・テンポなど）を
//  実行する。命令は INT DAh の機能と同じもので、プログラムから直接呼ぶこともできる。
//
//  パート: 0-2 = FM 1-3、3-5 = SSG 1-3、6-8 = FM 4-6（AH=03h の形だけ）、FFh = リズム。
//  作業領域: パート 0-5 は 0000h から、6-8 は 0300h から、リズムは 0360h から、32 バイトずつ。
//   +0 データのセグメント +2 その中の先頭 +4 バッファの大きさ（0 なら使わない） +6 次に読む位置
//   +8 残りのバイト数 +0Ah 残りがこれ以下になったら知らせる（bit15 有効） +0Ch 知らせる先（far）
//   +13h 演奏中（0 / FFh） +1Fh 有効（0 / FFh）。そのほかの状態はこのファイルの中に持つ。
//  リズムの型の表のセグメントは作業領域の 0380h に置かれる。
//
//  テンポのタイマ（OPNA のタイマ A に相当）と、音色のゆらぎのタイマ（タイマ B に相当、約 120Hz）は、
//  OPNA のタイマを使わずにこちらで数える（ゲームの割込みと取り合わないように）。
// -----------------------------------------------------------------------------
#include "machine.h"
#include <string.h>
#include <math.h>

static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }

static const int      kParts   = 10;          // 0-8 とリズム（9）
static const int      kRhythm  = 9;
static const int32_t  kTimerUnit = 144;       // タイマの 1 目盛り（マスタークロック数、18μs）
static const int32_t  kModPeriod = 144 * 16 * (256 - 0xE3);   // ゆらぎのタイマ

// ---- 小道具 --------------------------------------------------------------------
static bool is_ssg(int ch) { return ch >= 3 && ch <= 5; }
static bool is_fm(int ch) { return (ch >= 0 && ch < 3) || (ch >= 6 && ch <= 8); }
static int  fm_part(int ch) { return ch >= 6 ? 1 : 0; }
static int  fm_index(int ch) { return ch >= 6 ? ch - 6 : ch; }
static int  part_of(uint8_t al) { return al == 0xFF ? kRhythm : al; }   // INT DAh の AL から

static uint32_t rec_addr(Machine* m, int ch) {
    uint16_t o = ch == kRhythm ? 0x360 : (ch < 6 ? (uint16_t)(ch * 0x20) : (uint16_t)(0x300 + (ch - 6) * 0x20));
    return lin(m->avsfm.work_seg, o);
}
static uint8_t  rb(Machine* m, uint32_t a) { return mem_rb(m, a & 0xFFFFF); }
static uint16_t rw(Machine* m, uint32_t a) { return (uint16_t)(rb(m, a) | (rb(m, a + 1) << 8)); }
static void     wb(Machine* m, uint32_t a, uint8_t v) { mem_wb(m, a & 0xFFFFF, v); }
static void     ww(Machine* m, uint32_t a, uint16_t v) { wb(m, a, (uint8_t)v); wb(m, a + 1, (uint8_t)(v >> 8)); }

// 作業領域の項目
enum { R_SEG = 0, R_BASE = 2, R_SIZE = 4, R_POS = 6, R_LEFT = 8, R_WARN = 0x0A, R_CB = 0x0C, R_PLAY = 0x13, R_ON = 0x1F };
static uint16_t rec_w(Machine* m, int ch, int o) { return rw(m, rec_addr(m, ch) + o); }
static uint8_t  rec_b(Machine* m, int ch, int o) { return rb(m, rec_addr(m, ch) + o); }
static void     rec_setw(Machine* m, int ch, int o, uint16_t v) { ww(m, rec_addr(m, ch) + o, v); }
static void     rec_setb(Machine* m, int ch, int o, uint8_t v) { wb(m, rec_addr(m, ch) + o, v); }

// ---- OPNA へ書く ------------------------------------------------------------------
// タイマの登録（24h-27h）はこちらで数えるので書かない
static void put(Machine* m, int part, uint8_t reg, uint8_t v) {
    if (part == 0 && reg >= 0x24 && reg <= 0x27) return;
    opna_write(m, part, reg, v);
    if (part == 0 && reg == 0x28) {
        int code = v & 7;
        int ch = code < 3 ? code : (code >= 4 && code <= 6 ? code + 2 : -1);
        if (ch >= 0) m->avsfm.keylatch[ch] = v;
    }
}
static uint8_t peek(Machine* m, int part, uint8_t reg) { return m->opna.reg[part][reg]; }
// SSG の音量（08h-0Ah）は、命令で書いたものを覚えておき、音を出すときに使う
static void put_keep(Machine* m, uint8_t reg, uint8_t v) {
    if (reg >= 8 && reg <= 0x0A) m->avsfm.ssg_vol[reg - 8] = v;
    put(m, 0, reg, v);
}

// ---- 音の高さの表（平均律、A=440Hz） ----------------------------------------------------
static uint16_t fm_fnum(int semi) {          // ブロック 0 の F-Number（C0-B0、4MHz 相当で計算）
    double f = 440.0 * pow(2.0, (semi - 57) / 12.0);
    return (uint16_t)floor(f * 144.0 * 1048576.0 / 4000000.0 + 0.5);
}
static uint16_t ssg_period(int note) {       // ノート 0 = C1
    double f = 55.0 * pow(2.0, (note - 9) / 12.0);
    return (uint16_t)floor(125000.0 / f);
}

// ---- 音色 -----------------------------------------------------------------------
// 音色はパラメータ 0-32h。14h だけ 2 バイトなので、それより後ろは 1 つずれて並ぶ（全部で 52 バイト）
static int vpos(int p) { return p <= 0x14 ? p : p + 1; }
// 大きいほど速い・大きい、で渡される項目（OPNA へは最大値から引いて書く）
static uint8_t vflip(int pos) {
    if ((pos >= 1 && pos <= 4) || (pos >= 6 && pos <= 9) || (pos >= 0x0B && pos <= 0x0E)) return 31;   // AR DR SR
    if ((pos >= 0x10 && pos <= 0x13) || (pos >= 0x16 && pos <= 0x19)) return 15;                         // RR SL
    if (pos >= 0x1B && pos <= 0x1E) return 127;                                                          // TL
    return 0;
}
static uint16_t vraw(Machine* m, int ch, int p) {
    if (ch < 0 || ch > 8 || p > 0x32) return 0;
    const uint8_t* v = m->avsfm.voice[ch];
    if (p == 0x14) return (uint16_t)(v[0x14] | (v[0x15] << 8));
    return v[vpos(p)];
}
static uint16_t vchip(Machine* m, int ch, int p) {     // OPNA に書く値
    uint16_t x = vraw(m, ch, p);
    if (p == 0x14 || p > 0x32) return x;
    uint8_t mx = vflip(vpos(p));
    return mx ? (uint16_t)(uint8_t)(mx - x) : x;
}
static void vset(Machine* m, int ch, int p, uint16_t x) {
    if (ch < 0 || ch > 8 || p > 0x32) return;
    uint8_t* v = m->avsfm.voice[ch];
    if (p == 0x14) { v[0x14] = (uint8_t)x; v[0x15] = (uint8_t)(x >> 8); }
    else v[vpos(p)] = (uint8_t)x;
}
// 演算子 S1・S2・S3・S4 の登録の位置
static const uint8_t kSlot[4] = {0, 8, 4, 12};

static void voice_out(Machine* m, int ch) {
    if (!is_fm(ch)) return;
    int pt = fm_part(ch), fi = fm_index(ch);
    for (int n = 0; n < 4; n++) {
        uint8_t r = (uint8_t)(kSlot[n] + fi);
        put(m, pt, (uint8_t)(0x30 + r), (uint8_t)((vchip(m, ch, 0x29 + n) << 4) | vchip(m, ch, 0x24 + n)));
        put(m, pt, (uint8_t)(0x40 + r), (uint8_t)vchip(m, ch, 0x1A + n));
        put(m, pt, (uint8_t)(0x50 + r), (uint8_t)((vchip(m, ch, 0x1F + n) << 6) | vchip(m, ch, 1 + n)));
        put(m, pt, (uint8_t)(0x60 + r), (uint8_t)vchip(m, ch, 6 + n));
        put(m, pt, (uint8_t)(0x70 + r), (uint8_t)vchip(m, ch, 0x0B + n));
        put(m, pt, (uint8_t)(0x80 + r), (uint8_t)((vchip(m, ch, 0x15 + n) << 4) | vchip(m, ch, 0x10 + n)));
    }
    put(m, pt, (uint8_t)(0xB0 + fi), (uint8_t)vchip(m, ch, 0));
}
static void mute(Machine* m, int ch) {        // 4 つの演算子の TL を最小に
    if (!is_fm(ch)) return;
    for (int n = 0; n < 4; n++) put(m, fm_part(ch), (uint8_t)(0x40 + kSlot[n] + fm_index(ch)), 0x7F);
}
static void depth_update(Machine* m, int ch) {
    if (ch < 0 || ch > 8) return;
    m->avsfm.part[ch].mod_amt = (int16_t)((int8_t)vchip(m, ch, 0x19) * (int8_t)vchip(m, ch, 0x23));
}

// ---- 鍵盤 -------------------------------------------------------------------------
static void key_on(Machine* m, int ch, uint8_t slots) {
    AvsFm& f = m->avsfm;
    if (is_ssg(ch)) {
        int i = ch - 3;
        uint8_t mix = (uint8_t)(peek(m, 0, 7) & ~(1 << i));
        put(m, 0, 7, mix); f.mixer = mix;
        put(m, 0, (uint8_t)(8 + i), f.ssg_vol[i]);
        put(m, 0, 0x0D, peek(m, 0, 0x0D));      // エンベロープを最初から
        return;
    }
    if (!is_fm(ch)) return;
    put(m, 0, 0x28, (uint8_t)(((slots & 15) << 4) | (ch < 3 ? ch : ch - 2)));
}
static void key_off(Machine* m, int ch) {
    if (is_ssg(ch)) {
        int i = ch - 3;
        put(m, 0, (uint8_t)(8 + i), 0);
        put(m, 0, 7, (uint8_t)(peek(m, 0, 7) | (1 << i)));
        return;
    }
    if (!is_fm(ch)) return;
    put(m, 0, 0x28, (uint8_t)(ch < 3 ? ch : ch - 2));
}
static void freq_out(Machine* m, int ch, uint16_t v) {
    if (is_ssg(ch)) {
        int i = ch - 3;
        put(m, 0, (uint8_t)(i * 2), (uint8_t)v);
        put(m, 0, (uint8_t)(i * 2 + 1), (uint8_t)(v >> 8));
    } else if (is_fm(ch)) {
        put(m, fm_part(ch), (uint8_t)(0xA4 + fm_index(ch)), (uint8_t)(v >> 8));
        put(m, fm_part(ch), (uint8_t)(0xA0 + fm_index(ch)), (uint8_t)v);
    }
}
// 高さを決めて鳴らす
static void sound_note(Machine* m, int ch, uint8_t note) {
    uint16_t v;
    if (is_ssg(ch)) v = ssg_period(note > 103 ? 103 : note);
    else v = (uint16_t)((((note / 12) & 7) << 11) | fm_fnum(note % 12));
    m->avsfm.part[ch].freq = v;
    freq_out(m, ch, v);
    key_on(m, ch, (uint8_t)vchip(m, ch, 5));
    m->avsfm.part[ch].mod |= 3;
}

// ---- テンポ -------------------------------------------------------------------------
static void set_tempo(Machine* m, uint8_t tempo) {
    AvsFm& f = m->avsfm;
    unsigned t = tempo ? tempo : 1;
    unsigned div = 0, units = 0x400;
    while (units >= 0x400) {     // タイマ 1 回が 1024 目盛りに収まるまで、1 刻みを何回かに分ける
        div++;
        units = 8u * (8685u / (t * div));
    }
    if (units == 0) units = 0x400;
    f.sub_div = (uint8_t)div;
    f.sub_left = 0;
    f.a_period = (int32_t)(kTimerUnit * units);
    f.a_left = f.a_period;
    if (f.ext) f.rhy_wait = 0;
}
static void start_timers(Machine* m) {
    AvsFm& f = m->avsfm;
    if (!f.running) { f.running = 1; if (f.a_left <= 0) f.a_left = f.a_period; if (f.b_left <= 0) f.b_left = kModPeriod; }
}

// ---- 初期化 -----------------------------------------------------------------------
static void part_defaults(Machine* m, int ch) {
    rec_setw(m, ch, R_POS, 0); rec_setw(m, ch, R_LEFT, 0); rec_setw(m, ch, R_WARN, 0);
    rec_setb(m, ch, R_PLAY, 0); rec_setb(m, ch, R_ON, 0);
    AvsFmPart& p = m->avsfm.part[ch];
    memset(&p, 0, sizeof(p));
    p.key = 0x30; p.def_len = 6; p.gate = 0;
}
static void setup_basic(Machine* m) {
    AvsFm& f = m->avsfm;
    m->opna.reg[1][0xFE] |= 1;
    memset(f.voice, 0, sizeof(f.voice));
    f.ext = 0;
    put(m, 0, 0x21, 0);
    for (int ch = 0; ch < 6; ch++) { part_defaults(m, ch); mute(m, ch); key_off(m, ch); f.mod_dir[ch] = 1; f.mod_val[ch] = 0; }
    f.paused = 0;
    f.rnd = 0x37D;
    put(m, 0, 0x2D, 0);
    put_keep(m, 7, 0xBF);
    put_keep(m, 8, 8); put_keep(m, 9, 8); put_keep(m, 0x0A, 8);
    put_keep(m, 0x0B, 0xFF); put_keep(m, 0x0C, 0); put_keep(m, 0x0D, 1);
    put(m, 0, 0x29, 3);
    put(m, 0, 0x22, 0);
    for (int r = 0x90; r <= 0x9E; r++) if ((r & 3) != 3) put(m, 0, (uint8_t)r, 0);
    for (int r = 0xB4; r <= 0xB6; r++) put(m, 0, (uint8_t)r, 0xC0);
    m->opna.reg[1][0xFE] &= (uint8_t)~1;
    f.song = 0;
    set_tempo(m, 120);
    f.b_left = kModPeriod;
    start_timers(m);
}
static bool setup_ext(Machine* m) {
    AvsFm& f = m->avsfm;
    if (m->cfg.sound_board != 86) return false;       // OPNA（86 ボード）が要る
    m->opna.reg[1][0xFE] |= 1;
    f.ext = 1;
    f.rhy_wait = 0; f.rhy_pos = f.rhy_end = 0;
    put(m, 0, 0x29, 0x83);
    for (int ch = 6; ch < kParts; ch++) {
        part_defaults(m, ch);
        if (ch < 9) { mute(m, ch); key_off(m, ch); }
    }
    for (int ch = 0; ch < 9; ch++) f.mod_dir[ch] = 1;
    for (int r = 0x90; r <= 0x9E; r++) if ((r & 3) != 3) put(m, 1, (uint8_t)r, 0);
    for (int r = 0xB4; r <= 0xB6; r++) put(m, 1, (uint8_t)r, 0xC0);
    put(m, 0, 0x12, 0);
    put(m, 0, 0x11, 0x20);
    for (int r = 0x18; r <= 0x1D; r++) put(m, 0, (uint8_t)r, 0xD0);
    f.rhy_pat = 0xFF;
    return true;
}
static int last_part(Machine* m) { return m->avsfm.ext ? 8 : 5; }

static void stop_sound(Machine* m) {         // タイマを止め、全部の音を離す
    m->avsfm.running = 0;
    for (int ch = 0; ch <= last_part(m); ch++) key_off(m, ch);
}

// ---- 曲のデータ ---------------------------------------------------------------------
// リングバッファから 1 バイト。無ければ false
static bool fetch(Machine* m, int ch, uint8_t* out) {
    uint16_t size = rec_w(m, ch, R_SIZE);
    int16_t left = (int16_t)rec_w(m, ch, R_LEFT);
    if (!size || left <= 0) return false;
    uint16_t pos = rec_w(m, ch, R_POS);
    *out = rb(m, lin(rec_w(m, ch, R_SEG), (uint16_t)(rec_w(m, ch, R_BASE) + pos)));
    pos++;
    if ((int16_t)pos >= (int16_t)size) pos = (uint16_t)(pos - size);
    rec_setw(m, ch, R_POS, pos);
    rec_setw(m, ch, R_LEFT, (uint16_t)(left - 1));
    return true;
}

static void api(Machine* m, uint8_t ah, uint8_t al, uint16_t bx, uint16_t dx, uint16_t es, uint16_t* bx_out);

// パートの次の 1 つ（音符か命令）
static void next_event(Machine* m, int ch) {
    AvsFm& f = m->avsfm;
    uint8_t b, x, y;
    if (!fetch(m, ch, &b)) return;
    if (ch == kRhythm) {
        if (b < 0x80) {                         // リズムの型を選ぶ
            uint16_t tseg = rw(m, lin(f.work_seg, 0x380));
            uint16_t at = rw(m, lin(tseg, (uint16_t)(b * 2)));
            f.rhy_pat = b;
            if (!at) return;
            uint8_t bytes = rb(m, lin(tseg, at)), steps = rb(m, lin(tseg, (uint16_t)(at + 1)));
            AvsFmPart& p = f.part[kRhythm];
            p.total = steps; p.elapsed = 0; p.sound = 0xFF;
            f.rhy_pos = (uint16_t)(at + 2);
            f.rhy_end = (uint16_t)(at + 2 + bytes);
        } else if (b < 0xC0) {
            put(m, 0, 0x11, (uint8_t)(b - 0x80));   // リズムの全体の音量
        }
        return;
    }
    uint8_t al = (uint8_t)ch;
    if (b <= 0x80) {                            // 音符: 高さ（80h は休み）と長さ
        if (!fetch(m, ch, &x)) return;
        api(m, 0x13, al, (uint16_t)((b << 8) | x), 0, 0, nullptr);
        return;
    }
    switch (b) {
    case 0x81: if (fetch(m, ch, &x) && fetch(m, ch, &y)) api(m, 0x11, x, y, 0, 0, nullptr); break;   // 登録（表）
    case 0x82: if (fetch(m, ch, &x)) api(m, 0x12, al, x, 0, 0, nullptr); break;
    case 0x83: if (fetch(m, ch, &x)) api(m, 0x14, al, x, 0, 0, nullptr); break;
    case 0x84: if (fetch(m, ch, &x)) api(m, 0x15, al, x, 0, 0, nullptr); break;
    case 0x85: {                                // 音色: 形式、場所（オフセット、セグメント）
        uint8_t fl, lo, hi, s0 = 0, s1 = 0;
        if (!fetch(m, ch, &fl) || !fetch(m, ch, &lo) || !fetch(m, ch, &hi)) break;
        if (!(fl & 2) && (!fetch(m, ch, &s0) || !fetch(m, ch, &s1))) break;
        api(m, 0x16, al, (uint16_t)(lo | (hi << 8)), fl, (uint16_t)(s0 | (s1 << 8)), nullptr);
        break; }
    case 0x86: {
        uint8_t idx, lo, hi;
        if (fetch(m, ch, &idx) && fetch(m, ch, &lo) && fetch(m, ch, &hi)) api(m, 0x18, al, idx, (uint16_t)(lo | (hi << 8)), 0, nullptr);
        break; }
    case 0x87: api(m, 0x1B, al, 0, 0, 0, nullptr); break;
    case 0x88: api(m, 0x1C, al, 0, 0, 0, nullptr); break;
    case 0x89: if (fetch(m, ch, &x)) api(m, 0x1E, al, x, 0, 0, nullptr); break;
    case 0x8A: if (fetch(m, ch, &x)) api(m, 0x1F, al, x, 0, 0, nullptr); break;
    case 0x8B: if (fetch(m, ch, &x) && fetch(m, ch, &y)) api(m, 0x21, x, y, 0, 0, nullptr); break;   // 登録（裏）
    case 0x8C: if (fetch(m, ch, &x) && fetch(m, ch, &y)) api(m, 0x22, al, (uint16_t)((x << 8) | y), 0, 0, nullptr); break;
    case 0x8D: if (fetch(m, ch, &x)) api(m, 0x24, al, x, 0, 0, nullptr); break;
    case 0x8E: if (fetch(m, ch, &x)) api(m, 0x25, al, x, 0, 0, nullptr); break;
    case 0x8F: api(m, 0x26, al, 0, 0, 0, nullptr); break;
    default: break;                             // 90h 以上は使わない
    }
}

// 残りが少なくなったら知らせる、の設定は受け付けるが、プログラムを呼び返すことはしない
static void low_water(Machine* m, int ch) {
    uint16_t w = rec_w(m, ch, R_WARN);
    if (!(w & 0x8000)) return;
    if ((int16_t)(w & 0x7FFF) < (int16_t)rec_w(m, ch, R_LEFT)) return;
    if (!m->avsfm.cb_warned) { m->avsfm.cb_warned = 1; plog("[avs] FM 音源: データの残りを知らせる呼び出し（AH=1Dh）には対応していません\n"); }
}

// 1 刻み（拍の 1/48）ぶん、パートを進める。残っているデータか鳴っている音があれば 0 以外
static unsigned advance_part(Machine* m, int ch) {
    AvsFmPart& p = m->avsfm.part[ch];
    if (rec_b(m, ch, R_ON) == 0xFF) {
        for (;;) {
            if (p.sound) {
                uint8_t t = p.elapsed;
                if (!(p.key & 0x80) && t >= p.cut && p.sound != 0x7F) {   // ここで鍵を離す
                    p.sound = 0x7F;
                    key_off(m, ch);
                    p.mod |= 3;
                    t = p.elapsed;
                }
                if (t < p.total) { p.elapsed++; break; }
                p.sound = 0; p.elapsed = 0;
                if (rec_w(m, ch, R_LEFT) != 0) continue;
                key_off(m, ch);
                p.mod |= 3;
                rec_setb(m, ch, R_PLAY, 0);
                rec_setb(m, ch, R_ON, 0);
                break;
            }
            next_event(m, ch);
            low_water(m, ch);
            if ((int16_t)rec_w(m, ch, R_LEFT) > 0) continue;
            if (!p.sound) rec_setb(m, ch, R_PLAY, 0);
            break;
        }
    }
    return (unsigned)rec_w(m, ch, R_LEFT) | p.sound;
}

// リズム: 4 刻みごとに型の 1 段を鳴らす
static void rhythm_step_out(Machine* m) {
    AvsFm& f = m->avsfm;
    uint16_t tseg = rw(m, lin(f.work_seg, 0x380));
    uint16_t si = f.rhy_pos;
    while (si < f.rhy_end) {
        uint8_t b = rb(m, lin(tseg, si++));
        if ((b & 0xC0) == 0xC0) {               // 音量と定位: 対象の楽器（下 6 ビット）と値
            uint8_t v = rb(m, lin(tseg, si++));
            for (int i = 0; i < 6; i++) if (b & (1 << i)) put(m, 0, (uint8_t)(0x18 + i), v);
            continue;
        }
        if (b & 0x40) {                         // 2 つ続けて鳴らす
            if (b & 0x3F) put(m, 0, 0x10, b);
            b = rb(m, lin(tseg, si++));
        }
        if (b & 0x3F) put(m, 0, 0x10, b);
        break;
    }
    f.rhy_pos = si;
}
static unsigned advance_rhythm(Machine* m) {
    AvsFm& f = m->avsfm;
    if (rec_b(m, kRhythm, R_ON) != 0xFF) return 0;
    if ((int8_t)--f.rhy_wait > 0) return 1;
    f.rhy_wait = 4;
    AvsFmPart& p = f.part[kRhythm];
    p.elapsed++;
    if (p.elapsed < p.total) { rhythm_step_out(m); return 1; }
    p.sound = 0;
    for (;;) {
        if ((int16_t)rec_w(m, kRhythm, R_LEFT) <= 0) {
            rec_setb(m, kRhythm, R_PLAY, 0);
            rec_setb(m, kRhythm, R_ON, 0);
            return 0;
        }
        next_event(m, kRhythm);
        low_water(m, kRhythm);
        if ((int16_t)rec_w(m, kRhythm, R_LEFT) < 0) return 1;
        if (p.sound) break;
    }
    rhythm_step_out(m);
    return 1;
}

static void rewind_song(Machine* m) {        // 最初から（繰り返し）
    AvsFm& f = m->avsfm;
    int n = f.ext ? kParts : 6;
    for (int ch = 0; ch < n; ch++) {
        uint16_t size = rec_w(m, ch, R_SIZE);
        if (!size) continue;
        rec_setw(m, ch, R_LEFT, size);
        rec_setw(m, ch, R_POS, 0);
        rec_setb(m, ch, R_ON, 0xFF);
        rec_setb(m, ch, R_PLAY, 0xFF);
        AvsFmPart& p = f.part[ch];
        p.def_len = 0x30; p.gate = 6;
        p.key = 0; p.total = 0; p.elapsed = 0; p.cut = 0;
    }
    f.paused = 0;
}

static void beat(Machine* m) {               // テンポのタイマ
    AvsFm& f = m->avsfm;
    if (f.sub_left && --f.sub_left) return;
    f.sub_left = f.sub_div;
    unsigned busy = 0;
    if (f.ext) busy |= advance_rhythm(m);
    for (int ch = 0; ch <= last_part(m); ch++) busy |= advance_part(m, ch);
    if (f.paused) { stop_sound(m); return; }
    if (busy) return;
    if (!f.song) { stop_sound(m); return; }
    if (f.repeat == 1) { stop_sound(m); return; }
    if (f.repeat > 1) f.repeat--;
    rewind_song(m);
}

// ---- 音色のゆらぎ（LFO） ---------------------------------------------------------------
// 値を 1 歩進める。符号が変わる（行き過ぎる）なら戻して true
static bool mod_step(Machine* m, int ch, int16_t* out) {
    AvsFm& f = m->avsfm;
    int16_t inc = (int16_t)((int16_t)vchip(m, ch, 0x14) * f.mod_dir[ch]);
    int16_t old = f.mod_val[ch];
    int16_t nv = (int16_t)(old + inc);
    if ((old ^ nv) & 0x8000) { *out = old; return true; }
    f.mod_val[ch] = nv; *out = nv; return false;
}
static bool mod_wave(Machine* m, int ch, int16_t* out) {
    AvsFm& f = m->avsfm;
    AvsFmPart& p = f.part[ch];
    uint16_t speed = vchip(m, ch, 0x14);
    if (speed == 0) return false;               // 速さ 0 = ゆらさない（0 で割らないように）
    uint16_t span = (uint16_t)(0xFFFFu / speed);
    int16_t v;
    switch (vchip(m, ch, 0x0A)) {
    case 0:                                     // のこぎり
        if (mod_step(m, ch, &v)) f.mod_val[ch] = (int16_t)-f.mod_val[ch];
        break;
    case 1:                                     // 矩形
        v = (int16_t)(((p.mod_t / span) & 1) ? 0x8000 : 0x7FFF);
        f.mod_val[ch] = v;
        break;
    case 2:                                     // 三角
        if (mod_step(m, ch, &v)) f.mod_dir[ch] = (int16_t)-f.mod_dir[ch];
        break;
    case 3: {                                   // でたらめ（一定の間隔で変える）
        int16_t c = (int16_t)(span | 1);
        if ((int16_t)p.mod_t % c == 0) f.rnd = (uint16_t)(int16_t)(((int32_t)(int16_t)f.rnd * 0x383) % 0x7FFF);
        v = (int16_t)f.rnd;
        break; }
    case 4:                                     // 減っていって止まる
        mod_step(m, ch, &v);
        if (v < 0) { f.mod_dir[ch] = 0; v = 0; }
        break;
    case 5:                                     // 三角を 1 回
        if (mod_step(m, ch, &v)) f.mod_dir[ch] = (int16_t)-f.mod_dir[ch];
        if (f.mod_dir[ch] == -1 && v <= 0) { f.mod_dir[ch] = 0; v = 0; }
        break;
    default: return false;
    }
    *out = v;
    return true;
}
static void mod_amp(Machine* m, int ch, int16_t v) {    // 音量のゆらぎ（演算子ごとに TL を動かす）
    int8_t depth = (int8_t)vchip(m, ch, 0x1E);
    for (int n = 0; n < 4; n++) {
        int mask = vchip(m, ch, 0x2E + n) & 15;
        uint8_t d = (uint8_t)(int8_t)((mask * depth) / 15);
        int8_t dv = (int8_t)(int16_t)(((int32_t)d * v) / 0x7FFF);
        uint8_t tl = (uint8_t)vchip(m, ch, 0x1A + n);
        int8_t add = (int8_t)(((int)dv * (int8_t)tl) / 127);
        put(m, fm_part(ch), (uint8_t)(0x40 + kSlot[n] + fm_index(ch)), (uint8_t)(add + tl));
    }
}
static void mod_begin(Machine* m, int ch) {
    AvsFm& f = m->avsfm;
    AvsFmPart& p = f.part[ch];
    p.mod_t = 0;
    if (vchip(m, ch, 0x0A) == 4) { f.mod_val[ch] = 0x7FFF; f.mod_dir[ch] = -1; }
    else { f.mod_val[ch] = 0; f.mod_dir[ch] = 1; }
    p.mod = (uint8_t)((p.mod & ~3) | 2);
}
static void modulate(Machine* m) {
    AvsFm& f = m->avsfm;
    for (int ch = 0; ch <= last_part(m); ch++) {
        AvsFmPart& p = f.part[ch];
        bool tick = true;
        if (!(p.mod & 0x80)) { p.mod_t++; continue; }
        if (!p.sound || vchip(m, ch, 0x14) == 0) { p.mod |= 3; p.mod_t++; continue; }
        int st = p.mod & 3;
        if (st == 3) {                          // 鳴り始め: 遅れを数え始める
            uint8_t wait = (uint8_t)(vchip(m, ch, 0x0F) - 1);
            p.mod = (uint8_t)((p.mod & ~3) | 1);
            if (wait == 0) { mod_begin(m, ch); st = 2; }
            else if (wait == 0xFF) {            // 遅れ 0: 共通の時計に合わせる
                p.mod_t = f.mod_clock;
                f.mod_val[ch] = (int16_t)f.mod_clock;
                p.mod = (uint8_t)((p.mod & ~3) | 2);
                st = 2;
            } else { p.mod_t = (uint16_t)(wait * 4); p.mod_t++; continue; }
        } else if (st == 1) {
            uint8_t lo = (uint8_t)(p.mod_t - 1);
            p.mod_t = (uint16_t)((p.mod_t & 0xFF00) | lo);
            if (lo) continue;
            mod_begin(m, ch);
            st = 2;
        }
        if (st == 2) {
            int16_t v;
            if (mod_wave(m, ch, &v)) {
                if (p.mod_amt) {
                    int16_t a = (int16_t)(((int32_t)v * p.mod_amt) / 0x7FFF);
                    int16_t b = (int16_t)(((int32_t)a * (int16_t)p.freq) / 0x7FFF);
                    freq_out(m, ch, (uint16_t)(b + p.freq));
                }
                if (is_fm(ch)) mod_amp(m, ch, v);
            }
        }
        if (tick) p.mod_t++;
    }
    f.mod_clock++;
}

void avsfm_advance(Machine* m, uint32_t t) {
    AvsFm& f = m->avsfm;
    if (!f.work_seg || f.a_period <= 0) return;
    f.a_left -= (int32_t)t;
    f.b_left -= (int32_t)t;
    int guard = 64;
    while (f.running && (f.a_left <= 0 || f.b_left <= 0) && guard-- > 0) {
        if (f.a_left <= f.b_left) { f.a_left += f.a_period; beat(m); }
        else { f.b_left += kModPeriod; modulate(m); }
    }
    if (f.a_left < -f.a_period) f.a_left = f.a_period;
    if (f.b_left < -kModPeriod) f.b_left = kModPeriod;
}

// ---- 機能 ------------------------------------------------------------------------
static void note_cmd(Machine* m, int ch, uint8_t note, uint8_t len, bool tie) {
    if (ch < 0 || ch > 8) return;
    AvsFmPart& p = m->avsfm.part[ch];
    if (note > 0x80) return;
    if (note == 0x80) {                         // 休み
        p.total = len ? len : p.def_len;
        p.elapsed = 0;
        key_off(m, ch);
        p.key = 0x7F;
        p.sound = 0x7F;
        return;
    }
    uint8_t n = len ? len : p.def_len;
    p.total = n;
    p.elapsed = 0;
    uint8_t prev = p.key;
    p.key = note;
    if (!((prev & 0x80) && (prev & 0x7F) == note)) {   // 前の音とつながないなら鳴らし直す
        key_off(m, ch);
        sound_note(m, ch, note);
    }
    if (tie) { p.cut = n; p.key |= 0x80; }
    else p.cut = (uint8_t)((n * (p.gate + 1)) >> 3);
    p.sound = 0xFF;
}

static void api(Machine* m, uint8_t ah, uint8_t al, uint16_t bx, uint16_t dx, uint16_t es, uint16_t* bx_out) {
    AvsFm& f = m->avsfm;
    uint8_t bl = (uint8_t)bx, bh = (uint8_t)(bx >> 8);
    int ch = part_of(al);
    switch (ah) {
    case 0x01: {                                // データを足す: ES:BX = {セグメント, 0, (オフセット, 長さ) x パート数}
        int n = f.ext ? kParts : 6;
        uint32_t tbl = lin(es, bx);
        uint16_t src = rw(m, tbl);
        for (int c = 0; c < n; c++) {
            uint16_t off = rw(m, tbl + 4 + c * 4), len = rw(m, tbl + 6 + c * 4);
            if (!len) continue;
            uint16_t size = rec_w(m, c, R_SIZE);
            if (!size) continue;
            uint16_t wpos = (uint16_t)(rec_w(m, c, R_POS) + rec_w(m, c, R_LEFT));
            if ((int16_t)wpos >= (int16_t)size) wpos = (uint16_t)(wpos - size);
            uint16_t dseg = rec_w(m, c, R_SEG), dbase = rec_w(m, c, R_BASE);
            for (uint16_t i = 0; i < len; i++) {
                wb(m, lin(dseg, (uint16_t)(dbase + wpos)), rb(m, lin(src, (uint16_t)(off + i))));
                if (++wpos >= size) wpos = 0;
            }
            rec_setw(m, c, R_LEFT, (uint16_t)(rec_w(m, c, R_LEFT) + len));
            rec_setb(m, c, R_ON, 0xFF);
            rec_setb(m, c, R_PLAY, 0xFF);
            f.part[c].key = 0;
            f.paused = 0;
        }
        start_timers(m);
        break; }
    case 0x02: {                                // 止める（BL≠0 なら初期化し直す）
        f.running = 0;
        int n = f.ext ? kParts : 6;
        for (int c = 0; c < n; c++) {
            rec_setw(m, c, R_POS, 0); rec_setw(m, c, R_LEFT, 0);
            rec_setb(m, c, R_PLAY, 0); rec_setb(m, c, R_ON, 0);
            f.part[c].sound = 0;
            if (c < 9) { mute(m, c); key_off(m, c); }
        }
        f.song = 0;
        if (bl) { bool e = f.ext; setup_basic(m); if (e) setup_ext(m); }
        break; }
    case 0x04: {                                // 曲として始める: ES:BX = {セグメント, 大きさ} x パート数、AL = 回数
        f.song = 1; f.repeat = al;
        int n = f.ext ? kParts : 6;
        uint32_t tbl = lin(es, bx);
        for (int c = 0; c < n; c++, tbl += 4) {
            rec_setb(m, c, R_ON, 0);
            uint16_t size = rw(m, tbl + 2);
            rec_setw(m, c, R_SIZE, size);
            if (!size) continue;
            rec_setw(m, c, R_LEFT, size);
            rec_setw(m, c, R_BASE, 0);
            rec_setw(m, c, R_POS, 0);
            rec_setw(m, c, R_SEG, rw(m, tbl));
            rec_setb(m, c, R_ON, 0xFF);
            rec_setb(m, c, R_PLAY, 0xFF);
            AvsFmPart& p = f.part[c];
            p.sound = 0; p.key = 0; p.total = 0; p.elapsed = 0; p.cut = 0;
        }
        f.paused = 0;
        start_timers(m);
        break; }
    case 0x10: if (bx_out) *bx_out = peek(m, 0, al); break;
    case 0x11: {
        uint8_t v = bl;
        if (al == 0x27) break;                  // タイマはこちらで数える
        if (al == 7) v = (uint8_t)((v & 0x3F) | 0x80);
        put_keep(m, al, v);
        break; }
    case 0x12: if (ch <= 8 && bl <= 7) f.part[ch].gate = bl; break;
    case 0x13: note_cmd(m, ch, bh, bl, false); break;
    case 0x14: if (ch <= 8) f.part[ch].def_len = bl; break;
    case 0x15: set_tempo(m, bl); break;
    case 0x16: {                                // 音色: ES:BX、DL bit0 = 1 バイトずつ、bit1 = 作業領域 2FDh のセグメント
        if (ch > 8 || is_ssg(ch)) break;
        key_off(m, ch);
        uint16_t seg = (dx & 2) ? rw(m, lin(f.work_seg, 0x2FD)) : es;
        uint32_t s = lin(seg, bx);
        for (int p = 0; p < 0x32; p++) {
            if (dx & 1) {
                uint16_t v = rb(m, s++);
                if (p == 0x14) v = (uint16_t)(v | (rb(m, s++) << 8));
                vset(m, ch, p, v);
            } else {
                uint16_t v = rw(m, s); s += 2;
                vset(m, ch, p, p == 0x14 ? v : (uint16_t)(v & 0xFF));
            }
        }
        voice_out(m, ch);
        f.part[ch].mod |= 0x83;
        depth_update(m, ch);
        break; }
    case 0x17: if (ch <= 8 && bx_out) *bx_out = vraw(m, ch, bl); break;
    case 0x18:
        if (ch > 8 || bl > 0x32) break;
        vset(m, ch, bl, bl == 0x14 ? dx : (uint16_t)(dx & 0xFF));
        voice_out(m, ch);
        f.part[ch].mod |= 0x83;
        depth_update(m, ch);
        break;
    case 0x19: f.paused = 1; break;
    case 0x1A:
        if (!f.paused) break;
        f.paused = 0;
        for (int c = 0; c <= last_part(m); c++) if (is_fm(c)) put(m, 0, 0x28, f.keylatch[c]);
        put(m, 0, 7, f.mixer);
        start_timers(m);
        break;
    case 0x1B: if (ch <= 8) { f.part[ch].mod |= 0x83; depth_update(m, ch); } break;
    case 0x1C: if (ch <= 8) f.part[ch].mod &= 0x7F; break;
    case 0x1D:
        rec_setw(m, ch, R_WARN, dx);
        rec_setw(m, ch, R_CB, bx);
        rec_setw(m, ch, R_CB + 2, es);
        break;
    case 0x1E: {                                // 今の音を、鳴らし直さずに伸ばす
        if (ch > 8) break;
        AvsFmPart& p = f.part[ch];
        uint8_t n = bl ? bl : p.def_len;
        p.total = n; p.cut = n; p.sound = 0xFF; p.elapsed = 0; p.mod |= 3;
        break; }
    case 0x1F:                                  // 音量
        if (is_ssg(ch)) { f.ssg_vol[ch - 3] = bl > 16 ? 16 : bl; break; }
        if (ch == kRhythm) { put(m, 0, 0x11, bl); break; }
        if (is_fm(ch)) {
            static const uint8_t carriers[8] = {1, 1, 1, 1, 2, 3, 3, 4};
            static const uint8_t order[4] = {3, 1, 2, 0};   // 出力に出る演算子（S4 から）
            int alg = vraw(m, ch, 0) & 7;
            for (int i = 0; i < carriers[alg]; i++) vset(m, ch, 0x1A + order[i], bl);
            voice_out(m, ch);
            depth_update(m, ch);
        }
        break;
    case 0x20: if (bx_out) *bx_out = peek(m, 1, al); break;
    case 0x21: put(m, 1, al, bl); break;
    case 0x22: note_cmd(m, ch, bh, bl, true); break;
    case 0x23: {                                // 音色を ES:BX へ（DL=0: 1 項目 2 バイト）
        if (ch > 8 || is_ssg(ch)) break;
        uint32_t d = lin(es, bx);
        if ((dx & 0xFF) == 0) {
            for (int p = 0; p < 0x32; p++) { ww(m, d, vraw(m, ch, p)); d += 2; }
        } else {
            for (int i = 0; i < 0x33; i++) wb(m, d++, f.voice[ch][i]);
        }
        break; }
    case 0x24: {                                // 定位: BL bit0 = 左、bit1 = 右
        uint8_t pan = (uint8_t)(((bl & 1) << 7) | ((bl & 2) << 5));
        if (ch == kRhythm) {
            for (int i = 0; i < 6; i++) if (bh & (1 << i)) put(m, 0, (uint8_t)(0x18 + i), (uint8_t)((peek(m, 0, (uint8_t)(0x18 + i)) & 0x3F) | pan));
        } else if (is_fm(ch)) {
            uint8_t r = (uint8_t)(0xB4 + fm_index(ch));
            put(m, fm_part(ch), r, (uint8_t)((peek(m, fm_part(ch), r) & 0x3F) | pan));
        }
        break; }
    case 0x25: if (ch <= 8) { key_off(m, ch); f.part[ch].key = bl; sound_note(m, ch, bl); } break;
    case 0x26: if (ch <= 8) key_off(m, ch); break;
    default: break;
    }
}

void avsfm_reset(Machine* m) { memset(&m->avsfm, 0, sizeof(m->avsfm)); }

void avsfm_hle(Machine* m) {
    Cpu* c = &m->cpu;
    AvsFm& f = m->avsfm;
    uint16_t ax = (uint16_t)c->r[EAX], bx = (uint16_t)c->r[EBX], dx = (uint16_t)c->r[EDX];
    uint8_t ah = (uint8_t)(ax >> 8), al = (uint8_t)ax;
    static int seen[256];
    if (m->cfg.trace && seen[ah] < 4) { seen[ah]++; plog("[avs] INT DAh AX=%04X BX=%04X CX=%04X DX=%04X ES=%04X\n", ax, bx, (unsigned)(uint16_t)c->r[ECX], dx, c->sr[ES_]); }
    uint16_t out = bx;
    if (ah == 0x00 || ah == 0x03) {
        f.work_seg = c->sr[ES_];
        setup_basic(m);
        if (ah == 0x03) out = setup_ext(m) ? 0 : 1;
    } else if (f.work_seg) {
        api(m, ah, al, bx, dx, c->sr[ES_], &out);
    }
    if (ah == 0x03 || ah == 0x10 || ah == 0x17 || ah == 0x20) c->r[EBX] = (c->r[EBX] & 0xFFFF0000u) | out;
    set_cf(m, false);
}
