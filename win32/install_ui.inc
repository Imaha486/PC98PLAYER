// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  install_ui.inc  --  フロッピーイメージからのインストールの画面（main.cpp から取り込む）
//
//  PC98PLAYER.EXE の引数にフロッピーイメージ（1 枚以上）を渡すと、この画面になる。
//   ・左: フロッピーの中身（フォルダはクリックで開く、ファイルはクリックでインストーラに選ぶ）
//   ・右上: 選んだファイルの中身（BAT・テキストは先頭の行、実行ファイルは大きさ）
//   ・右下: インストーラに渡す引数、フロッピー／ハードディスク（インストール先＝カレントフォルダ）の
//           ドライブ名、カレントドライブ
//   ・「実行」でインストーラを動かす（PC98PLAYER.INI は作らない）。ディスクの入れ替えは F11 の画面で。
//     複数枚を渡したときは F11 の画面の「次のディスク」（N キー）で順に入れ替えられる。
//   ・インストーラ（バッチ）が終わったら、ゲームを起動するファイルを選んで INI を作るか聞く。
// -----------------------------------------------------------------------------

struct InstallUi {
    bool active = false, done = false, cancel = false;
    std::vector<std::wstring> disks;   // 渡されたイメージ（名前順）
    int disk = 0;
    std::wstring dest;                 // インストール先（ハードディスクとして見せるフォルダ）
    std::string cur;                   // フロッピーの中の今のフォルダ（"" = ルート、"SUB\\X"）
    struct Ent { std::string name; bool dir; uint32_t size; };
    std::vector<Ent> ents;             // 先頭が ".."（ルート以外）
    int scroll = 0;
    std::string chosen;                // 選んだファイル（フロッピーの中のパス。例 "INSTALL.BAT"）
    std::vector<std::wstring> preview;
    std::wstring args;
    bool args_focus = false;
    char fd = 'A', hd = 'B';
    bool cur_fd = true;                // カレントドライブ = フロッピー
    bool wp = true;                    // フロッピーを書き込み禁止にする（「ライトプロテクトにして下さい」と言うインストーラがある）
    std::wstring note;
    DWORD last_click = 0;
};
static InstallUi g_iu;
static bool iu_active() { return g_iu.active; }
static bool g_install_mode = false;

static const int IU_LIST_X = 12, IU_LIST_Y = 56, IU_LIST_W = 300, IU_ROW = 18, IU_ROWS = 13;
static const int IU_PV_X = 324, IU_PV_Y = 34, IU_PV_W = 304, IU_PV_H = 150;
static const int IU_ARG_X = 400, IU_ARG_Y = 190, IU_ARG_W = 228, IU_ARG_H = 20;
static const int IU_FD_Y = 214, IU_HD_Y = 238, IU_CUR_Y = 262, IU_WP_Y = 286, IU_LET_X = 440, IU_LET_W = 28, IU_LET_GAP = 32;
static const int IU_BTN_Y = 356, IU_RUN_X = 372, IU_CAN_X = 504, IU_BTN_W = 124, IU_BTN_H = 24;
static const char IU_LETTERS[] = "ABCDEF";

static bool is_floppy_image_name(const std::wstring& p) {
    size_t d = p.find_last_of(L'.');
    if (d == std::wstring::npos) return false;
    std::wstring e = upper(p.substr(d + 1));
    static const wchar_t* ex[] = {L"D88", L"D68", L"88D", L"D98", L"FDI", L"NFD", L"HDM", L"XDF", L"DUP", L"TFD", L"2HD", L"IMG", L"SCP", L"HFE"};
    for (auto* x : ex) if (e == x) return true;
    return false;
}
static std::wstring base_name(const std::wstring& p) {
    if (floppy::is_device_spec(U(p))) {   // 実機のドライブ
        std::wstring u = upper(p);
        if (u.compare(0, 4, L"FDD:") == 0 && u.size() >= 5) return L"フロッピードライブ " + u.substr(4, 1) + L":（実機）";
        return L"Greaseweazle（実機）";
    }
    size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? p : p.substr(k + 1);
}
static bool iu_real() { return floppy::is_device_spec(U(g_iu.disks[g_iu.disk])); }

static std::string iu_path(const std::string& name) {   // フロッピーの中のパス → fatfs の仮想パス
    std::string p = fatfs::root_path('A');
    if (!g_iu.cur.empty()) p += "\\" + g_iu.cur;
    if (!name.empty()) p += "\\" + name;
    return p;
}
static bool iu_is_text(const std::string& n) {
    std::string u = U(upper(W(n)));
    size_t d = u.find_last_of('.');
    std::string e = d == std::string::npos ? "" : u.substr(d + 1);
    return e == "BAT" || e == "TXT" || e == "DOC" || e == "ME" || e == "1ST" || e == "INF" || e == "CFG" || e == "NOW" ||
           e == "LST" || u.compare(0, 4, "READ") == 0;
}
static bool iu_is_exec(const std::string& n) {
    std::string u = U(upper(W(n)));
    size_t d = u.find_last_of('.');
    std::string e = d == std::string::npos ? "" : u.substr(d + 1);
    return e == "BAT" || e == "EXE" || e == "COM";
}
static void iu_preview() {
    g_iu.preview.clear();
    if (g_iu.chosen.empty()) return;
    std::string p = fatfs::root_path('A') + "\\" + g_iu.chosen;
    HostDirEntry st;
    if (!hostfs::stat(p, st)) return;
    if (iu_is_text(g_iu.chosen)) {
        void* h = hostfs::open(p, 0, false, false);
        if (!h) return;
        std::string buf(4096, '\0');
        int n = hostfs::read(h, &buf[0], (int)buf.size());
        hostfs::close(h);
        buf.resize(n > 0 ? (size_t)n : 0);
        // Shift_JIS → 画面の文字
        int wn = MultiByteToWideChar(932, 0, buf.data(), (int)buf.size(), nullptr, 0);
        std::wstring w(wn > 0 ? (size_t)wn : 0, L'\0');
        if (wn > 0) MultiByteToWideChar(932, 0, buf.data(), (int)buf.size(), &w[0], wn);
        size_t s = 0;
        while (s < w.size() && g_iu.preview.size() < 10) {
            size_t e = w.find(L'\n', s);
            std::wstring line = w.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            for (auto& c : line) if (c == L'\t') c = L' '; else if (c < 0x20 || c == 0x1A) c = L' ';
            if (line.size() > 40) line = line.substr(0, 40) + L"…";
            g_iu.preview.push_back(line);
            if (e == std::wstring::npos) break;
            s = e + 1;
        }
    } else {
        wchar_t b[96];
        swprintf(b, 96, iu_is_exec(g_iu.chosen) ? L"実行ファイル（%u バイト）" : L"（%u バイト）", (unsigned)st.size);
        g_iu.preview.push_back(b);
    }
}
static void iu_scan() {
    g_iu.ents.clear();
    g_iu.scroll = 0;
    std::vector<HostDirEntry> lst;
    fatfs::list_dir(iu_path(""), lst);
    std::sort(lst.begin(), lst.end(), [](const HostDirEntry& a, const HostDirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
    if (!g_iu.cur.empty()) g_iu.ents.push_back({"..", true, 0});
    for (auto& e : lst) if (e.name != "." && e.name != "..") g_iu.ents.push_back({e.name, e.is_dir, e.size});
}
// ルートからインストーラらしいファイルを選んでおく
static void iu_autoselect() {
    static const char* pri[] = {"INSTALL.BAT", "INSTALL.EXE", "INSTALL.COM", "INST.BAT", "INST.EXE", "SETUP.BAT", "SETUP.EXE", "HDINST.BAT", "HDINST.EXE"};
    auto up = [](const std::string& s) { return U(upper(W(s))); };
    for (auto* p : pri) for (auto& e : g_iu.ents) if (!e.dir && up(e.name) == p) { g_iu.chosen = e.name; iu_preview(); return; }
    for (auto& e : g_iu.ents) if (!e.dir && iu_is_exec(e.name) && up(e.name).find("INST") != std::string::npos) { g_iu.chosen = e.name; iu_preview(); return; }
    for (auto& e : g_iu.ents) if (!e.dir && up(e.name).find(".BAT") != std::string::npos) { g_iu.chosen = e.name; iu_preview(); return; }
}
static bool iu_insert(int k) {
    std::string err;
    floppy::set_drive_letter('A');
    if (!floppy::insert(U(g_iu.disks[k]), &err)) { g_iu.note = base_name(g_iu.disks[k]) + L" を読めません: " + W(err); return false; }
    g_iu.disk = k;
    g_iu.cur.clear();
    g_iu.chosen.clear();
    g_iu.preview.clear();
    iu_scan();
    iu_autoselect();
    g_iu.note.clear();
    return true;
}

static void draw_install(HDC dc, int dx, int dy, int dw, int dh) {
    (void)dh;
    double k = dw / 640.0;
    auto X = [&](double v) { return dx + (int)(v * k); };
    auto Y = [&](double v) { return dy + (int)(v * k); };
    auto S = [&](double v) { return (int)(v * k + 0.5); };
    auto box = [&](int x, int y, int w, int h, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c); RECT r = {X(x), Y(y), X(x + w), Y(y + h)}; FillRect(dc, &r, b); DeleteObject(b);
    };
    auto button = [&](int x, int y, int w, int h, const std::wstring& t, bool on, bool enabled = true) {
        box(x, y, w, h, !enabled ? RGB(45, 45, 55) : on ? RGB(70, 120, 200) : RGB(55, 62, 88));
        draw_text(dc, X(x), Y(y + (h - 13) / 2), S(13), enabled ? RGB(255, 255, 255) : RGB(120, 120, 130), t, true, S(w));
    };
    std::wstring title = L"インストールアシスタント ― " + base_name(g_iu.disks[g_iu.disk]);
    if (g_iu.disks.size() > 1) title += L"（" + std::to_wstring(g_iu.disk + 1) + L"/" + std::to_wstring(g_iu.disks.size()) + L" 枚目）";
    draw_text(dc, X(12), Y(8), S(17), RGB(140, 210, 255), title, false, 0);
    if (g_iu.disks.size() > 1) { button(560, 8, 32, 20, L"◀", false, g_iu.disk > 0); button(596, 8, 32, 20, L"▶", false, g_iu.disk + 1 < (int)g_iu.disks.size()); }
    // 左: ファイルの一覧
    std::wstring where = std::wstring(1, (wchar_t)g_iu.fd) + L":\\" + W(g_iu.cur) + (g_iu.cur.empty() ? L"" : L"\\");
    draw_text(dc, X(IU_LIST_X), Y(36), S(13), RGB(255, 230, 150), where, false, 0);
    box(IU_LIST_X, IU_LIST_Y - 2, IU_LIST_W, IU_ROWS * IU_ROW + 4, RGB(20, 22, 32));
    for (int i = 0; i < IU_ROWS; i++) {
        int n = g_iu.scroll + i;
        if (n >= (int)g_iu.ents.size()) break;
        const auto& e = g_iu.ents[n];
        std::string full = g_iu.cur.empty() ? e.name : g_iu.cur + "\\" + e.name;
        bool sel = !e.dir && full == g_iu.chosen;
        int y = IU_LIST_Y + i * IU_ROW;
        if (sel) box(IU_LIST_X, y, IU_LIST_W, IU_ROW, RGB(70, 120, 200));
        COLORREF c = e.dir ? RGB(255, 220, 140) : iu_is_exec(e.name) ? RGB(255, 255, 255) : RGB(160, 165, 180);
        draw_text(dc, X(IU_LIST_X + 6), Y(y + 2), S(13), c, (e.dir ? L"[" + W(e.name) + L"]" : W(e.name)), false, 0);
        if (!e.dir) {
            wchar_t sz[24]; swprintf(sz, 24, L"%u", (unsigned)e.size);
            draw_text(dc, X(IU_LIST_X + 200), Y(y + 2), S(12), RGB(130, 135, 150), sz, false, 0);
        }
    }
    if ((int)g_iu.ents.size() > IU_ROWS) {
        wchar_t sc[48]; swprintf(sc, 48, L"%d-%d / %d（ホイールで送る）", g_iu.scroll + 1, std::min<int>(g_iu.scroll + IU_ROWS, (int)g_iu.ents.size()), (int)g_iu.ents.size());
        draw_text(dc, X(IU_LIST_X), Y(IU_LIST_Y + IU_ROWS * IU_ROW + 4), S(11), RGB(130, 135, 150), sc, false, 0);
    }
    // 右上: 中身
    box(IU_PV_X, IU_PV_Y, IU_PV_W, IU_PV_H, RGB(20, 22, 32));
    draw_text(dc, X(IU_PV_X + 6), Y(IU_PV_Y + 4), S(13), RGB(255, 230, 150),
              g_iu.chosen.empty() ? L"インストーラのファイルを左から選んでください" : L"実行: " + W(g_iu.chosen), false, 0);
    for (size_t i = 0; i < g_iu.preview.size(); i++)
        draw_text(dc, X(IU_PV_X + 6), Y(IU_PV_Y + 24 + (int)i * 12), S(11), RGB(200, 205, 215), g_iu.preview[i], false, 0);
    // 右下: 引数とドライブ
    draw_text(dc, X(IU_PV_X), Y(IU_ARG_Y + 4), S(13), RGB(220, 220, 230), L"引数", false, 0);
    box(IU_ARG_X, IU_ARG_Y, IU_ARG_W, IU_ARG_H, g_iu.args_focus ? RGB(250, 250, 255) : RGB(200, 200, 210));
    std::wstring at = g_iu.args + ((g_iu.args_focus && (GetTickCount() / 500) % 2) ? L"_" : L"");
    draw_text(dc, X(IU_ARG_X + 4), Y(IU_ARG_Y + 4), S(13), RGB(0, 0, 0), at.empty() && !g_iu.args_focus ? L"（クリックして入力）" : at, false, 0);
    auto letters = [&](int y, const wchar_t* label, char cur) {
        draw_text(dc, X(IU_PV_X), Y(y + 4), S(13), RGB(220, 220, 230), label, false, 0);
        for (int i = 0; IU_LETTERS[i]; i++) button(IU_LET_X + i * IU_LET_GAP, y, IU_LET_W, 20, std::wstring(1, (wchar_t)IU_LETTERS[i]), IU_LETTERS[i] == cur);
    };
    letters(IU_FD_Y, L"フロッピー", g_iu.fd);
    letters(IU_HD_Y, L"ハードディスク", g_iu.hd);
    draw_text(dc, X(IU_PV_X), Y(IU_CUR_Y + 4), S(13), RGB(220, 220, 230), L"カレント", false, 0);
    button(IU_LET_X, IU_CUR_Y, 92, 20, L"フロッピー " + std::wstring(1, (wchar_t)g_iu.fd) + L":", g_iu.cur_fd);
    button(IU_LET_X + 96, IU_CUR_Y, 92, 20, L"ハード " + std::wstring(1, (wchar_t)g_iu.hd) + L":", !g_iu.cur_fd);
    draw_text(dc, X(IU_PV_X), Y(IU_WP_Y + 4), S(13), RGB(220, 220, 230), L"書き込み禁止", false, 0);
    button(IU_LET_X, IU_WP_Y, 92, 20, L"する", g_iu.wp);
    button(IU_LET_X + 96, IU_WP_Y, 92, 20, iu_real() ? L"つまみのまま" : L"しない", !g_iu.wp);
    // 下: インストール先と、実行するもの
    draw_text(dc, X(12), Y(312), S(12), RGB(170, 175, 190),
              L"ハードディスク " + std::wstring(1, (wchar_t)g_iu.hd) + L": ＝ " + g_iu.dest, false, 0);
    std::wstring cmd = g_iu.chosen.empty() ? L"" : std::wstring(1, (wchar_t)g_iu.fd) + L":\\" + W(g_iu.chosen) + (g_iu.args.empty() ? L"" : L" " + g_iu.args);
    draw_text(dc, X(12), Y(330), S(13), RGB(255, 255, 255), L"実行するもの: " + cmd + L"　（カレント " + std::wstring(1, (wchar_t)(g_iu.cur_fd ? g_iu.fd : g_iu.hd)) + L":）", false, 0);
    button(IU_RUN_X, IU_BTN_Y, IU_BTN_W, IU_BTN_H, L"実行（Enter）", true, !g_iu.chosen.empty());
    button(IU_CAN_X, IU_BTN_Y, IU_BTN_W, IU_BTN_H, L"やめる（Esc）", false);
    draw_text(dc, X(12), Y(IU_BTN_Y + 2), S(11), RGB(150, 155, 170), iu_real() ? L"ディスクはドライブで入れ替え（自動で読み直します）" : L"ディスクの入れ替えは実行後に F11 の画面で", false, 0);
    if (!g_iu.note.empty()) draw_text(dc, X(12), Y(384), S(13), RGB(255, 120, 120), g_iu.note, false, 0);
}

static void iu_run() {
    if (g_iu.chosen.empty()) { g_iu.note = L"インストーラのファイルを選んでください"; return; }
    g_iu.done = true;
}
// クリック（640x400 の座標）
static void iu_click(double lx, double ly) {
    g_iu.note.clear();
    auto in = [&](int x, int y, int w, int h) { return lx >= x && lx < x + w && ly >= y && ly < y + h; };
    bool was_focus = g_iu.args_focus;
    g_iu.args_focus = in(IU_ARG_X, IU_ARG_Y, IU_ARG_W, IU_ARG_H);
    if (g_iu.args_focus || was_focus) {}
    if (g_iu.disks.size() > 1) {
        if (in(560, 8, 32, 20) && g_iu.disk > 0) { iu_insert(g_iu.disk - 1); return; }
        if (in(596, 8, 32, 20) && g_iu.disk + 1 < (int)g_iu.disks.size()) { iu_insert(g_iu.disk + 1); return; }
    }
    if (in(IU_LIST_X, IU_LIST_Y, IU_LIST_W, IU_ROWS * IU_ROW)) {
        int n = g_iu.scroll + (int)((ly - IU_LIST_Y) / IU_ROW);
        if (n < 0 || n >= (int)g_iu.ents.size()) return;
        auto e = g_iu.ents[n];
        if (e.dir) {   // フォルダはクリックで開く
            if (e.name == "..") { size_t k = g_iu.cur.find_last_of('\\'); g_iu.cur = k == std::string::npos ? "" : g_iu.cur.substr(0, k); }
            else g_iu.cur = g_iu.cur.empty() ? e.name : g_iu.cur + "\\" + e.name;
            iu_scan();
        } else {
            g_iu.chosen = g_iu.cur.empty() ? e.name : g_iu.cur + "\\" + e.name;
            iu_preview();
            DWORD now = GetTickCount();
            if (now - g_iu.last_click < GetDoubleClickTime() && iu_is_exec(e.name)) iu_run();   // ダブルクリックで実行
            g_iu.last_click = now;
        }
        return;
    }
    for (int i = 0; IU_LETTERS[i]; i++) {
        char c = IU_LETTERS[i];
        if (in(IU_LET_X + i * IU_LET_GAP, IU_FD_Y, IU_LET_W, 20)) { if (c == g_iu.hd) g_iu.hd = g_iu.fd; g_iu.fd = c; return; }
        if (in(IU_LET_X + i * IU_LET_GAP, IU_HD_Y, IU_LET_W, 20)) { if (c == g_iu.fd) g_iu.fd = g_iu.hd; g_iu.hd = c; return; }
    }
    if (in(IU_LET_X, IU_CUR_Y, 92, 20)) { g_iu.cur_fd = true; return; }
    if (in(IU_LET_X + 96, IU_CUR_Y, 92, 20)) { g_iu.cur_fd = false; return; }
    if (in(IU_LET_X, IU_WP_Y, 92, 20)) { g_iu.wp = true; floppy::set_force_wprot(true); return; }
    if (in(IU_LET_X + 96, IU_WP_Y, 92, 20)) { g_iu.wp = false; floppy::set_force_wprot(false); return; }
    if (in(IU_RUN_X, IU_BTN_Y, IU_BTN_W, IU_BTN_H)) { iu_run(); return; }
    if (in(IU_CAN_X, IU_BTN_Y, IU_BTN_W, IU_BTN_H)) { g_iu.cancel = true; return; }
}
static void iu_to_logical(int x, int y, double* lx, double* ly) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int dx, dy, dw, dh; view_rect(rc.right, rc.bottom, &dx, &dy, &dw, &dh);
    *lx = dw > 0 ? (x - dx) * 640.0 / dw : 0;
    *ly = dh > 0 ? (y - dy) * 400.0 / dh : 0;
}
// wndproc から: 処理したら true
static bool iu_message(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_iu.active) return false;
    switch (msg) {
    case WM_LBUTTONDOWN: { double lx, ly; iu_to_logical((short)LOWORD(lp), (short)HIWORD(lp), &lx, &ly); iu_click(lx, ly); InvalidateRect(h, nullptr, FALSE); return true; }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -3 : 3;
        int mx = std::max(0, (int)g_iu.ents.size() - IU_ROWS);
        g_iu.scroll = std::max(0, std::min(mx, g_iu.scroll + d));
        InvalidateRect(h, nullptr, FALSE); return true; }
    case WM_CHAR:
        if (g_iu.args_focus) {
            wchar_t c = (wchar_t)wp;
            if (c == 8) { if (!g_iu.args.empty()) g_iu.args.pop_back(); }
            else if (c >= 0x20 && g_iu.args.size() < 120) g_iu.args += c;
            InvalidateRect(h, nullptr, FALSE);
        }
        return true;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { iu_run(); InvalidateRect(h, nullptr, FALSE); return true; }
        if (wp == VK_ESCAPE) { if (g_iu.args_focus) g_iu.args_focus = false; else g_iu.cancel = true; InvalidateRect(h, nullptr, FALSE); return true; }
        if (wp == VK_TAB) { g_iu.args_focus = !g_iu.args_focus; InvalidateRect(h, nullptr, FALSE); return true; }
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) return false;
        return true;
    case WM_KEYUP: case WM_SYSKEYUP: case WM_SYSCHAR:
        return true;
    }
    return false;
}

// 画面を出して、実行かやめるまで待つ。実行なら true
static bool install_ui_run(const std::vector<std::wstring>& disks, const std::wstring& dest) {
    g_iu = InstallUi();
    g_iu.disks = disks;
    g_iu.dest = dest;
    g_iu.active = true;
    floppy::set_force_wprot(true);
    if (!iu_insert(0)) {
        MessageBoxW(nullptr, g_iu.note.c_str(), L"PC98PLAYER", MB_ICONERROR);
        g_iu.active = false;
        return false;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
    MSG msg;
    DWORD blink = GetTickCount();
    while (!g_iu.done && !g_iu.cancel) {
        if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT) == WAIT_TIMEOUT) {
            if (g_iu.args_focus && GetTickCount() - blink > 450) { blink = GetTickCount(); InvalidateRect(g_hwnd, nullptr, FALSE); }
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_iu.cancel = true; PostQuitMessage(0); break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(g_hwnd)) g_iu.cancel = true;
    }
    g_iu.active = false;
    return g_iu.done;
}

// インストーラ（バッチ）が終わったとき: ゲームを起動するファイルを選んで INI を作り、起動し直す。
// true を返したら、この窓は閉じる（新しい PC98PLAYER がゲームを起動している）
static bool install_finished() {
    set_capture(false);
    std::wstring dest = g_iu.dest;
    std::wstring ini = dest + L"\\PC98PLAYER.INI";
    bool has = GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring msg;
    if (!has) {
        if (MessageBoxW(g_hwnd, L"インストーラが終わりました。\n\nゲームを起動するファイルを選んで PC98PLAYER.INI を作りますか？\n（いいえ: 作らずに、この画面をそのまま残します）",
                        L"インストールアシスタント", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
        std::string st = choose_start(dest, guess_start(dest), L"ゲームを起動するファイルを選んでください");
        if (st.empty()) return false;
        write_template_ini(ini, st, g_iu.hd);
        msg = L"PC98PLAYER.INI を作りました（Start=" + W(st) + L"、Drive=" + std::wstring(1, (wchar_t)g_iu.hd) + L"）。\n\n続けてゲームを起動しますか？";
    } else {
        msg = L"インストーラが終わりました。\n\nこのフォルダの PC98PLAYER.INI でゲームを起動しますか？";
    }
    if (MessageBoxW(g_hwnd, msg.c_str(), L"インストールアシスタント", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
    // インストール先をカレントにして、引数なしで PC98PLAYER を起動し直す
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dest.c_str(), &si, &pi)) {
        MessageBoxW(g_hwnd, L"起動できませんでした。インストール先のフォルダで PC98PLAYER を起動してください。", L"インストールアシスタント", MB_ICONWARNING);
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}
