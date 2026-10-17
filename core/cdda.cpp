// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  cdda.cpp  --  CD-ROM のトラック表と CD-DA の演奏（設計は cdda.h）
//  MP3・FLAC の展開には dr_mp3 / dr_flac（David Reid。パブリックドメイン / MIT-0）を使う。
// -----------------------------------------------------------------------------
#include "cdda.h"
#include "hostfs.h"
#include "state.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <algorithm>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../third_party/dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "../third_party/dr_libs/dr_flac.h"

void plog(const char* fmt, ...);

namespace cdda {

static std::vector<Track> s_tracks;       // 番号の順（データトラックを含む）
// 演奏の状態（位置は 44.1kHz の標本の通し番号 = HSG * 588）
static bool   s_busy = false, s_paused = false;
static double s_pos = 0, s_end = 0;
static uint32_t s_last_start = 0, s_last_end = 0;
static uint8_t s_vol0 = 0xFF, s_vol1 = 0xFF;

// ---- 音のファイルを読む ------------------------------------------------------
static size_t rd_cb(void* u, void* b, size_t n) { int r = hostfs::read(u, b, (int)n); return r > 0 ? (size_t)r : 0; }
static drmp3_bool32 mp3_seek_cb(void* u, int off, drmp3_seek_origin o) {
    int w = o == DRMP3_SEEK_SET ? 0 : o == DRMP3_SEEK_CUR ? 1 : 2;
    return hostfs::seek(u, off, w) >= 0;
}
static drmp3_bool32 mp3_tell_cb(void* u, drmp3_int64* c) { int64_t p = hostfs::seek(u, 0, 1); if (p < 0) return 0; *c = p; return 1; }
static drflac_bool32 flac_seek_cb(void* u, int off, drflac_seek_origin o) {
    int w = o == DRFLAC_SEEK_SET ? 0 : o == DRFLAC_SEEK_CUR ? 1 : 2;
    return hostfs::seek(u, off, w) >= 0;
}
static drflac_bool32 flac_tell_cb(void* u, drflac_int64* c) { int64_t p = hostfs::seek(u, 0, 1); if (p < 0) return 0; *c = p; return 1; }

struct Wav { int64_t data = 0, bytes = 0; int ch = 0, bits = 0, rate = 0, fmt = 0; };
static bool wav_header(void* h, Wav* w) {
    uint8_t b[12];
    hostfs::seek(h, 0, 0);
    if (hostfs::read(h, b, 12) != 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return false;
    int64_t o = 12;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t c[8];
        hostfs::seek(h, o, 0);
        if (hostfs::read(h, c, 8) != 8) break;
        uint32_t len = (uint32_t)(c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t)c[7] << 24));
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t f[16];
            if (hostfs::read(h, f, 16) != 16) return false;
            w->fmt = f[0] | (f[1] << 8); w->ch = f[2] | (f[3] << 8);
            w->rate = (int)(f[4] | (f[5] << 8) | (f[6] << 16) | ((uint32_t)f[7] << 24)); w->bits = f[14] | (f[15] << 8);
            if (w->fmt == 0xFFFE) w->fmt = 1;   // WAVE_FORMAT_EXTENSIBLE（中身は PCM とみなす）
        } else if (!memcmp(c, "data", 4)) { w->data = o + 8; w->bytes = len; break; }
        o += 8 + (int64_t)len + (len & 1);
    }
    return w->fmt == 1 && w->data && w->ch >= 1 && w->ch <= 8 && w->rate > 0 && (w->bits == 8 || w->bits == 16 || w->bits == 24);
}

// 今読んでいるトラックの読み手（1 つだけ持つ）
struct Reader {
    int no = -1, src = SRC_NONE;
    void* fh = nullptr;
    drmp3* mp3 = nullptr;
    drflac* flac = nullptr;
    Wav wav;
    int rate = 44100, ch = 2;
    int64_t base = 0;                 // BIN: トラックの先頭のバイト位置 / ほか: 標本の位置（CUE で WAV などの途中から始まるとき）
    std::vector<int16_t> buf;         // 展開した標本（ステレオ）
    int64_t buf_start = 0, next = 0;  // buf の先頭の標本番号 / 次に展開する標本番号（ファイルの位置と一致）
    void close() {
        if (mp3) { drmp3_uninit(mp3); delete mp3; mp3 = nullptr; }
        if (flac) { drflac_close(flac); flac = nullptr; }
        if (fh) { hostfs::close(fh); fh = nullptr; }
        no = -1; src = SRC_NONE; buf.clear(); buf_start = next = 0;
    }
    bool open(const Track& t) {
        close();
        no = t.no; src = t.src; base = t.file_off;
        fh = hostfs::open(t.path, 0, false, false);
        if (!fh) { src = SRC_NONE; return false; }
        if (src == SRC_BIN) { rate = 44100; ch = 2; }
        else if (src == SRC_WAV) { if (!wav_header(fh, &wav)) { close(); return false; } rate = wav.rate; ch = wav.ch; }
        else if (src == SRC_MP3) {
            mp3 = new drmp3();
            if (!drmp3_init(mp3, rd_cb, mp3_seek_cb, mp3_tell_cb, nullptr, fh, nullptr)) { delete mp3; mp3 = nullptr; close(); return false; }
            rate = (int)mp3->sampleRate; ch = (int)mp3->channels;
        } else if (src == SRC_FLAC) {
            flac = drflac_open(rd_cb, flac_seek_cb, flac_tell_cb, fh, nullptr);
            if (!flac) { close(); return false; }
            rate = (int)flac->sampleRate; ch = (int)flac->channels;
        }
        no = t.no;
        next = -1;   // 最初の展開では必ず位置を合わせる
        return true;
    }
    // idx 番目（トラックの先頭から）の標本から n 個を展開して buf に入れる
    void fill(int64_t idx) {
        const int N = 4096;
        if (idx != next) {   // 飛んだ: 読む位置を合わせる
            int64_t at = idx + (src == SRC_BIN ? 0 : base);
            if (src == SRC_BIN) hostfs::seek(fh, base + at * 4, 0);
            else if (src == SRC_WAV) hostfs::seek(fh, wav.data + at * wav.ch * (wav.bits / 8), 0);
            else if (src == SRC_MP3) drmp3_seek_to_pcm_frame(mp3, (drmp3_uint64)at);
            else if (src == SRC_FLAC) drflac_seek_to_pcm_frame(flac, (drflac_uint64)at);
        }
        buf.assign((size_t)N * 2, 0);
        int got = 0;
        if (src == SRC_BIN) {
            int r = hostfs::read(fh, buf.data(), N * 4);
            got = r > 0 ? r / 4 : 0;
        } else if (src == SRC_WAV) {
            int bps = wav.bits / 8, fb = bps * wav.ch;
            int64_t left = wav.bytes - ((idx + base) * fb);
            int want = (int)std::max<int64_t>(0, std::min<int64_t>(N, left / fb));
            std::vector<uint8_t> raw((size_t)want * fb);
            int r = want ? hostfs::read(fh, raw.data(), want * fb) : 0;
            got = r > 0 ? r / fb : 0;
            for (int i = 0; i < got; i++) {
                int v[2] = {0, 0};
                for (int c = 0; c < wav.ch && c < 8; c++) {
                    const uint8_t* p = &raw[(size_t)i * fb + c * bps];
                    int s = bps == 1 ? ((int)p[0] - 128) << 8 : bps == 2 ? (int16_t)(p[0] | (p[1] << 8)) : (int16_t)(p[1] | (p[2] << 8));
                    if (c < 2) v[c] = s;
                }
                if (wav.ch == 1) v[1] = v[0];
                buf[i * 2] = (int16_t)v[0]; buf[i * 2 + 1] = (int16_t)v[1];
            }
        } else {
            std::vector<int16_t> tmp((size_t)N * std::max(1, ch));
            uint64_t r = src == SRC_MP3 ? drmp3_read_pcm_frames_s16(mp3, N, tmp.data()) : drflac_read_pcm_frames_s16(flac, N, tmp.data());
            got = (int)r;
            for (int i = 0; i < got; i++) {
                int16_t a = tmp[(size_t)i * ch], b = ch >= 2 ? tmp[(size_t)i * ch + 1] : a;
                buf[i * 2] = a; buf[i * 2 + 1] = b;
            }
        }
        buf.resize((size_t)got * 2);
        buf_start = idx; next = idx + got;
    }
    void get(int64_t idx, int* l, int* r) {
        *l = *r = 0;
        if (src == SRC_NONE || idx < 0) return;
        if (idx < buf_start || idx >= buf_start + (int64_t)(buf.size() / 2)) {
            if (buf.empty() && next >= 0 && idx >= next) return;   // ファイルの終わり
            fill(idx);
            if (idx >= buf_start + (int64_t)(buf.size() / 2)) return;
        }
        size_t k = (size_t)(idx - buf_start) * 2;
        *l = buf[k]; *r = buf[k + 1];
    }
};
static Reader s_rd;

// ---- トラック表 ---------------------------------------------------------------
void reset() {
    s_rd.close();
    s_tracks.clear();
    s_busy = s_paused = false; s_pos = s_end = 0; s_last_start = s_last_end = 0;
    s_vol0 = s_vol1 = 0xFF;
}
bool has_audio() { for (auto& t : s_tracks) if (t.audio) return true; return false; }
int first_track() { return s_tracks.empty() ? 1 : s_tracks.front().no; }
int last_track() { return s_tracks.empty() ? 1 : s_tracks.back().no; }
uint32_t leadout() { return s_tracks.empty() ? 0 : s_tracks.back().start + s_tracks.back().length; }
const Track* track(int no) { for (auto& t : s_tracks) if (t.no == no) return &t; return nullptr; }
int track_at(uint32_t hsg) {
    for (size_t i = 0; i < s_tracks.size(); i++) {
        uint32_t e = i + 1 < s_tracks.size() ? s_tracks[i + 1].start : s_tracks[i].start + s_tracks[i].length;
        if (hsg >= s_tracks[i].start && hsg < e) return s_tracks[i].no;
    }
    return 0;
}

static std::string upper(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return s; }
static int src_of_ext(const std::string& name) {
    size_t d = name.find_last_of('.');
    std::string e = d == std::string::npos ? "" : upper(name.substr(d + 1));
    if (e == "WAV" || e == "WAVE") return SRC_WAV;
    if (e == "MP3") return SRC_MP3;
    if (e == "FLAC") return SRC_FLAC;
    return SRC_NONE;
}
static std::string dir_of(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}
// 音のファイルの長さ（44.1kHz の標本数）。読めなければ -1
static int64_t audio_frames44(const std::string& path, int src) {
    Track t; t.no = 0; t.src = src; t.path = path;
    Reader r;
    if (!r.open(t)) return -1;
    int64_t n = -1;
    if (src == SRC_WAV) n = r.wav.bytes / (r.wav.ch * (r.wav.bits / 8));
    else if (src == SRC_MP3) n = (int64_t)drmp3_get_pcm_frame_count(r.mp3);
    else if (src == SRC_FLAC) n = (int64_t)r.flac->totalPCMFrameCount;
    int rate = r.rate;
    r.close();
    if (n < 0 || rate <= 0) return -1;
    return (int64_t)((double)n * 44100.0 / rate);
}

static bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        uint8_t c = (uint8_t)s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || (n > 0 && i + (size_t)n >= s.size())) return false;
        for (int k = 1; k <= n; k++) if (((uint8_t)s[i + k] & 0xC0) != 0x80) return false;
        i += (size_t)n + 1;
    }
    return true;
}
static uint32_t msf(const std::string& s) {   // mm:ss:ff → フレーム
    int m = 0, sec = 0, f = 0;
    if (sscanf(s.c_str(), "%d:%d:%d", &m, &sec, &f) != 3) return 0;
    return (uint32_t)(m * 4500 + sec * 75 + f);
}
static std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> v; std::string cur; bool q = false, any = false;
    for (char c : line) {
        if (c == '"') { q = !q; any = true; continue; }
        if (!q && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) { if (any || !cur.empty()) v.push_back(cur); cur.clear(); any = false; continue; }
        cur.push_back(c);
    }
    if (any || !cur.empty()) v.push_back(cur);
    return v;
}

bool load_cue(const std::string& cue, std::string* err, std::string* data_file, uint32_t* sec_size, uint32_t* sec_off, int64_t* data_off) {
    reset();
    void* h = hostfs::open(cue, 0, false, false);
    if (!h) { if (err) *err = "CUE を開けません"; return false; }
    std::string txt; { char b[4096]; int r; while ((r = hostfs::read(h, b, sizeof(b))) > 0) txt.append(b, (size_t)r); }
    hostfs::close(h);
    if (txt.size() >= 3 && (uint8_t)txt[0] == 0xEF && (uint8_t)txt[1] == 0xBB && (uint8_t)txt[2] == 0xBF) txt.erase(0, 3);
    std::string dir = dir_of(cue);
    struct Ent { int no; bool audio; uint32_t ss, so; std::string file; int src; int64_t file_frames; uint32_t base, idx1, shift; bool has1; };
    std::vector<Ent> es;
    std::string file; int fsrc = SRC_NONE; int64_t fframes = 0; uint32_t fbase = 0, shift = 0;
    bool file_has_data2048 = false;
    size_t p = 0;
    while (p <= txt.size()) {
        size_t e = txt.find('\n', p); if (e == std::string::npos) e = txt.size();
        std::vector<std::string> t = tokens(txt.substr(p, e - p));
        p = e + 1;
        if (t.empty()) continue;
        std::string k = upper(t[0]);
        if (k == "FILE" && t.size() >= 2) {
            if (!file.empty()) fbase += (uint32_t)fframes;
            std::string name = t[1];
            if (!valid_utf8(name)) name = hostfs::from_sjis(name);   // CUE の中の名前がシフト JIS なら、ホストの名前（UTF-8）にする
            bool abs = (!name.empty() && (name[0] == '/' || name[0] == '\\')) || (name.size() > 1 && name[1] == ':');
            file = abs ? name : hostfs::join(dir, name);
            HostDirEntry st;
            if (!hostfs::stat(file, st)) {   // CUE に書かれた名前と大文字小文字が違うことがある
                std::vector<HostDirEntry> l; hostfs::list_dir(dir, l);
                for (auto& x : l) if (upper(x.name) == upper(name)) { file = hostfs::join(dir, x.name); break; }
            }
            std::string ty = t.size() >= 3 ? upper(t[2]) : "BINARY";
            fsrc = ty == "WAVE" ? SRC_WAV : ty == "MP3" ? SRC_MP3 : ty == "FLAC" ? SRC_FLAC : SRC_BIN;
            if (fsrc == SRC_WAV || fsrc == SRC_BIN) { int s2 = src_of_ext(file); if (s2 == SRC_MP3 || s2 == SRC_FLAC) fsrc = s2; }
            fframes = 0; file_has_data2048 = false;
            if (fsrc == SRC_BIN) { if (hostfs::stat(file, st)) fframes = st.size / 2352; }
            else { int64_t n = audio_frames44(file, fsrc); fframes = n > 0 ? (n + 587) / 588 : 0; }
        } else if (k == "TRACK" && t.size() >= 3) {
            Ent x{}; x.no = atoi(t[1].c_str()); std::string ty = upper(t[2]);
            x.audio = ty == "AUDIO";
            x.ss = 2352; x.so = 0;
            if (ty == "MODE1/2048") { x.ss = 2048; file_has_data2048 = true; }
            else if (ty == "MODE1/2352") x.so = 16;
            else if (ty == "MODE2/2352") x.so = 24;
            else if (ty == "MODE2/2336") { x.ss = 2336; x.so = 8; }
            x.file = file; x.src = fsrc; x.base = fbase; x.shift = shift; x.has1 = false;
            if (file_has_data2048 && fsrc == SRC_BIN) { HostDirEntry st; if (hostfs::stat(file, st)) fframes = st.size / 2048; }
            x.file_frames = fframes;
            es.push_back(x);
        } else if (k == "PREGAP" && t.size() >= 2 && !es.empty()) {
            shift += msf(t[1]); es.back().shift = shift;
        } else if (k == "POSTGAP" && t.size() >= 2) {
            shift += msf(t[1]);
        } else if (k == "INDEX" && t.size() >= 3 && !es.empty() && atoi(t[1].c_str()) == 1) {
            es.back().idx1 = msf(t[2]); es.back().has1 = true; es.back().shift = shift;
        }
    }
    if (!file.empty()) fbase += (uint32_t)fframes;
    if (es.empty()) { if (err) *err = "CUE にトラックがありません"; return false; }
    bool got_data = false;
    for (size_t i = 0; i < es.size(); i++) {
        Ent& x = es[i];
        Track tr;
        tr.no = x.no; tr.audio = x.audio;
        tr.start = x.base + x.idx1 + x.shift;
        tr.path = x.file;
        tr.src = x.audio ? x.src : SRC_NONE;
        tr.sec_size = x.ss; tr.sec_off = x.so;
        if (x.src == SRC_BIN) tr.file_off = (int64_t)x.idx1 * x.ss;
        else tr.file_off = (int64_t)x.idx1 * 588;   // 標本の位置
        uint32_t end;
        if (i + 1 < es.size()) end = es[i + 1].base + es[i + 1].idx1 + es[i + 1].shift;
        else end = x.base + (uint32_t)x.file_frames + x.shift;
        tr.length = end > tr.start ? end - tr.start : 0;
        if (!x.audio && !got_data) {
            got_data = true;
            if (data_file) *data_file = x.file;
            if (sec_size) *sec_size = x.ss;
            if (sec_off) *sec_off = x.so;
            if (data_off) *data_off = (int64_t)x.idx1 * x.ss;
        }
        s_tracks.push_back(tr);
    }
    // HSG はトラック 1 の先頭が 0 になるようにそろえる
    uint32_t b = s_tracks.front().start;
    for (auto& t : s_tracks) t.start -= b;
    if (!got_data && data_file) data_file->clear();
    return true;
}

void set_data_only(uint32_t sectors) {
    reset();
    Track t; t.no = 1; t.audio = false; t.start = 0; t.length = sectors;
    s_tracks.push_back(t);
}

bool is_audio_file(const std::string& name) { int s = src_of_ext(name); return s == SRC_WAV || s == SRC_MP3 || s == SRC_FLAC; }

int add_folder(const std::string& dir, std::string* info) {
    std::vector<HostDirEntry> l;
    if (!hostfs::list_dir(dir, l)) { if (info) *info = "フォルダを開けません"; return 0; }
    struct F { int no; std::string path; int src; };
    std::vector<F> fs;
    for (auto& e : l) {
        if (e.is_dir) continue;
        int src = src_of_ext(e.name);
        if (src == SRC_NONE) continue;
        size_t d = e.name.find_last_of('.');
        std::string stem = e.name.substr(0, d);
        if (stem.size() < 2) continue;
        char a = stem[stem.size() - 2], b = stem[stem.size() - 1];
        if (a < '0' || a > '9' || b < '0' || b > '9') continue;
        int no = (a - '0') * 10 + (b - '0');
        if (no < 1 || no > 99) continue;
        bool dup = false;
        for (auto& f : fs) if (f.no == no) dup = true;   // 同じ番号が 2 つあれば、先に見つかった（名前の順で前の）ほう
        if (!dup) fs.push_back({no, hostfs::join(dir, e.name), src});
    }
    std::sort(fs.begin(), fs.end(), [](const F& x, const F& y) { return x.no < y.no; });
    int n = 0;
    std::string skipped;
    for (auto& f : fs) {
        Track* t = nullptr;
        for (auto& x : s_tracks) if (x.no == f.no) t = &x;
        if (t && !t->audio) { skipped += " " + std::to_string(f.no); continue; }   // データトラックは置き換えない
        int64_t frames = audio_frames44(f.path, f.src);
        if (frames <= 0) { skipped += " " + std::to_string(f.no); continue; }
        uint32_t secs = (uint32_t)((frames + 587) / 588);
        if (t) {   // CUE にあるトラック: 位置はそのまま、音だけ置き換える
            t->src = f.src; t->path = f.path; t->file_off = 0;
        } else {
            // 足りない番号は短い無音のトラックで埋めて、番号を飛ばさない
            while ((s_tracks.empty() ? 0 : last_track()) + 1 < f.no) {
                Track g; g.no = s_tracks.empty() ? 1 : last_track() + 1; g.audio = true; g.start = s_tracks.empty() ? 0 : leadout() + 150; g.length = 300;
                s_tracks.push_back(g);
            }
            // 音楽 CD（データトラックが無い）なら、最初のトラックは 0（Red Book 00:02:00）から
            Track a; a.no = f.no; a.audio = true; a.start = s_tracks.empty() ? 0 : leadout() + 150; a.length = secs;
            a.src = f.src; a.path = f.path;
            s_tracks.push_back(a);
        }
        n++;
    }
    if (info) *info = std::to_string(n) + " 曲" + (skipped.empty() ? "" : "（使えなかった番号:" + skipped + "）");
    return n;
}

// ---- 演奏 --------------------------------------------------------------------
void play(uint32_t start, uint32_t count) {
    s_last_start = start; s_last_end = start + count;
    s_pos = (double)start * 588.0; s_end = (double)(start + count) * 588.0;
    s_busy = count > 0; s_paused = false;
}
void stop() {
    if (s_busy) { s_busy = false; s_paused = true; s_last_start = (uint32_t)(s_pos / 588.0); }
    else { s_paused = false; }
}
bool resume() {
    if (!s_paused) return false;
    s_paused = false;
    if (s_pos < s_end) s_busy = true;
    return true;
}
bool playing() { return s_busy; }
bool paused() { return s_paused; }
uint32_t position() { return (uint32_t)(s_pos / 588.0); }
void last_range(uint32_t* st, uint32_t* en) { *st = s_last_start; *en = s_last_end; }
void set_volume(uint8_t a, uint8_t b) { s_vol0 = a; s_vol1 = b; }
void get_volume(uint8_t* a, uint8_t* b) { *a = s_vol0; *b = s_vol1; }

void render_one(int rate, float* l, float* r) {
    if (!s_busy) return;
    uint32_t sec = (uint32_t)(s_pos / 588.0);
    const Track* t = nullptr;
    int no = track_at(sec);
    if (no) t = track(no);
    if (t && t->audio && t->src != SRC_NONE) {
        if (s_rd.no != t->no && !s_rd.open(*t)) s_rd.no = t->no;   // 開けなければ無音
        double local = s_pos - (double)t->start * 588.0;   // トラックの先頭からの 44.1kHz の標本
        double sp = s_rd.src == SRC_BIN ? local : local * s_rd.rate / 44100.0;
        int64_t i0 = (int64_t)floor(sp);
        double fr = sp - (double)i0;
        int l0, r0, l1, r1;
        s_rd.get(i0, &l0, &r0);
        s_rd.get(i0 + 1, &l1, &r1);
        float g0 = s_vol0 / 255.0f / 32768.0f, g1 = s_vol1 / 255.0f / 32768.0f;
        *l += (float)(l0 + (l1 - l0) * fr) * g0;
        *r += (float)(r0 + (r1 - r0) * fr) * g1;
    }
    s_pos += 44100.0 / rate;
    if (s_pos >= s_end) { s_busy = false; s_paused = false; }
}
void skip_seconds(double sec) {
    if (!s_busy) return;
    s_pos += sec * 44100.0;
    if (s_pos >= s_end) { s_busy = false; s_paused = false; }
}

void state_save(StateW& w) {
    w.tag("CDA1");
    w.pod(s_busy); w.pod(s_paused); w.pod(s_pos); w.pod(s_end);
    w.pod(s_last_start); w.pod(s_last_end); w.pod(s_vol0); w.pod(s_vol1);
}
void state_load(StateR& r) {
    if (!r.peek_tag("CDA1")) { s_busy = s_paused = false; return; }
    r.tag("CDA1");
    r.pod(s_busy); r.pod(s_paused); r.pod(s_pos); r.pod(s_end);
    r.pod(s_last_start); r.pod(s_last_end); r.pod(s_vol0); r.pod(s_vol1);
    s_rd.close();
}

} // namespace cdda
