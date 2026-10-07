// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  machine.cpp  --  仮想 PC-9801 のデバイスと時間軸
// -----------------------------------------------------------------------------
#include "machine.h"
#include "floppy.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

Machine* g_m = nullptr;
uint8_t* g_ram = nullptr;
uint32_t g_addr_mask = 0xFFFFF;
uint32_t g_side_fx = 0;
extern int g_irq_hint;

void egc_reset(Machine* m);
uint16_t egc_read_w(Machine* m, int plane, uint32_t off);
uint8_t  egc_read_b(Machine* m, int plane, uint32_t off);
void egc_write_w(Machine* m, int plane, uint32_t off, uint16_t v);
void egc_write_b(Machine* m, int plane, uint32_t off, uint8_t v);
static inline bool egc_on(Machine* m) { return m->egc_enabled && (m->grcg_mode & 0x80); }
void egc_out(Machine* m, uint16_t port, uint8_t v);
void egc_out16(Machine* m, uint16_t port, uint16_t v);
void gdc_draw_command(Machine* m, Gdc* g);

static FILE* s_log = nullptr;
void plog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!s_log) {
        const char* p = getenv("PC98PLAYER_LOG");
        if (p && *p) s_log = fopen(p, "w");
    }
    if (s_log) { vfprintf(s_log, fmt, ap); fflush(s_log); }
    else vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// ---- 日付 --------------------------------------------------------------------
#include <time.h>
static int days_from_civil(int y, int m, int d) {           // 1970-01-01 からの日数
    y -= m <= 2; int era = (y >= 0 ? y : y - 399) / 400; int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(int z, int* y, int* m, int* d) {
    z += 719468; int era = (z >= 0 ? z : z - 146096) / 146097; int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int yy = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100); int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1; *m = mp + (mp < 10 ? 3 : -9); *y = yy + (*m <= 2);
}
static int s_start_days = -1;   // 起動した日（実際の日付）
void machine_now(Machine* m, PcTime* o) {
    time_t now = time(nullptr); struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    int y = lt.tm_year + 1900, mo = lt.tm_mon + 1, d = lt.tm_mday;
    int today = days_from_civil(y, mo, d);
    if (s_start_days < 0) s_start_days = today;
    if (m && m->cfg.fake_date > 0) {
        int fy = m->cfg.fake_date / 10000, fm = (m->cfg.fake_date / 100) % 100, fd = m->cfg.fake_date % 100;
        civil_from_days(days_from_civil(fy, fm, fd) + (today - s_start_days), &y, &mo, &d);
    } else if (m && m->cfg.fake_year > 0) {
        y = m->cfg.fake_year;
        bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        if (mo == 2 && d == 29 && !leap) d = 28;
    }
    int z = days_from_civil(y, mo, d);
    o->year = y; o->month = mo; o->day = d; o->wday = ((z % 7) + 11) % 7;   // 1970-01-01 は木曜
    o->hour = lt.tm_hour; o->min = lt.tm_min; o->sec = lt.tm_sec;
}
uint16_t machine_file_date(Machine* m, uint16_t dd) {
    if (!m || (m->cfg.fake_date <= 0 && m->cfg.fake_year <= 0)) return dd;
    PcTime t; machine_now(m, &t);
    uint16_t today = (uint16_t)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    if (t.year < 1980) return 0x0021;
    return dd > today ? today : dd;
}

// ---- PIC --------------------------------------------------------------------
void pic_update_hint(Machine* m) {
    uint8_t p0 = m->pic[0].irr & ~m->pic[0].imr;
    uint8_t p1 = m->pic[1].irr & ~m->pic[1].imr;
    g_irq_hint = (p0 || p1) ? 1 : 0;
}
void machine_raise_irq(Machine* m, int irq) {
    if (irq < 0 || irq > 15) return;
    if (irq < 8) m->pic[0].irr |= (uint8_t)(1u << irq);
    else { m->pic[1].irr |= (uint8_t)(1u << (irq - 8)); m->pic[0].irr |= 0x80; }
    pic_update_hint(m);
}
void machine_eoi(Machine* m, int irq) {
    if (irq >= 8) {
        m->pic[1].isr &= (uint8_t)~(1u << (irq - 8));
        if (!m->pic[1].isr) m->pic[0].isr &= (uint8_t)~0x80;
    } else m->pic[0].isr &= (uint8_t)~(1u << irq);
}

// 優先順位: マスタの IR0 が最高。ISR に立っているより低い優先度は待たせる。
unsigned g_irq_count[16];
int pic_acknowledge(Machine* m) {
    Pic* p0 = &m->pic[0];
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (p0->isr & bit) {
            if (i != 7) break;            // より高い優先度が処理中
            // IR7（スレーブ）が処理中でも、PC-98 のマスタは特殊完全入れ子モード（ICW4=1Dh）なので、
            // スレーブの中で処理中のものより優先度の高い要求は通す（下のスレーブの走査がそれを判定する）。
            // スレーブ側だけ EOI してマスタの EOI を条件付きにするハンドラ（スレイヤーズのマウス処理など）で、
            // マスタの IR7 が処理中のまま残っても、音源やマウスの割込みが止まらないように。
        }
        if (!(p0->irr & bit) || (p0->imr & bit)) { if (!(i == 7 && (p0->isr & bit))) continue; }
        if (i == 7) {
            Pic* p1 = &m->pic[1];
            for (int j = 0; j < 8; j++) {
                uint8_t b2 = (uint8_t)(1u << j);
                if (p1->isr & b2) break;
                if ((p1->irr & b2) && !(p1->imr & b2)) {
                    p1->irr &= (uint8_t)~b2;
                    if (!p1->aeoi) p1->isr |= b2;
                    if (!(p1->irr & ~p1->imr)) p0->irr &= (uint8_t)~0x80;
                    if (!p0->aeoi) p0->isr |= 0x80;
                    pic_update_hint(m);
                    g_irq_count[8 + j]++;
                    return p1->base + j;
                }
            }
            continue;
        }
        p0->irr &= (uint8_t)~bit;
        if (!p0->aeoi) p0->isr |= bit;
        pic_update_hint(m);
        g_irq_count[i]++;
        return p0->base + i;
    }
    return -1;
}

static void pic_write(Machine* m, int c, int a1, uint8_t v) {
    Pic* p = &m->pic[c];
    if (!a1) {
        if (v & 0x10) {                  // ICW1
            p->icw_step = 1; p->icw4_needed = v & 1;
            p->imr = 0; p->isr = 0; p->irr = 0;
        } else if ((v & 0x18) == 0x08) { // OCW3
            if (v & 2) p->read_isr = v & 1;
        } else {                          // OCW2
            int cmd = v >> 5;
            // 1=非特定 EOI、5=回転つき非特定 EOI（0A0h）、3=特定 EOI、7=回転つき特定 EOI。
            // 優先順位の回転は扱わず、EOI としてだけ働かせる（R-FORCE の PCML は 0A0h を使う）
            if (cmd == 1 || cmd == 5) {
                for (int i = 0; i < 8; i++) if (p->isr & (1u << i)) { p->isr &= (uint8_t)~(1u << i); break; }
            } else if (cmd == 3 || cmd == 7) p->isr &= (uint8_t)~(1u << (v & 7));
        }
    } else {
        switch (p->icw_step) {
        case 1: p->base = v & 0xF8; p->icw_step = 2; break;
        case 2: p->icw_step = p->icw4_needed ? 3 : 0; break;
        case 3: p->aeoi = (v & 2) ? 1 : 0; p->icw_step = 0; break;
        default: p->imr = v; break;
        }
    }
    pic_update_hint(m);
}
static uint8_t pic_read(Machine* m, int c, int a1) {
    Pic* p = &m->pic[c];
    if (a1) return p->imr;
    return p->read_isr ? p->isr : p->irr;
}

// ---- PIT --------------------------------------------------------------------
static void pit_write(Machine* m, int ch, uint8_t v) {
    Pit* t = &m->pit[ch];
    if (t->access == 3) {
        if (!t->wr_hi) { t->reload = (uint16_t)((t->reload & 0xFF00) | v); t->wr_hi = 1; return; }
        t->reload = (uint16_t)((t->reload & 0x00FF) | (v << 8)); t->wr_hi = 0;
    } else if (t->access == 2) t->reload = (uint16_t)(v << 8);
    else t->reload = v;
    t->counter = t->reload;
    t->armed = 1;
    // ブザー（ch1）: 周波数変化（いつ変わったかも残す。BEEP の時分割和音のため）。
    // モード 0/1（ワンショット）は、書いた時から数え終わるまで出力が L になる。タイマ割込みのたびに
    // 数を書き換えてパルス幅で音を出す「BEEP の PCM 再生」（ルパン９８ など）なので別の印で残す
    if (ch == 1) m->beepw.push_back({m->ticks, ((t->mode == 0 || t->mode == 1) ? 0x200000 : 0x100000) | t->reload});
}
static uint8_t pit_read(Machine* m, int ch) {
    Pit* t = &m->pit[ch];
    uint16_t v = t->latched ? t->latch : t->counter;
    uint8_t r;
    if (t->access == 3) {
        r = t->rd_hi ? (uint8_t)(v >> 8) : (uint8_t)v;
        t->rd_hi ^= 1;
        if (!t->rd_hi) t->latched = 0;
    } else if (t->access == 2) { r = (uint8_t)(v >> 8); t->latched = 0; }
    else { r = (uint8_t)v; t->latched = 0; }
    return r;
}
static void pit_ctrl(Machine* m, uint8_t v) {
    int ch = v >> 6;
    if (ch == 3) return;
    Pit* t = &m->pit[ch];
    int acc = (v >> 4) & 3;
    if (acc == 0) { t->latch = t->counter; t->latched = 1; t->rd_hi = 0; return; }
    t->access = (uint8_t)acc; t->mode = (uint8_t)((v >> 1) & 7);
    if (t->mode > 5) t->mode &= 3;
    t->wr_hi = 0; t->rd_hi = 0;
    t->armed = 0;
    // ブザー（ch1）: 制御語を書いた時点で出力は止まる（モード 0/1 は L、ほかは H のまま）。数を書くまで鳴らない。
    // 「8255 でブザーを ON にし、ch1 をモード 0 にしただけ」で数を書かないソフト（MASL の 86 PCM 再生）が、
    // BIOS の 2kHz を鳴らし続けないように
    if (ch == 1) m->beepw.push_back({m->ticks, 0x400000 | ((t->mode == 0 || t->mode == 1) ? 1 : 0)});
}

// ---- OPNA -------------------------------------------------------------------
static void opna_recalc(Machine* m) {
    const uint8_t* R = m->opna.reg[0];
    uint32_t na = (uint32_t)(((R[0x24] << 2) | (R[0x25] & 3)) & 0x3FF);
    uint32_t nb = R[0x26];
    uint32_t pre = m->opna.prescale ? m->opna.prescale : 6;
    uint32_t k = 24 * pre;
    m->opna.timer_a_period = (int32_t)(k * (1024u - na));
    m->opna.timer_b_period = (int32_t)(k * 16u * (256u - nb));
    if (m->opna.timer_a_period <= 0) m->opna.timer_a_period = (int32_t)k;
    if (m->opna.timer_b_period <= 0) m->opna.timer_b_period = (int32_t)(k * 16u);
}
unsigned g_opna_writes = 0, g_opna_keyon = 0, g_opna_hist[2][256];
void opna_write(Machine* m, int part, uint8_t addr, uint8_t val) {
    g_opna_hist[part][addr]++;
    g_opna_writes++; if (part == 0 && addr == 0x28 && (val & 0xF0)) g_opna_keyon++;
    m->opna.reg[part][addr] = val;
    if (part == 0 && addr == 0x28) m->opna.keyon[val & 7] = (uint8_t)(val & 0xF0);
    if (part == 0) {
        if (addr == 0x2D) { m->opna.prescale = 6; opna_recalc(m); }
        if (addr == 0x2E) { m->opna.prescale = 3; opna_recalc(m); }
        if (addr == 0x2F) { m->opna.prescale = 2; opna_recalc(m); }
        if (addr == 0x24 || addr == 0x25 || addr == 0x26) opna_recalc(m);
        if (addr == 0x27) {
            opna_recalc(m);
            m->opna.irq_enable_a = (val >> 2) & 1;
            m->opna.irq_enable_b = (val >> 3) & 1;
            uint8_t ra = val & 1, rb = (val >> 1) & 1;
            if (ra && !m->opna.timer_a_run) m->opna.timer_a_count = m->opna.timer_a_period;
            if (rb && !m->opna.timer_b_run) m->opna.timer_b_count = m->opna.timer_b_period;
            m->opna.timer_a_run = ra; m->opna.timer_b_run = rb;
            if (val & 0x10) m->opna.status &= (uint8_t)~1;
            if (val & 0x20) m->opna.status &= (uint8_t)~2;
        }
    }
    if (m->cfg.fm_enable) m->regw.push_back({m->ticks, (uint8_t)part, addr, val});
}
static uint8_t opna_read_data(Machine* m, int part) {
    uint8_t a = m->opna.addr[part];
    if (part == 0) {
        if (a == 0xFF) return m->cfg.sound_board == 86 ? 0x01 : 0x00;   // YM2608 ID
        if (a == 0x0E) {
            // SSG I/O ポート A: 上位 2bit が割込みジャンパ、下位はジョイスティック（押していない=1）
            // 00=INT0(IRQ3) 01=INT6(IRQ13) 10=INT41(IRQ10) 11=INT5(IRQ12)
            int irqsel = 3;
            switch (m->cfg.sound_irq) { case 3: irqsel = 0; break; case 13: irqsel = 1; break;
                                         case 10: irqsel = 2; break; case 12: irqsel = 3; break; }
            // ポート B（reg 0Fh）の bit6 で 1P / 2P を選ぶ。2P は繋がっていないことにする
            uint8_t j = (m->opna.reg[0][0x0F] & 0x40) ? 0 : m->joy;
            return (uint8_t)((irqsel << 6) | (~j & 0x3F));
        }
        if (a == 0x0F) return 0xFF;
        if (a < 0x10) return m->opna.reg[0][a];
        return m->opna.reg[0][a];
    }
    return m->opna.reg[1][a];
}

// ---- GDC --------------------------------------------------------------------
static int gdc_param_count(uint8_t cmd) {
    if (cmd == 0x0E || cmd == 0x0F || cmd == 0x00) return 8;
    if (cmd == 0x4B) return 3;
    if (cmd == 0x47 || cmd == 0x46) return 1;
    if (cmd == 0x49) return 3;
    if (cmd == 0x4C) return 11;
    if (cmd == 0x4A) return 2;    // MASK
    if ((cmd & 0xF0) == 0x70) return 16;
    if ((cmd & 0xE4) == 0x20) return 2;   // WDAT
    return 0;
}
static void gdc_command(Machine* m, Gdc* g, uint8_t cmd) {
    g->cmd = cmd; g->pcount = 0;
    if (cmd == 0x0C) g->display = 0;
    else if (cmd == 0x0D || cmd == 0x6B) g->display = 1;
    else if ((cmd & 0xF0) == 0x70) g->pram_ptr = cmd & 0x0F;
    else if ((cmd & 0xE4) == 0x20) g->mode_write = cmd & 3;         // WDAT: 描き方も決まる
    else if (cmd == 0x6C || cmd == 0x68) gdc_draw_command(m, g);   // VECTE / TEXTE
    else if (cmd == 0xE0) {   // CSRR
        g->fifo.clear();
        g->fifo.push_back((uint8_t)g->ead); g->fifo.push_back((uint8_t)(g->ead >> 8));
        g->fifo.push_back((uint8_t)((g->ead >> 16) & 3)); g->fifo.push_back(0); g->fifo.push_back(0);
    }
}
static void gdc_param(Machine* m, Gdc* g, uint8_t v) {
    uint8_t c = g->cmd;
    int n = g->pcount++;
    if (c == 0x0E || c == 0x0F || c == 0x00) { if (n < 8) g->sync[n] = v; if (c == 0x00) g->display = 0; }
    else if (c == 0x4B) { if (n < 3) g->csrform[n] = v; }
    else if (c == 0x47) g->pitch = v;
    else if (c == 0x46) g->zoom = v;
    else if (c == 0x49) {
        if (n == 0) g->ead = (g->ead & ~0xFFu) | v;
        else if (n == 1) g->ead = (g->ead & ~0xFF00u) | ((uint32_t)v << 8);
        else if (n == 2) { g->ead = (g->ead & 0xFFFF) | ((uint32_t)(v & 3) << 16); g->dad = v >> 4; }
    }
    else if (c == 0x4A) {   // MASK: 1 になっているビットが描き始めの点
        if (n == 0) g->pattern = (uint16_t)((g->pattern & 0xFF00) | v); else g->pattern = (uint16_t)((g->pattern & 0xFF) | (v << 8));
        if (n == 1 && g->pattern) { int b = 0; while (!(g->pattern & (1 << b))) b++; g->dad = (uint8_t)b; }
    }
    else if (c == 0x4C) { if (n < 11) g->vect[n] = v; }
    else if ((c & 0xF0) == 0x70) { g->pram[g->pram_ptr & 15] = v; g->pram_ptr++; }
    else if ((c & 0xE4) == 0x20) {
        // WDAT: 1 語ずつ書き込み（グラフィック GDC のみ意味を持つ）
        g->params[n & 15] = v;
        if (g == &m->gdcs && ((n & 1) || (c & 0x18) != 0x00)) {
            g->params[15] = c; gdc_draw_command(m, g);
        }
    }
}
static uint8_t gdc_status(Machine* m, Gdc* g) {
    uint32_t line = m->frame_pos / LINE_TICKS;
    uint32_t inl = m->frame_pos % LINE_TICKS;
    uint8_t st = 0x04;                                 // FIFO 空
    if (line >= DISPLAY_LINES) st |= 0x20;             // VSYNC
    if (inl >= LINE_TICKS * 80 / 100) st |= 0x40;      // HBLANK
    if (!g->fifo.empty()) st |= 0x01;
    return st;
}

// ---- キーボード / マウス ----------------------------------------------------
void machine_key(Machine* m, uint8_t sc, bool down) {
    if (sc >= 0x80) return;
    if (down && m->kb_down[sc] && !m->cfg.key_repeat) return;
    m->kb_down[sc] = down;
    m->kb_queue.push_back((uint8_t)(sc | (down ? 0 : 0x80)));
}
void machine_mouse(Machine* m, int dx, int dy, int buttons) {
    // バスマウスのカウンタへは 1 回に ±100 まで（8 ビットのカウンタが一度に一周して向きが逆に見えないように）
    m->mouse.acc_x += dx > 100 ? 100 : dx < -100 ? -100 : dx;
    m->mouse.acc_y += dy > 100 ? 100 : dy < -100 ? -100 : dy;
    // HC を立てないゲームでは数え直されないので、下位 8 ビットを保ったまま大きくなりすぎないようにする
    if (m->mouse.acc_x > 0x10000 || m->mouse.acc_x < -0x10000) m->mouse.acc_x %= 256;
    if (m->mouse.acc_y > 0x10000 || m->mouse.acc_y < -0x10000) m->mouse.acc_y %= 256;
    m->mouse.hle_dx += dx; m->mouse.hle_dy += dy;
    Mouse* ms = &m->mouse;
    ms->x += dx; ms->y += dy;
    if (ms->x < ms->minx) ms->x = ms->minx; if (ms->x > ms->maxx) ms->x = ms->maxx;
    if (ms->y < ms->miny) ms->y = ms->miny; if (ms->y > ms->maxy) ms->y = ms->maxy;
    if (dx || dy) m->mdrv.events |= 1;
    m->mdrv.mick_x += dx; m->mdrv.mick_y += dy;
    for (int b = 0; b < 2; b++) {
        bool was = (ms->buttons >> b) & 1, now = (buttons >> b) & 1;
        if (now && !was) { ms->press_cnt[b]++; ms->press_x[b] = ms->x; ms->press_y[b] = ms->y; m->mdrv.events |= (uint16_t)(b ? 8 : 2); }
        if (!now && was) { ms->release_cnt[b]++; ms->release_x[b] = ms->x; ms->release_y[b] = ms->y; m->mdrv.events |= (uint16_t)(b ? 16 : 4); }
    }
    ms->buttons = (uint8_t)buttons;
}
void machine_joystick(Machine* m, uint8_t bits) { m->joy = (uint8_t)(bits & 0x3F); }

static uint8_t mouse_porta(Machine* m) {
    Mouse* ms = &m->mouse;
    uint8_t v = 0x40;
    if (!(ms->buttons & 1)) v |= 0x80;
    if (!(ms->buttons & 2)) v |= 0x20;
    int sel = (ms->portc >> 5) & 3;
    // HC（ポート C bit7）=1 の間は立ち上がりで保持した値、0 の間は今の移動量をそのまま読む。
    // 読み終えたら HC を立てて数え直すドライバ（瑠璃色の雪など）と、先に HC を立てて読むドライバの両方に合う
    int8_t lx, ly;
    if (ms->portc & 0x80) { lx = ms->lat_x; ly = ms->lat_y; }
    else {
        // HC=0 の間は実機の 8 ビットのカウンタそのもの（あふれたら一周する）。
        // HC を一度も立てず、前回読んだ値との差（256 で割った余り）で動きを求めるゲームがある（プリンセスメーカー2）。
        // ここで ±127 に止めると、動ける範囲が画面の一部に限られてしまう
        lx = (int8_t)(uint8_t)(ms->acc_x & 0xFF);
        ly = (int8_t)(uint8_t)(ms->acc_y & 0xFF);
    }
    uint8_t nib;
    switch (sel) {
    case 0: nib = (uint8_t)lx & 15; break;
    case 1: nib = ((uint8_t)lx >> 4) & 15; break;
    case 2: nib = (uint8_t)ly & 15; break;
    default: nib = ((uint8_t)ly >> 4) & 15; break;
    }
    return (uint8_t)(v | nib);
}
static void mouse_set_portc(Machine* m, uint8_t v) {
    Mouse* ms = &m->mouse;
    uint8_t old = ms->portc;
    ms->portc = v;
    if ((v & 0x80) && !(old & 0x80)) {
        int x = ms->acc_x, y = ms->acc_y;
        if (x > 127) x = 127; if (x < -128) x = -128;
        if (y > 127) y = 127; if (y < -128) y = -128;
        ms->lat_x = (int8_t)x; ms->lat_y = (int8_t)y;
        ms->acc_x -= x; ms->acc_y -= y;
    }
}

// ---- メモリ（0xA0000 以上）-------------------------------------------------
static inline int plane_of(uint32_t a, uint32_t* off) {
    if (a >= 0xA8000 && a < 0xB0000) { *off = a - 0xA8000; return 0; }
    if (a >= 0xB0000 && a < 0xB8000) { *off = a - 0xB0000; return 1; }
    if (a >= 0xB8000 && a < 0xC0000) { *off = a - 0xB8000; return 2; }
    if (a >= 0xE0000 && a < 0xE8000) { *off = a - 0xE0000; return 3; }
    return -1;
}

static uint8_t grcg_read(Machine* m, uint32_t off) {
    uint8_t r = 0xFF;
    uint8_t (*pl)[0x8000] = m->gvram[m->draw_bank];
    for (int p = 0; p < 4; p++) {
        if (m->grcg_mode & (1 << p)) continue;
        r &= (uint8_t)~(pl[p][off] ^ m->grcg_tile[p]);
    }
    return r;
}
static void grcg_write(Machine* m, uint32_t off, uint8_t v) {
    uint8_t (*pl)[0x8000] = m->gvram[m->draw_bank];
    bool rmw = (m->grcg_mode & 0x40) != 0;
    for (int p = 0; p < 4; p++) {
        if (m->grcg_mode & (1 << p)) continue;
        if (rmw) pl[p][off] = (uint8_t)((pl[p][off] & ~v) | (m->grcg_tile[p] & v));
        else pl[p][off] = m->grcg_tile[p];
    }
}

static uint8_t cg_read(Machine* m, uint32_t idx);
static void cg_write(Machine* m, uint32_t idx, uint8_t v);

// ---- MPU-PC98II -------------------------------------------------------------
static void mpu_push(Machine* m, uint8_t v) {
    Mpu& u = m->mpu;
    if (u.rcnt < sizeof(u.recv)) { u.recv[(u.rpos + u.rcnt) & 15] = v; u.rcnt++; }
    machine_raise_irq(m, m->cfg.midi_irq);
}
static void mpu_defaults(Mpu& u) {
    u.tempo = 100; u.timebase = 120; u.cth_rate = 240; u.cth_on = 0; u.cth_acc = 0; u.rel_tempo = 0x40;
    u.expect = 0; u.msg_left = 0; u.sysex = 0; u.run_status = 0;
}
// 「クロック・トゥ・ホスト」（FDh）の間隔（基準クロック）: 内部クロック（tempo*timebase/60 Hz）の rate/4 回ごと
static int64_t mpu_cth_period(const Machine* m) {
    const Mpu& u = m->mpu;
    int tempo = u.tempo ? u.tempo : 100, tb = u.timebase ? u.timebase : 120;
    int rate = u.cth_rate ? u.cth_rate : 240;
    double ticks = rate / 4.0; if (ticks < 1) ticks = 1;
    double rel = (u.rel_tempo ? u.rel_tempo : 0x40) / 64.0;          // E1h: 相対テンポ
    double speed = (m->cfg.midi_speed > 0 ? m->cfg.midi_speed : 100) / 100.0;   // INI の MidiSpeedFix
    int64_t p = (int64_t)((double)MASTER_CLOCK * 60.0 / ((double)tempo * tb * rel * speed) * ticks);
    return p < 1 ? 1 : p;
}
static void mpu_command(Machine* m, uint8_t v) {
    Mpu& u = m->mpu;
    if (v == 0xFF) {                         // リセット
        bool was_uart = u.uart;
        u.uart = 0; u.pending_cmd = 0;
        mpu_defaults(u);
        // 全チャンネルの音を止める
        for (int ch = 0; ch < 16; ch++) { m->midi_out.push_back((uint8_t)(0xB0 | ch)); m->midi_out.push_back(0x7B); m->midi_out.push_back(0); }
        if (!was_uart) mpu_push(m, 0xFE);
        return;
    }
    if (u.uart) return;                      // UART モード中はリセット以外を受け付けない
    if (v == 0x3F) { u.uart = 1; mpu_push(m, 0xFE); return; }
    mpu_push(m, 0xFE);
    if (v == 0xAC) mpu_push(m, 0x15);        // 版数 1.5
    else if (v == 0xAD) mpu_push(m, 0x01);
    else if (v == 0xAF) mpu_push(m, u.tempo);
    else if (v >= 0xD0 && v <= 0xD7) { u.pending_cmd = v; u.msg_left = 0xFF; }   // 次に MIDI メッセージが 1 つ来る
    else if (v == 0xDF) { u.pending_cmd = v; u.sysex = 1; }
    else if (v >= 0xE0 && v <= 0xEF) u.expect = v;                                // 値が 1 バイト続く
    else if (v >= 0xC2 && v <= 0xC8) { static const uint8_t tb[7] = {48, 72, 96, 120, 144, 168, 192}; u.timebase = tb[v - 0xC2]; }
    else if (v == 0x95) { if (!u.cth_on) { u.cth_on = 1; u.cth_acc = MASTER_CLOCK / 1000; } }   // 最初の FDh はすぐ
    else if (v == 0x94) u.cth_on = 0;
}
// データポートへの書き込み（UART モードはそのまま MIDI、インテリジェントモードは文脈で振り分け）
static void mpu_data(Machine* m, uint8_t v) {
    Mpu& u = m->mpu;
    if (u.uart) { m->midi_out.push_back(v); return; }
    if (u.expect) {
        if (u.expect == 0xE0) { u.tempo = v ? v : 1; u.rel_tempo = 0x40; }   // テンポ設定で相対テンポは等倍に戻る
        else if (u.expect == 0xE1) u.rel_tempo = v ? v : 1;
        else if (u.expect == 0xE7) u.cth_rate = v;
        u.expect = 0; return;
    }
    if (u.sysex) { m->midi_out.push_back(v); if (v == 0xF7) { u.sysex = 0; u.pending_cmd = 0; } return; }
    if (u.msg_left) {
        if (u.msg_left == 0xFF) {                   // 1 バイト目: ステータスか、ランニングステータスのデータ
            uint8_t st = v;
            int total;
            if (v & 0x80) { u.run_status = v; m->midi_out.push_back(v); st = v; total = ((st & 0xE0) == 0xC0) ? 1 : 2; }
            else {
                st = u.run_status; if (!st) { u.msg_left = 0; return; }
                m->midi_out.push_back(st); m->midi_out.push_back(v);
                total = ((st & 0xE0) == 0xC0) ? 0 : 1;
            }
            u.msg_left = (uint8_t)total;
            if (!u.msg_left) u.pending_cmd = 0;
            return;
        }
        m->midi_out.push_back(v);
        if (--u.msg_left == 0) u.pending_cmd = 0;
        return;
    }
    // それ以外（トラックデータ等）は MIDI には送らない
}
void mpu_advance(Machine* m, uint32_t t) {
    Mpu& u = m->mpu;
    if (!m->cfg.midi || u.uart || !u.cth_on) return;
    u.cth_acc -= t;
    while (u.cth_acc <= 0) { u.cth_acc += mpu_cth_period(m); mpu_push(m, 0xFD); }
}
static uint8_t mpu_read_data(Machine* m) {
    Mpu& u = m->mpu;
    if (u.rcnt) { u.last = u.recv[u.rpos]; u.rpos = (u.rpos + 1) & 15; u.rcnt--; }
    return u.last;
}

uint8_t mem_rb_slow(Machine* m, uint32_t a) {
    g_side_fx++;
    if (a < 0xA4000) {
        if (a >= 0xA2000 && (a & 1)) return 0xFF;
        return m->tvram[a - 0xA0000];
    }
    if (a < 0xA5000) return cg_read(m, a - 0xA4000);
    if (a < 0xA8000) return 0xFF;
    uint32_t off;
    int p = plane_of(a, &off);
    if (p >= 0) {
        if (egc_on(m)) return egc_read_b(m, p, off);
        if (m->grcg_mode & 0x80) return grcg_read(m, off);
        return m->gvram[m->draw_bank][p][off];
    }
    if (a >= 0xD0000 && a < 0xE0000 && m->cfg.ems) return m->ram[a];   // EMS ページフレーム
    if (a >= 0xCC000 && a < 0xD0000 && m->cfg.sound_bios && m->cfg.sound_board) return m->ram[a];   // サウンド BIOS の ROM
    if (a < 0xE0000) return 0xFF;
    if (a < 0x100000) return m->ram[a];
    if (a < PC98_RAM_SIZE) return m->ram[a];
    return 0xFF;
}
uint32_t g_watch_lo = 0, g_watch_hi = 0;
void mem_wb_slow(Machine* m, uint32_t a, uint8_t v) {
    if (a >= g_watch_lo && a < g_watch_hi) plog("[w] %05X <- %02X grcg=%02X tile=%02X,%02X,%02X,%02X @%04X:%04X\n", a, v, m->grcg_mode, m->grcg_tile[0], m->grcg_tile[1], m->grcg_tile[2], m->grcg_tile[3], m->cpu.op_cs, m->cpu.op_ip);
    if (a < 0xA4000) {
        if (a >= 0xA2000 && (a & 1)) return;
        m->tvram[a - 0xA0000] = v;
        return;
    }
    if (a < 0xA5000) { cg_write(m, a - 0xA4000, v); return; }
    if (a < 0xA8000) return;
    uint32_t off;
    int p = plane_of(a, &off);
    if (p >= 0) {
        if (egc_on(m)) { egc_write_b(m, p, off, v); return; }
        if (m->grcg_mode & 0x80) { grcg_write(m, off, v); return; }
        m->gvram[m->draw_bank][p][off] = v;
        return;
    }
    if (a >= 0xD0000 && a < 0xE0000 && m->cfg.ems) { m->ram[a] = v; return; }   // EMS ページフレーム
    if (a < 0xE8000) return;
    if (a < 0x100000) return;                 // ROM
    if (a < PC98_RAM_SIZE) m->ram[a] = v;
}
uint16_t mem_rw_slow(Machine* m, uint32_t a) {
    g_side_fx++;
    uint32_t b = a & g_addr_mask;
    if (egc_on(m)) {
        uint32_t off, off2;
        int p = plane_of(b, &off);
        if (p >= 0 && plane_of(b + 1, &off2) == p) return egc_read_w(m, p, off);
    }
    return (uint16_t)(mem_rb(m, a) | (mem_rb(m, a + 1) << 8));
}
void mem_ww_slow(Machine* m, uint32_t a, uint16_t v) {
    uint32_t b = a & g_addr_mask;
    if (b + 1 >= g_watch_lo && b < g_watch_hi) plog("[w] %05X <- %04X (w) @%04X:%04X\n", b, v, m->cpu.op_cs, m->cpu.op_ip);
    if (egc_on(m)) {
        uint32_t off, off2;
        int p = plane_of(b, &off);
        if (p >= 0 && plane_of(b + 1, &off2) == p) { egc_write_w(m, p, off, v); return; }
    }
    mem_wb(m, a, (uint8_t)v); mem_wb(m, a + 1, (uint8_t)(v >> 8));
}

// ---- CG ROM（漢字 ROM）-------------------------------------------------------
// PC-98 の CG ポート: A1h=コード上位(JIS 第 2 バイト)、A3h=コード下位(JIS 第 1 バイト-20h)、
// A5h=ライン(bit0-3)と左右(bit5: 1=左, 0=右)、A9h=読み書き。
static inline uint16_t cg_jis(Machine* m) {
    uint8_t lo = m->cg_code & 0xFF, hi = m->cg_code >> 8;
    return (uint16_t)((((lo & 0x7F) + 0x20) << 8) | (hi & 0x7F));
}
static uint8_t cg_pattern(Machine* m) {
    uint8_t lo = m->cg_code & 0xFF, hi = m->cg_code >> 8;
    int line = m->cg_line & 15;
    if (hi == 0) return font_get_ank(lo)[line];
    const uint8_t* g = font_get_kanji(cg_jis(m));
    int row = lo & 0x7F;
    if (row >= 0x09 && row <= 0x0B) return m->cg_left ? g[line * 2] : 0;   // 半角の区は右半分が無い
    return m->cg_left ? g[line * 2] : g[line * 2 + 1];
}
// A4000h-A4FFFh: CG ウインドウ。偶数番地が「低い側」、奇数番地が「高い側」
static uint8_t cg_read(Machine* m, uint32_t idx) {
    uint8_t lo = m->cg_code & 0xFF, hi = m->cg_code >> 8;
    int line = (idx >> 1) & 15;
    bool odd = idx & 1;
    if (hi == 0) return odd ? font_get_ank(lo)[line] : 0;
    int row = lo & 0x7F;
    const uint8_t* g = font_get_kanji(cg_jis(m));
    if (row >= 0x09 && row <= 0x0B) return (odd && m->cg_left) ? g[line * 2] : 0;
    if ((row >= 0x0C && row <= 0x0F) || (row >= 0x56 && row <= 0x5F)) return odd ? g[line * 2 + (m->cg_left ? 0 : 1)] : 0;
    return odd ? g[line * 2 + 1] : g[line * 2];
}
static void cg_write(Machine* m, uint32_t idx, uint8_t v) {
    if (!(idx & 1)) return;
    uint16_t jis = cg_jis(m);
    if ((m->cg_code >> 8) && fontrom_is_gaiji(jis)) fontrom_gaiji_write(jis, (int)((idx >> 1) & 15), m->cg_left != 0, v);
}

// ---- I/O --------------------------------------------------------------------
static void sysport_c_write(Machine* m, uint8_t v) {
    uint8_t old = m->portc;
    m->portc = v;
    if ((old ^ v) & 0x08) m->beepw.push_back({m->ticks, (v & 0x08) ? 0 : 1});
}

int g_trace_port_lo = -1, g_trace_port_hi = -1;
static unsigned s_port_log = 0;
uint8_t io_in8_(Machine* m, uint16_t port);
static bool unknown_io_log(Machine* m, uint16_t port);
uint8_t io_in8(Machine* m, uint16_t port) {
    g_side_fx++;
    uint8_t v = io_in8_(m, port);
    if (port >= g_trace_port_lo && port <= g_trace_port_hi && s_port_log++ < 3000) plog("[in ] %04X -> %02X @%04X:%04X\n", port, v, m->cpu.op_cs, m->cpu.op_ip);
    return v;
}
uint8_t io_in8_(Machine* m, uint16_t port) {
    switch (port) {
    case 0x00: return pic_read(m, 0, 0);
    case 0x02: return pic_read(m, 0, 1);
    case 0x08: return pic_read(m, 1, 0);
    case 0x0A: return pic_read(m, 1, 1);
    case 0x41: m->kb_ready = 0; return m->kb_data;
    case 0x43: return (uint8_t)(0x85 | (m->kb_ready ? 0x02 : 0));
    case 0x31: return 0x63;                 // DIP SW 2
    case 0x33: return 0xE8;                 // bit3: 高解像度ではない（普通の 400 ライン）
    case 0x35: return m->portc;
    // プリンタのポート B。bit7=1・bit2=1（プリンタは忙しくない）・bit5=1（8MHz 系）・bit4=DIP SW 1-3（実機の標準はオン）。
    // bit4 が 0 だとアナログ 16 色を使えない画面とみなして、パレットを 8 色の近似に落とすソフトがある（サバッシュII のロゴなど）
    case 0x42: return 0x84 | 0x20 | 0x10;
    case 0x30: return 0x00;                 // RS-232C 受信データ（何も来ない）
    case 0x32: return 0x85;                 // RS-232C 状態: DSR・送信バッファ空・送信できる（受信なし）
    case 0x60: return gdc_status(m, &m->gdcm);
    case 0x62: { if (m->gdcm.fifo.empty()) return 0; uint8_t v = m->gdcm.fifo.front(); m->gdcm.fifo.erase(m->gdcm.fifo.begin()); return v; }
    case 0xA0: return gdc_status(m, &m->gdcs);
    case 0xA2: { if (m->gdcs.fifo.empty()) return 0; uint8_t v = m->gdcs.fifo.front(); m->gdcs.fifo.erase(m->gdcs.fifo.begin()); return v; }
    case 0xA4: return m->disp_bank;
    case 0xA6: return m->draw_bank;
    case 0xA8: return m->analog ? m->palidx : m->degpal[0];
    case 0xAA: return m->analog ? m->pal[m->palidx & 15][0] : m->degpal[1];
    case 0xAC: return m->analog ? m->pal[m->palidx & 15][1] : m->degpal[2];
    case 0xAE: return m->analog ? m->pal[m->palidx & 15][2] : m->degpal[3];
    case 0xA9: { uint8_t v = cg_pattern(m); if (m->cfg.trace > 1) plog("[cg] code=%04X line=%02X left=%d -> %02X\n", m->cg_code, m->cg_line, m->cg_left, v); return v; }
    case 0x71: case 0x3FD9: return pit_read(m, 0);
    case 0x73: case 0x3FDB: return pit_read(m, 1);
    case 0x75: case 0x3FDD: return pit_read(m, 2);
    case 0x7FD9: return mouse_porta(m);
    case 0x7FDB: return 0x40;
    case 0x7FDD: return m->mouse.portc;
    case 0xF2: return m->a20 ? 0x00 : 0x01;
    case 0xF6: return m->a20 ? 0x00 : 0x01;
    case 0x5F: return 0xFF;
    case 0x9A0: return 0x00;                // 表示モード（24kHz）
    case 0x9A8: return 0x00;                // 31kHz 非対応
    case 0x0E8E: case 0x0E8F: return 0xFF;
    }
    if (m->cfg.midi && port == 0xE0D0) return mpu_read_data(m);
    if (m->cfg.midi && port == 0xE0D2) return (uint8_t)((m->mpu.rcnt ? 0x00 : 0x80) | 0x00);   // bit7=0: 受信データあり / bit6=0: 送れる
    if (m->cfg.sound_board) {
        bool is26 = m->cfg.sound_board == 26;
        switch (port) {
        case 0x188: return (uint8_t)(m->opna.status & 0x03);
        case 0x18A: return opna_read_data(m, 0);
        // 86 ボードは A460h bit0=0 の間 YM2203 互換で、拡張ポート 18Ch/18Eh が見えない（ONGCHK はこれで 86 を判別する）
        case 0x18C: return (is26 || !(m->opna.reg[1][0xFE] & 1)) ? 0xFF : (uint8_t)(m->opna.status & 0x03);
        case 0x18E: return (is26 || !(m->opna.reg[1][0xFE] & 1)) ? 0xFF : opna_read_data(m, 1);
        case 0xA460: return is26 ? 0xFF : (uint8_t)(0x40 | (m->opna.reg[1][0xFE] & 0x01));
        case 0xA466: case 0xA468: case 0xA46A: case 0xA46C: return is26 ? 0xFF : pcm86_in(m, port);
        }
    }
    if (unknown_io_log(m, port)) plog("[io] in  %04X\n", port);
    return 0xFF;
}

// 知らないポートの記録: ポートごとに最初の 8 回まで（合計 2000 行まで）。
// 同じポートを何十回も読むソフトがあっても、後から来る別のポートが埋もれないように。
static bool unknown_io_log(Machine* m, uint16_t port) {
    static uint8_t per_port[65536];
    if (!m->cfg.trace || m->unknown_io >= 2000 || per_port[port] >= 8) return false;
    per_port[port]++; m->unknown_io++;
    return true;
}
void io_out8(Machine* m, uint16_t port, uint8_t v) {
    g_side_fx++;
    if (port >= g_trace_port_lo && port <= g_trace_port_hi && s_port_log++ < 3000) plog("[out] %04X <- %02X @%04X:%04X\n", port, v, m->cpu.op_cs, m->cpu.op_ip);
    switch (port) {
    case 0x00: pic_write(m, 0, 0, v); return;
    case 0x02: pic_write(m, 0, 1, v); return;
    case 0x08: pic_write(m, 1, 0, v); return;
    case 0x0A: pic_write(m, 1, 1, v); return;
    case 0x41: case 0x43: return;           // 8251 コマンド
    // RS-232C（8251）: 送ったバイトは MIDI として出す（RS-232C の MIDI インターフェース。INI の MIDI=1 のとき）
    case 0x30: if (m->cfg.midi) m->midi_out.push_back(v); return;
    case 0x32: return;                      // モード・コマンドは覚えなくてよい（いつでも送れる扱い）
    case 0x35: sysport_c_write(m, v); return;
    case 0x37:
        if (!(v & 0x80)) {
            int bit = (v >> 1) & 7;
            uint8_t nc = (v & 1) ? (uint8_t)(m->portc | (1 << bit)) : (uint8_t)(m->portc & ~(1 << bit));
            sysport_c_write(m, nc);
        }
        return;
    case 0x60: gdc_param(m, &m->gdcm, v); return;
    case 0x62: gdc_command(m, &m->gdcm, v); return;
    case 0xA0: gdc_param(m, &m->gdcs, v); return;
    case 0xA2: gdc_command(m, &m->gdcs, v); return;
    case 0x64: m->vsync_armed = 1; return;
    case 0x68: m->modeff[(v >> 1) & 7] = v & 1; return;
    case 0x6A: {
        int bit = (v >> 1) & 0x7F;
        if (bit < 8) {
            if (bit == 2 && !m->modeff2[3]) return;   // EGC モードの切替は保護解除(07h)が要る
            m->modeff2[bit] = v & 1;
        }
        if (bit == 0) m->analog = v & 1;
        if (bit == 2) m->egc_enabled = m->modeff2[2];
        return; }
    case 0x6C: m->border = v; return;
    case 0x6E: return;
    case 0x7C: m->grcg_mode = v; m->grcg_idx = 0; return;
    case 0x7E: m->grcg_tile[m->grcg_idx & 3] = v; m->grcg_idx = (m->grcg_idx + 1) & 3; return;
    case 0xA1: m->cg_code = (uint16_t)((m->cg_code & 0x00FF) | (v << 8)); return;
    case 0xA3: m->cg_code = (uint16_t)((m->cg_code & 0xFF00) | v); return;
    case 0xA5: m->cg_line = v & 0x1F; m->cg_left = (v & 0x20) ? 1 : 0; return;
    case 0xA9: {                            // 外字の書き込み（76・77 区）
        uint16_t jis = cg_jis(m);
        if ((m->cg_code >> 8) && fontrom_is_gaiji(jis)) fontrom_gaiji_write(jis, m->cg_line & 15, m->cg_left != 0, v);
        return; }
    case 0xA4: m->disp_bank = v & 1; return;
    case 0xA6: m->draw_bank = v & 1; return;
    case 0xA8: if (m->analog) m->palidx = v & 15; else m->degpal[0] = v; return;
    case 0xAA: if (m->analog) m->pal[m->palidx & 15][0] = v & 15; else m->degpal[1] = v; return;
    case 0xAC: if (m->analog) m->pal[m->palidx & 15][1] = v & 15; else m->degpal[2] = v; return;
    case 0xAE: if (m->analog) m->pal[m->palidx & 15][2] = v & 15; else m->degpal[3] = v; return;
    case 0x71: case 0x3FD9: pit_write(m, 0, v); return;
    case 0x73: case 0x3FDB: pit_write(m, 1, v); return;
    case 0x75: case 0x3FDD: pit_write(m, 2, v); return;
    case 0x77: case 0x3FDF: pit_ctrl(m, v); return;
    case 0x7FDD: mouse_set_portc(m, v); return;
    case 0x7FDF:
        if (v & 0x80) {
            // 8255 のモード設定: 出力のポート（C の上位: HC・選択・割込み禁止）はすべて 0 になる
            mouse_set_portc(m, (uint8_t)(m->mouse.portc & 0x0F));
        } else {
            int bit = (v >> 1) & 7;
            uint8_t nc = (v & 1) ? (uint8_t)(m->mouse.portc | (1 << bit)) : (uint8_t)(m->mouse.portc & ~(1 << bit));
            mouse_set_portc(m, nc);
        }
        return;
    case 0xBFDB: m->mouse.freq = v & 3; return;
    case 0xF2: m->a20 = 1; g_addr_mask = 0x1FFFFF; return;
    case 0xF6: if (v == 0x02) { m->a20 = 1; g_addr_mask = 0x1FFFFF; } else if (v == 0x03) { m->a20 = 0; g_addr_mask = 0xFFFFF; } return;
    case 0x5F: return;
    case 0xF0: return;                      // CPU リセット（無視）
    }
    if (port >= 0x4A0 && port <= 0x4AF) { egc_out(m, port, v); return; }
    if (m->cfg.midi && port == 0xE0D0) { mpu_data(m, v); return; }
    if (m->cfg.midi && port == 0xE0D2) { mpu_command(m, v); return; }
    if (m->cfg.sound_board) {
        bool is26 = m->cfg.sound_board == 26;
        switch (port) {
        case 0x188: m->opna.addr[0] = v; return;
        case 0x18A: opna_write(m, 0, m->opna.addr[0], v); return;
        case 0x18C: if (!is26 && (m->opna.reg[1][0xFE] & 1)) m->opna.addr[1] = v; return;
        case 0x18E: if (!is26 && (m->opna.reg[1][0xFE] & 1)) opna_write(m, 1, m->opna.addr[1], v); return;
        case 0xA460: m->opna.reg[1][0xFE] = v; return;
        case 0xA466: case 0xA468: case 0xA46A: case 0xA46C: if (!is26) pcm86_out(m, port, v); return;
        }
    }
    if (unknown_io_log(m, port)) plog("[io] out %04X,%02X\n", port, v);
}

uint16_t io_in16(Machine* m, uint16_t port) {
    g_side_fx++;
    return (uint16_t)(io_in8(m, port) | (io_in8(m, (uint16_t)(port + 1)) << 8));
}
void io_out16(Machine* m, uint16_t port, uint16_t v) {
    g_side_fx++;
    if (port >= 0x4A0 && port <= 0x4AF) {
        if (port >= g_trace_port_lo && port <= g_trace_port_hi && s_port_log++ < 3000) plog("[out] %04X <- %04X (w) @%04X:%04X\n", port, v, m->cpu.op_cs, m->cpu.op_ip);
        egc_out16(m, port, v); return; }
    io_out8(m, port, (uint8_t)v);
    io_out8(m, (uint16_t)(port + 1), (uint8_t)(v >> 8));
}

// ---- HLE 振り分け ------------------------------------------------------------
void hle_trap(Machine* m, uint8_t n) {
    g_side_fx++;
    if (m->cfg.boot_fd && bios_rom_trap(m)) return;
    switch (n) {
    case HLE_INT20: case HLE_INT21: case HLE_INT25: case HLE_INT26: case HLE_INT27:
    case HLE_INT28: case HLE_INT29: case HLE_INT2F: case HLE_INT67: case HLE_XMS:
    case HLE_SHELL: case HLE_EXIT:
        dos_hle(m, n); return;
    default:
        bios_hle(m, n); return;
    }
}

void set_cf(Machine* m, bool on) {
    // HLE は INT の枠の中で呼ばれるので、戻りの IRET が拾う FLAGS（スタック上）も直す
    Cpu* c = &m->cpu;
    if (on) c->fl |= FL_CF; else c->fl &= ~FL_CF;
    uint32_t a = ((uint32_t)c->sr[SS_] << 4) + (uint16_t)(c->r[ESP] + 4);
    uint16_t f = mem_rw(m, a);
    f = on ? (uint16_t)(f | FL_CF) : (uint16_t)(f & ~FL_CF);
    mem_ww(m, a, f);
}
void set_zf(Machine* m, bool on) {
    Cpu* c = &m->cpu;
    if (on) c->fl |= FL_ZF; else c->fl &= ~FL_ZF;
    uint32_t a = ((uint32_t)c->sr[SS_] << 4) + (uint16_t)(c->r[ESP] + 4);
    uint16_t f = mem_rw(m, a);
    f = on ? (uint16_t)(f | FL_ZF) : (uint16_t)(f & ~FL_ZF);
    mem_ww(m, a, f);
}

// ---- 時間を進める -----------------------------------------------------------
static void advance(Machine* m, uint32_t t) {
    // 8253 チャネル 0
    m->pit_frac += (int32_t)t;
    int pt = m->pit_frac >> 2;
    m->pit_frac &= 3;
    for (int ch = 0; ch < 2; ch++) {
        Pit* p = &m->pit[ch];
        if (!p->armed || pt <= 0) continue;
        uint32_t reload = p->reload ? p->reload : 0x10000u;
        uint32_t cur = p->counter ? p->counter : reload;
        uint32_t left = (uint32_t)pt;
        if (p->mode == 0 || p->mode == 1 || p->mode == 4 || p->mode == 5) {
            if (left >= cur) {
                if (ch == 0 && p->armed == 1) machine_raise_irq(m, 0);
                p->armed = 2;   // 以降は数え続けるだけ
                cur = (uint32_t)((cur - left) & 0xFFFF);
            } else cur -= left;
            p->counter = (uint16_t)cur;
        } else {
            while (left >= cur) { left -= cur; cur = reload; if (ch == 0) machine_raise_irq(m, 0); }
            p->counter = (uint16_t)(cur - left);
        }
    }

    // PCM86
    if (m->pcm86.ctrl & 0x80) pcm86_advance(m, t);
    // MIDI（MPU のクロック・トゥ・ホスト）
    if (m->mpu.cth_on) mpu_advance(m, t);

    // OPNA タイマ
    if (m->opna.timer_a_run) {
        m->opna.timer_a_count -= (int32_t)t;
        while (m->opna.timer_a_count <= 0) {
            m->opna.timer_a_count += m->opna.timer_a_period;
            m->opna.status |= 1;
            if (m->opna.irq_enable_a) machine_raise_irq(m, m->cfg.sound_irq);
        }
    }
    if (m->opna.timer_b_run) {
        m->opna.timer_b_count -= (int32_t)t;
        while (m->opna.timer_b_count <= 0) {
            m->opna.timer_b_count += m->opna.timer_b_period;
            m->opna.status |= 2;
            if (m->opna.irq_enable_b) machine_raise_irq(m, m->cfg.sound_irq);
        }
    }

    // キーボード
    if (m->kb_delay > 0) m->kb_delay -= (int32_t)t;
    if (!m->kb_ready && m->kb_delay <= 0 && !m->kb_queue.empty()) {
        m->kb_data = m->kb_queue.front();
        m->kb_queue.erase(m->kb_queue.begin());
        m->kb_ready = 1;
        m->kb_delay = MASTER_CLOCK / 1000;   // 1ms 間隔で送る
        machine_raise_irq(m, 1);
    }

    // マウス割込み
    if (!(m->mouse.portc & 0x10)) {
        static const int hz[4] = {120, 60, 30, 15};
        m->mouse.timer -= (int32_t)t;
        if (m->mouse.timer <= 0) {
            m->mouse.timer += (int32_t)(MASTER_CLOCK / hz[m->mouse.freq & 3]);
            machine_raise_irq(m, 13);
        }
    }

    // フロッピーの入れ替え: 実機ではディスクを抜くと・入れると 1MB の FDC が「ドライブの準備が変わった」割込み
    //（IRQ 11 = INT 13h）を出し、BIOS がその結果（ST0）をユニットごとの作業域 0564h+ユニット×8 に残す。
    // 抜いたときは ST0 の bit3（準備できていない）が立つ。ELFDOS（同級生など）は INT 13h を横取りしてこれを見て、
    // そのドライブの読み込みキャッシュを捨てる（見られないと、入れ替えた後も前のディスクの中身を使い続ける）
    for (int u = 0; u < 2; u++) {
        uint32_t c = floppy::change_count_unit(u);
        if (c != m->fd_seen[u]) { m->fd_seen[u] = c; m->fd_irq_stage[u] = 2; m->fd_irq_wait[u] = 0; }
        if (!m->fd_irq_stage[u]) continue;
        m->fd_irq_wait[u] -= (int32_t)t;
        if (m->fd_irq_wait[u] > 0) continue;
        m->ram[0x564 + u * 8] = (uint8_t)((m->fd_irq_stage[u] == 2 ? 0xC8 : 0xC0) | u);
        machine_raise_irq(m, 11);
        m->fd_irq_stage[u]--;
        m->fd_irq_wait[u] = (int32_t)(MASTER_CLOCK / 30);   // 抜いてから入れるまで少し間をあける
    }

    // 垂直同期
    uint32_t before = m->frame_pos;
    m->frame_pos += t;
    uint32_t vs = DISPLAY_LINES * LINE_TICKS;
    if (before < vs && m->frame_pos >= vs) {
        if (m->vsync_armed) { m->vsync_armed = 0; machine_raise_irq(m, 2); }
    }
    m->ticks += t;
}

void machine_run_frame(Machine* m) {
    const int SLICE = 96;
    while (m->frame_pos < FRAME_TICKS) {
        int used = cpu_run(&m->cpu, SLICE);
        m->cyc_acc += (uint64_t)used * MASTER_CLOCK;
        uint32_t t = (uint32_t)(m->cyc_acc / m->cpu_hz);
        m->cyc_acc %= m->cpu_hz;
        if (t) advance(m, t);
    }
    m->frame_pos -= FRAME_TICKS;
    m->frame_count++;
    extern unsigned long long g_frame_dbg; g_frame_dbg = m->frame_count;
}

Machine* machine_create(const Config& cfg) {
    Machine* m = new Machine();
    g_m = m;
    m->cfg = cfg;
    m->ram = (uint8_t*)calloc(1, PC98_RAM_SIZE);
    g_ram = m->ram;
    g_addr_mask = 0xFFFFF;
    memset(m->tvram, 0, sizeof(m->tvram));
    memset(m->gvram, 0, sizeof(m->gvram));
    { extern int g_idle_skip; g_idle_skip = (cfg.idle_skip && !getenv("PC98PLAYER_NOIDLE")) ? 1 : 0; }
    m->cpu_hz = (uint32_t)cfg.cpu_mhz * 1000000u;
    if (m->cpu_hz < 1000000u) m->cpu_hz = 8000000u;
    cpu_reset(&m->cpu, m);
    m->pic[0].base = 0x08; m->pic[1].base = 0x10;
    m->pic[0].imr = 0xFF; m->pic[1].imr = 0xFF;
    m->opna.prescale = 6;
    for (int p = 0; p < 2; p++) for (int a = 0xB4; a <= 0xB6; a++) m->opna.reg[p][a] = 0xC0;
    opna_recalc(m);
    m->portc = 0x08 | 0x10 | 0x20;   // ブザー OFF
    m->mouse.portc = 0x10;           // マウス割込み禁止
    m->mouse.maxx = 639; m->mouse.maxy = 399;
    m->mouse.x = 320; m->mouse.y = 200;
    m->mouse.mickey_x = 8; m->mouse.mickey_y = 16;
    m->gdcm.display = 1; m->gdcs.display = 0;
    m->gdcs.pram[3] = 0x19;   // LEN1 = 400
    m->gdcm.csrform[0] = 0x0F;   // 16 ライン/行
    m->modeff[3] = 1;            // 高解像度（400 ライン）
    // 既定のデジタルパレット（0..7 がそのまま）
    for (int u = 0; u < 2; u++) { m->fd_seen[u] = floppy::change_count_unit(u); m->fd_irq_stage[u] = 0; m->fd_irq_wait[u] = 0; }
    m->degpal[0] = 0x37; m->degpal[1] = 0x15; m->degpal[2] = 0x26; m->degpal[3] = 0x04;
    for (int i = 0; i < 16; i++) {
        m->pal[i][0] = (i & 4) ? 15 : 0;   // G
        m->pal[i][1] = (i & 2) ? 15 : 0;   // R
        m->pal[i][2] = (i & 1) ? 15 : 0;   // B
        if (i >= 8) { for (int k = 0; k < 3; k++) m->pal[i][k] = m->pal[i][k] ? 15 : 0; }
        if (i == 8) { m->pal[i][0] = m->pal[i][1] = m->pal[i][2] = 7; }
        if (i > 0 && i < 8) { for (int k = 0; k < 3; k++) if (m->pal[i][k]) m->pal[i][k] = 10; }
    }
    egc_reset(m);
    pcm86_reset(m);
    mpu_defaults(m->mpu);
    fontrom_reset_gaiji();
    bios_init(m);
    if (!cfg.boot_fd) dos_init(m);   // ブートモードは MS-DOS を用意しない（IPL から起動する）
    return m;
}
void machine_destroy(Machine* m) {
    if (!m) return;
    free(m->ram);
    delete m;
    if (g_m == m) g_m = nullptr;
}

// ---- ステートセーブ ----------------------------------------------------------
#include "state.h"

static void gdc_save(StateW& w, const Gdc& g) {
    w.pod(g.cmd); w.pod(g.pcount); w.pod(g.params); w.pod(g.pram); w.pod(g.pram_ptr);
    w.pod(g.display); w.pod(g.zoom); w.pod(g.csrform); w.pod(g.sync); w.pod(g.pitch);
    w.pod(g.ead); w.pod(g.dad); w.pod(g.vect); w.pod(g.mode_write); w.pod(g.pattern);
    w.u32((uint32_t)g.fifo.size()); w.bytes(g.fifo.data(), g.fifo.size());
}
static void gdc_load(StateR& r, Gdc& g) {
    r.pod(g.cmd); r.pod(g.pcount); r.pod(g.params); r.pod(g.pram); r.pod(g.pram_ptr);
    r.pod(g.display); r.pod(g.zoom); r.pod(g.csrform); r.pod(g.sync); r.pod(g.pitch);
    r.pod(g.ead); r.pod(g.dad); r.pod(g.vect); r.pod(g.mode_write); r.pod(g.pattern);
    uint32_t n = r.u32(); if (n > 4096) { r.ok = false; return; }
    g.fifo.resize(n); r.bytes(g.fifo.data(), n);
}

void machine_state_save(Machine* m, StateW& w) {
    w.tag("CPU ");
    Cpu c = m->cpu; c.m = nullptr;
    w.pod(c);
    w.tag("MEM ");
    w.bytes(m->ram, PC98_RAM_SIZE);
    w.pod(m->tvram); w.bytes(m->gvram, sizeof(m->gvram));
    w.tag("DEV ");
    w.pod(m->pic); w.pod(m->pit); w.pod(m->pit_frac);
    gdc_save(w, m->gdcm); gdc_save(w, m->gdcs);
    w.pod(m->opna); w.pod(m->mouse);
    w.pod(m->analog); w.pod(m->pal); w.pod(m->palidx); w.pod(m->degpal);
    w.pod(m->disp_bank); w.pod(m->draw_bank); w.pod(m->modeff); w.pod(m->modeff2);
    w.pod(m->gfx_200); w.pod(m->gfx_200_lower); w.pod(m->gfx_color); w.pod(m->border); w.pod(m->egc_enabled);
    w.pod(m->grcg_mode); w.pod(m->grcg_tile); w.pod(m->grcg_idx);
    w.pod(m->cg_code); w.pod(m->cg_line); w.pod(m->cg_left);
    w.u32((uint32_t)m->kb_queue.size()); w.bytes(m->kb_queue.data(), m->kb_queue.size());
    w.pod(m->kb_data); w.pod(m->kb_ready); w.pod(m->kb_delay); w.pod(m->kb_down);
    w.pod(m->portc); w.pod(m->beep_on); w.pod(m->a20);
    w.pod(m->ticks); w.pod(m->frame_pos); w.pod(m->frame_count); w.pod(m->vsync_armed);
    w.pod(m->cyc_acc);
    fontrom_state_save(w);
    egc_state_save(w);
    w.tag("MPU2"); w.pod(m->mpu);
    pcm86_state_save(m, w);
    w.tag("MDRV"); w.pod(m->mdrv);
}

void machine_state_load(Machine* m, StateR& r) {
    r.tag("CPU ");
    Machine* keep = m->cpu.m;
    r.pod(m->cpu);
    m->cpu.m = keep;
    r.tag("MEM ");
    r.bytes(m->ram, PC98_RAM_SIZE);
    r.pod(m->tvram); r.bytes(m->gvram, sizeof(m->gvram));
    r.tag("DEV ");
    r.pod(m->pic); r.pod(m->pit); r.pod(m->pit_frac);
    gdc_load(r, m->gdcm); gdc_load(r, m->gdcs);
    r.pod(m->opna); r.pod(m->mouse);
    r.pod(m->analog); r.pod(m->pal); r.pod(m->palidx); r.pod(m->degpal);
    r.pod(m->disp_bank); r.pod(m->draw_bank); r.pod(m->modeff); r.pod(m->modeff2);
    r.pod(m->gfx_200); r.pod(m->gfx_200_lower); r.pod(m->gfx_color); r.pod(m->border); r.pod(m->egc_enabled);
    r.pod(m->grcg_mode); r.pod(m->grcg_tile); r.pod(m->grcg_idx);
    r.pod(m->cg_code); r.pod(m->cg_line); r.pod(m->cg_left);
    uint32_t n = r.u32(); if (n > 65536) { r.ok = false; return; }
    m->kb_queue.resize(n); r.bytes(m->kb_queue.data(), n);
    r.pod(m->kb_data); r.pod(m->kb_ready); r.pod(m->kb_delay); r.pod(m->kb_down);
    r.pod(m->portc); r.pod(m->beep_on); r.pod(m->a20);
    r.pod(m->ticks); r.pod(m->frame_pos); r.pod(m->frame_count); r.pod(m->vsync_armed);
    r.pod(m->cyc_acc);
    fontrom_state_load(r);
    egc_state_load(r);
    if (r.peek_tag("MPU2")) { r.tag("MPU2"); r.pod(m->mpu); }
    else {
        memset(&m->mpu, 0, sizeof(m->mpu)); mpu_defaults(m->mpu);
        if (r.peek_tag("MPU ")) { r.tag("MPU "); uint8_t old[21]; r.bytes(old, sizeof(old)); }   // 旧形式は読み飛ばす
    }
    m->midi_out.clear();
    pcm86_state_load(m, r);
    if (r.peek_tag("MDRV")) { r.tag("MDRV"); uint16_t so = m->mdrv.stub_off, po = m->mdrv.ptr_off; r.pod(m->mdrv); m->mdrv.stub_off = so; m->mdrv.ptr_off = po; }
    // 派生状態の作り直し
    g_addr_mask = m->a20 ? 0x1FFFFF : 0xFFFFF;
    m->regw.clear(); m->beepw.clear();
    m->quit = 0; m->status.clear();
    pic_update_hint(m);
}
