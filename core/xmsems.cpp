// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  xmsems.cpp  --  HIMEM.SYS（XMS 3.0）と EMM386（LIM EMS 4.0）の HLE
//
//  拡張メモリ/EMS の中身は仮想 PC-98 の物理メモリとは別の器に持ち、
//  ゲームからは XMS の移動（0Bh）と EMS のページフレーム経由でだけ見える。
//  EMS のページフレームは D000h（16KB x 4）。マップ時にフレームと器の間で
//  内容を写し替える方式なので、メモリアクセスの速い経路には手を入れない。
// -----------------------------------------------------------------------------
#include "machine.h"
#include "state.h"
#include <string.h>
#include <vector>
#include <string>

#define ROMSEG 0xF000
static const uint16_t EMS_FRAME_SEG = 0xD000;
static const int      EMS_PAGE = 0x4000;
static const uint16_t EMS_DEV_SEG = 0xF7F0;     // INT 67h のベクタのセグメント（:000A に "EMMXXXX0"）

struct XmsBlock { bool used = false; uint32_t kb = 0; uint8_t locks = 0; std::vector<uint8_t> data; };
struct EmsHandle { bool used = false; char name[8] = {0}; std::vector<uint8_t> data; uint16_t saved[8]; bool has_saved = false; };

static std::vector<XmsBlock>  s_xms;           // 添字 = ハンドル（0 は未使用）
static uint32_t s_xms_total_kb = 0;
static bool     s_hma_used = false;
static std::vector<EmsHandle> s_ems;           // 添字 = ハンドル（0 はシステム用）
static uint32_t s_ems_total_pages = 0;
static int32_t  s_frame[4][2];                 // 物理ページ -> {ハンドル, 論理ページ}（-1 = 空）
static uint16_t s_xms_entry_off = 0;

static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }
#define R(m) (m)->cpu.r
static inline uint16_t AX(Machine* m) { return (uint16_t)R(m)[EAX]; }
static inline uint16_t BX(Machine* m) { return (uint16_t)R(m)[EBX]; }
static inline uint16_t CX(Machine* m) { return (uint16_t)R(m)[ECX]; }
static inline uint16_t DX(Machine* m) { return (uint16_t)R(m)[EDX]; }
static inline uint8_t  AH(Machine* m) { return (uint8_t)(R(m)[EAX] >> 8); }
static inline uint8_t  AL(Machine* m) { return (uint8_t)R(m)[EAX]; }
static inline void SET16(Machine* m, int r, uint16_t v) { R(m)[r] = (R(m)[r] & 0xFFFF0000u) | v; }
static inline void SETAX(Machine* m, uint16_t v) { SET16(m, EAX, v); }
static inline void SETBX(Machine* m, uint16_t v) { SET16(m, EBX, v); }
static inline void SETCX(Machine* m, uint16_t v) { SET16(m, ECX, v); }
static inline void SETDX(Machine* m, uint16_t v) { SET16(m, EDX, v); }
static inline void SETAH(Machine* m, uint8_t v) { R(m)[EAX] = (R(m)[EAX] & 0xFFFF00FFu) | ((uint32_t)v << 8); }
static inline void SETAL(Machine* m, uint8_t v) { R(m)[EAX] = (R(m)[EAX] & 0xFFFFFF00u) | v; }
static inline void SETBL(Machine* m, uint8_t v) { R(m)[EBX] = (R(m)[EBX] & 0xFFFFFF00u) | v; }
static inline void SETBH(Machine* m, uint8_t v) { R(m)[EBX] = (R(m)[EBX] & 0xFFFF00FFu) | ((uint32_t)v << 8); }

bool xms_enabled(Machine* m) { return m->cfg.xms_kb > 0; }
bool ems_enabled(Machine* m) { return m->cfg.ems; }

// ---- XMS ---------------------------------------------------------------------
static uint32_t xms_used_kb() { uint32_t u = 0; for (auto& b : s_xms) if (b.used) u += b.kb; return u; }
static int xms_new_handle() {
    for (size_t i = 1; i < s_xms.size(); i++) if (!s_xms[i].used) return (int)i;
    if (s_xms.size() >= 128) return -1;
    s_xms.emplace_back();
    return (int)s_xms.size() - 1;
}
static XmsBlock* xms_block(uint16_t h) { return (h > 0 && h < s_xms.size() && s_xms[h].used) ? &s_xms[h] : nullptr; }

// 0Bh の移動: ハンドル 0 は通常メモリ（オフセットは seg:off）
static bool xms_ptr(Machine* m, uint16_t h, uint32_t off, uint32_t len, uint8_t** host, uint32_t* guest, uint8_t* err) {
    if (h == 0) { *host = nullptr; *guest = lin((uint16_t)(off >> 16), (uint16_t)off); return true; }
    XmsBlock* b = xms_block(h);
    if (!b) { *err = 0xA3; return false; }
    if ((uint64_t)off + len > b->data.size()) { *err = 0xA4; return false; }
    *host = b->data.data() + off; (void)m;
    return true;
}

static void xms_call(Machine* m) {
    uint8_t fn = AH(m);
    uint32_t free_kb = s_xms_total_kb > xms_used_kb() ? s_xms_total_kb - xms_used_kb() : 0;
    if (m->cfg.trace) {   // 機能ごとに最初の 4 回（失敗はすべて下で記録）
        static uint8_t seen[256];
        if (seen[fn] < 4) { seen[fn]++; plog("[xms] AH=%02X BX=%04X DX=%04X（空き %u KB）\n", fn, (unsigned)(uint16_t)m->cpu.r[EBX], (unsigned)(uint16_t)m->cpu.r[EDX], (unsigned)free_kb); }
    }
    auto fail = [&](uint8_t e) { SETAX(m, 0); SETBL(m, e); if (m->cfg.trace) plog("[xms] AH=%02X -> エラー %02X\n", fn, e); };
    switch (fn) {
    case 0x00: SETAX(m, 0x0300); SETBX(m, 0x0395); SETDX(m, 1); return;
    case 0x01: if (s_hma_used) { fail(0x91); return; } s_hma_used = true; SETAX(m, 1); return;
    case 0x02: if (!s_hma_used) { fail(0x93); return; } s_hma_used = false; SETAX(m, 1); return;
    case 0x03: case 0x05:
        m->a20 = 1; g_addr_mask = A20_ON_MASK; SETAX(m, 1); SETBL(m, 0); return;
    case 0x04: case 0x06:
        m->a20 = 0; g_addr_mask = 0xFFFFF; SETAX(m, 1); SETBL(m, 0); return;
    case 0x07: SETAX(m, m->a20 ? 1 : 0); SETBL(m, 0); return;
    case 0x08: SETAX(m, (uint16_t)(free_kb > 0xFFFF ? 0xFFFF : free_kb)); SETDX(m, (uint16_t)(free_kb > 0xFFFF ? 0xFFFF : free_kb)); SETBL(m, 0); return;
    case 0x88: R(m)[EAX] = free_kb; R(m)[EDX] = free_kb; R(m)[ECX] = 0x10FFFF + s_xms_total_kb * 1024; SETBL(m, 0); return;
    case 0x09: case 0x89: {
        uint32_t kb = fn == 0x09 ? DX(m) : R(m)[EDX];
        if (kb > free_kb) { fail(0xA0); return; }
        int h = xms_new_handle();
        if (h < 0) { fail(0xA1); return; }
        XmsBlock& b = s_xms[h];
        b.used = true; b.kb = kb; b.locks = 0; b.data.assign((size_t)kb * 1024, 0);
        SETAX(m, 1); SETDX(m, (uint16_t)h); return; }
    case 0x0A: {
        XmsBlock* b = xms_block(DX(m));
        if (!b) { fail(0xA2); return; }
        if (b->locks) { fail(0xAB); return; }
        b->used = false; b->data.clear(); b->data.shrink_to_fit(); b->kb = 0;
        SETAX(m, 1); return; }
    case 0x0B: {
        uint32_t p = lin(m->cpu.sr[DS_], (uint16_t)R(m)[ESI]);
        uint32_t len = mem_rd(m, p);
        uint16_t sh = mem_rw(m, p + 4); uint32_t so = mem_rd(m, p + 6);
        uint16_t dh = mem_rw(m, p + 10); uint32_t doff = mem_rd(m, p + 12);
        if (len & 1) { fail(0xA7); return; }
        uint8_t *hs, *hd; uint32_t gs = 0, gd = 0; uint8_t e = 0;
        if (!xms_ptr(m, sh, so, len, &hs, &gs, &e)) { fail(e == 0xA3 ? 0xA3 : 0xA4); return; }
        if (!xms_ptr(m, dh, doff, len, &hd, &gd, &e)) { fail(e == 0xA3 ? 0xA5 : 0xA6); return; }
        if (hs && hd) memmove(hd, hs, len);
        else if (hs) for (uint32_t i = 0; i < len; i++) mem_wb(m, gd + i, hs[i]);
        else if (hd) for (uint32_t i = 0; i < len; i++) hd[i] = mem_rb(m, gs + i);
        else {
            std::vector<uint8_t> t(len);
            for (uint32_t i = 0; i < len; i++) t[i] = mem_rb(m, gs + i);
            for (uint32_t i = 0; i < len; i++) mem_wb(m, gd + i, t[i]);
        }
        SETAX(m, 1); SETBL(m, 0); return; }
    case 0x0C: {
        XmsBlock* b = xms_block(DX(m));
        if (!b) { fail(0xA2); return; }
        if (b->locks < 255) b->locks++;
        // 物理アドレスは形だけ（ハンドルごとに 16MB 超の別領域として見せる）
        uint32_t a = 0x01000000u + (uint32_t)DX(m) * 0x00400000u;
        SETDX(m, (uint16_t)(a >> 16)); SETBX(m, (uint16_t)a); SETAX(m, 1); return; }
    case 0x0D: {
        XmsBlock* b = xms_block(DX(m));
        if (!b) { fail(0xA2); return; }
        if (!b->locks) { fail(0xAA); return; }
        b->locks--; SETAX(m, 1); return; }
    case 0x0E: case 0x8E: {
        XmsBlock* b = xms_block(DX(m));
        if (!b) { fail(0xA2); return; }
        int freeh = 0; for (size_t i = 1; i < 128; i++) if (i >= s_xms.size() || !s_xms[i].used) freeh++;
        SETBH(m, b->locks);
        if (fn == 0x0E) { SETBL(m, (uint8_t)(freeh > 255 ? 255 : freeh)); SETDX(m, (uint16_t)(b->kb > 0xFFFF ? 0xFFFF : b->kb)); }
        else { SETCX(m, (uint16_t)freeh); R(m)[EDX] = b->kb; }
        SETAX(m, 1); return; }
    case 0x0F: case 0x8F: {
        XmsBlock* b = xms_block(DX(m));
        if (!b) { fail(0xA2); return; }
        if (b->locks) { fail(0xAB); return; }
        uint32_t kb = fn == 0x0F ? BX(m) : R(m)[EBX];
        if (kb > b->kb && kb - b->kb > free_kb) { fail(0xA0); return; }
        b->kb = kb; b->data.resize((size_t)kb * 1024, 0);
        SETAX(m, 1); return; }
    case 0x10: SETAX(m, 0); SETBL(m, 0xB1); SETDX(m, 0); return;   // UMB なし
    case 0x11: case 0x12: fail(0xB2); return;
    default: fail(0x80); return;
    }
}

// INT 2Fh AX=43xxh
bool xms_int2f(Machine* m) {
    if (!xms_enabled(m)) return false;
    uint16_t ax = AX(m);
    if (ax == 0x4300) { SETAL(m, 0x80); return true; }
    if (ax == 0x4310) { cpu_setsr(&m->cpu, ES_, ROMSEG); SETBX(m, s_xms_entry_off); return true; }
    return false;
}

// ---- EMS ---------------------------------------------------------------------
static EmsHandle* ems_h(uint16_t h) { return (h < s_ems.size() && s_ems[h].used) ? &s_ems[h] : nullptr; }
static uint32_t ems_used_pages() { uint32_t u = 0; for (auto& h : s_ems) if (h.used) u += (uint32_t)(h.data.size() / EMS_PAGE); return u; }
static uint32_t ems_free_pages() { uint32_t u = ems_used_pages(); return s_ems_total_pages > u ? s_ems_total_pages - u : 0; }
static uint8_t* frame_ptr(Machine* m, int p) { return m->ram + lin(EMS_FRAME_SEG, 0) + p * EMS_PAGE; }
static uint8_t* page_ptr(int h, int l) {
    if (h < 0 || (size_t)h >= s_ems.size() || !s_ems[h].used) return nullptr;
    if ((size_t)(l + 1) * EMS_PAGE > s_ems[h].data.size()) return nullptr;
    return s_ems[h].data.data() + (size_t)l * EMS_PAGE;
}
// フレームの中身を器へ書き戻す
static void ems_flush(Machine* m, int p) {
    uint8_t* d = page_ptr(s_frame[p][0], s_frame[p][1]);
    if (d) memcpy(d, frame_ptr(m, p), EMS_PAGE);
}
static void ems_flush_all(Machine* m) { for (int p = 0; p < 4; p++) ems_flush(m, p); }
static void ems_reload_all(Machine* m) {
    for (int p = 0; p < 4; p++) { uint8_t* d = page_ptr(s_frame[p][0], s_frame[p][1]); if (d) memcpy(frame_ptr(m, p), d, EMS_PAGE); }
}
static uint8_t ems_map(Machine* m, int p, int h, int l) {
    if (p < 0 || p > 3) return 0x8B;
    if (l != 0xFFFF && !page_ptr(h, l)) return ems_h((uint16_t)h) ? 0x8A : 0x83;
    ems_flush_all(m);
    if (l == 0xFFFF) { s_frame[p][0] = -1; s_frame[p][1] = -1; return 0; }
    s_frame[p][0] = h; s_frame[p][1] = l;
    memcpy(frame_ptr(m, p), page_ptr(h, l), EMS_PAGE);
    return 0;
}
static void ems_unmap_handle(Machine* m, int h) {
    for (int p = 0; p < 4; p++) if (s_frame[p][0] == h) { ems_flush(m, p); s_frame[p][0] = s_frame[p][1] = -1; }
}

// 57h 用: 領域の 1 バイトずつの読み書き（通常メモリ or EMS）
struct EmsRegion { uint8_t type; uint16_t h, off, segpage; };
static bool ems_region_addr(const EmsRegion& r, uint32_t i, uint8_t** host, uint32_t* guest) {
    if (r.type == 0) { *host = nullptr; *guest = lin(r.segpage, 0) + r.off + i; return true; }
    uint32_t o = (uint32_t)r.off + i;
    uint8_t* p = page_ptr(r.h, r.segpage + (int)(o / EMS_PAGE));
    if (!p) return false;
    *host = p + (o % EMS_PAGE); return true;
}

static void ems_call(Machine* m) {
    uint8_t fn = AH(m);
    if (m->cfg.trace) {   // 機能ごとに最初の 4 回（失敗はすべて下で記録）
        static uint8_t seen[256];
        if (seen[fn] < 4) { seen[fn]++; plog("[ems] INT67 AX=%04X BX=%04X DX=%04X（空き %d ページ）\n", (unsigned)(uint16_t)m->cpu.r[EAX], (unsigned)(uint16_t)m->cpu.r[EBX], (unsigned)(uint16_t)m->cpu.r[EDX], (int)ems_free_pages()); }
    }
    auto ret = [&](uint8_t st) { SETAH(m, st); if (st && m->cfg.trace) plog("[ems] AH=%02X -> エラー %02X\n", fn, st); };
    switch (fn) {
    case 0x40: ret(0); return;
    case 0x41: SETBX(m, EMS_FRAME_SEG); ret(0); return;
    case 0x42: SETBX(m, (uint16_t)ems_free_pages()); SETDX(m, (uint16_t)s_ems_total_pages); ret(0); return;
    case 0x43: case 0x5A: {
        uint16_t n = BX(m);
        if (fn == 0x43 && n == 0) { ret(0x89); return; }
        if (n > s_ems_total_pages) { ret(0x87); return; }
        if (n > ems_free_pages()) { ret(0x88); return; }
        size_t h = 1;
        for (; h < s_ems.size(); h++) if (!s_ems[h].used) break;
        if (h >= 255) { ret(0x85); return; }
        if (h == s_ems.size()) s_ems.emplace_back();
        s_ems[h] = EmsHandle(); s_ems[h].used = true; s_ems[h].data.assign((size_t)n * EMS_PAGE, 0);
        SETDX(m, (uint16_t)h); ret(0); return; }
    case 0x44: {
        uint16_t h = DX(m);
        if (!ems_h(h)) { ret(0x83); return; }
        ret(ems_map(m, AL(m), h, BX(m))); return; }
    case 0x45: {
        uint16_t h = DX(m);
        if (!ems_h(h)) { ret(0x83); return; }
        ems_unmap_handle(m, h);
        if (h == 0) { s_ems[0].data.clear(); ret(0); return; }
        s_ems[h] = EmsHandle(); ret(0); return; }
    case 0x46: SETAL(m, 0x40); ret(0); return;
    case 0x47: {
        EmsHandle* e = ems_h(DX(m));
        if (!e) { ret(0x83); return; }
        if (e->has_saved) { ret(0x8D); return; }
        for (int p = 0; p < 4; p++) { e->saved[p * 2] = (uint16_t)s_frame[p][0]; e->saved[p * 2 + 1] = (uint16_t)s_frame[p][1]; }
        e->has_saved = true; ret(0); return; }
    case 0x48: {
        EmsHandle* e = ems_h(DX(m));
        if (!e) { ret(0x83); return; }
        if (!e->has_saved) { ret(0x8E); return; }
        ems_flush_all(m);
        for (int p = 0; p < 4; p++) { s_frame[p][0] = (int16_t)e->saved[p * 2]; s_frame[p][1] = (int16_t)e->saved[p * 2 + 1]; }
        ems_reload_all(m);
        e->has_saved = false; ret(0); return; }
    case 0x4B: { int n = 0; for (auto& h : s_ems) if (h.used) n++; SETBX(m, (uint16_t)n); ret(0); return; }
    case 0x4C: { EmsHandle* e = ems_h(DX(m)); if (!e) { ret(0x83); return; } SETBX(m, (uint16_t)(e->data.size() / EMS_PAGE)); ret(0); return; }
    case 0x4D: {
        uint32_t a = lin(m->cpu.sr[ES_], (uint16_t)R(m)[EDI]); int n = 0;
        for (size_t h = 0; h < s_ems.size(); h++) if (s_ems[h].used) { mem_ww(m, a, (uint16_t)h); mem_ww(m, a + 2, (uint16_t)(s_ems[h].data.size() / EMS_PAGE)); a += 4; n++; }
        SETBX(m, (uint16_t)n); ret(0); return; }
    case 0x4E: case 0x4F: {
        uint8_t al = AL(m);
        if (fn == 0x4F) {
            // 部分マップ: 簡単のため全物理ページを保存/復元する（大きさは 4 ページぶん）
            if (al == 0x02) { SETAL(m, 18); ret(0); return; }
            if (al == 0x00) {
                uint32_t d = lin(m->cpu.sr[ES_], (uint16_t)R(m)[EDI]);
                mem_ww(m, d, 4); d += 2;
                for (int p = 0; p < 4; p++) { mem_ww(m, d, (uint16_t)s_frame[p][0]); mem_ww(m, d + 2, (uint16_t)s_frame[p][1]); d += 4; }
                ret(0); return;
            }
            if (al == 0x01) {
                uint32_t s = lin(m->cpu.sr[DS_], (uint16_t)R(m)[ESI]) + 2;
                ems_flush_all(m);
                for (int p = 0; p < 4; p++) { s_frame[p][0] = (int16_t)mem_rw(m, s); s_frame[p][1] = (int16_t)mem_rw(m, s + 2); s += 4; }
                ems_reload_all(m); ret(0); return;
            }
            ret(0x8F); return;
        }
        if (al == 0x03) { SETAL(m, 16); ret(0); return; }
        if (al == 0x00 || al == 0x02) {
            uint32_t d = lin(m->cpu.sr[ES_], (uint16_t)R(m)[EDI]);
            for (int p = 0; p < 4; p++) { mem_ww(m, d, (uint16_t)s_frame[p][0]); mem_ww(m, d + 2, (uint16_t)s_frame[p][1]); d += 4; }
        }
        if (al == 0x01 || al == 0x02) {
            uint32_t s = lin(m->cpu.sr[DS_], (uint16_t)R(m)[ESI]);
            ems_flush_all(m);
            for (int p = 0; p < 4; p++) { s_frame[p][0] = (int16_t)mem_rw(m, s); s_frame[p][1] = (int16_t)mem_rw(m, s + 2); s += 4; }
            ems_reload_all(m);
        }
        if (al > 3) { ret(0x8F); return; }
        ret(0); return; }
    case 0x50: {
        uint16_t h = DX(m);
        if (!ems_h(h)) { ret(0x83); return; }
        uint32_t s = lin(m->cpu.sr[DS_], (uint16_t)R(m)[ESI]);
        for (int i = 0; i < CX(m); i++, s += 4) {
            uint16_t l = mem_rw(m, s), pp = mem_rw(m, s + 2);
            int p = AL(m) == 1 ? (int)((pp - EMS_FRAME_SEG) / 0x400) : pp;
            if (AL(m) == 1 && (pp < EMS_FRAME_SEG || ((pp - EMS_FRAME_SEG) % 0x400))) { ret(0x8B); return; }
            uint8_t st = ems_map(m, p, h, l);
            if (st) { ret(st); return; }
        }
        ret(0); return; }
    case 0x51: {
        uint16_t h = DX(m);
        EmsHandle* e = ems_h(h);
        if (!e) { ret(0x83); return; }
        uint32_t cur = (uint32_t)(e->data.size() / EMS_PAGE), n = BX(m);
        if (n > cur && n - cur > ems_free_pages()) { ret(0x88); return; }
        ems_flush_all(m);
        e->data.resize((size_t)n * EMS_PAGE, 0);
        for (int p = 0; p < 4; p++) if (s_frame[p][0] == h && s_frame[p][1] >= (int)n) s_frame[p][0] = s_frame[p][1] = -1;
        SETBX(m, (uint16_t)n); ret(0); return; }
    case 0x52: if (AL(m) == 0x00) { SETAL(m, 0); ret(0); } else if (AL(m) == 0x01) ret(0); else if (AL(m) == 0x02) { SETAL(m, 0); ret(0); } else ret(0x8F); return;
    case 0x53: {
        EmsHandle* e = ems_h(DX(m));
        if (!e) { ret(0x83); return; }
        if (AL(m) == 0) { uint32_t d = lin(m->cpu.sr[ES_], (uint16_t)R(m)[EDI]); for (int i = 0; i < 8; i++) mem_wb(m, d + i, (uint8_t)e->name[i]); ret(0); return; }
        if (AL(m) == 1) { uint32_t s = lin(m->cpu.sr[DS_], (uint16_t)R(m)[ESI]); for (int i = 0; i < 8; i++) e->name[i] = (char)mem_rb(m, s + i); ret(0); return; }
        ret(0x8F); return; }
    case 0x54: {
        if (AL(m) == 0x02) { SETBX(m, 255); ret(0); return; }
        if (AL(m) == 0x00) {
            uint32_t d = lin(m->cpu.sr[ES_], (uint16_t)R(m)[EDI]); int n = 0;
            for (size_t h = 0; h < s_ems.size(); h++) if (s_ems[h].used) { mem_ww(m, d, (uint16_t)h); for (int i = 0; i < 8; i++) mem_wb(m, d + 2 + i, (uint8_t)s_ems[h].name[i]); d += 10; n++; }
            SETAL(m, (uint8_t)n); ret(0); return;
        }
        if (AL(m) == 0x01) {
            char nm[8]; uint32_t s = lin(m->cpu.sr[DS_], (uint16_t)R(m)[ESI]);
            for (int i = 0; i < 8; i++) nm[i] = (char)mem_rb(m, s + i);
            for (size_t h = 0; h < s_ems.size(); h++) if (s_ems[h].used && !memcmp(s_ems[h].name, nm, 8)) { SETDX(m, (uint16_t)h); ret(0); return; }
            ret(0xA0); return;
        }
        ret(0x8F); return; }
    case 0x57: {
        if (AL(m) > 1) { ret(0x8F); return; }
        uint32_t s = lin(m->cpu.sr[DS_], (uint16_t)R(m)[ESI]);
        uint32_t len = mem_rd(m, s);
        EmsRegion a{mem_rb(m, s + 4), mem_rw(m, s + 5), mem_rw(m, s + 7), mem_rw(m, s + 9)};
        EmsRegion b{mem_rb(m, s + 11), mem_rw(m, s + 12), mem_rw(m, s + 14), mem_rw(m, s + 16)};
        if (len > 0x100000) { ret(0x96); return; }
        if ((a.type == 1 && !ems_h(a.h)) || (b.type == 1 && !ems_h(b.h))) { ret(0x83); return; }
        ems_flush_all(m);
        std::vector<uint8_t> t(len);
        for (uint32_t i = 0; i < len; i++) {
            uint8_t* hp; uint32_t g;
            if (!ems_region_addr(a, i, &hp, &g)) { ret(0x93); return; }
            t[i] = hp ? *hp : mem_rb(m, g);
        }
        std::vector<uint8_t> t2;
        if (AL(m) == 1) {
            t2.resize(len);
            for (uint32_t i = 0; i < len; i++) { uint8_t* hp; uint32_t g; if (!ems_region_addr(b, i, &hp, &g)) { ret(0x93); return; } t2[i] = hp ? *hp : mem_rb(m, g); }
        }
        for (uint32_t i = 0; i < len; i++) {
            uint8_t* hp; uint32_t g;
            if (!ems_region_addr(b, i, &hp, &g)) { ret(0x93); return; }
            if (hp) *hp = t[i]; else mem_wb(m, g, t[i]);
        }
        if (AL(m) == 1) for (uint32_t i = 0; i < len; i++) { uint8_t* hp; uint32_t g; ems_region_addr(a, i, &hp, &g); if (hp) *hp = t2[i]; else mem_wb(m, g, t2[i]); }
        // 通常メモリ側にフレームが含まれていた場合は、そちらの内容を正にする
        bool frame_touched = false;
        for (const EmsRegion* r : {&a, &b}) if (r->type == 0) {
            uint32_t lo = lin(r->segpage, r->off), hi = lo + len, flo = lin(EMS_FRAME_SEG, 0), fhi = flo + 4 * EMS_PAGE;
            if (lo < fhi && hi > flo) frame_touched = true;
        }
        if (frame_touched) ems_flush_all(m);
        ems_reload_all(m);
        ret(0); return; }
    case 0x58: {
        if (AL(m) == 0x01) { SETCX(m, 4); ret(0); return; }
        if (AL(m) == 0x00) {
            uint32_t d = lin(m->cpu.sr[ES_], (uint16_t)R(m)[EDI]);
            for (int p = 0; p < 4; p++) { mem_ww(m, d, (uint16_t)(EMS_FRAME_SEG + p * 0x400)); mem_ww(m, d + 2, (uint16_t)p); d += 4; }
            SETCX(m, 4); ret(0); return;
        }
        ret(0x8F); return; }
    case 0x59: {
        if (AL(m) == 0x00) {
            uint32_t d = lin(m->cpu.sr[ES_], (uint16_t)R(m)[EDI]);
            mem_ww(m, d, 0x400); mem_ww(m, d + 2, 0); mem_ww(m, d + 4, 16); mem_ww(m, d + 6, 0); mem_ww(m, d + 8, 0);
            ret(0); return;
        }
        if (AL(m) == 0x01) { SETBX(m, (uint16_t)ems_free_pages()); SETDX(m, (uint16_t)s_ems_total_pages); ret(0); return; }
        ret(0x8F); return; }
    case 0xDE: ret(0x84); return;   // VCPI なし
    default: ret(0x84); return;
    }
}

bool ems_is_device(const std::string& name) { return name == "EMMXXXX0"; }

// ---- 入口 --------------------------------------------------------------------
void xmsems_hle(Machine* m, uint8_t n) {
    if (n == HLE_XMS) xms_call(m);
    else if (n == HLE_INT67) {
        if (ems_enabled(m)) ems_call(m); else SETAH(m, 0x84);
    }
}

extern uint16_t bios_hle_stub(Machine* m, uint8_t n);
void xmsems_init(Machine* m) {
    s_xms.clear(); s_xms.resize(1);
    s_xms_total_kb = m->cfg.xms_kb > 0 ? (uint32_t)m->cfg.xms_kb : 0;
    s_hma_used = false;
    s_ems.clear(); s_ems.resize(1); s_ems[0].used = true;   // ハンドル 0（システム用、0 ページ）
    s_ems_total_pages = m->cfg.ems ? (uint32_t)(m->cfg.ems_kb / 16) : 0;
    for (int p = 0; p < 4; p++) s_frame[p][0] = s_frame[p][1] = -1;
    // XMS の入口: F1 E0 / RETF
    {
        uint16_t o = bios_hle_stub(m, HLE_XMS);
        m->ram[lin(ROMSEG, (uint16_t)(o + 2))] = 0xCB;
        s_xms_entry_off = o;
    }
    // INT 67h: ベクタのセグメントの 000Ah に装置名、0012h に入口
    if (m->cfg.ems) {
        uint32_t b = lin(EMS_DEV_SEG, 0);
        memset(m->ram + b, 0, 0x20);
        memset(m->ram + b, 0xFF, 4);                 // 装置の鎖: 次は無し
        m->ram[b + 4] = 0x00; m->ram[b + 5] = 0xC0;  // 属性: 文字装置 + IOCTL
        memcpy(m->ram + b + 0x0A, "EMMXXXX0", 8);
        m->ram[b + 0x12] = 0xF1; m->ram[b + 0x13] = HLE_INT67; m->ram[b + 0x14] = 0xCF;
        m->ram[0x67 * 4] = 0x12; m->ram[0x67 * 4 + 1] = 0;
        m->ram[0x67 * 4 + 2] = (uint8_t)(EMS_DEV_SEG & 0xFF); m->ram[0x67 * 4 + 3] = (uint8_t)(EMS_DEV_SEG >> 8);
        memset(m->ram + lin(EMS_FRAME_SEG, 0), 0, 4 * EMS_PAGE);
    }
}

// ---- ステート ----------------------------------------------------------------
void xmsems_state_save(Machine* m, StateW& w) {
    ems_flush_all(m);
    w.tag("XMEM");
    w.u32((uint32_t)s_xms.size());
    for (auto& b : s_xms) { w.pod(b.used); w.u32(b.kb); w.pod(b.locks); w.u32((uint32_t)b.data.size()); if (!b.data.empty()) w.bytes(b.data.data(), b.data.size()); }
    w.pod(s_hma_used);
    w.u32((uint32_t)s_ems.size());
    for (auto& h : s_ems) { w.pod(h.used); w.bytes(h.name, 8); w.bytes(h.saved, sizeof(h.saved)); w.pod(h.has_saved); w.u32((uint32_t)h.data.size()); if (!h.data.empty()) w.bytes(h.data.data(), h.data.size()); }
    w.bytes(s_frame, sizeof(s_frame));
}
void xmsems_state_load(Machine* m, StateR& r) {
    if (!r.peek_tag("XMEM")) {         // 旧形式のステートには無い: 空にしておく
        s_xms.assign(1, XmsBlock()); s_hma_used = false;
        s_ems.assign(1, EmsHandle()); s_ems[0].used = true;
        for (int p = 0; p < 4; p++) s_frame[p][0] = s_frame[p][1] = -1;
        (void)m; return;
    }
    r.tag("XMEM");
    uint32_t n = r.u32(); if (n > 256) { r.ok = false; return; }
    s_xms.assign(n, XmsBlock());
    for (auto& b : s_xms) { r.pod(b.used); b.kb = r.u32(); r.pod(b.locks); uint32_t sz = r.u32(); if (sz > 0x10000000u) { r.ok = false; return; } b.data.resize(sz); if (sz) r.bytes(b.data.data(), sz); }
    r.pod(s_hma_used);
    n = r.u32(); if (n > 256) { r.ok = false; return; }
    s_ems.assign(n, EmsHandle());
    for (auto& h : s_ems) { r.pod(h.used); r.bytes(h.name, 8); r.bytes(h.saved, sizeof(h.saved)); r.pod(h.has_saved); uint32_t sz = r.u32(); if (sz > 0x10000000u) { r.ok = false; return; } h.data.resize(sz); if (sz) r.bytes(h.data.data(), sz); }
    r.bytes(s_frame, sizeof(s_frame));
    // フレームの中身は RAM ごと復元済み
}
