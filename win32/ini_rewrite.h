// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  ini_rewrite.h  --  INI の [PC98PLAYER] の値だけを書き換える（コメント・並び・改行はそのまま）
//
//  ・既にある「キー=値」は値だけを置き換える（キーの綴りや = の前の空白、行末の " ;コメント" は残す）。
//    同じキーが何度も書いてあれば全部置き換える（読む側は最後のものを使うため）。
//  ・無いキーは、[PC98PLAYER] の最後の「キー=値」の行の次に足す（後ろのコメントの塊より前）。
//  ・[PC98PLAYER] が無ければ、ファイルの最後に作る。
//  Windows の API は使わない（文字コードの変換は呼ぶ側で）。
// -----------------------------------------------------------------------------
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cwctype>

namespace inirw {

static std::wstring trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r')) a++;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r')) b--;
    return s.substr(a, b - a);
}
static std::wstring upper(std::wstring s) { for (auto& c : s) c = (wchar_t)towupper(c); return s; }

// text: INI 全体。sets: (キー, 値)。戻り値: 書き換えた全体
static std::wstring rewrite(const std::wstring& text, const std::vector<std::pair<std::wstring, std::wstring>>& sets,
                            const std::wstring& section = L"PC98PLAYER") {
    bool crlf = text.find(L"\r\n") != std::wstring::npos || text.find(L'\n') == std::wstring::npos;
    const std::wstring eol = crlf ? L"\r\n" : L"\n";
    std::vector<std::wstring> lines;
    {
        size_t p = 0;
        while (p <= text.size()) {
            size_t e = text.find(L'\n', p);
            if (e == std::wstring::npos) { if (p < text.size()) lines.push_back(text.substr(p)); break; }
            std::wstring l = text.substr(p, e - p);
            if (!l.empty() && l.back() == L'\r') l.pop_back();
            lines.push_back(l);
            p = e + 1;
        }
    }
    const std::wstring usec = upper(section);
    std::vector<bool> done(sets.size(), false);
    int sec_head = -1, last_key = -1;
    bool in_sec = false;
    for (size_t i = 0; i < lines.size(); i++) {
        std::wstring t = trim(lines[i]);
        if (t.empty() || t[0] == L';' || t[0] == L'#') continue;
        if (t[0] == L'[') {
            size_t r = t.find(L']');
            std::wstring name = upper(trim(t.substr(1, r == std::wstring::npos ? std::wstring::npos : r - 1)));
            in_sec = name == usec;
            if (in_sec && sec_head < 0) { sec_head = (int)i; last_key = (int)i; }
            continue;
        }
        if (!in_sec) continue;
        size_t eq = lines[i].find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = upper(trim(lines[i].substr(0, eq)));
        last_key = (int)i;
        for (size_t s = 0; s < sets.size(); s++) {
            if (upper(sets[s].first) != k) continue;
            std::wstring rest = lines[i].substr(eq + 1);
            size_t sc = rest.find(L" ;");
            std::wstring tail = sc == std::wstring::npos ? L"" : rest.substr(sc);
            lines[i] = lines[i].substr(0, eq + 1) + sets[s].second + tail;
            done[s] = true;
        }
    }
    std::vector<std::wstring> add;
    for (size_t s = 0; s < sets.size(); s++) if (!done[s]) add.push_back(sets[s].first + L"=" + sets[s].second);
    if (!add.empty()) {
        if (sec_head < 0) {
            if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back(L"");
            lines.push_back(L"[" + section + L"]");
            for (auto& a : add) lines.push_back(a);
        } else {
            lines.insert(lines.begin() + last_key + 1, add.begin(), add.end());
        }
    }
    std::wstring out;
    for (size_t i = 0; i < lines.size(); i++) { out += lines[i]; out += eol; }
    return out;
}

}  // namespace inirw
