// SPDX-License-Identifier: MIT
//  hostfs.cpp -- ホストのファイルシステム層
#include "hostfs.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace hostfs_native {
static std::wstring W(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    return w;
}
static std::string U(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::string from_sjis(const std::string& s) {
    int n = MultiByteToWideChar(932, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(932, 0, s.c_str(), -1, &w[0], n);
    return U(w);
}
std::string to_sjis(const std::string& s) {
    std::wstring w = W(s);
    BOOL bad = FALSE;
    int n = WideCharToMultiByte(932, 0, w.c_str(), -1, nullptr, 0, nullptr, &bad);
    std::string r(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(932, 0, w.c_str(), -1, &r[0], n, nullptr, &bad);
    return r;
}
std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char c = a.back();
    return (c == '\\' || c == '/') ? a + b : a + "\\" + b;
}
static void ft_to_dos(const FILETIME& ft, uint16_t* d, uint16_t* t) {
    FILETIME lt; FileTimeToLocalFileTime(&ft, &lt);
    WORD dd = 0, tt = 0; FileTimeToDosDateTime(&lt, &dd, &tt);
    *d = dd; *t = tt;
}
bool list_dir(const std::string& dir, std::vector<HostDirEntry>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(join(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        HostDirEntry e;
        e.name = U(fd.cFileName);
        if (e.name == "." || e.name == "..") continue;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = fd.nFileSizeLow;
        e.readonly = (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        ft_to_dos(fd.ftLastWriteTime, &e.dos_date, &e.dos_time);
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}
bool stat(const std::string& path, HostDirEntry& e) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(W(path).c_str(), GetFileExInfoStandard, &a)) return false;
    e.is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.size = a.nFileSizeLow;
    e.readonly = (a.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    ft_to_dos(a.ftLastWriteTime, &e.dos_date, &e.dos_time);
    return true;
}
void* open(const std::string& path, int mode, bool create, bool trunc) {
    DWORD acc = mode == 0 ? GENERIC_READ : mode == 1 ? GENERIC_WRITE : (GENERIC_READ | GENERIC_WRITE);
    DWORD disp = create ? (trunc ? CREATE_ALWAYS : OPEN_ALWAYS) : OPEN_EXISTING;
    HANDLE h = CreateFileW(W(path).c_str(), acc, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE && mode != 0 && !create) {
        // 読み取り専用のファイルを読み書きで開こうとした（CD から写したゲームでよくある）
        h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    return h == INVALID_HANDLE_VALUE ? nullptr : (void*)h;
}
void close(void* h) { CloseHandle((HANDLE)h); }
int read(void* h, void* buf, int n) { DWORD r = 0; if (!ReadFile((HANDLE)h, buf, (DWORD)n, &r, nullptr)) return -1; return (int)r; }
int write(void* h, const void* buf, int n) { DWORD r = 0; if (!WriteFile((HANDLE)h, buf, (DWORD)n, &r, nullptr)) return -1; return (int)r; }
int64_t seek(void* h, int64_t off, int whence) {
    LARGE_INTEGER li, np; li.QuadPart = off;
    DWORD w = whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx((HANDLE)h, li, &np, w)) return -1;
    return np.QuadPart;
}
bool truncate_here(void* h) { return SetEndOfFile((HANDLE)h) != 0; }
bool remove(const std::string& p) { return DeleteFileW(W(p).c_str()) != 0; }
bool rename(const std::string& a, const std::string& b) { return MoveFileW(W(a).c_str(), W(b).c_str()) != 0; }
bool mkdir(const std::string& p) { return CreateDirectoryW(W(p).c_str(), nullptr) != 0; }
bool rmdir(const std::string& p) { return RemoveDirectoryW(W(p).c_str()) != 0; }
bool set_time(void* h, uint16_t d, uint16_t t) {
    FILETIME lt, ft;
    if (!DosDateTimeToFileTime(d, t, &lt)) return false;
    LocalFileTimeToFileTime(&lt, &ft);
    return SetFileTime((HANDLE)h, nullptr, nullptr, &ft) != 0;
}
bool get_time(void* h, uint16_t* d, uint16_t* t) {
    FILETIME ft;
    if (!GetFileTime((HANDLE)h, nullptr, nullptr, &ft)) return false;
    ft_to_dos(ft, d, t);
    return true;
}
}
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <utime.h>

#include <iconv.h>
namespace hostfs_native {
static std::string conv(const char* to, const char* from, const std::string& s) {
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) return s;
    std::string out(s.size() * 4 + 8, '\0');
    char* in = const_cast<char*>(s.data()); size_t il = s.size();
    char* op = &out[0]; size_t ol = out.size();
    size_t r = iconv(cd, &in, &il, &op, &ol);
    iconv_close(cd);
    if (r == (size_t)-1) return s;
    out.resize(out.size() - ol);
    return out;
}
std::string from_sjis(const std::string& s) { for (unsigned char c : s) if (c >= 0x80) return conv("UTF-8", "CP932", s); return s; }
std::string to_sjis(const std::string& s) { for (unsigned char c : s) if (c >= 0x80) return conv("CP932", "UTF-8", s); return s; }
std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}
static void tm_to_dos(time_t tt, uint16_t* d, uint16_t* t) {
    struct tm lt; localtime_r(&tt, &lt);
    *d = (uint16_t)(((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
    *t = (uint16_t)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
}
bool list_dir(const std::string& dir, std::vector<HostDirEntry>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        HostDirEntry e;
        e.name = de->d_name;
        struct stat st;
        if (::stat(join(dir, e.name).c_str(), &st) != 0) continue;
        e.is_dir = S_ISDIR(st.st_mode);
        e.size = (uint32_t)st.st_size;
        e.readonly = !(st.st_mode & S_IWUSR);
        tm_to_dos(st.st_mtime, &e.dos_date, &e.dos_time);
        out.push_back(e);
    }
    closedir(d);
    return true;
}
bool stat(const std::string& path, HostDirEntry& e) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    e.is_dir = S_ISDIR(st.st_mode);
    e.size = (uint32_t)st.st_size;
    e.readonly = !(st.st_mode & S_IWUSR);
    tm_to_dos(st.st_mtime, &e.dos_date, &e.dos_time);
    return true;
}
void* open(const std::string& path, int mode, bool create, bool trunc) {
    int fl = mode == 0 ? O_RDONLY : mode == 1 ? O_WRONLY : O_RDWR;
    if (create) fl |= O_CREAT;
    if (trunc) fl |= O_TRUNC;
    int fd = ::open(path.c_str(), fl, 0644);
    if (fd < 0 && mode != 0 && !create) fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;
    return (void*)(intptr_t)(fd + 1);
}
static inline int FD(void* h) { return (int)(intptr_t)h - 1; }
void close(void* h) { ::close(FD(h)); }
int read(void* h, void* buf, int n) { return (int)::read(FD(h), buf, (size_t)n); }
int write(void* h, const void* buf, int n) { return (int)::write(FD(h), buf, (size_t)n); }
int64_t seek(void* h, int64_t off, int whence) { return (int64_t)lseek(FD(h), (off_t)off, whence); }
bool truncate_here(void* h) { off_t p = lseek(FD(h), 0, SEEK_CUR); return ftruncate(FD(h), p) == 0; }
bool remove(const std::string& p) { return ::unlink(p.c_str()) == 0; }
bool rename(const std::string& a, const std::string& b) { return ::rename(a.c_str(), b.c_str()) == 0; }
bool mkdir(const std::string& p) { return ::mkdir(p.c_str(), 0755) == 0; }
bool rmdir(const std::string& p) { return ::rmdir(p.c_str()) == 0; }
bool set_time(void*, uint16_t, uint16_t) { return true; }
bool get_time(void* h, uint16_t* d, uint16_t* t) {
    struct stat st; if (fstat(FD(h), &st) != 0) return false;
    tm_to_dos(st.st_mtime, d, t); return true;
}
}
#endif
