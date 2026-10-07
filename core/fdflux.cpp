// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  fdflux.cpp  --  フラックス（磁気信号）からのセクタの復調と、SCP / HFE の読み込み
//
//  フロッピーの磁気の反転の間隔（フラックス）を、PLL でセル（クロック込みのビット）列に
//  直し、MFM（倍密度）と FM（単密度）の同期マーク（A1h のクロック抜け 4489h 等）を探して
//  ID 部・データ部を取り出す。FDC と同じように「ID の後に来るデータ部」を組にする。
//  普通の吸い出し（セクタ単位）では消えてしまう次のようなものもそのまま残る:
//   ・ID の C/H/R/N がずれたセクタ、同じ ID が 2 つあるトラック、トラックの途中に隠れた ID
//   ・ID/データの CRC エラー、デリーテッドデータ、データ部の無いセクタ
//   ・読むたびに内容が変わるセクタ（弱い磁化）: 各回転のデータを「再試行データ」として持つ
//
//  SCP（SuperCard Pro 形式。Greaseweazle の標準）と HFE（HxC。v1 と v3）を読む。
//  いずれも最初に全トラックを復調する。書き込みはできない（書き込み禁止として扱う）。
// -----------------------------------------------------------------------------
#include "floppy.h"
#include "machine.h"
#include <string.h>
#include <math.h>
#include <algorithm>

namespace {

inline uint16_t rd16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32le(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

// CRC-CCITT（多項式 1021h, 初期値 FFFFh）。FDC の ID/データ部の CRC
uint16_t crc16(uint16_t crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// 1 回転の中で見つかったセクタ（ID とデータ部）
struct RawSec {
    double  angle = 0;          // 回転の中の位置（0..1）
    uint8_t c = 0, h = 0, r = 0, n = 0;
    bool    fm = false;
    bool    id_ok = false;      // ID の CRC が合った
    bool    has_data = false;   // データ部が見つかった
    bool    deleted = false;
    bool    data_ok = false;    // データの CRC が合った
    std::vector<uint8_t> data;
};

// セル列（1 セル 1 バイト, 0/1）から 1 バイト（データビットは 2 セルずつの後ろ側）
inline uint8_t mfm_byte(const uint8_t* cells, size_t p) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v = (uint8_t)((v << 1) | cells[p + i * 2 + 1]);
    return v;
}

// MFM: 同期マーク 4489h（クロックの抜けた A1h）を探す。
// [from, id_limit) で始まる ID を採る（データ部は end まで読んでよい）。
void scan_mfm(const std::vector<uint8_t>& cells, size_t from, size_t id_limit, size_t end, double rev_len,
              std::vector<RawSec>& out) {
    if (end > cells.size()) end = cells.size();
    uint16_t reg = 0;
    int last_id = -1;           // 直前の ID（データ部を組にする）
    size_t last_id_end = 0;
    for (size_t i = from; i < end; i++) {
        reg = (uint16_t)((reg << 1) | cells[i]);
        if (reg != 0x4489 || i < from + 15) continue;
        size_t sync_start = i - 15;
        size_t p = i + 1;
        while (p + 16 <= end) {   // 続く 4489h は読み飛ばす
            uint16_t w = 0;
            for (int k = 0; k < 16; k++) w = (uint16_t)((w << 1) | cells[p + k]);
            if (w != 0x4489) break;
            p += 16;
        }
        if (p + 16 > end) break;
        uint8_t mark = mfm_byte(cells.data(), p);
        p += 16;
        static const uint8_t a1x3[3] = {0xA1, 0xA1, 0xA1};
        if (mark == 0xFE) {
            if (sync_start >= id_limit) { i = p - 1; reg = 0; continue; }
            if (p + 6 * 16 > end) break;
            uint8_t b[7]; b[0] = mark;
            for (int k = 0; k < 6; k++) b[1 + k] = mfm_byte(cells.data(), p + k * 16);
            uint16_t crc = crc16(crc16(0xFFFF, a1x3, 3), b, 5);
            RawSec s;
            s.angle = (double)(sync_start - from) / rev_len;
            s.c = b[1]; s.h = b[2]; s.r = b[3]; s.n = b[4];
            s.id_ok = crc == (uint16_t)((b[5] << 8) | b[6]);
            out.push_back(s);
            last_id = (int)out.size() - 1;
            last_id_end = p + 6 * 16;
            i = last_id_end - 1; reg = 0;
        } else if (mark >= 0xF8 && mark <= 0xFB) {
            // データ部: 直前の ID から 60 バイト以内なら組にする（FDC は ID の後のギャップの中で待つ）
            if (last_id >= 0 && !out[last_id].has_data && sync_start >= last_id_end && sync_start - last_id_end < 60 * 16) {
                RawSec& s = out[last_id];
                s.has_data = true;
                s.deleted = mark == 0xF8 || mark == 0xF9;
                size_t len = (size_t)128 << std::min<int>(s.n, 7);
                size_t avail = (end - p) / 16;
                size_t take = std::min(len + 2, avail);
                std::vector<uint8_t> d(take);
                for (size_t k = 0; k < take; k++) d[k] = mfm_byte(cells.data(), p + k * 16);
                if (take == len + 2) {
                    uint16_t crc = crc16(crc16(crc16(0xFFFF, a1x3, 3), &mark, 1), d.data(), len);
                    s.data_ok = crc == (uint16_t)((d[len] << 8) | d[len + 1]);
                }
                d.resize(std::min(take, len));
                s.data.swap(d);
            }
            last_id = -1;
            // データ部の中に隠れた ID もあり得るので、データの中も続けて探す
            i = p - 1; reg = 0;
        }
    }
}

// FM（単密度）: 1 FM セル = MFM の 2 セル。クロック C7h + データ FEh/FBh/F8h のマークを探す
uint16_t fm_pattern(uint8_t clk, uint8_t dat) {
    uint16_t v = 0;
    for (int i = 7; i >= 0; i--) v = (uint16_t)((v << 2) | (((clk >> i) & 1) << 1) | ((dat >> i) & 1));
    return v;
}
void scan_fm(const std::vector<uint8_t>& cells, size_t from, size_t id_limit, size_t end, double rev_len,
             std::vector<RawSec>& out) {
    if (end > cells.size()) end = cells.size();
    static const uint16_t P_ID = fm_pattern(0xC7, 0xFE), P_DAM = fm_pattern(0xC7, 0xFB), P_DDAM = fm_pattern(0xC7, 0xF8);
    for (int ph = 0; ph < 2; ph++) {
        // FM のセル列（MFM の 2 セルのどちらかに反転があれば 1）
        std::vector<uint8_t> f;
        f.reserve((end - from) / 2 + 1);
        for (size_t i = from + ph; i + 1 < end; i += 2) f.push_back((uint8_t)(cells[i] | cells[i + 1]));
        auto fbyte = [&](size_t p) { uint8_t v = 0; for (int k = 0; k < 8; k++) v = (uint8_t)((v << 1) | f[p + k * 2 + 1]); return v; };
        uint16_t reg = 0;
        int last_id = -1; size_t last_end = 0;
        size_t lim = (id_limit - from) / 2;
        std::vector<RawSec> found;
        for (size_t i = 0; i < f.size(); i++) {
            reg = (uint16_t)((reg << 1) | f[i]);
            if (i < 15) continue;
            size_t st = i - 15, p = i + 1;
            if (reg == P_ID) {
                if (st >= lim) continue;
                if (p + 6 * 16 > f.size()) break;
                uint8_t b[7]; b[0] = 0xFE;
                for (int k = 0; k < 6; k++) b[1 + k] = fbyte(p + k * 16);
                RawSec s;
                s.angle = (double)(st * 2) / rev_len;
                s.fm = true;
                s.c = b[1]; s.h = b[2]; s.r = b[3]; s.n = b[4];
                s.id_ok = crc16(0xFFFF, b, 5) == (uint16_t)((b[5] << 8) | b[6]);
                found.push_back(s);
                last_id = (int)found.size() - 1; last_end = p + 6 * 16;
                i = last_end - 1; reg = 0;
            } else if (reg == P_DAM || reg == P_DDAM) {
                if (last_id >= 0 && !found[last_id].has_data && st >= last_end && st - last_end < 40 * 16) {
                    RawSec& s = found[last_id];
                    uint8_t mark = reg == P_DAM ? 0xFB : 0xF8;
                    s.has_data = true; s.deleted = mark == 0xF8;
                    size_t len = (size_t)128 << std::min<int>(s.n, 7);
                    size_t avail = (f.size() - p) / 16, take = std::min(len + 2, avail);
                    std::vector<uint8_t> d(take);
                    for (size_t k = 0; k < take; k++) d[k] = fbyte(p + k * 16);
                    if (take == len + 2)
                        s.data_ok = crc16(crc16(0xFFFF, &mark, 1), d.data(), len) == (uint16_t)((d[len] << 8) | d[len + 1]);
                    d.resize(std::min(take, len));
                    s.data.swap(d);
                }
                last_id = -1;
                i = p - 1; reg = 0;
            }
        }
        if (!found.empty()) { out.insert(out.end(), found.begin(), found.end()); return; }
    }
}

// 反転の間隔の分布から 1 セルの長さを推定する（MFM の 2T（最短の山）/ 2）
double estimate_cell(const std::vector<float>& flux, size_t a, size_t b) {
    const int BIN = 25, NB = 800;   // 25ns 刻み、20us まで
    std::vector<uint32_t> h(NB, 0);
    uint32_t mx = 0;
    for (size_t i = a; i < b; i++) {
        int k = (int)(flux[i] / BIN);
        if (k > 0 && k < NB) { h[k]++; }
    }
    // なめらかにしてから、最大の 10% を超える最初の山
    std::vector<uint32_t> s(NB, 0);
    for (int k = 2; k < NB - 2; k++) { s[k] = h[k - 2] + h[k - 1] + h[k] + h[k + 1] + h[k + 2]; mx = std::max(mx, s[k]); }
    if (!mx) return 0;
    for (int k = 3; k < NB - 3; k++) {
        if (s[k] * 10 < mx) continue;
        if (s[k] >= s[k - 1] && s[k] >= s[k + 1]) {
            // 山の周り ±15% の平均
            double center = (k + 0.5) * BIN, sum = 0; uint64_t n = 0;
            for (size_t i = a; i < b; i++) if (flux[i] > center * 0.85 && flux[i] < center * 1.15) { sum += flux[i]; n++; }
            return n ? sum / n / 2.0 : center / 2.0;
        }
    }
    return 0;
}

// フラックス → セル列（PLL）。flux の添字 → セルの添字 も返す
void flux_to_cells(const std::vector<float>& flux, double cell0, std::vector<uint8_t>& cells, std::vector<size_t>& pos_of_flux) {
    cells.clear(); cells.reserve((size_t)(flux.size() * 3));
    pos_of_flux.resize(flux.size() + 1);
    double cell = cell0, carry = 0;
    for (size_t i = 0; i < flux.size(); i++) {
        pos_of_flux[i] = cells.size();
        double t = flux[i] + carry;
        int n = (int)floor(t / cell + 0.5);
        if (n < 1) n = 1;
        if (n > 64) { n = (int)(t / cell); carry = 0; for (int k = 0; k < n - 1; k++) cells.push_back(0); cells.push_back(1); continue; }
        for (int k = 0; k < n - 1; k++) cells.push_back(0);
        cells.push_back(1);
        double err = t - n * cell;
        carry = err * 0.3;                                   // 位相のずれを少し持ち越す
        if (n <= 4) {
            cell += err / n * 0.03;                          // 回転むらに少しずつ合わせる
            cell = std::max(cell0 * 0.9, std::min(cell0 * 1.1, cell));
        }
    }
    pos_of_flux[flux.size()] = cells.size();
}

// 各回転で見つかったセクタを、ID と回転の中の位置でまとめる
void merge_revs(const std::vector<std::vector<RawSec>>& revs, int cyl, int head, FdTrack& out) {
    struct Slot { RawSec base; std::vector<const RawSec*> v; };
    std::vector<Slot> slots;
    for (const auto& rv : revs) for (const auto& s : rv) {
        Slot* hit = nullptr;
        for (auto& sl : slots) {
            double d = fabs(sl.base.angle - s.angle);
            d = std::min(d, 1.0 - d);
            if (sl.base.c == s.c && sl.base.h == s.h && sl.base.r == s.r && sl.base.n == s.n && sl.base.fm == s.fm && d < 0.03) { hit = &sl; break; }
        }
        if (!hit) { slots.push_back(Slot{s, {}}); hit = &slots.back(); }
        hit->v.push_back(&s);
    }
    std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) { return a.base.angle < b.base.angle; });
    out.secs.clear(); out.diags.clear(); out.rot = 0;
    for (auto& sl : slots) {
        FdSector o;
        o.c = sl.base.c; o.h = sl.base.h; o.r = sl.base.r; o.n = sl.base.n;
        o.fm = sl.base.fm; o.deleted = false; o.file_off = -1; o.copies = 1; o.next_copy = 0;
        const RawSec* good = nullptr; const RawSec* idok = nullptr;
        std::vector<const RawSec*> bad;
        for (auto* s : sl.v) {
            if (!s->id_ok) continue;
            if (!idok) idok = s;
            if (s->has_data && s->data_ok) { if (!good) good = s; }
            else if (s->has_data) bad.push_back(s);
        }
        size_t ss = (size_t)128 << std::min<int>(o.n, 7);
        if (good) {
            o.status = 0x00; o.deleted = good->deleted; o.data = good->data;
        } else if (!bad.empty()) {
            // どの回転でも CRC エラー: 回ごとに内容が違えば「読むたびに変わるデータ」として持つ
            o.status = 0xB0; o.deleted = bad[0]->deleted;
            std::vector<std::vector<uint8_t>> uniq;
            for (auto* s : bad) {
                std::vector<uint8_t> d = s->data; d.resize(ss, 0);
                if (std::find(uniq.begin(), uniq.end(), d) == uniq.end()) uniq.push_back(d);
            }
            if (uniq.size() > 1) {
                o.copies = (int)uniq.size();
                for (auto& d : uniq) o.data.insert(o.data.end(), d.begin(), d.end());
            } else o.data = bad[0]->data;
        } else if (idok) {
            o.status = 0xF0;   // データ部のアドレスマークが無い
        } else {
            o.status = 0xA0;   // ID の CRC エラー
        }
        out.secs.push_back(o);
    }
    (void)cyl; (void)head;
}

bool any_id_ok(const std::vector<RawSec>& v) {
    for (const auto& x : v) if (x.id_ok) return true;
    return false;
}

int guess_media(double cell_ns, const FdTrack& t) {
    if (cell_ns <= 0) return -1;
    if (cell_ns < 1500) {
        int n2 = 0;
        for (const auto& s : t.secs) if (s.n == 2) n2++;
        return n2 >= 17 ? FD_144 : FD_2HD;
    }
    return cell_ns < 3000 ? FD_2DD : FD_2D;
}

} // namespace

namespace fdflux {

bool decode_flux(const std::vector<float>& flux, const std::vector<size_t>& index, int cyl, int head, FdTrack& out, int* media_out) {
    out.secs.clear(); out.diags.clear();
    if (media_out) *media_out = -1;
    if (flux.size() < 100) return false;
    size_t a = index.empty() ? 0 : index[0];
    double cell = estimate_cell(flux, a, flux.size());
    if (cell <= 0) return false;
    std::vector<uint8_t> cells; std::vector<size_t> pos;
    flux_to_cells(flux, cell, cells, pos);
    // 回転の区切り（セルの位置）
    std::vector<size_t> ix;
    for (size_t i : index) if (i <= flux.size()) ix.push_back(pos[i]);
    if (ix.size() < 2) { ix.clear(); ix.push_back(0); ix.push_back(cells.size()); }
    std::vector<std::vector<RawSec>> revs;
    for (size_t k = 0; k + 1 < ix.size(); k++) {
        size_t from = ix[k], to = ix[k + 1];
        if (to <= from + 1000) continue;
        double len = (double)(to - from);
        size_t end = std::min(cells.size(), to + (size_t)(len * 0.12));   // 索引をまたぐセクタのために少し先まで
        std::vector<RawSec> rv;
        scan_mfm(cells, from, to, end, len, rv);
        if (!any_id_ok(rv)) scan_fm(cells, from, to, end, len, rv);   // MFM が無いトラックだけ FM として探す
        revs.push_back(std::move(rv));
    }
    merge_revs(revs, cyl, head, out);
    if (media_out) *media_out = guess_media(cell, out);
    return !out.secs.empty();
}

bool decode_cells(const std::vector<uint8_t>& cells_in, double cell_ns, int cyl, int head, FdTrack& out, int* media_out) {
    out.secs.clear(); out.diags.clear();
    if (media_out) *media_out = -1;
    if (cells_in.size() < 1000) return false;
    // 1 回転ぶん。索引をまたぐセクタのため先頭を少し後ろへつなぐ
    std::vector<uint8_t> cells = cells_in;
    size_t len = cells_in.size();
    cells.insert(cells.end(), cells_in.begin(), cells_in.begin() + std::min(len, len / 8));
    std::vector<std::vector<RawSec>> revs(1);
    scan_mfm(cells, 0, len, cells.size(), (double)len, revs[0]);
    if (!any_id_ok(revs[0])) scan_fm(cells, 0, len, cells.size(), (double)len, revs[0]);
    merge_revs(revs, cyl, head, out);
    if (media_out) *media_out = guess_media(cell_ns, out);
    return !out.secs.empty();
}

} // namespace fdflux

// ---- SCP -----------------------------------------------------------------------------
//  "SCP" 版 種類 回転数 開始トラック 終了トラック フラグ セル幅(0=16bit) 面 分解能 チェックサム
//  +10h: トラックの位置 x168。トラック: "TRK" 番号, 回転ごとに {索引までの時間, 反転数, データの位置}
//  データは 16bit ビッグエンディアンの間隔（25ns x (分解能+1) 単位。0 は 65536 の繰り上がり）
bool FloppyImage::load_scp(const std::vector<uint8_t>& d, std::string* err) {
    if (d.size() < 0x10 + 168 * 4 || memcmp(d.data(), "SCP", 3) != 0) return false;
    int revs = d[5], bitw = d[9] ? d[9] : 16, sides = d[10];
    double tick = 25.0 * (d[11] + 1);
    if (revs < 1 || (bitw != 16 && bitw != 8)) { if (err) *err = "SCP の形式に対応していません"; return false; }
    int media_votes[4] = {0, 0, 0, 0};
    int found = 0;
    for (int t = 0; t < 168; t++) {
        uint32_t o = rd32le(&d[0x10 + t * 4]);
        if (!o || o + 4 + revs * 12 > d.size() || memcmp(&d[o], "TRK", 3) != 0) continue;
        int tn = d[o + 3];
        // トラック番号 = C*2+H（片面だけのイメージでも同じ数え方）
        int cyl = tn / 2, head = tn & 1;
        (void)sides;
        if (cyl * 2 + head >= MAX_TRACKS) continue;
        std::vector<float> flux; std::vector<size_t> index;
        for (int r = 0; r < revs; r++) {
            const uint8_t* e = &d[o + 4 + r * 12];
            uint32_t cnt = rd32le(e + 4), off = rd32le(e + 8);
            index.push_back(flux.size());
            uint64_t acc = 0;
            for (uint32_t k = 0; k < cnt; k++) {
                uint64_t p = (uint64_t)o + off + (uint64_t)k * (bitw / 8);
                if (p + bitw / 8 > d.size()) break;
                uint32_t v = bitw == 16 ? (uint32_t)((d[p] << 8) | d[p + 1]) : d[p];
                if (v == 0) { acc += bitw == 16 ? 65536 : 256; continue; }
                flux.push_back((float)((acc + v) * tick));
                acc = 0;
            }
        }
        index.push_back(flux.size());
        FdTrack tr; int md = -1;
        if (fdflux::decode_flux(flux, index, cyl, head, tr, &md)) {
            trk[cyl * 2 + head] = std::move(tr);
            if (md >= 0 && md < 4) media_votes[md]++;
            if (cyl + 1 > cylinders) cylinders = cyl + 1;
            found++;
        }
    }
    if (!found) { if (err) *err = "SCP からセクタを読み取れません（MFM/FM ではないディスク？）"; return false; }
    int best = FD_2HD;
    for (int k = 0; k < 4; k++) if (media_votes[k] > media_votes[best]) best = k;
    media = best;
    wprot = true;
    format = "SCP";
    return true;
}

// ---- HFE -----------------------------------------------------------------------------
//  v1 "HXCPICFE": +9 トラック数, +10 面数, +11 符号化, +12 ビットレート(kbps), +18 トラック表の位置(512 単位)
//  トラック表: {位置(512 単位), 長さ(バイト, 両面の合計)}。データは 512 バイトごとに 256 バイトずつ面 0/面 1。
//  ビットは各バイトの下位から。v3 "HXCHFEV3" は途中に命令（ビット反転した F0h-F4h）が入る。
bool FloppyImage::load_hfe(const std::vector<uint8_t>& d, std::string* err) {
    bool v3 = d.size() >= 8 && !memcmp(d.data(), "HXCHFEV3", 8);
    if (d.size() < 0x200 || (!v3 && memcmp(d.data(), "HXCPICFE", 8) != 0)) return false;
    int ntr = d[9], nsd = d[10];
    int rate = rd16le(&d[12]);
    uint32_t lut = (uint32_t)rd16le(&d[18]) * 512;
    if (!ntr || !nsd || nsd > 2 || lut + ntr * 4 > d.size()) { if (err) *err = "HFE の形式が正しくありません"; return false; }
    double cell_ns = rate > 0 ? 1e6 / (rate * 2.0) : 1000.0;
    int media_votes[4] = {0, 0, 0, 0};
    int found = 0;
    for (int t = 0; t < ntr && t < 84; t++) {
        uint32_t off = (uint32_t)rd16le(&d[lut + t * 4]) * 512, len = rd16le(&d[lut + t * 4 + 2]);
        for (int side = 0; side < nsd; side++) {
            std::vector<uint8_t> cells;
            cells.reserve(len * 4);
            int skip = 0;
            for (uint32_t k = 0; k < len / 2; k++) {
                uint32_t blk = k / 256, in = k % 256;
                uint64_t p = (uint64_t)off + blk * 512 + side * 256 + in;
                if (p >= d.size()) break;
                uint8_t b = d[p];
                if (v3 && (b & 0x0F) == 0x0F) {   // 命令（F0h-F4h をビット反転したもの）
                    uint8_t op = b;
                    if (op == 0x0F || op == 0x8F) continue;                       // NOP / 索引の位置
                    if (op == 0x4F) { k++; continue; }                            // ビットレートの変更（1 バイト）
                    if (op == 0xCF) { k++; if (k < len / 2) { uint32_t b2 = k / 256, i2 = k % 256; skip = d[(uint64_t)off + b2 * 512 + side * 256 + i2] & 7; } continue; }
                    if (op == 0x2F) { for (int i = 0; i < 8; i++) cells.push_back((uint8_t)(rand() & 1)); continue; }   // 弱いビット
                }
                for (int i = skip; i < 8; i++) cells.push_back((uint8_t)((b >> i) & 1));
                skip = 0;
            }
            FdTrack tr; int md = -1;
            if (fdflux::decode_cells(cells, cell_ns, t, side, tr, &md)) {
                trk[t * 2 + side] = std::move(tr);
                if (md >= 0 && md < 4) media_votes[md]++;
                if (t + 1 > cylinders) cylinders = t + 1;
                found++;
            }
        }
    }
    if (!found) { if (err) *err = "HFE からセクタを読み取れません"; return false; }
    int best = FD_2HD;
    for (int k = 0; k < 4; k++) if (media_votes[k] > media_votes[best]) best = k;
    media = best;
    wprot = true;
    format = v3 ? "HFE(v3)" : "HFE";
    return true;
}
