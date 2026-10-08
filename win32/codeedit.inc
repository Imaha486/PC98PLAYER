// SPDX-License-Identifier: MIT
//  codeedit.inc -- コードエディタ（F11 の画面の「その他」→「C: コードエディタ」、または INI の CodeEditor=1）
//
//  メモリへの書き込みを「コード」として何行も並べておき、まとめて実行する別窓。
//  1 行 = 「番地 接続記号 値」:
//    番地     16 進（1A2B0）または セグメント:オフセット（1A2B:0000）。通常メモリ 640KB の中
//             「開始-終点」（00100-00200）なら、その範囲（両端を含む）を値の繰り返しで埋める
//    接続記号 半角スペース … 「実行」を押した瞬間に 1 回だけ書き込む
//             半角 %       … 「実行」を押してから「停止」を押すまで、毎フレーム書き込み続ける
//    値       FF4422… = 16 進のバイト列（2 桁で 1 バイト、番地から順に）
//             s99 / m999 / l99999 = 10 進の値を 1 / 2 / 4 バイトで（下位バイトが先）
//  ; から行末まではコメント。空行は飛ばす。
//  「実行」を押すたびに、書き込み続けていたコードはいったん全部止めて、今の内容を読み直す
//  （コードを消して「実行」すれば、書き込み続けるコードもなくなる）。下の欄に書き込み続けているコードを表示する。
//  メモリエディタの候補一覧の行をダブルクリックすると、その番地と半角スペースを末尾に書き足す。
//  書いた内容はゲームのフォルダの PC98PLAYER.SAV\CODES.TXT に残し、次に開いたときに戻す。

namespace codeedit {

using memedit::S;
static HWND s_mon = nullptr, s_mon_lbl = nullptr;
static HWND s_wnd = nullptr, s_edit = nullptr, s_status = nullptr, s_pause = nullptr, s_run = nullptr, s_stop = nullptr, s_help = nullptr;
static HFONT s_font = nullptr;
enum { ID_RUN = 201, ID_STOP, ID_PAUSE2 };

struct Code { uint32_t addr; uint32_t len; std::vector<uint8_t> bytes; int line; std::wstring src; };   // len = 書く長さ（範囲なら範囲の長さ、値を繰り返す）
static std::vector<Code> s_active;     // 「%」の行（停止を押すまで毎フレーム書く）
static std::wstring s_loaded_root;     // CODES.TXT を読んだゲームのフォルダ

static void set_status(const std::wstring& t) { if (s_status) SetWindowTextW(s_status, t.c_str()); }

static std::wstring codes_path() {
    if (!g_p) return L"";
    return W(hostfs::join(hostfs::join(g_p->ps.cfg.root, "PC98PLAYER.SAV"), "CODES.TXT"));
}
static std::wstring get_text() {
    int n = GetWindowTextLengthW(s_edit);
    std::wstring t((size_t)n + 1, L'\0');
    GetWindowTextW(s_edit, &t[0], n + 1);
    t.resize((size_t)n);
    return t;
}
static void save_text() {
    if (!s_edit || !g_p) return;
    std::string u = U(get_text());
    hostfs::mkdir(hostfs::join(g_p->ps.cfg.root, "PC98PLAYER.SAV"));
    HANDLE h = CreateFileW(codes_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr; WriteFile(h, u.data(), (DWORD)u.size(), &wr, nullptr); CloseHandle(h);
}
static void load_text() {
    if (!s_edit || !g_p) return;
    s_loaded_root = W(g_p->ps.cfg.root);
    HANDLE h = CreateFileW(codes_path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        SetWindowTextW(s_edit, L"; 例（; から後ろはコメント）\r\n"
                               L"; 34784 3F420F00   ← 「実行」を押したときに 1 回だけ書く（16 進のバイト列）\r\n"
                               L"; 34784%l999999    ← 「停止」を押すまで書き続ける（10 進 4 バイト）\r\n"
                               L"; 00100-00200 m999 ← 範囲を E7 03 E7 03 … で埋める\r\n");
        return;
    }
    DWORD sz = GetFileSize(h, nullptr), rd = 0;
    std::string d(sz, '\0');
    ReadFile(h, &d[0], sz, &rd, nullptr); CloseHandle(h);
    d.resize(rd);
    std::wstring w = W(d);
    // 改行を CRLF にそろえる（EDIT は CRLF でないと改行にならない）
    std::wstring t;
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] == L'\r') continue;
        if (w[i] == L'\n') t += L"\r\n"; else t += w[i];
    }
    SetWindowTextW(s_edit, t.c_str());
}

// 番地: 16 進（〜5 桁）または SSSS:OOOO
static bool parse_addr(const std::wstring& a, uint32_t* out) {
    size_t c = a.find(L':');
    auto hex = [](const std::wstring& s, uint32_t lim, uint32_t* v) {
        if (s.empty() || s.size() > 8) return false;
        for (wchar_t ch : s) if (!iswxdigit(ch)) return false;
        unsigned long x = wcstoul(s.c_str(), nullptr, 16);
        if (x > lim) return false;
        *v = (uint32_t)x; return true;
    };
    if (c != std::wstring::npos) {
        uint32_t sg, of;
        if (!hex(a.substr(0, c), 0xFFFF, &sg) || !hex(a.substr(c + 1), 0xFFFF, &of)) return false;
        *out = (sg << 4) + of;
        return true;
    }
    return hex(a, 0xFFFFF, out);
}

// 1 行を解く。空行・コメントだけなら kind = 0、1 回 = 1、毎フレーム = 2
static bool parse_line(std::wstring line, int* kind, Code* code, std::wstring* err) {
    size_t sc = line.find(L';');
    if (sc != std::wstring::npos) line = line.substr(0, sc);
    for (auto& ch : line) if (ch == L'　' || ch == L'\t') ch = L' ';
    while (!line.empty() && line.back() == L' ') line.pop_back();
    size_t b = line.find_first_not_of(L' ');
    if (b == std::wstring::npos) { *kind = 0; return true; }
    line = line.substr(b);
    // 番地の終わり = 最初のスペースか %（どちらかが接続記号）
    size_t p = line.find_first_of(L" %");
    if (p == std::wstring::npos) { *err = L"接続記号（半角スペースか %）と値がありません"; return false; }
    std::wstring a = line.substr(0, p);
    bool every = line[p] == L'%';
    std::wstring v = line.substr(p + 1);
    // 「34784 % FF」のように記号の前後に空白があっても受け付ける
    size_t q = v.find_first_not_of(L' ');
    if (!every && q != std::wstring::npos && v[q] == L'%') { every = true; v = v.substr(q + 1); }
    q = v.find_first_not_of(L' ');
    if (q == std::wstring::npos) { *err = L"値がありません"; return false; }
    v = v.substr(q);
    uint32_t hi = 0;
    bool range = false;
    {   // 「開始-終点」: 範囲を値の繰り返しで埋める
        size_t dash = a.find(L'-');
        if (dash != std::wstring::npos) {
            range = true;
            if (!parse_addr(a.substr(0, dash), &code->addr)) { *err = L"開始の番地が読めません: " + a.substr(0, dash); return false; }
            if (!parse_addr(a.substr(dash + 1), &hi)) { *err = L"終点の番地が読めません: " + a.substr(dash + 1); return false; }
            if (hi < code->addr) { *err = L"終点が開始より前です: " + a; return false; }
        } else if (!parse_addr(a, &code->addr)) { *err = L"番地が読めません: " + a; return false; }
    }
    code->bytes.clear();
    wchar_t k0 = (wchar_t)towlower(v[0]);
    if ((k0 == L's' || k0 == L'm' || k0 == L'l') && v.size() >= 2) {
        uint32_t val; int n;
        if (!memedit::parse_value(v, &val, &n, err)) return false;
        for (int i = 0; i < n; i++) code->bytes.push_back((uint8_t)(val >> (8 * i)));
    } else if (!memedit::parse_hex_bytes(v, &code->bytes, err)) return false;
    code->len = range ? hi - code->addr + 1 : (uint32_t)code->bytes.size();
    if (code->addr + code->len > memedit::MEM_SIZE) { *err = L"640KB（9FFFFh）の終わりを越えます"; return false; }
    *kind = every ? 2 : 1;
    return true;
}

static void apply(const Code& c) {
    uint8_t* m = memedit::ram();
    if (!m) return;
    for (uint32_t i = 0; i < c.len; i++) m[c.addr + i] = c.bytes[i % c.bytes.size()];
}

// 書き込み続けているコードの一覧（下の欄）
static void update_monitor() {
    if (!s_mon) return;
    SendMessageW(s_mon, WM_SETREDRAW, FALSE, 0);
    SendMessageW(s_mon, LB_RESETCONTENT, 0, 0);
    if (s_active.empty()) SendMessageW(s_mon, LB_ADDSTRING, 0, (LPARAM)L"（なし）");
    for (auto& c : s_active) {
        wchar_t b[64];
        if (c.len == c.bytes.size()) swprintf(b, 64, L"%4d 行目  ", c.line);
        else swprintf(b, 64, L"%4d 行目  [%05X-%05X]  ", c.line, c.addr, c.addr + c.len - 1);
        std::wstring t = b + c.src;
        SendMessageW(s_mon, LB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    SendMessageW(s_mon, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_mon, nullptr, TRUE);
    std::wstring l = L"書き込み続けているコード: " + std::to_wstring(s_active.size()) + L" 行（「停止」で全部止まる）";
    SetWindowTextW(s_mon_lbl, l.c_str());
}

static void update_buttons() {
    update_monitor();
    if (!s_stop) return;
    EnableWindow(s_stop, !s_active.empty());
    std::wstring t = L"コードエディタ - PC98PLAYER";
    if (!s_active.empty()) t += L"  [書き込み続けています: " + std::to_wstring(s_active.size()) + L" 行]";
    SetWindowTextW(s_wnd, t.c_str());
}

static void run() {
    // 「実行」のたびに、書き込み続けていたコードはいったん全部止めて、今の内容だけで作り直す
    size_t was = s_active.size();
    s_active.clear();
    update_buttons();
    std::wstring stopped = was ? L"（前に書き込み続けていた " + std::to_wstring(was) + L" 行は止めました）" : L"";
    std::wstring text = get_text();
    std::vector<Code> once, every;
    int ln = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        ln++;
        int kind = 0; Code c; std::wstring err;
        if (!parse_line(line, &kind, &c, &err)) {
            set_status(std::to_wstring(ln) + L" 行目: " + err + L"（何も書き込んでいません）" + stopped);
            MessageBeep(MB_ICONWARNING);
            // その行を選んで見せる
            int st = (int)SendMessageW(s_edit, EM_LINEINDEX, (WPARAM)(ln - 1), 0);
            if (st >= 0) SendMessageW(s_edit, EM_SETSEL, (WPARAM)st, (LPARAM)(st + (int)line.size()));
            SetFocus(s_edit);
            return;
        }
        c.line = ln;
        {   // 一覧に出す元の行（コメントと前後の空白を除く）
            std::wstring t = line; size_t sc = t.find(L';'); if (sc != std::wstring::npos) t = t.substr(0, sc);
            size_t a = t.find_first_not_of(L" \t　"), z = t.find_last_not_of(L" \t　");
            c.src = a == std::wstring::npos ? L"" : t.substr(a, z - a + 1);
        }
        if (kind == 1) once.push_back(c);
        else if (kind == 2) every.push_back(c);
        if (e == std::wstring::npos) break;
        pos = e + 1;
    }
    if (once.empty() && every.empty()) { save_text(); set_status(L"実行するコードがありません" + stopped); return; }
    for (auto& c : once) apply(c);
    s_active = every;
    for (auto& c : s_active) apply(c);
    save_text();
    wchar_t b[160];
    swprintf(b, 160, L"1 回だけの書き込み %u 行を実行。書き込み続ける行: %u 行", (unsigned)once.size(), (unsigned)every.size());
    set_status(b + (was ? L"（前の " + std::to_wstring(was) + L" 行は止めて読み直し）" : std::wstring()));
    update_buttons();
}
static void stop() {
    if (s_active.empty()) return;
    s_active.clear();
    set_status(L"書き込み続けていたコードを止めました");
    update_buttons();
}
// 毎フレーム、エミュレーションを 1 フレーム進める前に呼ぶ
static void tick() { for (auto& c : s_active) apply(c); }

static bool pause_requested() {
    if (!s_wnd || !s_pause || !IsWindowVisible(s_wnd)) return false;
    if (SendMessageW(s_pause, BM_GETCHECK, 0, 0) != BST_CHECKED) return false;
    return GetForegroundWindow() == s_wnd;
}

static void layout(int w, int h) {
    MoveWindow(s_run, S(8), S(8), S(110), S(28), TRUE);
    MoveWindow(s_stop, S(124), S(8), S(110), S(28), TRUE);
    MoveWindow(s_pause, S(246), S(11), S(200), S(22), TRUE);
    MoveWindow(s_help, S(8), S(42), w - S(16), S(76), TRUE);
    int mh = S(92);   // 書き込み続けているコードの欄
    int eh = h - S(122) - S(48) - mh - S(24);
    if (eh < S(60)) eh = S(60);
    MoveWindow(s_edit, S(8), S(122), w - S(16), eh, TRUE);
    MoveWindow(s_mon_lbl, S(8), S(122) + eh + S(4), w - S(16), S(20), TRUE);
    MoveWindow(s_mon, S(8), S(122) + eh + S(24), w - S(16), mh, TRUE);
    MoveWindow(s_status, S(8), h - S(42), w - S(16), S(36), TRUE);
}
static WNDPROC s_edit_proc = nullptr;
static LRESULT CALLBACK edit_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CHAR && wp == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }   // Ctrl+A
    if (msg == WM_KEYDOWN && wp == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) { run(); return 0; }   // Ctrl+Enter = 実行
    if (msg == WM_CHAR && wp == L'\n') return 0;
    return CallWindowProcW(s_edit_proc, h, msg, wp, lp);
}
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: layout(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(480); mm->ptMinTrackSize.y = S(380); return 0; }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_RUN) { run(); return 0; }
        if (LOWORD(wp) == ID_STOP) { stop(); return 0; }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp; SetTextColor(dc, RGB(30, 30, 40)); SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_CLOSE: save_text(); ShowWindow(h, SW_HIDE); return 0;   // 閉じても書き込み続ける行はそのまま（止めるのは「停止」）
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void create(HINSTANCE inst) {
    if (s_wnd) return;
    if (!memedit::s_ui) {   // フォントと拡大率はメモリエディタと共通（まだ開いていなければ作る）
        HDC dc = GetDC(nullptr); memedit::s_k = GetDeviceCaps(dc, LOGPIXELSY) / 96.0; ReleaseDC(nullptr, dc);
        if (memedit::s_k < 1.0) memedit::s_k = 1.0;
        memedit::s_ui = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, FF_DONTCARE, L"Yu Gothic UI");
    }
    s_font = CreateFontW(-S(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"ＭＳ ゴシック");
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc; wc.hInstance = inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE); wc.lpszClassName = L"PC98PLAYER_CODEEDIT";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    RECT wr = {0, 0, S(560), S(540)};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    s_wnd = CreateWindowExW(0, L"PC98PLAYER_CODEEDIT", L"コードエディタ - PC98PLAYER", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, inst, nullptr);
    auto mk = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, t, WS_CHILD | WS_VISIBLE | st, 0, 0, 10, 10, s_wnd, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)memedit::s_ui, TRUE);
        return c;
    };
    s_run = mk(L"BUTTON", L"実行", BS_PUSHBUTTON, ID_RUN);
    s_stop = mk(L"BUTTON", L"停止", BS_PUSHBUTTON, ID_STOP);
    s_pause = mk(L"BUTTON", L"操作中はゲームを止める", BS_AUTOCHECKBOX, ID_PAUSE2);
    s_help = mk(L"STATIC", L"1 行に「番地 値」（スペース = 実行したときに 1 回）または「番地%値」（停止するまで書き続ける）。番地を「開始-終点」にすると範囲を値の繰り返しで埋める。"
                           L"値は FF4422（16 進のバイト列）か s99 / m999 / l99999（10 進）。; から後ろはコメント。Ctrl+Enter でも実行", SS_LEFT, 0);
    s_edit = mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN, 0);
    SendMessageW(s_edit, WM_SETFONT, (WPARAM)s_font, TRUE);
    SendMessageW(s_edit, EM_SETLIMITTEXT, 256 * 1024, 0);
    s_status = mk(L"STATIC", L"", SS_LEFT, 0);
    s_mon_lbl = mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0);
    s_mon = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, 0);
    SendMessageW(s_mon, WM_SETFONT, (WPARAM)s_font, TRUE);
    s_edit_proc = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)edit_sub);
    load_text();
    set_status(L"「実行」で書き込みます");
    update_buttons();
    RECT cr; GetClientRect(s_wnd, &cr); layout(cr.right, cr.bottom);
}
static void show(HINSTANCE inst) {
    create(inst);
    if (g_p && W(g_p->ps.cfg.root) != s_loaded_root) load_text();   // INI を読み直してゲームのフォルダが変わった
    ShowWindow(s_wnd, SW_SHOW);
    SetForegroundWindow(s_wnd);
    SetFocus(s_edit);
}
// メモリエディタの候補一覧でダブルクリック: 末尾に「番地 」を書き足し、続けて値を打てるようにする
static void append_addr(uint32_t a) {
    create(GetModuleHandleW(nullptr));
    if (g_p && W(g_p->ps.cfg.root) != s_loaded_root) load_text();
    std::wstring t = get_text();
    // 最後の行に何か書いてあれば改行してから
    size_t nl = t.find_last_of(L'\n');
    std::wstring last = nl == std::wstring::npos ? t : t.substr(nl + 1);
    bool blank = last.find_first_not_of(L" \t\r　") == std::wstring::npos;
    wchar_t b[32]; swprintf(b, 32, L"%05X ", a);
    std::wstring add = (blank ? L"" : L"\r\n") + std::wstring(b);
    if (blank && !last.empty()) {   // 空白だけの最後の行は消して書く
        t.resize(nl == std::wstring::npos ? 0 : nl + 1);
        SetWindowTextW(s_edit, t.c_str());
    }
    int n = GetWindowTextLengthW(s_edit);
    SendMessageW(s_edit, EM_SETSEL, (WPARAM)n, (LPARAM)n);
    SendMessageW(s_edit, EM_REPLACESEL, TRUE, (LPARAM)add.c_str());
    SendMessageW(s_edit, EM_SCROLLCARET, 0, 0);
    if (!IsWindowVisible(s_wnd)) ShowWindow(s_wnd, SW_SHOWNOACTIVATE);
    set_status(std::wstring(L"番地 ") + b + L"を書き足しました。続けて値を打って「実行」（Ctrl+Enter）");
    SetForegroundWindow(s_wnd);
    SetFocus(s_edit);
}

// プログラム再起動: 書き込み続けるコードは止める（起動の途中のメモリを書き換えて止まらないように）
static void on_reboot() {
    if (!s_active.empty()) { s_active.clear(); set_status(L"プログラム再起動したので、書き込み続けていたコードを止めました"); update_buttons(); }
}

}  // namespace codeedit
