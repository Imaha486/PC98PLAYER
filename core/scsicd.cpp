// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  scsicd.cpp  --  SCSI の CD-ROM ドライブ（ディスク BIOS の INT 1Bh から直接使う）
//
//  MSCDEX を通さず、ディスク BIOS の SCSI の機能で CD-ROM ドライブへ直接コマンドを送るソフトがある
//  （PC-9821 のデモ CD など）。CDROM= で入れた CD を、SCSI ID 2 の CD-ROM ドライブとしても見せる。
//
//  ・0000:0460h + ID×4 … SCSI の装置の表。CD-ROM の ID の先頭バイトを 0 以外にする
//  ・INT 1Bh AL=A0h+ID、AH=x4h（センス）… BL=05h（CD-ROM）を返す
//  ・INT 1Bh AL=C0h+ID … バスのフェーズを 1 つずつ進める低水準の呼び出し。
//      AH=07h 選択 → AH=1Ah コマンドを送る → AH=19h データを受け取る（あれば）→ AH=1Bh 状態
//      → AH=1Fh メッセージ → AH=03h ACK を下げる
//    戻りの AH は「次のフェーズ」: 88h+（0=データ出力 1=データ入力 2=コマンド 3=状態 7=メッセージ入力）
//  ・コマンドは SCSI-2 の基本的なものと、NEC の CD-ROM ドライブ独自のもの（D8h-DEh: 曲の頭出し・
//    演奏・一時停止・サブコード Q・ディスク情報）。音は MSCDEX と同じ CD-DA の再生（cdda.cpp）で出す。
// -----------------------------------------------------------------------------
#include "machine.h"
#include "cdda.h"
#include <string.h>
#include <vector>
#include <string>
#include <stdio.h>

bool dos_cd_present(Machine* m);
bool dos_cd_read(uint32_t lba, uint8_t* out);

static const int CD_ID = 2;

static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }
static inline uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) % 10) << 4 | (v % 10)); }
static inline unsigned unbcd(uint8_t v) { return (unsigned)((v >> 4) * 10 + (v & 15)); }

// ---- 状態（セーブには入れない: 1 回のコマンドの間だけの値と、頭出しの位置） ----
static std::vector<uint8_t> s_data;     // データ入力フェーズで渡すもの
static uint8_t  s_status = 0;           // 00h = GOOD / 02h = CHECK CONDITION
static uint8_t  s_sense = 0;            // センスキー
static int      s_next = 0;             // 次のフェーズ
static uint32_t s_search = 0;           // 頭出しした位置（HSG）
static bool     s_hold = false;         // 頭出しして止まっている
static bool     s_still = false;        // 一時停止（STILL）

void scsicd_setup(Machine* m) {
    uint32_t a = 0x460 + CD_ID * 4;
    bool on = dos_cd_present(m);
    m->ram[a] = on ? 0x01 : 0x00; m->ram[a + 1] = 0; m->ram[a + 2] = 0; m->ram[a + 3] = 0;
}

static void msf_of(uint32_t hsg, uint8_t* p) {   // HSG → 絶対時間（BCD の分・秒・フレーム）
    uint32_t v = hsg + 150;
    p[0] = bcd(v / 4500); p[1] = bcd((v / 75) % 60); p[2] = bcd(v % 75);
}
static uint32_t hsg_of_pos(const uint8_t* cdb, uint8_t type, bool* ok) {
    *ok = true;
    uint32_t pos = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) | cdb[5];
    switch (type & 0xC0) {
    case 0x00: return pos;                                           // 論理ブロック
    case 0x40: {                                                     // BCD の分・秒・フレーム
        uint32_t v = unbcd(cdb[2]) * 4500 + unbcd(cdb[3]) * 75 + unbcd(cdb[4]);
        return v >= 150 ? v - 150 : 0; }
    case 0x80: {                                                     // BCD のトラック番号
        int no = cdb[2] < 0xA0 ? (int)unbcd(cdb[2]) : cdb[2];
        const cdda::Track* t = cdda::track(no);
        if (!t) { *ok = false; return 0; }
        return t->start; }
    }
    *ok = false;
    return 0;
}

static uint8_t ctl_of(int no) { const cdda::Track* t = cdda::track(no); return (t && !t->audio) ? 0x41 : 0x01; }

static void run_command(Machine* m, const uint8_t* cdb) {
    s_data.clear(); s_status = 0; s_sense = 0;
    auto fail = [&](uint8_t key) { s_status = 0x02; s_sense = key; };
    uint8_t op = cdb[0];
    if (m->cfg.trace) {
        static int n = 0;
        if (n++ < 400) plog("[scsi] CD-ROM コマンド %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                            cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8], cdb[9]);
    }
    switch (op) {
    case 0x00: case 0x1B: case 0x1E: case 0x2B: case 0x15: case 0x55: case 0xDB: case 0x0D: case 0xC5:
        return;                                                      // 準備の確認・回転・取り出し禁止・シーク など
    case 0x03: {                                                     // REQUEST SENSE
        s_data.assign(18, 0); s_data[0] = 0x70; s_data[7] = 10;
        if (cdb[4] < s_data.size()) s_data.resize(cdb[4]);
        return; }
    case 0x12: {                                                     // INQUIRY
        static const char id[] = "NEC     CD-ROM DRIVE:84 1.0 ";
        s_data.assign(36, 0);
        s_data[0] = 0x05; s_data[1] = 0x80; s_data[2] = 0x02; s_data[3] = 0x02; s_data[4] = 31;
        memcpy(&s_data[8], id, 28);
        if (cdb[4] < s_data.size()) s_data.resize(cdb[4]);
        return; }
    case 0x25: {                                                     // READ CAPACITY
        uint32_t last = cdda::leadout() ? cdda::leadout() - 1 : 0;
        s_data = {(uint8_t)(last >> 24), (uint8_t)(last >> 16), (uint8_t)(last >> 8), (uint8_t)last, 0, 0, 8, 0};
        return; }
    case 0x08: case 0x28: {                                          // READ(6) / READ(10)
        uint32_t lba, n;
        if (op == 0x08) { lba = ((uint32_t)(cdb[1] & 0x1F) << 16) | (cdb[2] << 8) | cdb[3]; n = cdb[4] ? cdb[4] : 256; }
        else { lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) | cdb[5]; n = (uint32_t)(cdb[7] << 8) | cdb[8]; }
        if (n > 32) n = 32;
        s_data.resize(n * 2048);
        for (uint32_t i = 0; i < n; i++) if (!dos_cd_read(lba + i, &s_data[i * 2048])) { s_data.clear(); fail(0x03); return; }
        cdda::stop();
        return; }
    case 0x43: {                                                     // READ TOC（SCSI-2）
        bool msf = (cdb[1] & 2) != 0;
        int start = cdb[6];
        std::vector<uint8_t> d(4);
        auto ent = [&](int no, uint32_t hsg, uint8_t ctl) {
            uint8_t e[8] = {0, ctl, (uint8_t)no, 0, 0, 0, 0, 0};   // ctl: 上位 4 ビット ADR、下位 4 ビット CONTROL
            if (msf) { uint32_t v = hsg + 150; e[5] = (uint8_t)(v / 4500); e[6] = (uint8_t)((v / 75) % 60); e[7] = (uint8_t)(v % 75); }
            else { e[4] = (uint8_t)(hsg >> 24); e[5] = (uint8_t)(hsg >> 16); e[6] = (uint8_t)(hsg >> 8); e[7] = (uint8_t)hsg; }
            d.insert(d.end(), e, e + 8);
        };
        for (int no = cdda::first_track(); no <= cdda::last_track(); no++) {
            const cdda::Track* t = cdda::track(no);
            if (t && no >= start) ent(no, t->start, ctl_of(no) == 0x41 ? 0x14 : 0x10);
        }
        ent(0xAA, cdda::leadout(), 0x10);
        uint16_t len = (uint16_t)(d.size() - 2);
        d[0] = (uint8_t)(len >> 8); d[1] = (uint8_t)len; d[2] = (uint8_t)cdda::first_track(); d[3] = (uint8_t)cdda::last_track();
        uint16_t al = (uint16_t)((cdb[7] << 8) | cdb[8]);
        if (al && d.size() > al) d.resize(al);
        s_data = d;
        return; }
    case 0x42: {                                                     // READ SUB-CHANNEL（現在位置）
        uint32_t p = cdda::playing() || cdda::paused() ? cdda::position() : s_search;
        int no = cdda::track_at(p); const cdda::Track* t = cdda::track(no);
        uint32_t rel = t && p >= t->start ? p - t->start : 0;
        bool msf = (cdb[1] & 2) != 0;
        std::vector<uint8_t> d(16, 0);
        d[1] = cdda::playing() ? 0x11 : cdda::paused() ? 0x12 : 0x13;
        d[3] = 12; d[4] = 1; d[5] = (uint8_t)(t && !t->audio ? 0x14 : 0x10); d[6] = (uint8_t)no; d[7] = 1;
        auto put = [&](uint8_t* q, uint32_t v, bool abs) {
            if (msf) { if (abs) v += 150; q[1] = (uint8_t)(v / 4500); q[2] = (uint8_t)((v / 75) % 60); q[3] = (uint8_t)(v % 75); }
            else { q[0] = (uint8_t)(v >> 24); q[1] = (uint8_t)(v >> 16); q[2] = (uint8_t)(v >> 8); q[3] = (uint8_t)v; }
        };
        put(&d[8], p, true); put(&d[12], rel, false);
        uint16_t al = (uint16_t)((cdb[7] << 8) | cdb[8]);
        if (!(cdb[2] & 0x40)) d.resize(4);
        if (al && d.size() > al) d.resize(al);
        s_data = d;
        return; }
    case 0x45: case 0x47: {                                          // PLAY AUDIO(10) / PLAY AUDIO MSF
        uint32_t st, en;
        if (op == 0x45) { st = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) | cdb[5]; en = st + (uint32_t)((cdb[7] << 8) | cdb[8]); }
        else { st = cdb[3] * 4500u + cdb[4] * 75u + cdb[5]; en = cdb[6] * 4500u + cdb[7] * 75u + cdb[8]; st = st >= 150 ? st - 150 : 0; en = en >= 150 ? en - 150 : 0; }
        if (en > st) cdda::play(st, en - st);
        s_hold = s_still = false;
        return; }
    case 0x4B:                                                       // PAUSE / RESUME
        if (cdb[8] & 1) cdda::resume(); else if (cdda::playing()) cdda::stop();
        return;
    // ---- NEC の CD-ROM ドライブ独自のコマンド ----
    case 0xD8: {                                                     // 頭出し（bit0 = すぐ演奏）
        bool ok; uint32_t p = hsg_of_pos(cdb, cdb[9], &ok);
        if (!ok) { fail(0x05); return; }
        s_search = p; s_still = false;
        if (cdb[1] & 1) { cdda::play(p, cdda::leadout() > p ? cdda::leadout() - p : 0); s_hold = false; }
        else { if (cdda::playing()) cdda::stop(); s_hold = true; }
        return; }
    case 0xD9: {                                                     // 演奏（頭出しした位置から、ここで指定した位置まで）
        uint32_t end;
        if ((cdb[9] & 0xC0) == 0xC0) end = cdda::leadout();
        else { bool ok; end = hsg_of_pos(cdb, cdb[9], &ok); if (!ok) { fail(0x05); return; } }
        uint32_t from = s_hold || s_still ? s_search : (cdda::playing() ? cdda::position() : s_search);
        if (s_still && cdda::paused()) from = cdda::position();
        if (end > from) cdda::play(from, end - from);
        s_hold = s_still = false;
        return; }
    case 0xDA:                                                       // 一時停止（STILL）
        if (cdda::playing()) { s_search = cdda::position(); cdda::stop(); }
        s_still = true;
        return;
    case 0xDC:                                                       // 取り出し（ここでは止めるだけ）
        if (cdda::playing()) cdda::stop();
        s_hold = s_still = false;
        return;
    case 0xDD: {                                                     // サブコード Q と演奏の状態（10 バイト）
        bool pl = cdda::playing();
        uint32_t p = pl || s_still ? cdda::position() : s_search;
        int no = cdda::track_at(p); const cdda::Track* t = cdda::track(no);
        uint32_t rel = t && p >= t->start ? p - t->start : 0;
        std::vector<uint8_t> d(10, 0);
        d[0] = pl ? 0x00 : s_still ? 0x01 : s_hold ? 0x02 : 0x03;
        d[1] = (uint8_t)(t && !t->audio ? 0x14 : 0x10);
        d[2] = bcd((unsigned)no); d[3] = 0x01;
        d[4] = bcd(rel / 4500); d[5] = bcd((rel / 75) % 60); d[6] = bcd(rel % 75);
        msf_of(p, &d[7]);
        size_t al = cdb[1] & 0x1F;
        if (al && d.size() > al) d.resize(al);
        s_data = d;
        return; }
    case 0xDE: {                                                     // ディスク情報
        int type = cdb[1] & 3;
        std::vector<uint8_t> d;
        if (type == 0) d = {bcd((unsigned)cdda::first_track()), bcd((unsigned)cdda::last_track()), 0, 0};
        else if (type == 1) { d.assign(4, 0); msf_of(cdda::leadout(), &d[0]); }
        else if (type == 2) {
            int no = cdb[2] < 0xA0 ? (int)unbcd(cdb[2]) : cdb[2];
            const cdda::Track* t = cdda::track(no);
            if (!t) { fail(0x05); return; }
            d.assign(4, 0); msf_of(t->start, &d[0]); d[3] = ctl_of(no);
        } else {
            // 全体の目次: 長さ（2 バイト、上位から）+ 10 バイトの項目
            //（制御/ADR, トラック 00, ポイント, 分秒フレーム 3 バイト, 0, ポイントの分秒フレーム 3 バイト）
            d.assign(2, 0);
            auto ent = [&](uint8_t point, uint8_t ctl, uint8_t pm, uint8_t ps, uint8_t pf) {
                uint8_t e[10] = {ctl, 0, point, 0, 0, 0, 0, pm, ps, pf};
                d.insert(d.end(), e, e + 10);
            };
            ent(0xA0, 0x01, bcd((unsigned)cdda::first_track()), 0, 0);
            ent(0xA1, 0x01, bcd((unsigned)cdda::last_track()), 0, 0);
            { uint8_t q[3]; msf_of(cdda::leadout(), q); ent(0xA2, 0x01, q[0], q[1], q[2]); }
            for (int no = cdda::first_track(); no <= cdda::last_track(); no++) {
                const cdda::Track* t = cdda::track(no);
                if (!t) continue;
                uint8_t q[3]; msf_of(t->start, q);
                ent(bcd((unsigned)no), ctl_of(no), q[0], q[1], q[2]);
            }
            uint16_t len = (uint16_t)(d.size() - 2);
            d[0] = (uint8_t)(len >> 8); d[1] = (uint8_t)len;
            if (m->cfg.trace) { std::string h; char t[4]; for (size_t i = 0; i < d.size(); i++) { snprintf(t, sizeof t, "%02X ", d[i]); h += t; } plog("[scsi] 目次 %s\n", h.c_str()); }
            uint16_t al = (uint16_t)((cdb[7] << 8) | cdb[8]);
            if (al && d.size() > al) d.resize(al);
        }
        s_data = d;
        return; }
    default:
        if (m->cfg.trace) plog("[scsi] CD-ROM: 知らないコマンド %02Xh\n", op);
        fail(0x05);
        return;
    }
}

// INT 1Bh（SCSI の装置）。扱ったら true
bool scsicd_int1b(Machine* m) {
    Cpu* c = &m->cpu;
    uint8_t al = (uint8_t)c->r[EAX], ah = (uint8_t)(c->r[EAX] >> 8);
    auto ret = [&](uint8_t v, bool cf) { c->r[EAX] = (c->r[EAX] & 0xFFFF00FFu) | ((uint32_t)v << 8); set_cf(m, cf); };
    if ((al & 0xF0) == 0xA0) {                                       // SCSI の装置（ディスク BIOS の普通の機能）
        if ((al & 7) != CD_ID || !dos_cd_present(m)) return false;
        if ((ah & 0x0F) == 0x04) { c->r[EBX] = (c->r[EBX] & 0xFFFFFF00u) | 0x05; ret(0x00, false); return true; }   // センス: CD-ROM
        ret(0x60, true); return true;
    }
    if ((al & 0xF0) != 0xC0) return false;                           // 低水準の SCSI の呼び出し
    int id = al & 7;
    uint8_t fn = ah & 0x1F;
    if (m->cfg.trace) { static int n = 0; if (n++ < 300) plog("[scsi] INT1B AX=%02X%02X ES:BX=%04X:%04X CX=%04X\n", ah, al, c->sr[ES_], (unsigned)(uint16_t)c->r[EBX], (unsigned)(uint16_t)c->r[ECX]); }
    uint32_t buf = lin(c->sr[ES_], (uint16_t)c->r[EBX]);
    uint16_t cnt = (uint16_t)c->r[ECX];
    switch (fn) {
    case 0x00: s_next = 0; ret(0x00, false); return true;            // リセット
    case 0x03: ret(0x00, false); return true;                        // ACK を下げる
    case 0x07: case 0x09:                                            // 選択
        if (id != CD_ID || !dos_cd_present(m)) { ret(0x42, true); return true; }   // 応答なし
        s_next = 2;
        ret(0x8A, false); return true;                               // 次はコマンド
    case 0x1A: {                                                     // コマンド
        uint8_t cdb[12] = {0};
        for (int i = 0; i < 12 && i < (cnt ? cnt : 12); i++) cdb[i] = mem_rb(m, buf + (uint32_t)i);
        run_command(m, cdb);
        s_next = (s_status == 0 && !s_data.empty()) ? 1 : 3;
        ret((uint8_t)(0x88 | s_next), false); return true; }
    case 0x19: {                                                     // データを受け取る
        size_t n = (cnt && cnt < s_data.size()) ? cnt : s_data.size();   // CX=0 は「あるだけ全部」
        for (size_t i = 0; i < n; i++) mem_wb(m, buf + (uint32_t)i, s_data[i]);
        s_data.clear(); s_next = 3;
        ret(0x8B, false); return true; }
    case 0x18: s_next = 3; ret(0x8B, false); return true;            // データを送る（受け流す）
    case 0x1B: mem_wb(m, buf, s_status); s_next = 7; ret(0x8F, false); return true;   // 状態
    case 0x1F: mem_wb(m, buf, 0x00); s_next = 0; ret(0x00, false); return true;       // メッセージ（コマンド完了）
    }
    ret(0x42, true);
    return true;
}
