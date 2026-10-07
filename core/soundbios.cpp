// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  soundbios.cpp  --  FM 音源ボードのサウンド BIOS（INT D2h）の代わり（INI の SoundBIOS=1）
//
//  PC-9801-26(K)/86 には CC00:0000 からサウンド BIOS の ROM があり、INT D2h で FM・PSG を
//  鳴らせる。MIMPI などの MIDI プレーヤーは、この ROM の有無と中の音色の表を調べ、
//  INT D2h で FM 音源を鳴らす。NEC の ROM の中身は使えないので、ここでは:
//    ・CC00:0008 と CC00:2E08 に INT D2h の受け口（HLE トラップ）を置く
//      （MIMPI は ROM の版を見分けたあと、INT D2h のベクタを CEE0:0008 = ROM の +2E08h に向け直す）
//    ・CC00:1CB4 に音色の表（128 個のオフセット。先頭は 23EDh）を置き、音色は自前で作った FM 音色
//      （このファイルで定義。下の「音色の形」）を並べる
//    ・INT D2h の機能のうち、MIMPI が使うもの（00h 初期化・11h レジスタ書き込み・13h 発音・
//      16h 音色設定・1Fh 音量）を実装する。それ以外は何もしないで戻る
//  発音の音の高さ（13h の BH）は半音単位の番号（MIDI のノート番号 - 12）として扱う。
// -----------------------------------------------------------------------------
#include "machine.h"
#include <math.h>

static const uint32_t ROMBASE = 0xCC000;
static const uint16_t TABLE = 0x1CB4;   // 音色の表（MIMPI はここの先頭が 23EDh かで ROM の版を見分ける）
static const uint16_t VOICES = 0x23ED;  // 音色データの先頭
static const int VSIZE = 25;            // 音色 1 個の大きさ

// 音色の形（25 バイト）: [0]=FB<<3|ALG、続いてオペレータ 4 個ぶん（レジスタの順: +0,+4,+8,+Ch）×
//   { DT<<4|MUL, TL, KS<<6|AR, DR, SR, SL<<4|RR }
struct Op { uint8_t dtmul, tl, ksar, dr, sr, slrr; };
struct Patch { uint8_t fbalg; Op op[4]; };

// 自前の音色（数種類の型）。ALG4 は +8 と +Ch が発音（キャリア）、ALG7 は全部
static const Patch PATCHES[] = {
    // 0: ピアノ
    {0x24, {{0x31, 0x24, 0x5F, 0x0A, 0x03, 0x25}, {0x33, 0x2C, 0x5F, 0x0C, 0x04, 0x36},
            {0x01, 0x00, 0x5F, 0x06, 0x02, 0x36}, {0x01, 0x04, 0x5F, 0x07, 0x02, 0x36}}},
    // 1: エレピ・鐘・チェレスタ
    {0x04, {{0x0E, 0x30, 0x1F, 0x0E, 0x05, 0x47}, {0x01, 0x22, 0x1F, 0x08, 0x03, 0x27},
            {0x01, 0x02, 0x1F, 0x05, 0x02, 0x37}, {0x01, 0x06, 0x1F, 0x06, 0x02, 0x37}}},
    // 2: オルガン
    {0x07, {{0x01, 0x14, 0x1F, 0x00, 0x00, 0x08}, {0x02, 0x18, 0x1F, 0x00, 0x00, 0x08},
            {0x04, 0x20, 0x1F, 0x00, 0x00, 0x08}, {0x01, 0x10, 0x1F, 0x00, 0x00, 0x08}}},
    // 3: ギター・はじく音
    {0x2C, {{0x02, 0x1E, 0x5F, 0x0C, 0x05, 0x46}, {0x01, 0x28, 0x5F, 0x0A, 0x04, 0x46},
            {0x01, 0x00, 0x5F, 0x08, 0x04, 0x46}, {0x01, 0x08, 0x5F, 0x09, 0x04, 0x46}}},
    // 4: ベース
    {0x24, {{0x00, 0x1A, 0x5F, 0x08, 0x03, 0x36}, {0x01, 0x30, 0x5F, 0x08, 0x03, 0x36},
            {0x00, 0x00, 0x5F, 0x05, 0x02, 0x26}, {0x00, 0x04, 0x5F, 0x05, 0x02, 0x26}}},
    // 5: ストリングス・パッド（ゆっくり立ち上がる）
    {0x1C, {{0x01, 0x26, 0x0E, 0x02, 0x00, 0x16}, {0x02, 0x30, 0x0E, 0x02, 0x00, 0x16},
            {0x01, 0x00, 0x0E, 0x02, 0x00, 0x15}, {0x01, 0x04, 0x0E, 0x02, 0x00, 0x15}}},
    // 6: ブラス
    {0x2C, {{0x01, 0x1C, 0x14, 0x04, 0x01, 0x16}, {0x01, 0x26, 0x14, 0x04, 0x01, 0x16},
            {0x01, 0x00, 0x13, 0x03, 0x00, 0x16}, {0x01, 0x04, 0x13, 0x03, 0x00, 0x16}}},
    // 7: 笛・リード
    {0x24, {{0x03, 0x2A, 0x16, 0x02, 0x00, 0x17}, {0x01, 0x34, 0x16, 0x02, 0x00, 0x17},
            {0x01, 0x00, 0x15, 0x02, 0x00, 0x17}, {0x02, 0x10, 0x15, 0x02, 0x00, 0x17}}},
    // 8: シンセリード（のこぎり波ふう）
    {0x3C, {{0x01, 0x18, 0x1F, 0x02, 0x00, 0x17}, {0x01, 0x22, 0x1F, 0x02, 0x00, 0x17},
            {0x01, 0x00, 0x1F, 0x02, 0x00, 0x17}, {0x01, 0x06, 0x1F, 0x02, 0x00, 0x17}}},
    // 9: 打楽器（木琴・ティンパニ・効果音）
    {0x04, {{0x04, 0x20, 0x1F, 0x12, 0x08, 0x58}, {0x01, 0x28, 0x1F, 0x10, 0x08, 0x58},
            {0x01, 0x00, 0x1F, 0x0C, 0x06, 0x48}, {0x01, 0x06, 0x1F, 0x0D, 0x06, 0x48}}},
};

// ROM の音色番号 → 型。MIMPI の対応表（MT-32 の音色番号 → ROM の音色番号）から見た使われ方で決めた
static int patch_of(int v) {
    static const uint8_t map[128] = {
        // 0-15
        3, 3, 5, 0, 3, 1, 1, 7, 8, 8, 8, 2, 9, 0, 0, 0,
        // 16-31
        0, 0, 5, 0, 7, 3, 8, 5, 6, 5, 0, 0, 0, 6, 7, 7,
        // 32-47
        1, 1, 1, 1, 7, 7, 8, 7, 2, 8, 8, 4, 6, 2, 2, 8,
        // 48-63
        6, 4, 8, 9, 9, 7, 0, 9, 5, 5, 9, 5, 9, 9, 4, 5,
        // 64-79
        5, 5, 5, 6, 0, 5, 7, 1, 5, 0, 4, 4, 7, 4, 0, 4,
        // 80-127
        0, 7, 0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    };
    return map[v & 127];
}

void soundbios_init(Machine* m) {
    if (!m->cfg.sound_bios || !m->cfg.sound_board) return;
    uint8_t* r = m->ram + ROMBASE;
    for (int i = 0; i < 0x4000; i++) r[i] = 0xFF;
    for (uint16_t e : {(uint16_t)0x0008, (uint16_t)0x2E08}) { r[e] = 0xF1; r[e + 1] = HLE_SNDBIOS; r[e + 2] = 0xCF; }   // HLE; IRET
    // 音色は 96 個（MIMPI が使うのは 0〜81 番）。+2E08h の受け口に重ならないように
    for (int v = 0; v < 128; v++) {
        uint16_t off = (uint16_t)(VOICES + (v < 96 ? v : 0) * VSIZE);
        r[TABLE + v * 2] = (uint8_t)off; r[TABLE + v * 2 + 1] = (uint8_t)(off >> 8);
        const Patch& p = PATCHES[patch_of(v)];
        r[off] = p.fbalg;
        for (int o = 0; o < 4; o++) {
            const Op& q = p.op[o];
            uint8_t* d = r + off + 1 + o * 6;
            d[0] = q.dtmul; d[1] = q.tl; d[2] = q.ksar; d[3] = q.dr; d[4] = q.sr; d[5] = q.slrr;
        }
    }
    // INT D2h → CC00:0008
    m->ram[0xD2 * 4 + 0] = 0x08; m->ram[0xD2 * 4 + 1] = 0x00;
    m->ram[0xD2 * 4 + 2] = 0x00; m->ram[0xD2 * 4 + 3] = 0xCC;
}

// FM の 3 チャンネルの状態（音量を変えるときに、音色のキャリアの TL を基準にする）
static uint8_t s_alg[3], s_tl[3][4];

static bool is_carrier(int alg, int o) {   // o はレジスタの順（+0,+4,+8,+Ch）
    static const uint8_t mask[8] = {0x8, 0x8, 0x8, 0x8, 0xC, 0xE, 0xE, 0xF};
    return (mask[alg & 7] >> o) & 1;
}
static void wr(Machine* m, uint8_t a, uint8_t v) { opna_write(m, 0, a, v); }

static void set_volume(Machine* m, int ch, int vol) {
    if (ch < 0 || ch > 2) return;
    int att = vol <= 0 ? 127 : (0x7F - (vol > 0x7F ? 0x7F : vol)) / 2 + 10;   // +10: PSG・ほかのソフトと音量をそろえる
    for (int o = 0; o < 4; o++) {
        if (!is_carrier(s_alg[ch], o)) continue;
        int tl = s_tl[ch][o] + att; if (tl > 127) tl = 127;
        wr(m, (uint8_t)(0x40 + o * 4 + ch), (uint8_t)tl);
    }
}

static void note_on(Machine* m, int ch, int note) {
    double f = 440.0 * pow(2.0, ((note + 12) - 69) / 12.0);
    if (ch >= 0 && ch < 3) {
        int block = 0; double fn = 0;
        for (block = 0; block < 8; block++) {
            fn = f * 144.0 * (double)(1 << (20 - block)) / 3993600.0;
            if (fn < 2048) break;
        }
        if (block > 7) { block = 7; fn = 2047; }
        int fnum = (int)(fn + 0.5); if (fnum > 2047) fnum = 2047;
        wr(m, 0x28, (uint8_t)ch);   // いったん離す
        wr(m, (uint8_t)(0xA4 + ch), (uint8_t)((block << 3) | (fnum >> 8)));
        wr(m, (uint8_t)(0xA0 + ch), (uint8_t)fnum);
        wr(m, 0x28, (uint8_t)(0xF0 | ch));
    } else if (ch >= 3 && ch < 6) {
        int tp = (int)(62400.0 / f + 0.5); if (tp < 1) tp = 1; if (tp > 0xFFF) tp = 0xFFF;
        int c = ch - 3;
        wr(m, (uint8_t)(c * 2), (uint8_t)tp);
        wr(m, (uint8_t)(c * 2 + 1), (uint8_t)(tp >> 8));
    }
}

void soundbios_hle(Machine* m) {
    uint16_t ax = (uint16_t)m->cpu.r[EAX], bx = (uint16_t)m->cpu.r[EBX];
    uint8_t ah = (uint8_t)(ax >> 8), al = (uint8_t)ax;
    switch (ah) {
    case 0x00:   // 初期化: 全部の音を止め、PSG は出力を止める
        for (int c = 0; c < 3; c++) {
            wr(m, 0x28, (uint8_t)c);
            for (int o = 0; o < 4; o++) wr(m, (uint8_t)(0x40 + o * 4 + c), 0x7F);
            s_alg[c] = 7; for (int o = 0; o < 4; o++) s_tl[c][o] = 0x7F;
            wr(m, (uint8_t)(0xB4 + c), 0xC0);
        }
        wr(m, 0x07, 0x3F); wr(m, 0x08, 0); wr(m, 0x09, 0); wr(m, 0x0A, 0);
        break;
    case 0x11: wr(m, al, (uint8_t)bx); break;              // AL のレジスタへ BL を書く
    case 0x13: note_on(m, al, bx >> 8); break;             // AL のチャンネルを BH の高さで鳴らす
    case 0x16: {                                           // AL のチャンネルに ES:BX の音色
        if (al > 2) break;
        uint32_t a = ((uint32_t)m->cpu.sr[ES_] << 4) + bx;
        uint8_t v[VSIZE];
        for (int i = 0; i < VSIZE; i++) v[i] = mem_rb(m, (a + i) & 0xFFFFF);
        int ch = al;
        wr(m, 0x28, (uint8_t)ch);
        s_alg[ch] = v[0] & 7;
        wr(m, (uint8_t)(0xB0 + ch), v[0]);
        for (int o = 0; o < 4; o++) {
            const uint8_t* q = v + 1 + o * 6;
            uint8_t r = (uint8_t)(o * 4 + ch);
            s_tl[ch][o] = q[1] & 0x7F;
            wr(m, (uint8_t)(0x30 + r), q[0]); wr(m, (uint8_t)(0x40 + r), q[1]); wr(m, (uint8_t)(0x50 + r), q[2]);
            wr(m, (uint8_t)(0x60 + r), q[3]); wr(m, (uint8_t)(0x70 + r), q[4]); wr(m, (uint8_t)(0x80 + r), q[5]);
        }
        wr(m, (uint8_t)(0xB4 + ch), 0xC0);
        break; }
    case 0x1F: set_volume(m, al, (uint8_t)bx); break;      // AL のチャンネルの音量 BL
    default:
        if (m->cfg.trace) { static int n = 0; if (n++ < 20) plog("[sndbios] INT D2h AH=%02Xh 未対応\n", ah); }
        break;
    }
}
