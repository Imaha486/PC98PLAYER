// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  avsdrv.cpp  --  内蔵の AVSDRV.SYS（PC-9821 の PCM ドライバ、INT D9h）
//
//  AVSDRV.SYS は MS-DOS に付いてくるファイルで、ゲームのディスクには入っていないことが多い。
//  ゲームの CONFIG.SYS に DEVICE=AVSDRV.SYS があれば（または INI の AVSDRV=1）、代わりにこれを用意する。
//   ・装置 AVSDRV$$（ゲームはこの名前を開けるかで、ドライバが組み込まれているかを見る）
//   ・INT D9h（AH が機能の番号。失敗は CF=1・AH=エラー番号）
//  中身はゲームの使い方から決めたもの（公開された資料は見つからない）。
//   AH=10h  ES:BX の「バッファの記述子」を再生の待ち行列に入れる（鳴っていなければすぐ鳴らす）
//   AH=11h  止めて、待ち行列を空にする
//   AH=12h  AX の bit0 = 鳴っている、ES:BX = 鳴っている記述子
//   そのほか（初期化 00h・設定 05h/07h/09h/0Bh など）は、成功を返すだけ
//  記述子（16h バイト）:
//   +00h  形式（bit7: 16 ビット、bit0 と bit1 が両方 1 ならステレオ）
//   +01h  標本化周波数の番号（PC-9801-86 の PCM と同じ 0=44.1kHz … 3=16.5kHz … 7=4.1kHz）
//   +03h  状態（鳴らし終えたら 01h にする）
//   +06h  バッファ（オフセット・セグメント）
//   +0Ah  長さ（バイト、32 ビット）
//   +12h  鳴らし終えたバイト数（32 ビット。鳴っている間、ドライバが 800h バイト単位で書き換える）
//  16 ビットの標本は上位バイトが先（PC-9801-86 の FIFO と同じ）。
//  音は PC-9801-86 の PCM の FIFO へ流し込んで鳴らす（音量・フィルタはそちらのものを使う）。
// -----------------------------------------------------------------------------
#include "machine.h"
#include <string.h>

#define ROMSEG 0xF000

uint16_t bios_rom_alloc(Machine* m, int n);
uint16_t bios_hle_stub(Machine* m, uint8_t n);

static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }
static inline uint8_t rb(Machine* m, uint32_t a) { return m->ram[a & 0xFFFFF]; }
static inline uint16_t rw(Machine* m, uint32_t a) { return (uint16_t)(rb(m, a) | (rb(m, a + 1) << 8)); }
static inline uint32_t rd(Machine* m, uint32_t a) { return rw(m, a) | ((uint32_t)rw(m, a + 2) << 16); }
static inline void wb(Machine* m, uint32_t a, uint8_t v) { if ((a & 0xFFFFF) < 0xA0000) m->ram[a & 0xFFFFF] = v; }
static inline void wd(Machine* m, uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) wb(m, a + i, (uint8_t)(v >> (i * 8))); }

static const int k_fifo_keep = 1024;   // FIFO にためておく量（これより多いと再生位置の報告が遅れる）

void avsdrv_reset(Machine* m) {
    uint16_t ds = m->avs.dev_seg;
    uint8_t on = m->avs.on;
    memset(&m->avs, 0, sizeof(m->avs));
    m->avs.dev_seg = ds; m->avs.on = on;
}

void avsdrv_install(Machine* m) {
    if (m->avs.on) return;
    avsdrv_reset(m);
    // INT D9h
    uint16_t o = bios_hle_stub(m, HLE_AVSDRV);
    m->ram[0xD9 * 4 + 0] = (uint8_t)o; m->ram[0xD9 * 4 + 1] = (uint8_t)(o >> 8);
    m->ram[0xD9 * 4 + 2] = (uint8_t)ROMSEG; m->ram[0xD9 * 4 + 3] = (uint8_t)(ROMSEG >> 8);
    // 装置ヘッダ（文字装置 AVSDRV$$）。戦略・割込みルーチンは RETF だけ。NUL の次につなぐ
    uint16_t h = bios_rom_alloc(m, 0x20);
    uint32_t b = lin(ROMSEG, h);
    uint32_t nul = dos_nul_addr();
    m->ram[b + 0] = m->ram[nul + 0]; m->ram[b + 1] = m->ram[nul + 1];
    m->ram[b + 2] = m->ram[nul + 2]; m->ram[b + 3] = m->ram[nul + 3];
    m->ram[b + 4] = 0x00; m->ram[b + 5] = 0x80;          // 属性: 文字装置
    m->ram[b + 6] = (uint8_t)(h + 0x12); m->ram[b + 7] = (uint8_t)((h + 0x12) >> 8);
    m->ram[b + 8] = (uint8_t)(h + 0x12); m->ram[b + 9] = (uint8_t)((h + 0x12) >> 8);
    memcpy(m->ram + b + 0x0A, "AVSDRV$$", 8);
    m->ram[b + 0x12] = 0xCB;                              // RETF
    m->ram[nul + 0] = (uint8_t)h; m->ram[nul + 1] = (uint8_t)(h >> 8);
    m->ram[nul + 2] = (uint8_t)ROMSEG; m->ram[nul + 3] = (uint8_t)(ROMSEG >> 8);
    m->avs.dev_seg = ROMSEG;
    m->avs.on = 1;
    plog("[avs] 内蔵の AVSDRV.SYS（INT D9h）を用意しました\n");
}

// 記述子の形式
static void desc_fmt(Machine* m, uint16_t seg, uint16_t off, bool* b16, bool* st, int* rate) {
    uint32_t d = lin(seg, off);
    uint8_t f = rb(m, d);
    *b16 = (f & 0x80) != 0;
    *st = (f & 3) == 3;
    *rate = rb(m, d + 1) & 7;
}

static void start_hw(Machine* m) {
    bool b16, st; int rate;
    desc_fmt(m, m->avs.q_seg[0], m->avs.q_off[0], &b16, &st, &rate);
    Pcm86& p = m->pcm86;
    p.rpos = p.wpos = 0; p.count = 0; p.irqflag = 0;
    p.dactrl = (uint8_t)(0x30 | (b16 ? 0 : 0x40));   // いつもステレオ（モノラルは左右に同じ標本）
    p.ctrl = (uint8_t)(0x80 | rate);
    p.acc = 0;
    m->avs.ratio = st ? 1 : 2;
    m->avs.playing = 1;
}

static void pop_front(Machine* m) {
    AvsDrv& a = m->avs;
    if (!a.qn) return;
    uint32_t d = lin(a.q_seg[0], a.q_off[0]);
    wd(m, d + 0x12, rd(m, d + 0x0A));
    wb(m, d + 3, 0x01);
    for (int i = 1; i < a.qn; i++) { a.q_seg[i - 1] = a.q_seg[i]; a.q_off[i - 1] = a.q_off[i]; a.q_start[i - 1] = a.q_start[i]; }
    a.qn--;
}

// 鳴らし終えた（スピーカーへ出た）通しのバイト数
static uint32_t played(Machine* m) {
    uint32_t inf = (uint32_t)m->pcm86.count / (m->avs.ratio ? m->avs.ratio : 1);
    return m->avs.fed > inf ? m->avs.fed - inf : 0;
}

static void update_pos(Machine* m) {
    AvsDrv& a = m->avs;
    uint32_t pl = played(m);
    while (a.qn) {
        uint32_t d = lin(a.q_seg[0], a.q_off[0]);
        uint32_t len = rd(m, d + 0x0A);
        if (pl - a.q_start[0] >= len) { pop_front(m); continue; }
        // 再生位置は 2KB（800h バイト）単位で知らせる。実機のドライバは FIFO へ 2KB ずつ送るので、
        // 位置もその単位で進む。ゲームには「前に見たときから 2KB の境目をいくつ越えたか」を
        // (今 - 前) & 3800h で数えるものがあり、細かく進めると 1 回の差が 2KB に届かず、
        // 数えそこねる（声が終わっても会話が先へ進まない）
        wd(m, d + 0x12, (pl - a.q_start[0]) & ~0x7FFu);
        break;
    }
}

void avsdrv_advance(Machine* m, uint32_t t) {
    (void)t;
    AvsDrv& a = m->avs;
    if (!a.playing) return;
    Pcm86& p = m->pcm86;
    // FIFO を補う
    while (p.count < k_fifo_keep) {
        int qi = -1;
        uint32_t fed = a.fed;
        for (int i = 0; i < a.qn; i++) {
            uint32_t d = lin(a.q_seg[i], a.q_off[i]);
            if (fed - a.q_start[i] < rd(m, d + 0x0A)) { qi = i; break; }
        }
        if (qi < 0) break;
        uint32_t d = lin(a.q_seg[qi], a.q_off[qi]);
        bool b16, st; int rate;
        desc_fmt(m, a.q_seg[qi], a.q_off[qi], &b16, &st, &rate);
        uint32_t buf = lin(rw(m, d + 8), rw(m, d + 6));
        uint32_t len = rd(m, d + 0x0A);
        uint32_t i = fed - a.q_start[qi];
        int fr = b16 ? 2 : 1;
        if (st) fr *= 2;
        uint32_t n = 0;
        while (i + fr <= len && p.count + 4 <= 0x8000 && p.count < k_fifo_keep) {
            for (int k = 0; k < (st ? 1 : 2); k++)
                for (int j = 0; j < fr; j++) { p.fifo[p.wpos] = rb(m, buf + i + j); p.wpos = (p.wpos + 1) & 0x7FFF; p.count++; }
            i += fr; n += fr;
        }
        if (i + fr > len && i < len) n += len - i;   // 半端は飛ばす
        a.fed += n;
        if (n == 0) break;
    }
    update_pos(m);
    if (!a.qn && p.count == 0) { a.playing = 0; p.ctrl &= (uint8_t)~0x80; }
}

void avsdrv_hle(Machine* m) {
    AvsDrv& a = m->avs;
    Cpu* c = &m->cpu;
    uint16_t ax = (uint16_t)c->r[EAX];
    uint8_t ah = (uint8_t)(ax >> 8);
    static int seen[256];
    if (m->cfg.trace && seen[ah] < 8) { seen[ah]++; plog("[avs] INT D9h AX=%04X BX=%04X CX=%04X DX=%04X ES=%04X\n", ax, (unsigned)(uint16_t)c->r[EBX], (unsigned)(uint16_t)c->r[ECX], (unsigned)(uint16_t)c->r[EDX], c->sr[ES_]); }
    auto ok = [&](uint16_t v) { c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | v; set_cf(m, false); };
    switch (ah) {
    case 0x10: {   // 待ち行列に入れる
        if (a.qn >= 8) { c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | 0x0100; set_cf(m, true); return; }
        uint16_t es = c->sr[ES_], bx = (uint16_t)c->r[EBX];
        uint32_t d = lin(es, bx);
        wd(m, d + 0x12, 0); wb(m, d + 3, 0);
        if (!a.playing) { a.qn = 0; a.fed = 0; }
        uint32_t st = a.fed;
        if (a.qn) { uint32_t pd = lin(a.q_seg[a.qn - 1], a.q_off[a.qn - 1]); st = a.q_start[a.qn - 1] + rd(m, pd + 0x0A); }
        a.q_seg[a.qn] = es; a.q_off[a.qn] = bx; a.q_start[a.qn] = st; a.qn++;
        if (!a.playing) start_hw(m);
        avsdrv_advance(m, 0);
        ok(0);
        return; }
    case 0x11:     // 止める
        a.qn = 0; a.playing = 0; a.fed = 0;
        m->pcm86.ctrl &= (uint8_t)~0x80; m->pcm86.count = 0; m->pcm86.rpos = m->pcm86.wpos = 0;
        ok(0);
        return;
    case 0x12:     // 状態
        update_pos(m);
        if (a.playing && a.qn) { cpu_setsr(c, ES_, a.q_seg[0]); c->r[EBX] = (c->r[EBX] & 0xFFFF0000u) | a.q_off[0]; ok(1); }
        else ok(0);
        return;
    case 0x00:     // 初期化
        a.qn = 0; a.playing = 0; a.fed = 0;
        m->pcm86.ctrl &= (uint8_t)~0x80;
        ok(0);
        return;
    default:
        ok(0);
        return;
    }
}
