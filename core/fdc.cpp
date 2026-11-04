// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  fdc.cpp  --  1MB の FDD のコントローラ（µPD765）と DMA（8237）
//
//  BIOS（INT 1Bh）を通さず、ポート 90h・92h・94h に直接命令を送ってディスクを読むソフト向け。
//  読んだデータは DMA のチャネル 2 で主記憶へ送る（モードが「検査」なら送らない）。
//  命令が終わると割込み（IRQ 11 = INT 13h）を出す。
//  対応している命令: SPECIFY・SENSE DRIVE STATUS・RECALIBRATE・SENSE INTERRUPT STATUS・SEEK・
//  READ ID・READ DATA・READ DELETED DATA・WRITE DATA。そのほかは「不正な命令」として返す。
//  資料: µPD765 のデータシート（命令の形と状態レジスタ）
// -----------------------------------------------------------------------------
#include "machine.h"
#include "floppy.h"
#include <string.h>
#include <algorithm>
#include <vector>

namespace floppy { int floppy_head_pos(int unit); void floppy_set_head_pos(int unit, int cyl); }
using floppy::floppy_head_pos; using floppy::floppy_set_head_pos;

// 状態レジスタ（90h）
static const uint8_t MS_RQM = 0x80, MS_DIO = 0x40, MS_BUSY = 0x10;
// 割込みまでの時間（基準クロック）。すぐに出すと、割込みを待つ準備が間に合わないソフトがある
static const int32_t k_irq_delay = (int32_t)(MASTER_CLOCK / 5000);   // 約 0.2ms

void fdc_reset(Machine* m) {
    memset(&m->fdc, 0, sizeof(m->fdc));
    memset(&m->dma, 0, sizeof(m->dma));
    m->dma.mask = 0x0F;
}

// ---- DMA ---------------------------------------------------------------------
static bool dma_out(Machine* m, uint16_t port, uint8_t v) {
    Dma8237& d = m->dma;
    if (port >= 0x01 && port <= 0x0F && (port & 1)) {
        int ch = (port - 1) >> 2;
        bool is_cnt = ((port - 1) >> 1) & 1;
        uint16_t& base = is_cnt ? d.base_cnt[ch] : d.base_addr[ch];
        uint16_t& cur = is_cnt ? d.cnt[ch] : d.addr[ch];
        if (!d.ff) base = (uint16_t)((base & 0xFF00) | v); else base = (uint16_t)((base & 0x00FF) | (v << 8));
        cur = base;
        d.ff ^= 1;
        return true;
    }
    switch (port) {
    case 0x11: return true;                                                       // 命令
    case 0x13: return true;                                                       // 要求
    case 0x15: if (v & 4) d.mask |= (uint8_t)(1 << (v & 3)); else d.mask &= (uint8_t)~(1 << (v & 3)); return true;
    case 0x17: d.mode[v & 3] = v; return true;
    case 0x19: d.ff = 0; return true;
    case 0x1B: d.ff = 0; d.mask = 0x0F; return true;                              // 全体の初期化
    case 0x1D: d.mask = 0; return true;
    case 0x1F: d.mask = v & 0x0F; return true;
    case 0x21: d.bank[1] = v; return true;
    case 0x23: d.bank[2] = v; return true;
    case 0x25: d.bank[3] = v; return true;
    case 0x27: d.bank[0] = v; return true;
    }
    return false;
}
static bool dma_in(Machine* m, uint16_t port, uint8_t* v) {
    Dma8237& d = m->dma;
    if (port >= 0x01 && port <= 0x0F && (port & 1)) {
        int ch = (port - 1) >> 2;
        bool is_cnt = ((port - 1) >> 1) & 1;
        uint16_t cur = is_cnt ? d.cnt[ch] : d.addr[ch];
        const Fdc765& f = m->fdc;
        if (ch == 2 && f.phase == 3) {   // FDC の転送中: 時間に応じて進んだところを見せる
            uint64_t now = m->ticks;
            uint32_t done = 0;
            if (now >= f.xfer_t1) done = f.xfer_bytes;
            else if (now > f.xfer_t0) done = (uint32_t)((double)f.xfer_bytes * (double)(now - f.xfer_t0) / (double)(f.xfer_t1 - f.xfer_t0));
            cur = is_cnt ? (uint16_t)(f.cnt0 - done) : (uint16_t)(f.addr0 + done);
        }
        *v = d.ff ? (uint8_t)(cur >> 8) : (uint8_t)cur;
        d.ff ^= 1;
        return true;
    }
    if (port == 0x11) { *v = 0; return true; }   // 状態（終わったチャネルは示さない）
    return false;
}
// DMA の 1 バイト（チャネル ch）。転送の向き: to_mem = 装置→主記憶。終わり（数が尽きた）なら false
static void dma_mem_write(Machine* m, uint32_t a, uint8_t v) {
    if (a < 0xA0000u || (a >= 0x100000u && a < m->ram_size)) m->ram[a] = v;
    else mem_wb(m, a, v);
}
static uint8_t dma_mem_read(Machine* m, uint32_t a) {
    if (a < 0xA0000u || (a >= 0x100000u && a < m->ram_size)) return m->ram[a];
    return mem_rb(m, a);
}
// 送れるバイト数（数のレジスタ + 1）。止めてあれば 0
static uint32_t dma_left(Machine* m, int ch) {
    if (m->dma.mask & (1 << ch)) return 0;
    return (uint32_t)m->dma.cnt[ch] + 1;
}
// n バイト送る。src が nullptr なら主記憶から読んで dst へ（書き込み命令用）
static void dma_transfer(Machine* m, int ch, const uint8_t* src, uint8_t* dst, uint32_t n) {
    Dma8237& d = m->dma;
    uint8_t xfer = (d.mode[ch] >> 2) & 3;   // 0 = 検査、1 = 主記憶へ書く、2 = 主記憶から読む
    bool dec = (d.mode[ch] & 0x20) != 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = ((uint32_t)d.bank[ch] << 16) | d.addr[ch];
        if (xfer == 1 && src) dma_mem_write(m, a, src[i]);
        else if (xfer == 2 && dst) dst[i] = dma_mem_read(m, a);
        d.addr[ch] = (uint16_t)(dec ? d.addr[ch] - 1 : d.addr[ch] + 1);
        d.cnt[ch] = (uint16_t)(d.cnt[ch] - 1);
    }
    if (d.cnt[ch] == 0xFFFF) {   // 終わり
        if (d.mode[ch] & 0x10) { d.addr[ch] = d.base_addr[ch]; d.cnt[ch] = d.base_cnt[ch]; }   // 自動初期化
        else d.mask |= (uint8_t)(1 << ch);
    }
}

// ---- FDC ---------------------------------------------------------------------
static int cmd_length(uint8_t c) {
    switch (c & 0x1F) {
    case 0x03: return 3;   // SPECIFY
    case 0x04: return 2;   // SENSE DRIVE STATUS
    case 0x07: return 2;   // RECALIBRATE
    case 0x08: return 1;   // SENSE INTERRUPT STATUS
    case 0x0F: return 3;   // SEEK
    case 0x0A: return 2;   // READ ID
    case 0x06: case 0x0C: case 0x05: case 0x09: case 0x02: case 0x11: case 0x19: case 0x1D: return 9;
    case 0x0D: return 6;   // FORMAT
    default: return 1;
    }
}
// 読み書きの結果: 転送にかかる時間（µs）の後で返す
static void set_result_after(Machine* m, const uint8_t* r, double xfer_us, uint32_t bytes, uint16_t cnt0, uint16_t addr0) {
    Fdc765& f = m->fdc;
    memcpy(f.res, r, 7);
    f.nres = 7; f.rpos = 0;
    // 命令からデータ部の先頭までの時間（ヘッドの位置決めと回転待ち。ここでは一定）
    const double lead_us = 1500.0;
    f.xfer_t0 = m->ticks + (uint64_t)(lead_us * MASTER_CLOCK / 1e6);
    f.xfer_t1 = f.xfer_t0 + (uint64_t)(xfer_us * MASTER_CLOCK / 1e6) + 1;
    f.xfer_bytes = bytes; f.cnt0 = cnt0; f.addr0 = addr0;
    f.phase = 3;
    if (m->cfg.trace) { plog("[fdc]   結果"); for (int i = 0; i < 7; i++) plog(" %02X", r[i]); plog("（転送 %u バイト・%.0f µs）\n", bytes, xfer_us); }
}
static void set_result(Machine* m, const uint8_t* r, int n, bool irq) {
    Fdc765& f = m->fdc;
    memcpy(f.res, r, (size_t)n);
    if (m->cfg.trace && n) { plog("[fdc]   結果"); for (int i = 0; i < n; i++) plog(" %02X", r[i]); plog("\n"); }
    f.nres = (uint8_t)n; f.rpos = 0;
    f.phase = n ? 2 : 0;
    if (irq) f.irq_wait = k_irq_delay;
}
// BIOS の結果コード（イメージのセクタの状態）→ ST1・ST2
static void status_bits(uint8_t st, uint8_t* st1, uint8_t* st2) {
    switch (st & 0xF0) {
    case 0xA0: *st1 |= 0x20; break;                  // ID の CRC
    case 0xB0: *st1 |= 0x20; *st2 |= 0x20; break;    // データの CRC
    case 0xC0: *st1 |= 0x04; break;                  // セクタが無い
    case 0xD0: *st1 |= 0x04; *st2 |= 0x10; break;    // シリンダ違い
    case 0xE0: *st1 |= 0x01; break;                  // アドレスマークが無い
    }
}

static void do_rw(Machine* m) {
    Fdc765& f = m->fdc;
    uint8_t code = f.cmd[0] & 0x1F;
    bool mt = (f.cmd[0] & 0x80) != 0, mf = (f.cmd[0] & 0x40) != 0, sk = (f.cmd[0] & 0x20) != 0;
    int us = f.cmd[1] & 3, hd = (f.cmd[1] >> 2) & 1;
    uint8_t C = f.cmd[2], H = f.cmd[3], R = f.cmd[4], N = f.cmd[5], EOT = f.cmd[6];
    bool write = code == 0x05 || code == 0x09;
    bool want_del = code == 0x0C || code == 0x09;
    FloppyImage* im = floppy::image_unit(us);
    uint8_t st0 = (uint8_t)((hd << 2) | us), st1 = 0, st2 = 0;
    if (!im) {
        uint8_t r[7] = {(uint8_t)(st0 | 0x48), 0, 0, C, H, R, N};   // 準備できていない
        set_result(m, r, 7, true); return;
    }
    if (write && im->wprot) {
        uint8_t r[7] = {(uint8_t)(st0 | 0x40), 0x02, 0, C, H, R, N};
        set_result(m, r, 7, true); return;
    }
    uint32_t left = dma_left(m, 2);
    if (!left) left = 0xFFFFFFFFu;    // DMA を使っていない: 終わりのセクタ（EOT）まで
    uint16_t cnt0 = m->dma.cnt[2], addr0 = m->dma.addr[2];
    double xfer_us = 0; uint32_t moved = 0;
    for (;;) {
        FdTrack* t = im->track(floppy_head_pos(us), hd);
        FdSector* s = nullptr;
        if (t) for (auto& x : t->secs) if (x.c == C && x.h == H && x.r == R && x.n == N && x.fm == !mf) { s = &x; break; }
        if (!s) {
            st0 |= 0x40;
            st1 |= (t && !t->secs.empty()) ? 0x04 : 0x01;
            if (t) for (auto& x : t->secs) if (x.r == R && x.c != C) { st2 |= 0x10; break; }
            break;
        }
        if (s->deleted != want_del && !write) {
            st2 |= 0x40;                                // 種類の違うデータ（CM）
            if (sk) goto next_sector;                   // 飛ばす
        }
        {
            uint32_t ss = 128u << (N & 7);
            uint32_t n = left < ss ? left : ss;
            if (write) {
                std::vector<uint8_t> buf(ss, 0);
                dma_transfer(m, 2, nullptr, buf.data(), n);
                im->write_sector(s, buf.data(), (int)ss);
                s->deleted = want_del;
            } else {
                std::vector<uint8_t> buf(ss, 0);
                memcpy(buf.data(), s->data.data(), std::min<size_t>(ss, s->data.size()));
                dma_transfer(m, 2, buf.data(), nullptr, n);
            }
            if (left != 0xFFFFFFFFu) left -= n;
            // 転送にかかる時間: 1MB の FDD（MFM 500kbps）の速さ。フラックスのイメージで測ってあれば、
            // 同じトラックの同じ大きさのセクタとの比（書き込み密度の違い）を反映する
            //（読み取ったドライブの回転数の違いは、トラックの中の比で打ち消す）
            double per_byte = mf ? 16.0 : 32.0;
            if (s->data_us > 0 && t) {
                std::vector<double> v;
                for (auto& x : t->secs) if (x.n == s->n && x.data_us > 0) v.push_back(x.data_us);
                if (v.size() >= 3) {
                    std::sort(v.begin(), v.end());
                    double med = v[v.size() / 2];
                    if (med > 0) per_byte *= s->data_us / med;
                }
            }
            xfer_us += per_byte * n; moved += n;
            if (s->status >= 0x20) { st0 |= 0x40; status_bits(s->status, &st1, &st2); break; }
            if (st2 & 0x40) break;                      // 種類の違うデータを読んだら、そのセクタで終わる
        }
    next_sector:
        if (R == EOT) {
            if (mt && hd == 0) { hd = 1; H ^= 1; R = 1; st0 = (uint8_t)((st0 & ~0x04) | 0x04); }
            else { R = 1; C++; if (left) { st0 |= 0x40; st1 |= 0x80; } break; }   // トラックの終わり
        } else R++;
        if (left == 0) break;                           // DMA の数が尽きた（ふつうの終わり）
    }
    uint8_t r[7] = {st0, st1, st2, C, H, R, N};
    set_result_after(m, r, xfer_us, moved, cnt0, addr0);
}

static void execute(Machine* m) {
    Fdc765& f = m->fdc;
    uint8_t code = f.cmd[0] & 0x1F;
    int us = f.cmd[1] & 3;
    switch (code) {
    case 0x03: f.phase = 0; return;                                   // SPECIFY（結果なし）
    case 0x04: {                                                       // SENSE DRIVE STATUS → ST3
        FloppyImage* im = floppy::image_unit(us);
        uint8_t st3 = (uint8_t)((f.cmd[1] & 7) | 0x08);               // 両面
        if (im) st3 |= 0x20;                                           // 準備できている
        if (im && im->wprot) st3 |= 0x40;
        if (floppy_head_pos(us) == 0) st3 |= 0x10;                               // トラック 0
        set_result(m, &st3, 1, false); return; }
    case 0x07: case 0x0F: {                                            // RECALIBRATE / SEEK
        f.pcn[us] = code == 0x07 ? 0 : f.cmd[2];
        floppy_set_head_pos(us, f.pcn[us]);
        f.seek_st0[us] = (uint8_t)(0x20 | (f.cmd[1] & 7) | (floppy::image_unit(us) ? 0 : 0x48));
        f.seek_end |= (uint8_t)(1 << us);
        f.phase = 0; f.irq_wait = k_irq_delay; return; }
    case 0x08: {                                                       // SENSE INTERRUPT STATUS
        for (int u = 0; u < 4; u++) if (f.seek_end & (1 << u)) {
            f.seek_end &= (uint8_t)~(1 << u);
            uint8_t r[2] = {f.seek_st0[u], (uint8_t)floppy_head_pos(u)};
            set_result(m, r, 2, false); return;
        }
        uint8_t r = 0x80;                                              // 割込みの理由が無い
        set_result(m, &r, 1, false); return; }
    case 0x0A: {                                                       // READ ID
        int hd = (f.cmd[1] >> 2) & 1;
        FloppyImage* im = floppy::image_unit(us);
        FdTrack* t = im ? im->track(floppy_head_pos(us), hd) : nullptr;
        uint8_t st0 = (uint8_t)(f.cmd[1] & 7);
        if (!im) { uint8_t r[7] = {(uint8_t)(st0 | 0x48), 0, 0, 0, 0, 0, 0}; set_result(m, r, 7, true); return; }
        if (!t || t->secs.empty()) { uint8_t r[7] = {(uint8_t)(st0 | 0x40), 0x01, 0, 0, 0, 0, 0}; set_result(m, r, 7, true); return; }
        FdSector& s = t->secs[t->rot % t->secs.size()];
        t->rot = (t->rot + 1) % (int)t->secs.size();
        uint8_t r[7] = {st0, 0, 0, s.c, s.h, s.r, s.n};
        set_result(m, r, 7, true); return; }
    case 0x06: case 0x0C: case 0x05: case 0x09:
        do_rw(m); return;
    default: {
        uint8_t r = 0x80;                                              // 不正な命令
        if (m->cfg.trace) plog("[fdc] 対応していない命令 %02Xh\n", f.cmd[0]);
        set_result(m, &r, 1, false); return; }
    }
}

static uint8_t msr(Machine* m) {
    Fdc765& f = m->fdc;
    if (f.phase == 3) return MS_BUSY;   // 実行中（DMA で転送している）
    if (f.phase == 2) return MS_RQM | MS_DIO | MS_BUSY;
    if (f.phase == 1) return MS_RQM | MS_BUSY;
    return MS_RQM;
}

bool fdc_io_in(Machine* m, uint16_t port, uint8_t* v) {
    Fdc765& f = m->fdc;
    switch (port) {
    case 0x90: *v = msr(m); return true;
    case 0x92:
        if (f.phase == 2) {
            *v = f.res[f.rpos++];
            if (f.rpos >= f.nres) f.phase = 0;
        } else *v = 0xFF;
        return true;
    case 0x94: *v = 0x40; return true;
    }
    return dma_in(m, port, v);
}

bool fdc_io_out(Machine* m, uint16_t port, uint8_t v) {
    Fdc765& f = m->fdc;
    switch (port) {
    case 0x92:
        if (f.phase >= 2) return true;            // 実行中・結果を読んでいる途中は受け付けない
        if (f.phase == 0) { f.ncmd = 0; f.need = (uint8_t)cmd_length(v); f.phase = 1; }
        f.cmd[f.ncmd++] = v;
        if (m->cfg.trace && f.ncmd == f.need) {
            plog("[fdc] 命令"); for (int i = 0; i < f.ncmd; i++) plog(" %02X", f.cmd[i]); plog("\n");
        }
        if (f.ncmd >= f.need) execute(m);
        return true;
    case 0x94:
        if ((v & 0x80) && !(f.ctrl & 0x80)) {      // リセット
            f.phase = 0; f.ncmd = 0; f.seek_end = 0;
        }
        f.ctrl = v;
        return true;
    case 0x90: return true;
    }
    return dma_out(m, port, v);
}

void fdc_advance(Machine* m, uint32_t t) {
    Fdc765& f = m->fdc;
    if (f.phase == 3 && m->ticks >= f.xfer_t1) { f.phase = 2; f.irq_wait = 1; }   // 転送が終わった: 結果を返し、割込み
    if (f.irq_wait <= 0) return;
    f.irq_wait -= (int32_t)t;
    if (f.irq_wait <= 0) { f.irq_wait = 0; machine_raise_irq(m, 11); }
}
