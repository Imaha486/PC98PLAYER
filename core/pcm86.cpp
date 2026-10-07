// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  pcm86.cpp  --  PC-9801-86 の PCM 部（A466h-A46Ch）
//
//  32KB の FIFO、8 段の標本化周波数、8/16bit・モノ/ステレオ、FIFO 残量による割込み。
//  再生位置は基準クロックで進め（決定的）、取り出した標本は時刻つきで Player へ渡す。
//  レジスタの意味は PC-9801-86 の公開資料にもとづく。
// -----------------------------------------------------------------------------
#include "machine.h"
#include "state.h"
#include <string.h>

static const uint32_t k_rates[8] = {44100, 33075, 22050, 16538, 11025, 8269, 5513, 4134};

static inline int frame_bytes(const Pcm86& p) {
    // dactrl bit6: 1=8bit / bit5: 左 / bit4: 右
    bool b8 = (p.dactrl & 0x40) != 0;
    int ch = ((p.dactrl >> 5) & 1) + ((p.dactrl >> 4) & 1);
    if (ch == 0) ch = 1;
    return (b8 ? 1 : 2) * ch;
}
static inline int pcm_threshold(const Pcm86& p) { return p.thresh ? p.thresh : 0x80; }

static void check_irq(Machine* m) {
    Pcm86& p = m->pcm86;
    if ((p.ctrl & 0x20) && (p.ctrl & 0x80) && !p.irqflag && p.count <= pcm_threshold(p)) {
        p.irqflag = 1;
        machine_raise_irq(m, m->cfg.sound_irq);
    }
}

// t 基準クロックぶん再生を進める
void pcm86_advance(Machine* m, uint32_t t) {
    Pcm86& p = m->pcm86;
    if (!(p.ctrl & 0x80) || (p.ctrl & 0x40)) return;   // 停止中 / 録音方向
    uint32_t rate = k_rates[p.ctrl & 7];
    p.acc += (uint64_t)t * rate;
    uint64_t base_tick = m->ticks;
    while (p.acc >= MASTER_CLOCK) {
        p.acc -= MASTER_CLOCK;
        int fb = frame_bytes(p);
        int16_t l = 0, r = 0;
        if (p.count >= fb) {
            auto get = [&](bool b8) -> int16_t {
                if (b8) { int8_t v = (int8_t)p.fifo[p.rpos]; p.rpos = (p.rpos + 1) & 0x7FFF; p.count--; return (int16_t)(v * 256); }
                int16_t v = (int16_t)((p.fifo[p.rpos] << 8) | p.fifo[(p.rpos + 1) & 0x7FFF]);
                p.rpos = (p.rpos + 2) & 0x7FFF; p.count -= 2; return v;
            };
            bool b8 = (p.dactrl & 0x40) != 0;
            bool L = (p.dactrl & 0x20) != 0, R = (p.dactrl & 0x10) != 0;
            if (L && R) { l = get(b8); r = get(b8); }
            else if (L) l = get(b8);
            else if (R) r = get(b8);
            else get(b8);
        } else if (p.count > 0) {
            p.rpos = (p.rpos + p.count) & 0x7FFF; p.count = 0;   // 半端は捨てる
        }
        if (m->pcm_out.size() < 65536) {
            // この標本が出る時刻（t の区間内を等分するほど厳密でなくてよい）
            PcmSample s; s.tick = base_tick; s.l = l; s.r = r;
            m->pcm_out.push_back(s);
        }
        check_irq(m);
    }
}

uint8_t pcm86_in(Machine* m, uint16_t port) {
    Pcm86& p = m->pcm86;
    switch (port) {
    case 0xA466: {
        uint8_t v = 0;
        if (p.count >= 0x8000) v |= 0x80;          // FIFO 満杯
        if (p.count == 0) v |= 0x40;               // FIFO 空
        // bit0: LR クロック。標本化周波数で L/R が切り替わるたびに反転する。
        // これの変化を待って 1 標本ずつ書くドライバがある（R-FORCE の PCML など）
        if (((m->ticks * k_rates[p.ctrl & 7] * 2) / MASTER_CLOCK) & 1) v |= 0x01;
        return v; }
    case 0xA468: return (uint8_t)((p.ctrl & ~0x10) | (p.irqflag ? 0x10 : 0));
    case 0xA46A: return p.dactrl;
    case 0xA46C: return 0;
    }
    return 0xFF;
}

void pcm86_out(Machine* m, uint16_t port, uint8_t v) {
    Pcm86& p = m->pcm86;
    switch (port) {
    case 0xA466:
        if ((v & 0xE0) == 0xA0) p.vol = (uint8_t)((~v) & 15);
        return;
    case 0xA468: {
        uint8_t chg = p.ctrl ^ v;
        if ((chg & 0x08) && (v & 0x08)) { p.rpos = p.wpos = 0; p.count = 0; }   // FIFO リセット
        if (!(v & 0x10)) p.irqflag = 0;                                          // 割込みの消去
        if ((chg & 0x80) && (v & 0x80)) p.acc = 0;
        p.ctrl = (uint8_t)(v & ~0x10);
        check_irq(m);
        return; }
    case 0xA46A:
        if (p.ctrl & 0x20) p.thresh = (v == 0xFF) ? 0x7FFC : (uint16_t)((v + 1) << 7);
        else p.dactrl = v;
        return;
    case 0xA46C:
        if (p.count < 0x8000) { p.fifo[p.wpos] = v; p.wpos = (p.wpos + 1) & 0x7FFF; p.count++; }
        return;
    }
}

void pcm86_reset(Machine* m) {
    memset(&m->pcm86, 0, sizeof(m->pcm86));
    m->pcm86.vol = 15;
    m->pcm86.dactrl = 0x32;   // 16bit ステレオ
    m->pcm86.thresh = 0x80;
    m->pcm_out.clear();
}

void pcm86_state_save(Machine* m, StateW& w) { w.tag("P86 "); w.pod(m->pcm86); }
void pcm86_state_load(Machine* m, StateR& r) {
    if (r.peek_tag("P86 ")) { r.tag("P86 "); r.pod(m->pcm86); }
    else pcm86_reset(m);
    m->pcm_out.clear();
}
