// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  fdreal.cpp  --  実機のフロッピードライブ
//
//  1) USB フロッピー（Windows のフロッピードライブ \\.\A: 等）
//     Windows のドライバ経由で「論理セクタ」を読み書きする。ID は C/H/1..n/N の決まった形になる
//     （USB フロッピーの中の制御回路が ID を扱うので、ID をずらしたセクタや READ ID 等は扱えない）。
//     PC-98 の 1.25MB（1024 バイト x 8 セクタ x 77 シリンダ）は 3 モード対応のドライブで読める。
//     書き込みはボリュームをロックできたときだけ（できなければ書き込み禁止として扱う）。
//
//  2) Greaseweazle（フロッピードライブの磁気信号＝フラックスをそのまま読む USB 基板）
//     公開されている通信手順（USB CDC のシリアル）で直接やり取りし、トラックを読むたびに
//     フラックスを復調する（fdflux.cpp）。ID のずれた・隠れたセクタ、CRC エラー、読むたびに
//     変わるセクタ等、プロテクトの情報もそのまま出てくる。読み込みのみ。
//     Windows 以外（開発用のテスト）ではシリアルの代わりに端末（/dev/tty* や pty）を開く。
// -----------------------------------------------------------------------------
#include "fdreal.h"
#include "machine.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>
#endif

uint32_t fdreal_now_ms() {
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static std::string upper(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return s; }

// 名前の種類: 1 = USB フロッピー（ブロックデバイス）, 2 = Greaseweazle, 0 = ファイル
static int spec_kind(const std::string& spec, std::string* arg) {
    std::string u = upper(spec);
    if (u.size() >= 4 && u.compare(0, 4, "FDD:") == 0) { if (arg) *arg = spec.substr(4); return 1; }
    if (u.size() == 6 && u.compare(0, 4, "\\\\.\\") == 0 && u[5] == ':') { if (arg) *arg = spec.substr(4, 1); return 1; }
    if (u == "GW" || u == "GW:") { if (arg) arg->clear(); return 2; }
    if (u.size() > 3 && u.compare(0, 3, "GW:") == 0) { if (arg) *arg = spec.substr(3); return 2; }
    return 0;
}

std::string fdreal_format_name(const std::string& spec) {
    int k = spec_kind(spec, nullptr);
    return k == 1 ? "USB-FDD" : k == 2 ? "Greaseweazle" : "";
}

// ---- Greaseweazle の設定 ---------------------------------------------------------------
static std::string s_gw_drive = "A";
static int s_gw_revs = 3;

namespace floppy {
bool is_device_spec(const std::string& name) { return spec_kind(name, nullptr) != 0; }
void set_gw_options(const std::string& drive, int revs) {
    s_gw_drive = drive.empty() ? "A" : upper(drive);
    s_gw_revs = revs < 1 ? 1 : revs > 8 ? 8 : revs;
}
}

// =====================================================================================
//  1) ブロックデバイス（USB フロッピー）
// =====================================================================================
class BlockDrive : public FdSource {
public:
    int cyls = 80, heads = 2, spt = 8, bps = 1024;
    bool can_write = false;
    std::string err;
    virtual ~BlockDrive() {}
    // off バイト目から len バイト（bps の倍数）。失敗したら false と err
    virtual bool read_at(int64_t off, uint8_t* buf, int len) = 0;
    virtual bool write_at(int64_t off, const uint8_t* buf, int len) = 0;
    virtual bool requery() { return true; }   // 媒体の形を取り直す（入れ替え後）

    uint8_t n_of() const { return (uint8_t)(bps == 128 ? 0 : bps == 256 ? 1 : bps == 512 ? 2 : bps == 1024 ? 3 : 4); }
    void set_hints() {
        media_hint = bps == 1024 ? FD_2HD : spt >= 18 ? FD_144 : spt >= 15 ? FD_2HD : FD_2DD;
        cyl_hint = cyls;
    }
    bool fetch(int cyl, int head, FdTrack& out) override {
        out.secs.clear(); out.diags.clear();
        err.clear();
        if (cyl == 0 && head == 0) requery();              // 入れ替えたかもしれない: 形と書き込み禁止を取り直す
        if (cyl >= cyls || head >= heads) return false;   // 範囲外 = 何も無いトラック
        std::vector<uint8_t> buf((size_t)spt * bps);
        int64_t off = ((int64_t)cyl * heads + head) * spt * bps;
        // トラックをまとめて読む。失敗したら少し間をおいてもう一度（USB フロッピーは、変わった
        // トラックへ移った直後の読み込みだけ失敗することがある。T98-Next + NFDMAKE で読めるのもこのため）
        bool whole = false;
        for (int a = 0; a < 2 && !whole; a++) {
            err.clear();
            whole = read_at(off, buf.data(), (int)buf.size());
            if (!whole) {
                plog("[fd] USB-FDD C=%02X H=%02X: トラックをまとめて読めません（%s）\n", cyl, head, err.c_str());
                if (!err.empty() && err[0] == '!') { err = err.substr(1); return false; }   // ディスクが無い／入れ替えた
            }
        }
        for (int r = 1; r <= spt; r++) {
            FdSector s;
            s.c = (uint8_t)cyl; s.h = (uint8_t)head; s.r = (uint8_t)r; s.n = n_of();
            s.deleted = false; s.fm = false; s.file_off = -1; s.copies = 1; s.next_copy = 0;
            s.status = 0;
            uint8_t* p = &buf[(size_t)(r - 1) * bps];
            if (!whole) {   // 1 セクタずつ読み直す（何度か）。どうしても読めないセクタはデータ CRC エラー
                bool ok = false;
                int a;
                for (a = 0; a < 5 && !ok; a++) {
                    if (a == 2 || a == 4) {   // ヘッドを動かしてからやり直す（トラック 0 を読んで戻る）
                        std::vector<uint8_t> t0((size_t)bps);
                        err.clear(); read_at(0, t0.data(), bps);
                        requery();
                    }
                    err.clear();
                    ok = read_at(off + (int64_t)(r - 1) * bps, p, bps);
                }
                if (!ok) {
                    plog("[fd] USB-FDD C=%02X H=%02X R=%02X: 読めません（%s）\n", cyl, head, r, err.c_str());
                    s.status = 0xB0; memset(p, 0, (size_t)bps);
                    s.file_off = -2;   // 実機で読めなかった印（次に使うときにもう一度読みに行く）
                } else if (a > 1) plog("[fd] USB-FDD C=%02X H=%02X R=%02X: %d 回目で読めました\n", cyl, head, r, a);
            }
            s.data.assign(p, p + bps);
            out.secs.push_back(s);
        }
        err.clear();
        return true;
    }
    bool write(int cyl, int head, const FdSector& s, const uint8_t* buf, int len) override {
        if (!can_write || s.r < 1 || s.r > spt) return false;
        std::vector<uint8_t> tmp((size_t)bps, 0);
        memcpy(tmp.data(), buf, (size_t)std::min(len, bps));
        if (len < bps && s.data.size() >= (size_t)bps) memcpy(tmp.data() + len, s.data.data() + len, (size_t)(bps - len));
        int64_t off = (((int64_t)cyl * heads + head) * spt + (s.r - 1)) * bps;
        return write_at(off, tmp.data(), bps);
    }
    bool writable() const override { return can_write; }
    std::string last_error() const override { return err; }
};

#ifdef _WIN32
class WinDrive : public BlockDrive {
public:
    HANDLE h = INVALID_HANDLE_VALUE;
    bool media_wp = false;       // ディスクが書き込み禁止
    bool writable() const override { return can_write && !media_wp; }
    bool locked = false;
    uint8_t* abuf = nullptr;            // セクタ境界にそろえたバッファ（FILE_FLAG_NO_BUFFERING 用）
    size_t abuf_size = 0;
    ~WinDrive() override {
        if (h != INVALID_HANDLE_VALUE) {
            DWORD br;
            if (locked) DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr);
            CloseHandle(h);
        }
        if (abuf) VirtualFree(abuf, 0, MEM_RELEASE);
    }
    bool ensure_buf(size_t n) {
        if (abuf_size >= n) return true;
        if (abuf) VirtualFree(abuf, 0, MEM_RELEASE);
        abuf = (uint8_t*)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        abuf_size = abuf ? n : 0;
        return abuf != nullptr;
    }
    bool requery() override {
        DISK_GEOMETRY g; DWORD br;
        if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &g, sizeof(g), &br, nullptr)) return false;
        cyls = (int)g.Cylinders.QuadPart; heads = (int)g.TracksPerCylinder; spt = (int)g.SectorsPerTrack; bps = (int)g.BytesPerSector;
        if (cyls <= 0 || heads <= 0 || spt <= 0 || bps < 128 || bps > 4096) return false;
        set_hints();
        // ディスクの書き込み禁止のつまみ
        media_wp = !DeviceIoControl(h, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &br, nullptr) && GetLastError() == ERROR_WRITE_PROTECT;
        return true;
    }
    bool io(int64_t off, uint8_t* buf, int len, bool wr) {
        if (!ensure_buf((size_t)len)) { err = "メモリが足りません"; return false; }
        LARGE_INTEGER li; li.QuadPart = off;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) { err = "位置を合わせられません"; return false; }
        DWORD done = 0;
        if (wr) memcpy(abuf, buf, (size_t)len);
        BOOL ok = wr ? WriteFile(h, abuf, (DWORD)len, &done, nullptr) : ReadFile(h, abuf, (DWORD)len, &done, nullptr);
        if (ok && done == (DWORD)len) { if (!wr) memcpy(buf, abuf, (size_t)len); return true; }
        DWORD e = GetLastError();
        if (e == ERROR_NOT_READY || e == ERROR_NO_MEDIA_IN_DRIVE) { err = "!ディスクが入っていません（Windows のエラー " + std::to_string((unsigned long)e) + "）"; return false; }
        if (e == ERROR_MEDIA_CHANGED) {   // 入れ替えた: 形を取り直してもう一度
            if (requery()) {
                ok = wr ? WriteFile(h, abuf, (DWORD)len, &done, nullptr) : ReadFile(h, abuf, (DWORD)len, &done, nullptr);
                if (ok && done == (DWORD)len) { if (!wr) memcpy(buf, abuf, (size_t)len); return true; }
            }
            err = "!ディスクが入れ替わりました（Windows のエラー " + std::to_string((unsigned long)GetLastError()) + "）"; return false;
        }
        char b[64]; snprintf(b, sizeof(b), "読み書きの失敗（Windows のエラー %lu）", (unsigned long)e);
        err = b;
        return false;
    }
    bool read_at(int64_t off, uint8_t* buf, int len) override { return io(off, buf, len, false); }
    bool write_at(int64_t off, const uint8_t* buf, int len) override { return io(off, const_cast<uint8_t*>(buf), len, true); }
};

static BlockDrive* open_block(const std::string& arg, std::string* err) {
    if (arg.empty() || !isalpha((unsigned char)arg[0])) { if (err) *err = "ドライブ名が正しくありません"; return nullptr; }
    wchar_t path[8] = L"\\\\.\\A:";
    path[4] = (wchar_t)toupper((unsigned char)arg[0]);
    WinDrive* d = new WinDrive();
    d->h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
    bool rw = d->h != INVALID_HANDLE_VALUE;
    if (!rw) d->h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
    if (d->h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (err) *err = e == ERROR_ACCESS_DENIED ? "ドライブを開けません（管理者として実行すると開ける場合があります）" : "ドライブを開けません";
        delete d; return nullptr;
    }
    if (!d->requery()) {
        if (err) *err = "ディスクが入っていないか、この形のディスクを読めません（1.25MB は 3 モード対応のドライブが必要です）";
        delete d; return nullptr;
    }
    if (rw) {
        DWORD br;
        // 書き込むには Windows のファイルシステムから切り離す（ロック）。できなければ読み込みのみ
        if (DeviceIoControl(d->h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr)) {
            d->locked = true;
            DeviceIoControl(d->h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr);
            d->can_write = true;
        }
    }
    plog("[fd] USB-FDD %c: %d シリンダ x %d 面 x %d セクタ x %d バイト%s\n", (char)path[4], d->cyls, d->heads, d->spt, d->bps,
         d->can_write ? "" : "（読み込みのみ）");
    return d;
}
#else
// Windows 以外: ブロックデバイスや「ベタ」のファイルを同じように扱う（テスト用）
class FileDrive : public BlockDrive {
public:
    int fd = -1;
    ~FileDrive() override { if (fd >= 0) close(fd); }
    int fail_left = -1; int64_t fail_lo = 0, fail_hi = 0;   // テスト用: PC98PLAYER_FDFAIL=始め:終わり:回数
    bool read_at(int64_t off, uint8_t* buf, int len) override {
        if (fail_left < 0) {
            fail_left = 0;
            if (const char* e = getenv("PC98PLAYER_FDFAIL")) { long long a, b; int n; if (sscanf(e, "%lld:%lld:%d", &a, &b, &n) == 3) { fail_lo = a; fail_hi = b; fail_left = n; } }
        }
        if (fail_left > 0 && off < fail_hi && off + len > fail_lo) { fail_left--; err = "テスト用の失敗"; return false; }
        if (pread(fd, buf, (size_t)len, off) == len) return true;
        err = "読み込みの失敗"; return false;
    }
    bool write_at(int64_t off, const uint8_t* buf, int len) override {
        if (pwrite(fd, buf, (size_t)len, off) == len) return true;
        err = "書き込みの失敗"; return false;
    }
};
static BlockDrive* open_block(const std::string& arg, std::string* err) {
    FileDrive* d = new FileDrive();
    d->fd = open(arg.c_str(), O_RDWR);
    d->can_write = d->fd >= 0;
    if (d->fd < 0) d->fd = open(arg.c_str(), O_RDONLY);
    if (d->fd < 0) { if (err) *err = "開けません"; delete d; return nullptr; }
    off_t sz = lseek(d->fd, 0, SEEK_END);
    struct G { off_t size; int c, h, spt, ss; };
    static const G g[] = { {1261568, 77, 2, 8, 1024}, {1228800, 80, 2, 15, 512}, {1474560, 80, 2, 18, 512},
                           {737280, 80, 2, 9, 512}, {655360, 80, 2, 8, 512} };
    bool ok = false;
    for (const auto& x : g) if (x.size == sz) { d->cyls = x.c; d->heads = x.h; d->spt = x.spt; d->bps = x.ss; ok = true; }
    if (!ok) { if (err) *err = "大きさから形が分かりません"; delete d; return nullptr; }
    d->set_hints();
    return d;
}
#endif

// =====================================================================================
//  2) Greaseweazle
// =====================================================================================
class Serial {
public:
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD cur_timeout = 0xFFFFFFFF;
    ~Serial() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool open(const std::string& port) {
        std::string p = port;
        if (p.compare(0, 4, "\\\\.\\") != 0) p = "\\\\.\\" + p;
        h = CreateFileA(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        set_baud(9600);
        return true;
    }
    void set_baud(DWORD b) {
        DCB dcb = {}; dcb.DCBlength = sizeof(dcb);
        GetCommState(h, &dcb);
        dcb.BaudRate = b; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE; dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE; dcb.fOutX = FALSE; dcb.fInX = FALSE;
        SetCommState(h, &dcb);
    }
    // 通信の立て直し: 10000bps に一度切り替えると Greaseweazle は送受信を捨てて待ち受けに戻る
    void reset_comms() { set_baud(10000); set_baud(9600); PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR); }
    bool write(const void* p, size_t n) { DWORD w = 0; return WriteFile(h, p, (DWORD)n, &w, nullptr) && w == n; }
    // 何か届けば返る（届いた分だけ）。timeout_ms で 0
    int read(void* p, size_t n, int timeout_ms) {
        if ((DWORD)timeout_ms != cur_timeout) {
            COMMTIMEOUTS to = {};
            to.ReadIntervalTimeout = MAXDWORD; to.ReadTotalTimeoutMultiplier = MAXDWORD; to.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
            to.WriteTotalTimeoutConstant = 2000;
            SetCommTimeouts(h, &to);
            cur_timeout = (DWORD)timeout_ms;
        }
        DWORD r = 0;
        if (!ReadFile(h, p, (DWORD)n, &r, nullptr)) return -1;
        return (int)r;
    }
#else
    int fd = -1;
    ~Serial() { if (fd >= 0) ::close(fd); }
    bool open(const std::string& port) {
        fd = ::open(port.c_str(), O_RDWR | O_NOCTTY);
        if (fd < 0) return false;
        struct termios t;
        if (tcgetattr(fd, &t) == 0) { cfmakeraw(&t); cfsetspeed(&t, B9600); tcsetattr(fd, TCSANOW, &t); }
        return true;
    }
    void reset_comms() { tcflush(fd, TCIOFLUSH); }
    bool write(const void* p, size_t n) {
        const uint8_t* q = (const uint8_t*)p;
        while (n) { ssize_t w = ::write(fd, q, n); if (w <= 0) return false; q += w; n -= (size_t)w; }
        return true;
    }
    int read(void* p, size_t n, int timeout_ms) {
        struct pollfd pf = {fd, POLLIN, 0};
        int r = poll(&pf, 1, timeout_ms);
        if (r <= 0) return r;
        ssize_t k = ::read(fd, p, n);
        return k < 0 ? -1 : (int)k;
    }
#endif
    bool read_exact(void* p, size_t n, int timeout_ms) {
        uint8_t* q = (uint8_t*)p;
        uint32_t t0 = fdreal_now_ms();
        while (n) {
            int k = read(q, n, 100);
            if (k < 0) return false;
            q += k; n -= (size_t)k;
            if (n && (int)(fdreal_now_ms() - t0) > timeout_ms) return false;
        }
        return true;
    }
};

class GwDrive : public FdSource {
public:
    Serial ser;
    uint32_t sample_freq = 72000000;
    int bus = 1, unit = 0;             // 1 = IBM PC（A/B）, 2 = Shugart（0/1/2）
    int revs = 3;
    bool motor = false;
    uint32_t last_use = 0;
    std::string err;
    int fw_major = 0, fw_minor = 0;

    enum { C_GETINFO = 0, C_SEEK = 2, C_HEAD = 3, C_MOTOR = 6, C_READFLUX = 7, C_FLUXSTATUS = 9, C_SELECT = 12, C_DESELECT = 13, C_BUSTYPE = 14 };
    static const char* ack_text(int a) {
        switch (a) {
        case 1: return "命令が通じません"; case 2: return "索引穴の信号がありません（ディスクが入っていない？）";
        case 3: return "トラック 0 が見つかりません"; case 4: return "フラックスがあふれました"; case 6: return "書き込み禁止";
        case 7: return "ドライブがありません（GWDrive の設定？）"; case 8: return "バスの種類が決まっていません";
        case 9: return "ドライブ番号が正しくありません"; case 11: return "シリンダ番号が正しくありません";
        default: return "エラー";
        }
    }
    // 命令を送って、返事（命令番号, 結果）を受け取る
    int cmd(const uint8_t* c, int n) {
        if (!ser.write(c, (size_t)n)) return -1;
        uint8_t r[2];
        if (!ser.read_exact(r, 2, 3000)) return -1;
        if (r[0] != c[0]) return -2;
        return r[1];
    }
    bool open(const std::string& port, std::string* e) {
        if (!ser.open(port)) { if (e) *e = "ポート " + port + " を開けません"; return false; }
        ser.reset_comms();
        uint8_t gi[3] = {C_GETINFO, 3, 0};
        uint8_t info[32];
        if (cmd(gi, 3) != 0 || !ser.read_exact(info, 32, 2000)) { if (e) *e = "Greaseweazle から返事がありません（" + port + "）"; return false; }
        fw_major = info[0]; fw_minor = info[1];
        if (!info[2]) { if (e) *e = "Greaseweazle が更新モードです（通常のファームウェアで起動してください）"; return false; }
        sample_freq = (uint32_t)(info[4] | (info[5] << 8) | (info[6] << 16) | ((uint32_t)info[7] << 24));
        if (sample_freq < 1000000) sample_freq = 72000000;
        // ドライブ: A/B = IBM PC のバス, 0/1/2 = Shugart のバス
        std::string d = s_gw_drive;
        if (d == "B") { bus = 1; unit = 1; }
        else if (d == "0" || d == "1" || d == "2") { bus = 2; unit = d[0] - '0'; }
        else { bus = 1; unit = 0; }
        revs = s_gw_revs;
        uint8_t bt[3] = {C_BUSTYPE, 3, (uint8_t)bus};
        int a = cmd(bt, 3);
        if (a != 0) { if (e) *e = std::string("Greaseweazle: ") + (a < 0 ? "通信できません" : ack_text(a)); return false; }
        plog("[fd] Greaseweazle %s: ファームウェア %d.%d, %u Hz, ドライブ %s, %d 回転ずつ読む\n",
             port.c_str(), fw_major, fw_minor, sample_freq, d.c_str(), revs);
        return true;
    }
    ~GwDrive() override { motor_off(); }
    void motor_off() {
        if (!motor) return;
        uint8_t m[4] = {C_MOTOR, 4, (uint8_t)unit, 0};
        cmd(m, 4);
        uint8_t ds[2] = {C_DESELECT, 2};
        cmd(ds, 2);
        motor = false;
    }
    bool motor_on() {
        if (motor) return true;
        uint8_t s[3] = {C_SELECT, 3, (uint8_t)unit};
        int a = cmd(s, 3);
        if (a != 0) { err = std::string("Greaseweazle: ") + (a < 0 ? "通信できません" : ack_text(a)); return false; }
        uint8_t m[4] = {C_MOTOR, 4, (uint8_t)unit, 1};
        a = cmd(m, 4);
        if (a != 0) { err = std::string("Greaseweazle: ") + (a < 0 ? "通信できません" : ack_text(a)); return false; }
        motor = true;
        return true;
    }
    static uint32_t rd28(const uint8_t* p) {
        return ((uint32_t)(p[0] & 0xFE) >> 1) | ((uint32_t)(p[1] & 0xFE) << 6) | ((uint32_t)(p[2] & 0xFE) << 13) | ((uint32_t)(p[3] & 0xFE) << 20);
    }
    bool fetch(int cyl, int head, FdTrack& out) override {
        out.secs.clear(); out.diags.clear();
        err.clear();
        last_use = fdreal_now_ms();
        if (cyl > 83) return false;
        if (!motor_on()) return false;
        uint8_t sk[3] = {C_SEEK, 3, (uint8_t)cyl};
        int a = cmd(sk, 3);
        if (a != 0) { err = std::string("Greaseweazle: ") + (a < 0 ? "通信できません" : ack_text(a)); return false; }
        uint8_t hd[3] = {C_HEAD, 3, (uint8_t)head};
        if (cmd(hd, 3) != 0) { err = "Greaseweazle: 面を切り替えられません"; return false; }
        // 索引穴を revs+1 回数えるまで読む（最初の索引までは回転の途中なので使わない）
        uint16_t nidx = (uint16_t)(revs + 1);
        uint8_t rf[8] = {C_READFLUX, 8, 0, 0, 0, 0, (uint8_t)nidx, (uint8_t)(nidx >> 8)};
        a = cmd(rf, 8);
        if (a != 0) { err = std::string("Greaseweazle: ") + (a < 0 ? "通信できません" : ack_text(a)); return false; }
        std::vector<uint8_t> dat;
        dat.reserve(1 << 20);
        uint8_t tmp[16384];
        uint32_t t0 = fdreal_now_ms();
        while (true) {
            int k = ser.read(tmp, sizeof(tmp), 200);
            if (k < 0) { err = "Greaseweazle: 通信できません"; return false; }
            if (k > 0) { dat.insert(dat.end(), tmp, tmp + k); if (dat.back() == 0) break; }
            if (fdreal_now_ms() - t0 > 8000) { err = "Greaseweazle: 読み込みが終わりません"; ser.reset_comms(); return false; }
        }
        uint8_t fs[2] = {C_FLUXSTATUS, 2};
        a = cmd(fs, 2);
        if (a != 0) {
            err = std::string("Greaseweazle: ") + (a < 0 ? "通信できません" : ack_text(a));
            if (a == 2) err = "ディスクが入っていません";
            return false;
        }
        // フラックスの並び → 反転の間隔（ns）と索引の位置
        double ns_per_tick = 1e9 / sample_freq;
        std::vector<float> flux;
        std::vector<double> idx_time;     // 索引の時刻（tick）
        std::vector<double> flux_time;    // 各反転の時刻
        flux.reserve(dat.size());
        double now = 0, pending = 0;
        for (size_t i = 0; i < dat.size(); ) {
            uint8_t b = dat[i];
            if (b == 0) break;
            if (b == 255) {
                if (i + 6 > dat.size()) break;
                uint8_t op = dat[i + 1];
                uint32_t v = rd28(&dat[i + 2]);
                if (op == 1) idx_time.push_back(now + pending + v);      // 索引
                else if (op == 2) pending += v;                           // 反転の無い時間
                i += 6;
                continue;
            }
            uint32_t v;
            if (b < 250) { v = b; i += 1; }
            else { if (i + 2 > dat.size()) break; v = 250 + (uint32_t)(b - 250) * 255 + dat[i + 1] - 1; i += 2; }
            double dt = pending + v;
            pending = 0;
            now += dt;
            flux.push_back((float)(dt * ns_per_tick));
            flux_time.push_back(now);
        }
        std::vector<size_t> index;
        for (double it : idx_time) {
            size_t k = (size_t)(std::lower_bound(flux_time.begin(), flux_time.end(), it) - flux_time.begin());
            index.push_back(k);
        }
        int md = -1;
        bool ok = fdflux::decode_flux(flux, index, cyl, head, out, &md);
        if (cyl == 0 && head == 0 && md >= 0) {
            media_hint = md;
            int n3 = 0; for (auto& s : out.secs) if (s.n == 3) n3++;
            cyl_hint = (md == FD_2HD && n3 >= 8) ? 77 : 80;
        }
        last_use = fdreal_now_ms();
        if (!ok) return false;   // 未フォーマット等（err は空 = 何も無いトラック）
        return true;
    }
    void idle(uint32_t now) override {
        if (motor && now - last_use > 3000) motor_off();
    }
    std::string last_error() const override { return err; }
};

#ifdef _WIN32
// USB の ID（VID 1209 / PID 4D69 等）から Greaseweazle の COM ポートを探す
static std::vector<std::string> find_gw_ports() {
    std::vector<std::string> out;
    static const GUID ports = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    HDEVINFO di = SetupDiGetClassDevsW(&ports, nullptr, nullptr, DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA dd; dd.cbSize = sizeof(dd);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t id[512];
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, id, 512, nullptr)) continue;
        for (wchar_t* p = id; *p; p++) *p = towupper(*p);
        if (!wcsstr(id, L"VID_1209&PID_4D69") && !wcsstr(id, L"VID_1209&PID_0001")) continue;
        HKEY k = SetupDiOpenDevRegKey(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        wchar_t name[64]; DWORD sz = sizeof(name), type = 0;
        if (RegQueryValueExW(k, L"PortName", nullptr, &type, (BYTE*)name, &sz) == ERROR_SUCCESS && type == REG_SZ) {
            char a[64]; int n = WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof(a), nullptr, nullptr);
            if (n > 0) out.push_back(a);
        }
        RegCloseKey(k);
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}
#else
static std::vector<std::string> find_gw_ports() {
    std::vector<std::string> out;
    const char* e = getenv("PC98PLAYER_GW_PORT");   // テスト用
    if (e && *e) out.push_back(e);
    return out;
}
#endif

FdSource* fdreal_open(const std::string& spec, std::string* err) {
    std::string arg;
    int k = spec_kind(spec, &arg);
    if (k == 1) return open_block(arg, err);
    if (k == 2) {
        std::vector<std::string> ports;
        if (!arg.empty()) ports.push_back(arg);
        else {
            ports = find_gw_ports();
            if (ports.empty()) { if (err) *err = "Greaseweazle が見つかりません（GW:COM5 のようにポートも指定できます）"; return nullptr; }
        }
        std::string last;
        for (auto& p : ports) {
            GwDrive* g = new GwDrive();
            if (g->open(p, &last)) return g;
            delete g;
        }
        if (err) *err = last;
        return nullptr;
    }
    if (err) *err = "実機のドライブの名前ではありません";
    return nullptr;
}

namespace fdreal {
std::vector<Device> list_devices() {
    std::vector<Device> out;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_REMOVABLE) continue;
        wchar_t dev[8] = L"\\\\.\\A:"; dev[4] = (wchar_t)(L'A' + i);
        HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DISK_GEOMETRY g[20]; DWORD br = 0;
        bool floppy = false, mode3 = false;
        if (DeviceIoControl(h, IOCTL_DISK_GET_MEDIA_TYPES, nullptr, 0, g, sizeof(g), &br, nullptr)) {
            for (DWORD k = 0; k < br / sizeof(DISK_GEOMETRY); k++) {
                int t = (int)g[k].MediaType;
                if ((t >= 1 && t <= 10) || (t >= 14 && t <= 19)) floppy = true;
                if (t == 18 || t == 19) mode3 = true;    // F3_1Pt23_1024 / F5_1Pt23_1024
            }
        }
        CloseHandle(h);
        if (!floppy) continue;
        char c = (char)('A' + i);
        Device d;
        d.spec = std::string("FDD:") + c;
        d.label = std::string("USB フロッピー ") + c + ":" + (mode3 ? "（1.25MB 対応）" : "（1.25MB 非対応）");
        out.push_back(d);
    }
#endif
    for (auto& p : find_gw_ports()) {
        Device d; d.spec = "GW:" + p; d.label = "Greaseweazle（" + p + "）"; out.push_back(d);
    }
    return out;
}
}
