// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  floppy.cpp  --  フロッピーディスクのイメージとディスク BIOS（INT 1Bh）
//
//  対応形式
//   ・D88（.D88/.D68/.88D/.D98 など。複数枚を連結したファイルは先頭の 1 枚）
//   ・ヘッダ無しのベタ（.HDM/.XDF/.DUP/.TFD/.2HD/.IMG 等。大きさで形を決める）
//   ・FDI（Anex86。4096 バイト前後のヘッダ + ベタ）
//   ・NFD（T98-Next。r0 = 固定の ID 表、r1 = トラックごとの ID 表と再試行データ）
//   ・SCP / HFE（フラックス。fdflux.cpp で復調。読み込みのみ）
//   ・実機のドライブ（fdreal.cpp。USB フロッピー / Greaseweazle）
//  いずれも各セクタのデータがファイルのどこにあるかを覚え、書き込みはその場所へ反映する。
//
//  INT 1Bh はフロッピーの DA（90h=1MB, 30h=1.44MB, 10h/70h=640KB）だけを扱う。
//  セクタは ID（C/H/R/N）で探すので、ID をずらしたセクタや CRC エラーのセクタ、
//  デリーテッドデータ等（キーディスクの確認に使われるもの）もイメージどおりに返る。
// -----------------------------------------------------------------------------
#include "floppy.h"
#include "hostfs.h"
#include "machine.h"
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include "fdreal.h"

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    void* h = hostfs::open(path, 0, false, false);
    if (!h) return false;
    int64_t n = hostfs::seek(h, 0, 2);
    hostfs::seek(h, 0, 0);
    if (n < 0 || n > 64 * 1024 * 1024) { hostfs::close(h); return false; }
    out.resize((size_t)n);
    int r = n ? hostfs::read(h, out.data(), (int)n) : 0;
    hostfs::close(h);
    return r == (int)n;
}
static std::string upper_ext(const std::string& p) {
    size_t d = p.find_last_of('.');
    size_t s = p.find_last_of("\\/");
    if (d == std::string::npos || (s != std::string::npos && d < s)) return "";
    std::string e = p.substr(d + 1);
    for (auto& c : e) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return e;
}

// ---- 各形式 --------------------------------------------------------------------
bool FloppyImage::load_d88(const std::vector<uint8_t>& d) {
    if (d.size() < 0x2B0) return false;
    uint32_t size = rd32(&d[0x1C]);
    if (size < 0x20 || size > d.size()) size = (uint32_t)d.size();
    wprot = d[0x1A] != 0;
    uint8_t mt = d[0x1B];
    media = mt == 0x20 ? FD_2HD : mt == 0x10 ? FD_2DD : mt == 0x30 ? FD_144 : FD_2D;
    // トラック表の長さ: 先頭トラックの位置から逆算（164 本が普通、古いものは 160）
    uint32_t first = 0;
    for (int t = 0; t < 164; t++) { uint32_t o = rd32(&d[0x20 + t * 4]); if (o && (!first || o < first)) first = o; }
    int ntr = first ? std::min<int>(164, (int)((first - 0x20) / 4)) : 164;
    for (int t = 0; t < ntr && t < MAX_TRACKS; t++) {
        uint32_t o = rd32(&d[0x20 + t * 4]);
        if (!o || o >= size) continue;
        uint32_t p = o;
        int count = -1;
        for (int k = 0; k < 256 && p + 16 <= size; k++) {
            const uint8_t* hd = &d[p];
            if (count < 0) count = rd16(hd + 4);
            if (k >= count) break;
            FdSector s;
            s.c = hd[0]; s.h = hd[1]; s.r = hd[2]; s.n = hd[3];
            s.fm = hd[6] == 0x40;
            s.deleted = hd[7] == 0x10;
            s.status = hd[8];
            uint16_t len = rd16(hd + 14);
            if (p + 16 + len > size) len = (uint16_t)(size - p - 16);
            s.data.assign(d.begin() + p + 16, d.begin() + p + 16 + len);
            s.file_off = p + 16;
            s.copies = 1; s.next_copy = 0;
            trk[t].secs.push_back(s);
            p += 16 + len;
        }
        if (t / 2 + 1 > cylinders) cylinders = t / 2 + 1;
    }
    format = "D88";
    return true;
}

bool FloppyImage::load_raw(const std::vector<uint8_t>& d, int64_t base, int cyl, int hd, int spt, int ssize, int md) {
    int n = ssize == 128 ? 0 : ssize == 256 ? 1 : ssize == 512 ? 2 : ssize == 1024 ? 3 : ssize == 2048 ? 4 : 2;
    int64_t pos = base;
    for (int c = 0; c < cyl; c++) for (int h = 0; h < hd; h++) {
        int t = c * 2 + h;
        if (t >= MAX_TRACKS) continue;
        for (int r = 1; r <= spt; r++) {
            if (pos + ssize > (int64_t)d.size()) break;
            FdSector s;
            s.c = (uint8_t)c; s.h = (uint8_t)h; s.r = (uint8_t)r; s.n = (uint8_t)n;
            s.status = 0; s.deleted = false; s.fm = false;
            s.data.assign(d.begin() + pos, d.begin() + pos + ssize);
            s.file_off = pos; s.copies = 1; s.next_copy = 0;
            trk[t].secs.push_back(s);
            pos += ssize;
        }
    }
    cylinders = cyl; heads = hd; media = md;
    return true;
}

bool FloppyImage::load_fdi(const std::vector<uint8_t>& d) {
    if (d.size() < 32) return false;
    // ヘッダ: +0 予備, +4 種類(90h=1MB 2HD, 30h=1.44MB, 10h=640KB), +8 ヘッダ長, +12 データ長,
    //        +16 セクタ長, +20 セクタ数/トラック, +24 面数, +28 シリンダ数
    uint32_t type = rd32(&d[4]), hsize = rd32(&d[8]);
    uint32_t ssize = rd32(&d[16]), spt = rd32(&d[20]), heads_ = rd32(&d[24]), cyls = rd32(&d[28]);
    if (hsize == 0 || hsize > d.size() || !ssize || ssize > 2048 || !spt || spt > 64 || !heads_ || heads_ > 2 || !cyls || cyls > 84) return false;
    int md = (type & 0xF0) == 0x90 ? FD_2HD : (type & 0xF0) == 0x30 ? FD_144 : FD_2DD;
    if (spt == 18) md = FD_144;
    load_raw(d, hsize, (int)cyls, (int)heads_, (int)spt, (int)ssize, md);
    format = "FDI";
    return true;
}

// NFD の ID 表の結果（BIOS の結果コードが 0 でも、FDC のステータス ST1/ST2 に異常があればそこから作る）
static uint8_t nfd_status(uint8_t st, uint8_t st1, uint8_t st2) {
    if (st) return st;
    if (st1 & 0x20) return (st2 & 0x20) ? 0xB0 : 0xA0;   // DE: データ部 / ID 部の CRC エラー
    if (st1 & 0x04) return 0xC0;                         // ND: セクタが無い
    if (st1 & 0x01) return (st2 & 0x01) ? 0xF0 : 0xE0;   // MA: アドレスマークが無い
    if (st2 & 0x10) return 0xD0;                         // WC: シリンダ違い
    if (st2 & 0x40) return 0x10;                         // CM: デリーテッドデータ
    return 0x00;
}

// NFD r0: ヘッダ（0x120 から 163 トラック x 26 個の 16 バイトの ID）の後にデータが順に並ぶ
bool FloppyImage::load_nfd0(const std::vector<uint8_t>& d) {
    if (d.size() < 0x120) return false;
    uint32_t hsize = rd32(&d[0x110]);
    if (hsize < 0x120 + 163 * 26 * 16 || hsize > d.size()) return false;
    wprot = d[0x114] != 0;
    int64_t pos = hsize;
    bool any_hd = false;
    for (int t = 0; t < 163; t++) {
        for (int k = 0; k < 26; k++) {
            const uint8_t* e = &d[0x120 + (t * 26 + k) * 16];
            if (e[0] == 0xFF) continue;
            FdSector s;
            s.c = e[0]; s.h = e[1]; s.r = e[2]; s.n = e[3];
            s.fm = e[4] == 0; s.deleted = e[5] != 0; s.status = nfd_status(e[6], e[8], e[9]);
            if (e[3] >= 3) any_hd = true;
            int len = 128 << (e[3] & 7);
            if (pos + len > (int64_t)d.size()) len = (int)std::max<int64_t>(0, (int64_t)d.size() - pos);
            s.data.assign(d.begin() + pos, d.begin() + pos + len);
            s.file_off = pos; s.copies = 1; s.next_copy = 0;
            pos += len;
            if (t < MAX_TRACKS) trk[t].secs.push_back(s);
        }
        if (!trk[t].secs.empty() && t / 2 + 1 > cylinders) cylinders = t / 2 + 1;
    }
    media = any_hd || cylinders > 80 ? FD_2HD : FD_2DD;
    format = "NFD(r0)";
    return true;
}

// NFD r1: 0x120 からトラックごとのヘッダの位置（164 本）。トラックのヘッダは
// {セクタ数, 特殊読み込み数, 予備} 16 バイト、続いてセクタ ID（16 バイト）と特殊読み込み（READ DIAGNOSTIC）の
// ID（16 バイト）。データはヘッダ（dwHeadSize）の後にトラック順に並ぶ:
// 各トラックの「セクタのデータ（再試行があればその回数ぶん）」→「特殊読み込みのデータ（同）」。
//   セクタ ID : C H R N MFM DDAM 結果 ST0 ST1 ST2 再試行数 PDA 予備x4
//   特殊読込ID: 命令 C H R N 結果 ST0 ST1 ST2 再試行数 データ長(4) PDA 予備
bool FloppyImage::load_nfd1(const std::vector<uint8_t>& d) {
    if (d.size() < 0x120 + 164 * 4) return false;
    wprot = d[0x114] != 0;
    uint32_t hsize = rd32(&d[0x110]);
    if (hsize < 0x120 + 164 * 4 || hsize > d.size()) return false;
    bool any_hd = false;
    uint64_t datap = hsize;
    auto take = [&](uint64_t len) -> std::pair<uint64_t, uint64_t> {   // (位置, 実際の長さ)
        uint64_t p = datap, n = datap + len <= d.size() ? len : (datap < d.size() ? d.size() - datap : 0);
        datap += len;
        return {p, n};
    };
    for (int t = 0; t < 164; t++) {
        uint32_t o = rd32(&d[0x120 + t * 4]);
        if (!o || o + 16 > d.size()) continue;
        int nsec = rd16(&d[o]), ndiag = rd16(&d[o + 2]);
        if (nsec > 255 || ndiag > 255 || o + 16 + (uint32_t)(nsec + ndiag) * 16 > d.size()) continue;
        const uint8_t* ids = &d[o + 16];
        for (int k = 0; k < nsec; k++) {
            const uint8_t* e = ids + k * 16;
            FdSector s;
            s.c = e[0]; s.h = e[1]; s.r = e[2]; s.n = e[3];
            s.fm = e[4] == 0; s.deleted = e[5] != 0; s.status = nfd_status(e[6], e[8], e[9]);
            if (s.deleted && s.status == 0x10) s.status = 0x10;
            if (e[3] >= 3) any_hd = true;
            int len = 128 << (e[3] & 7);
            s.copies = 1 + e[10]; s.next_copy = 0;
            auto r = take((uint64_t)len * s.copies);
            s.data.assign(d.begin() + r.first, d.begin() + r.first + r.second);
            s.file_off = (int64_t)r.first;
            if ((int)s.data.size() < len * s.copies) s.copies = 1;
            if (t < MAX_TRACKS) trk[t].secs.push_back(s);
        }
        for (int k = 0; k < ndiag; k++) {
            const uint8_t* e = ids + (nsec + k) * 16;
            FdDiag g;
            g.c = e[1]; g.h = e[2]; g.r = e[3]; g.n = e[4];
            g.status = nfd_status(e[5], e[7], e[8]);
            uint32_t len = rd32(e + 10);
            g.copies = 1 + e[9]; g.next_copy = 0;
            auto r = take((uint64_t)len * g.copies);
            g.data.assign(d.begin() + r.first, d.begin() + r.first + r.second);
            g.file_off = (int64_t)r.first;
            if (g.data.size() < (size_t)len * g.copies) g.copies = 1;
            if (t < MAX_TRACKS) trk[t].diags.push_back(g);
        }
        if ((!trk[t].secs.empty() || !trk[t].diags.empty()) && t / 2 + 1 > cylinders) cylinders = t / 2 + 1;
    }
    media = any_hd || cylinders > 80 ? FD_2HD : FD_2DD;
    format = "NFD(r1)";
    return true;
}

bool FloppyImage::load(const std::string& p, std::string* err) {
    std::vector<uint8_t> d;
    if (!read_file(p, d)) { if (err) *err = "ファイルを読めません"; return false; }
    for (auto& t : trk) { t.secs.clear(); t.diags.clear(); t.rot = 0; }
    cylinders = 0; heads = 2; wprot = false;
    path = p;
    std::string ext = upper_ext(p);
    bool ok = false;
    if (d.size() >= 3 && !memcmp(d.data(), "SCP", 3)) { ok = load_scp(d, err); if (!ok) return false; }
    else if (d.size() >= 8 && (!memcmp(d.data(), "HXCPICFE", 8) || !memcmp(d.data(), "HXCHFEV3", 8))) { ok = load_hfe(d, err); if (!ok) return false; }
    else if (d.size() >= 15 && !memcmp(d.data(), "T98FDDIMAGE.R0", 14)) ok = load_nfd0(d);
    else if (d.size() >= 15 && !memcmp(d.data(), "T98FDDIMAGE.R1", 14)) ok = load_nfd1(d);
    else if (ext == "FDI") ok = load_fdi(d);
    else if (d.size() >= 0x2B0 && rd32(&d[0x1C]) >= 0x2B0 && rd32(&d[0x1C]) <= d.size() + 0x100 &&
             (rd32(&d[0x20]) == 0x2B0 || rd32(&d[0x20]) == 0x2A0 || (rd32(&d[0x20]) >= 0x2A0 && rd32(&d[0x20]) < 0x1000)) &&
             (d[0x1B] == 0x00 || d[0x1B] == 0x10 || d[0x1B] == 0x20 || d[0x1B] == 0x30)) ok = load_d88(d);
    // 拡張子が違っても、FDI のヘッダ（ヘッダ長 + データ長 = ファイルの大きさ）なら FDI として読む
    if (!ok && (ext == "FDI" || (d.size() >= 32 && rd32(&d[0]) == 0 && (uint64_t)rd32(&d[8]) + rd32(&d[12]) == d.size())))
        ok = load_fdi(d);
    if (!ok) {
        // ベタ: 大きさで形を決める
        struct G { size_t size; int c, h, spt, ss, md; };
        static const G g[] = {
            {1261568, 77, 2, 8, 1024, FD_2HD},   // 1.25MB（PC-98 標準）
            {1228800, 80, 2, 15, 512, FD_2HD},   // 1.2MB（2HC）
            {1474560, 80, 2, 18, 512, FD_144},   // 1.44MB
            {737280, 80, 2, 9, 512, FD_2DD},     // 720KB
            {655360, 80, 2, 8, 512, FD_2DD},     // 640KB
            {327680, 40, 2, 8, 512, FD_2D},      // 320KB
        };
        for (const auto& x : g) if (d.size() == x.size) { load_raw(d, 0, x.c, x.h, x.spt, x.ss, x.md); ok = true; break; }
        if (ok) format = "ベタ";
    }
    if (!ok) { if (err) *err = "対応していない形式です"; return false; }
    setup_geometry();
    return true;
}

FdSector* FloppyImage::find(int cyl, int head, uint8_t c, uint8_t h, uint8_t r, uint8_t n) {
    FdTrack* t = track(cyl, head);
    if (!t) return nullptr;
    for (auto& s : t->secs) if (s.c == c && s.h == h && s.r == r && s.n == n) return &s;
    return nullptr;
}

bool FloppyImage::write_sector(FdSector* s, const uint8_t* buf, int len) {
    if (wprot || !s) return false;
    int ss = 128 << (s->n & 7);
    if (len > ss) len = ss;
    if (src) {   // 実機のドライブ
        int cyl = -1, head = 0;
        for (int k = 0; k < MAX_TRACKS && cyl < 0; k++)
            for (auto& x : trk[k].secs) if (&x == s) { cyl = k / 2; head = k & 1; break; }
        if (cyl < 0 || !src->write(cyl, head, *s, buf, len)) return false;
        if ((int)s->data.size() < len) s->data.resize(len);
        memcpy(s->data.data(), buf, (size_t)len);
        return true;
    }
    if ((int)s->data.size() < len) s->data.resize(len);
    memcpy(s->data.data(), buf, (size_t)len);
    if (s->file_off < 0) return true;
    void* h = hostfs::open(path, 2, false, false);
    if (!h) return false;
    bool ok = hostfs::seek(h, s->file_off, 0) == s->file_off && hostfs::write(h, buf, len) == len;
    // NFD の再試行データは 1 つ目だけ書き換え、以降も同じにする
    for (int k = 1; ok && k < s->copies; k++) {
        if ((int)s->data.size() >= (k + 1) * ss) memcpy(&s->data[k * ss], buf, (size_t)len);
        ok = hostfs::seek(h, s->file_off + (int64_t)k * ss, 0) >= 0 && hostfs::write(h, buf, len) == len;
    }
    hostfs::close(h);
    return ok;
}

void FloppyImage::setup_geometry() {
    // トラック 0 表のセクタ数と長さから。起動セクタに BPB があればそちらを優先
    static const FdTrack empty;
    const FdTrack* p0 = track(0, 0);
    const FdTrack& t0 = p0 ? *p0 : empty;
    int spt = 0, ss = 1024;
    for (const auto& s : t0.secs) { if (s.status >= 0xA0) continue; if (s.r > spt && s.r <= 64) spt = s.r; ss = 128 << (s.n & 7); }
    // 先頭トラックが FM 128 バイトで、その他が MFM の N88 形式などは、トラック 1 を見る
    if (!t0.secs.empty() && t0.secs[0].n == 0) {
        const FdTrack* p1 = track(0, 1);
        if (p1 && !p1->secs.empty()) { spt = 0; for (const auto& s : p1->secs) { if (s.r > spt && s.r <= 64) spt = s.r; ss = 128 << (s.n & 7); } }
    }
    lsec_size = ss; lsec_spt = spt ? spt : 8; lsec_heads = 2;
    uint8_t boot[2048];
    FdSector* b = find(0, 0, 0, 0, 1, (uint8_t)(ss == 128 ? 0 : ss == 256 ? 1 : ss == 512 ? 2 : 3));
    if (b && b->data.size() >= 32) {
        memcpy(boot, b->data.data(), std::min<size_t>(b->data.size(), sizeof(boot)));
        uint16_t bps = rd16(boot + 11), sp = rd16(boot + 0x18), hd = rd16(boot + 0x1A);
        if (bps == (uint16_t)ss && sp >= 1 && sp <= 64 && hd >= 1 && hd <= 2) { lsec_spt = sp; lsec_heads = hd; }
    }
    lsec_total = (uint32_t)(cylinders * lsec_heads * lsec_spt);
}

FdSector* FloppyImage::lba_sector(uint32_t lba) {
    int spt = lsec_spt, hd = lsec_heads;
    int c = (int)(lba / (uint32_t)(spt * hd)), h = (int)((lba / (uint32_t)spt) % (uint32_t)hd), r = (int)(lba % (uint32_t)spt) + 1;
    FdTrack* t = track(c, h);
    if (!t) return nullptr;
    for (auto& s : t->secs) if (s.r == r && (128 << (s.n & 7)) == lsec_size) return &s;
    return nullptr;
}
FloppyImage::~FloppyImage() { delete src; }

FdTrack* FloppyImage::track(int cyl, int head) {
    int t = cyl * 2 + head;
    if (t < 0 || t >= MAX_TRACKS) return nullptr;
    // 実機で読めなかったセクタ（file_off == -2）があるトラックは、使うたびにもう一度読みに行く（ゲームのやり直しに合わせる）
    if (src && loaded[t]) {
        for (auto& x : trk[t].secs) if (x.file_off == -2) { loaded[t] = false; break; }
    }
    if (src && !loaded[t]) {
        FdTrack nt;
        if (src->fetch(cyl, head, nt)) { trk[t] = std::move(nt); loaded[t] = true; not_ready = false; }
        else {
            trk[t].secs.clear(); trk[t].diags.clear();
            std::string e = src->last_error();
            if (!e.empty()) plog("[fd] C=%02X H=%02X を読めません: %s\n", cyl, head, e.c_str());
            not_ready = !e.empty();   // 理由があれば「ディスクが無い」等。空なら未フォーマットのトラック
            if (!not_ready) loaded[t] = true;
        }
    }
    return &trk[t];
}

bool FloppyImage::open_source(FdSource* s, const std::string& name, const std::string& fmt, std::string* err) {
    delete src; src = s;
    for (int t = 0; t < MAX_TRACKS; t++) { trk[t].secs.clear(); trk[t].diags.clear(); trk[t].rot = 0; loaded[t] = false; }
    path = name; format = fmt;
    wprot = !s->writable();
    // トラック 0 を読んで媒体の形を決める
    FdTrack* t0 = track(0, 0);
    if (not_ready || !t0 || t0->secs.empty()) {
        if (err) *err = src->last_error().empty() ? "ディスクを読めません（未フォーマット？）" : src->last_error();
        return false;
    }
    int n3 = 0, n2 = 0, spt = 0;
    for (auto& x : t0->secs) { if (x.n == 3) n3++; if (x.n == 2) n2++; if (x.r > spt) spt = x.r; }
    media = (src->media_hint >= 0 && src->media_hint <= 3) ? src->media_hint : FD_2HD;
    if (src->cyl_hint > 0) cylinders = src->cyl_hint;
    // 1024 バイト x 8 = PC-98 の 2HD（77 シリンダ）、512 x 18 = 1.44MB、512 x 15 = 2HC、512 x 8/9 = 2DD
    if (cylinders <= 0) cylinders = (n3 >= 8 && media == FD_2HD) ? 77 : 80;
    setup_geometry();
    return true;
}

bool FloppyImage::check_media_change() {
    if (!src) return false;
    if (!loaded[0]) return false;   // まだ読んでいない（次に使うときに読む）
    FdTrack nt;
    bool ok = src->fetch(0, 0, nt);
    bool same = ok && nt.secs.size() == trk[0].secs.size();
    if (same) {
        for (size_t i = 0; i < nt.secs.size() && same; i++) {
            const FdSector &a = nt.secs[i], &b = trk[0].secs[i];
            same = a.c == b.c && a.h == b.h && a.r == b.r && a.n == b.n && a.status == b.status && a.data == b.data;
        }
    }
    if (same) return false;
    for (int t = 0; t < MAX_TRACKS; t++) { loaded[t] = false; trk[t].secs.clear(); trk[t].diags.clear(); trk[t].rot = 0; }
    if (ok) { trk[0] = std::move(nt); loaded[0] = true; not_ready = false; setup_geometry(); }
    else not_ready = !src->last_error().empty();
    wprot = !src->writable() || floppy::force_wprot();   // 新しいディスクの書き込み禁止のつまみ
    return true;
}

void FloppyImage::refetch() {
    if (!src) return;
    for (int t = 0; t < MAX_TRACKS; t++) { loaded[t] = false; trk[t].rot = 0; }
    not_ready = false;
    setup_geometry();
}

// D88 で書き出す（ID・状態・デリーテッドを残す。読むたびに変わるデータは 1 つ目だけ）
bool FloppyImage::save_d88(const std::string& out_path, std::string* err) {
    std::vector<uint8_t> o(0x2B0, 0);
    std::string name = "PC98PLAYER";
    memcpy(&o[0], name.data(), name.size());
    o[0x1A] = 0;
    o[0x1B] = media == FD_2HD ? 0x20 : media == FD_144 ? 0x30 : media == FD_2DD ? 0x10 : 0x00;
    int ncyl = std::min(cylinders > 0 ? cylinders : 80, 82);
    for (int t = 0; t < ncyl * 2 && t < 164; t++) {
        FdTrack* tr = track(t / 2, t & 1);
        if (not_ready) { if (err) *err = "ディスクが読めなくなりました"; return false; }
        if (!tr || tr->secs.empty()) continue;
        uint32_t pos = (uint32_t)o.size();
        o[0x20 + t * 4] = (uint8_t)pos; o[0x21 + t * 4] = (uint8_t)(pos >> 8);
        o[0x22 + t * 4] = (uint8_t)(pos >> 16); o[0x23 + t * 4] = (uint8_t)(pos >> 24);
        for (auto& s : tr->secs) {
            size_t ss = (size_t)128 << (s.n & 7);
            size_t len = s.copies > 1 ? std::min(ss, s.data.size()) : s.data.size();
            if (s.status == 0x00 || s.status == 0x10 || s.status == 0xB0) len = std::max(len, std::min<size_t>(ss, 16384));
            if (s.status == 0xA0 || s.status == 0xE0 || s.status == 0xF0) len = 0;
            uint8_t h[16] = {};
            h[0] = s.c; h[1] = s.h; h[2] = s.r; h[3] = s.n;
            h[4] = (uint8_t)tr->secs.size(); h[5] = (uint8_t)(tr->secs.size() >> 8);
            h[6] = s.fm ? 0x40 : 0x00;
            h[7] = s.deleted ? 0x10 : 0x00;
            h[8] = s.status;
            h[14] = (uint8_t)len; h[15] = (uint8_t)(len >> 8);
            o.insert(o.end(), h, h + 16);
            for (size_t i = 0; i < len; i++) o.push_back(i < s.data.size() ? s.data[i] : 0);
        }
    }
    uint32_t total = (uint32_t)o.size();
    o[0x1C] = (uint8_t)total; o[0x1D] = (uint8_t)(total >> 8); o[0x1E] = (uint8_t)(total >> 16); o[0x1F] = (uint8_t)(total >> 24);
    void* h = hostfs::open(out_path, 1, true, true);
    if (!h) { if (err) *err = "書き出し先を作れません"; return false; }
    bool ok = hostfs::write(h, o.data(), (int)o.size()) == (int)o.size();
    hostfs::close(h);
    if (!ok && err) *err = "書き出しに失敗しました";
    return ok;
}

bool FloppyImage::read_lba(uint32_t lba, uint8_t* buf) {
    FdSector* s = lba_sector(lba);
    if (!s) { memset(buf, 0, (size_t)lsec_size); return false; }
    size_t n = std::min<size_t>(s->data.size(), (size_t)lsec_size);
    memcpy(buf, s->data.data(), n);
    if (n < (size_t)lsec_size) memset(buf + n, 0, (size_t)lsec_size - n);
    return true;
}
bool FloppyImage::write_lba(uint32_t lba, const uint8_t* buf) {
    FdSector* s = lba_sector(lba);
    return s && write_sector(s, buf, lsec_size);
}

// ---- 本体とのつなぎ ---------------------------------------------------------------
namespace floppy {
static FloppyImage* s_img = nullptr;
static char s_letter = 'B';
static uint32_t s_changes = 0;
static char s_letter2 = 0;
// 入れ替えた直後は、SENSE に何回か「準備ができていない（60h）」と答える（実機でディスクを抜いて入れたときと同じ）。
// インストーラには「ディスクが抜かれた → 入った」を SENSE で待つものがある（ドラゴンナイト4 など）
static int s_swap_sense[2];          // 2 台目のドライブの DOS のドライブ名（0 = DOS からは見せない）
static uint32_t s_changes2 = 0;
static int s_head_pos[4];          // 各ユニットのヘッド位置（シリンダ）
static bool s_force_wp = false;
void set_force_wprot(bool on) { s_force_wp = on; if (s_img && on) s_img->wprot = true; }
bool force_wprot() { return s_force_wp; }

static std::string s_folder;
const std::string& folder() { return s_folder; }
std::string current_path() { return s_img ? s_img->path : s_folder; }

bool insert(const std::string& path0, std::string* err) {
    std::string path = path0;
    // フォルダ: 末尾の区切りを除いてから確かめる（"C:\" のようなドライブの直下はそのまま）
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
    HostDirEntry de;
    if (!is_device_spec(path) && hostfs::stat(path, de) && de.is_dir) {
        delete s_img; s_img = nullptr;
        s_folder = path;
        s_changes++;
        fatfs::reset();
        return true;
    }
    FloppyImage* im = new FloppyImage();
    if (is_device_spec(path)) {
        // 同じ実機を開き直すこともあるので、先に今のものを閉じる（ポートは同時に 1 つしか開けない）
        if (s_img && s_img->src) { delete s_img; s_img = nullptr; s_changes++; fatfs::reset(); }
        FdSource* src = fdreal_open(path, err);
        if (!src) { delete im; return false; }
        std::string fmt = fdreal_format_name(path);
        if (!im->open_source(src, path, fmt, err)) { delete im; return false; }
    } else if (!im->load(path, err)) { delete im; return false; }
    if (s_force_wp) im->wprot = true;
    delete s_img;
    s_img = im;
    s_folder.clear();
    s_changes++;
    s_swap_sense[0] = 2;
    fatfs::reset();
    return true;
}
void eject() { delete s_img; s_img = nullptr; s_folder.clear(); s_changes++; s_swap_sense[0] = 0; fatfs::reset(); }
void clear_swap_flags() { s_swap_sense[0] = s_swap_sense[1] = 0; }
FloppyImage* image() { return s_img; }

// ユニット 1（2 台目のドライブ）。ブートモード（DOS を使わず IPL から起動するディスク）で使う。
// DOS のドライブには出さない（INT 1Bh からだけ見える）
static FloppyImage* s_img2 = nullptr;
bool insert_unit(int unit, const std::string& path, std::string* err) {
    if (unit == 0) return insert(path, err);
    if (unit != 1) { if (err) *err = "ドライブの番号が違います"; return false; }
    FloppyImage* im = new FloppyImage();
    if (is_device_spec(path)) {
        FdSource* src = fdreal_open(path, err);
        if (!src) { delete im; return false; }
        if (!im->open_source(src, path, fdreal_format_name(path), err)) { delete im; return false; }
    } else if (!im->load(path, err)) { delete im; return false; }
    if (s_force_wp) im->wprot = true;
    delete s_img2; s_img2 = im;
    s_changes2++;
    s_swap_sense[1] = 2;
    fatfs::reset();
    return true;
}
// イメージの中身を調べる: MS-DOS のファイル表（FAT）があるか、IPL（起動用のセクタ）があるか
bool probe_image(const std::string& path, bool* dos, bool* bootable) {
    *dos = false; *bootable = false;
    FloppyImage im;
    std::string e;
    if (!im.load(path, &e)) return false;
    FdTrack* t = im.track(0, 0);
    if (!t || t->secs.empty()) return true;
    const FdSector* b = nullptr;
    for (auto& x : t->secs) if (!x.fm && x.r == 1) { b = &x; break; }
    if (!b) for (auto& x : t->secs) if (x.r == 1) { b = &x; break; }
    if (!b || b->data.empty()) return true;
    bool same = true;
    for (size_t i = 1; i < b->data.size() && same; i++) same = b->data[i] == b->data[0];
    *bootable = !same;
    if (b->fm || b->n == 0) return true;   // 先頭が FM 128 バイト = N88-BASIC などの形式（MS-DOS ではない）
    const uint8_t* d = b->data.data();
    if (b->data.size() >= 32) {
        unsigned bps = d[11] | (d[12] << 8), spc = d[13], nf = d[16];
        if (bps == (128u << (b->n & 7)) && spc >= 1 && spc <= 64 && (spc & (spc - 1)) == 0 && nf >= 1 && nf <= 2) { *dos = true; return true; }
    }
    // BPB の無い古い形式: 2 番目の論理セクタ（FAT の先頭）が F0〜FF FF FF で始まるか
    std::vector<uint8_t> f((size_t)im.lsec_size);
    if (im.lsec_size >= 4 && im.read_lba(1, f.data()) && f[0] >= 0xF0 && f[1] == 0xFF && f[2] == 0xFF) *dos = true;
    return true;
}

void eject_unit(int unit) { if (unit == 0) eject(); else if (unit == 1) { delete s_img2; s_img2 = nullptr; s_changes2++; fatfs::reset(); } }
FloppyImage* image_unit(int unit) { return unit == 0 ? s_img : unit == 1 ? s_img2 : nullptr; }
std::string current_path_unit(int unit) { if (unit == 0) return current_path(); return (unit == 1 && s_img2) ? s_img2->path : std::string(); }
char drive_letter() { return s_letter; }
void set_drive_letter(char c) { if (c >= 'a' && c <= 'z') c = (char)(c - 32); s_letter = (c >= 'A' && c <= 'Z') ? c : 0; }
uint32_t change_count() { return s_changes + s_changes2 * 0x10000u; }   // どちらかのドライブが入れ替わると変わる
uint32_t change_count_unit(int unit) { return unit == 0 ? s_changes : s_changes2; }
char drive_letter_unit(int unit) { return unit == 0 ? s_letter : unit == 1 ? s_letter2 : 0; }
void set_drive_letter_unit(int unit, char c) {
    if (unit == 0) { set_drive_letter(c); return; }
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (unit == 1) s_letter2 = (c >= 'A' && c <= 'Z') ? c : 0;
}
int unit_of_letter(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (!c) return -1;
    if (c == s_letter) return 0;
    if (c == s_letter2) return 1;
    return -1;
}
static uint32_t s_last_access = 0;
void poll_change() {
    if (!s_img || !s_img->src) return;
    uint32_t now = fdreal_now_ms();
    uint32_t idle = now - s_last_access;
    s_last_access = now;
    static int thr = -1;
    if (thr < 0) { const char* e = getenv("PC98PLAYER_FD_IDLE_MS"); thr = e ? atoi(e) : 1500; }   // テスト用に変えられる
    if ((int)idle < thr) return;   // 続けて使っている間は確かめない（入れ替えは、止まっている間に起きる）
    if (s_img->check_media_change()) {
        s_changes++;
        plog("[fd] ディスクが入れ替わったので読み直します%s\n", s_img->not_ready ? "（今は入っていません）" : "");
    }
    s_last_access = fdreal_now_ms();
}
void media_changed() { if (s_img) s_img->refetch(); s_changes++; fatfs::reset(); }
void idle() { if (s_img && s_img->src) s_img->src->idle(fdreal_now_ms()); }

bool is_fd_da(uint8_t al) {
    uint8_t da = al & 0xF0;
    return da == 0x90 || da == 0x30 || da == 0x10 || da == 0x70 || da == 0xB0 || da == 0xF0;
}

// DA と媒体が合うか（2HD の口で 2DD の円盤を読もうとした等は読めない）
static bool media_ok(uint8_t al, const FloppyImage* im) {
    uint8_t da = al & 0xF0;
    bool hd = im->media == FD_2HD || im->media == FD_144;
    if (da == 0x10 || da == 0x70) return !hd;           // 640KB の口
    return hd;                                          // 1MB / 1.44MB の口
}

// 記録密度（AH の bit6 MF: 1=MFM 倍密度, 0=FM 単密度）が合うセクタか。
// FM のセクタは MFM では読めない。MFM のセクタを FM で読むのは、そのトラックに FM のセクタがあるとき
//（＝密度がきちんと記録されているイメージ）だけ断る（密度を記録していない形式で読めなくならないように）
static bool density_ok(const FdSector& s, bool mf, const FdTrack* t) {
    if (s.fm) return !mf;
    if (mf) return true;
    if (t) for (auto& x : t->secs) if (x.fm) return false;
    return true;
}

static void bios_int1b_body(Machine* m);
// INT 1Bh。Trace=1 のときは、その後でゲームが「読んだ内容・結果コード・ID」を何と比べたかを記録する（cpu.cpp）
void bios_int1b(Machine* m) {
    Cpu* c = &m->cpu;
    uint8_t ah0 = (uint8_t)(c->r[EAX] >> 8), cmd = ah0 & 0x0F;
    uint32_t buf = ((uint32_t)c->sr[ES_] << 4) + (uint16_t)c->r[EBP];
    uint32_t len = (uint16_t)c->r[EBX];
    if (!len) len = 128u << ((c->r[ECX] >> 8) & 7);
    bios_int1b_body(m);
    if (!m->cfg.trace) return;
    bool read = cmd == 0x02 || cmd == 0x06 || cmd == 0x0C;
    cpu_dw_begin(buf, read ? len : 0, "");
    char lab[40];
    snprintf(lab, sizeof(lab), "INT1B AH=%02Xh の結果", ah0);
    cpu_dw_taint8(4, (uint8_t)(c->r[EAX] >> 8), lab);   // AH
    if (cmd == 0x0A) {   // READ ID: 読めた ID
        cpu_dw_taint8(1, (uint8_t)c->r[ECX], "READ ID の C");
        cpu_dw_taint8(5, (uint8_t)(c->r[ECX] >> 8), "READ ID の N");
        cpu_dw_taint8(6, (uint8_t)(c->r[EDX] >> 8), "READ ID の H");
        cpu_dw_taint8(2, (uint8_t)c->r[EDX], "READ ID の R");
    }
    if (read) plog("[fdchk] ↓ここから、読んだ %u バイト（[%05X]〜）と結果 AH を比べる命令を記録\n", len, buf & 0xFFFFF);
}

static void bios_int1b_body(Machine* m) {
    Cpu* c = &m->cpu;
    auto AH = [&]() { return (uint8_t)(c->r[EAX] >> 8); };
    uint8_t al = (uint8_t)c->r[EAX];
    auto ret = [&](uint8_t st) {
        if (m->cfg.trace && st != 0x00) plog("[fd]   → 結果 %02Xh\n", st);
        c->r[EAX] = (c->r[EAX] & 0xFFFF00FFu) | ((uint32_t)st << 8);
        set_cf(m, st >= 0x20);   // 00h 正常 / 10h 正常（デリーテッド検出・書き込み禁止の状態）
    };
    uint8_t cmd = AH() & 0x0F;
    int unit = al & 3;
    if (unit == 0) poll_change();
    FloppyImage* im = (unit == 0) ? s_img : (unit == 1) ? s_img2 : nullptr;
    if (m->cfg.trace) plog("[fd] INT1B AH=%02X AL=%02X C=%02X H=%02X R=%02X N=%02X BX=%04X\n",
                           AH(), al, (unsigned)(uint8_t)c->r[ECX], (unsigned)(uint8_t)(c->r[EDX] >> 8),
                           (unsigned)(uint8_t)c->r[EDX], (unsigned)(uint8_t)(c->r[ECX] >> 8), (unsigned)(c->r[EBX] & 0xFFFF));
    if (cmd == 0x03) { s_head_pos[unit] = 0; ret(0x00); return; }             // 初期化
    if (!im) { ret(0x60); return; }                                            // 入っていない
    if (!media_ok(al, im)) { ret(cmd == 0x04 ? 0x60 : 0xE0); return; }
    if (im->src && im->not_ready) {   // 実機: 前回ディスクが無かった。もう一度だけ確かめる
        im->not_ready = false;
        int c0 = s_head_pos[unit] < 0 ? 0 : s_head_pos[unit];
        im->loaded[c0 * 2] = false;
        im->track(c0, 0);
        if (im->not_ready) { ret(0x60); return; }
    }
    uint8_t C = (uint8_t)c->r[ECX], N = (uint8_t)(c->r[ECX] >> 8);
    uint8_t H = (uint8_t)(c->r[EDX] >> 8), R = (uint8_t)c->r[EDX];
    uint16_t bx = (uint16_t)c->r[EBX];
    uint32_t buf = ((uint32_t)c->sr[ES_] << 4) + (uint16_t)c->r[EBP];
    bool mt = (AH() & 0x80) != 0, seek = (AH() & 0x10) != 0, mf = (AH() & 0x40) != 0;
    switch (cmd) {
    case 0x04: { // SENSE: bit4 = 書き込み禁止、bit0 = 2HD（1MB の口）、AH=84h なら bit3 = 1MB/640KB 両用ドライブ
        if (unit < 2 && s_swap_sense[unit] > 0) { s_swap_sense[unit]--; ret(0x60); return; }   // 入れ替えの途中
        uint8_t st = im->wprot ? 0x10 : 0x00;
        if (al & 0x80) st |= 0x01;
        if ((c->r[EAX] & 0x8F40) == 0x8400) st |= 0x08;
        ret(st);
        return; }
    case 0x07:   // RECALIBRATE
        s_head_pos[unit] = 0; ret(0x00); return;
    case 0x0A: { // READ ID
        if (seek) s_head_pos[unit] = C;
        FdTrack* t = im->track(s_head_pos[unit], H & 1);
        if (!t || t->secs.empty()) { ret(0xE0); return; }
        FdSector* sp = nullptr;
        for (size_t k = 0; k < t->secs.size(); k++) {   // 回転位置から、密度の合う ID を探す
            FdSector& x = t->secs[t->rot % t->secs.size()];
            t->rot = (t->rot + 1) % (int)t->secs.size();
            if (density_ok(x, mf, t)) { sp = &x; break; }
        }
        if (!sp) { ret(0xE0); return; }
        FdSector& s = *sp;
        c->r[ECX] = (c->r[ECX] & 0xFFFF0000u) | ((uint32_t)s.n << 8) | s.c;
        c->r[EDX] = (c->r[EDX] & 0xFFFF0000u) | ((uint32_t)s.h << 8) | s.r;
        ret(0x00); return; }
    case 0x01: case 0x02: case 0x06: case 0x0C: case 0x05: case 0x09: {   // VERIFY / READ DIAG / READ / READ DELETED / WRITE / WRITE DELETED
        if (seek) s_head_pos[unit] = C;
        if (cmd == 0x02) {
            // READ DIAGNOSTIC の結果が記録されているイメージ（NFD r1）は、それをそのまま返す
            FdTrack* t = im->track(s_head_pos[unit], H & 1);
            FdDiag* g = nullptr;
            if (t) {
                for (auto& x : t->diags) if (x.n == N && x.r == R && x.c == C && x.h == H) { g = &x; break; }
                if (!g) for (auto& x : t->diags) if (x.n == N) { g = &x; break; }
            }
            if (g) {
                uint32_t len = bx ? bx : (128u << (N & 7));
                size_t one = g->copies > 1 ? g->data.size() / g->copies : g->data.size();
                const uint8_t* src = g->data.data() + (g->copies > 1 ? one * (g->next_copy % g->copies) : 0);
                if (g->copies > 1) g->next_copy = (g->next_copy + 1) % g->copies;
                for (uint32_t i = 0; i < len; i++) mem_wb(m, buf + i, i < one ? src[i] : 0);
                ret(g->status); return;
            }
        }
        bool write = cmd == 0x05 || cmd == 0x09;
        if (write && im->wprot) { ret(0x70); return; }
        uint32_t left = bx ? bx : (128u << (N & 7));
        uint8_t curH = H, curR = R;
        int phead = H & 1;
        uint8_t st = 0x00;
        while (left > 0) {
            FdSector* s = nullptr;
            FdTrack* t = im->track(s_head_pos[unit], phead);
            if (t) {
                if (cmd == 0x02) {   // READ DIAGNOSTIC: ID を見ずにトラックの先頭から順に
                    if (!t->secs.empty()) s = &t->secs[(curR - R) % t->secs.size()];
                } else {
                    for (auto& x : t->secs) if (x.c == C && x.h == curH && x.r == curR && x.n == N && density_ok(x, mf, t)) { s = &x; break; }
                }
            }
            if (!s) {
                // 同じ R・N で C/H が違うセクタがあれば「シリンダ違い」
                bool other = false;
                if (t) for (auto& x : t->secs) if (x.r == curR) { other = true; break; }
                st = t && !t->secs.empty() ? (other ? 0xD0 : 0xC0) : 0xE0;
                // プロテクトの確認で読まれる特殊なセクタがイメージに記録されていないことが多いので、
                // 見つからなかった ID をログに残す（イメージを作り直すときの手がかり）
                static int s_missing_log = 0;
                if (s_missing_log < 16) {
                    s_missing_log++;
                    plog("[fd] セクタが見つかりません: C=%02X H=%02X R=%02X N=%02X（シリンダ %d 面 %d。このトラックの ID:",
                         C, curH, curR, N, s_head_pos[unit], phead);
                    if (t) for (auto& x : t->secs) plog(" %02X", x.r);
                    plog("）→ 結果 %02Xh\n", st);
                }
                break;
            }
            int ss = 128 << (s->n & 7);
            int n = (int)std::min<uint32_t>(left, (uint32_t)ss);
            if (write) {
                std::vector<uint8_t> tmp((size_t)n);
                for (int i = 0; i < n; i++) tmp[i] = mem_rb(m, buf + i);
                if (!im->write_sector(s, tmp.data(), n)) { st = 0x70; break; }
                s->deleted = cmd == 0x09;
            } else if (cmd != 0x01) {
                const uint8_t* src = s->data.data();
                size_t avail = s->data.size();
                if (s->copies > 1 && avail >= (size_t)ss * s->copies) {   // 再試行データ（読むたびに変わるセクタ）
                    src += (size_t)ss * (s->next_copy % s->copies);
                    s->next_copy = (s->next_copy + 1) % s->copies;
                    avail = (size_t)ss;
                }
                for (int i = 0; i < n; i++) mem_wb(m, buf + i, i < (int)avail ? src[i] : 0);
            }
            // 結果: イメージに記録された状態。デリーテッドは読み方との組み合わせで決まる
            uint8_t sst = s->status;
            if (!write) {
                if (cmd == 0x0C) sst = s->deleted ? (sst == 0x10 ? 0x00 : sst) : (sst ? sst : 0x10);
                else if (s->deleted && sst == 0x00) sst = 0x10;
            }
            buf += (uint32_t)n; left -= (uint32_t)n;
            if (sst != 0x00) { st = sst; break; }
            curR++;
            if (left > 0) {
                // トラックの終わり（次の R が無い）なら、MT 指定で裏面へ
                FdTrack* tt = im->track(s_head_pos[unit], phead);
                bool has = false;
                if (tt) for (auto& x : tt->secs) if (x.r == curR) { has = true; break; }
                if (!has && mt && phead == 0) { phead = 1; curH = (uint8_t)(curH | 1); curR = 1; }
            }
        }
        ret(st); return; }
    case 0x0D: { // FORMAT: 形は変えられないので、同じ形のトラックならデータだけ埋める
        if (seek) s_head_pos[unit] = C;
        if (im->wprot) { ret(0x70); return; }
        FdTrack* t = im->track(s_head_pos[unit], H & 1);
        if (!t) { ret(0xE0); return; }
        uint8_t fill = (uint8_t)c->r[EDX];
        for (auto& s : t->secs) {
            std::vector<uint8_t> tmp((size_t)(128 << (s.n & 7)), fill);
            im->write_sector(&s, tmp.data(), (int)tmp.size());
            s.deleted = false; s.status = 0;
        }
        ret(0x00); return; }
    case 0x0E:   // 動作モードの設定
        ret(0x00); return;
    default:
        ret(0x40); return;
    }
}
} // namespace floppy
