// -----------------------------------------------------------------------------
//  memedit.inc  --  メモリエディタ（main.cpp から #include する）
//
//  INI の MemoryEditor=1、または F11 の画面の「その他」→「M: メモリエディタ」で開く別ウインドウ。
//  ・左: 640KB（00000h〜9FFFFh）の 16 進ダンプ。0.1 秒ごとに読み直し、変わったバイトは赤で示す。
//        クリックで選び、0〜9・A〜F を打つとその場でメモリを書き換える（上位→下位の 4 ビットずつ）。
//  ・右: 絞り込みサーチ。
//        [Search RESET]  640KB を検索バッファに写し、全アドレスを候補にする
//        入力欄          FF → 1 バイト、FFFF → 2 バイト … の 16 進（値として読み、リトルエンディアンで比べる）
//                        s99 / m999 / l99999999 は 10 進で 1 / 2 / 4 バイト（大文字も可）
//                        アドレスは 1 バイトずつずらして調べる（奇数番地も取りこぼさない）
//        (1)BYTE (2)WORD (4)DWORD と > < = != : 検索バッファ（前回の内容）と今のメモリを比べて絞る
//        どの操作の後も、検索バッファは今のメモリで置き換える（次の比較は「前回から」になる）
//  ・候補の一覧をクリックすると、ダンプのその番地へ移る。
// -----------------------------------------------------------------------------

namespace memedit {

static const uint32_t MEM_SIZE = 0xA0000;     // 640KB
static const int PANEL_W0 = 320;               // 右の欄の幅（96dpi のとき）
static double s_k = 1.0;                        // 画面の拡大率（dpi / 96）
static int S(int v) { return (int)(v * s_k + 0.5); }
#define PANEL_W S(PANEL_W0)
static const int ROW_BYTES = 16;

static HWND s_wnd = nullptr, s_dump = nullptr, s_list = nullptr;
static HWND s_addr = nullptr, s_input = nullptr, s_status = nullptr;
static HWND s_rad[3] = {};
static HFONT s_font = nullptr, s_ui = nullptr;
static int s_cw = 8, s_ch = 16;                // 等幅フォントの 1 文字の大きさ
static uint32_t s_top = 0;                     // ダンプの先頭行のアドレス
static uint32_t s_cur = 0;                     // 選んでいるバイト
static int s_nib = 0;                          // 0 = 上位 4 ビットを待つ, 1 = 下位
static uint32_t s_anchor = 0;                  // 範囲指定の起点（s_cur と同じなら範囲なし）
static bool s_dragging = false;
static uint32_t sel_lo() { return std::min(s_anchor, s_cur); }
static uint32_t sel_hi() { return std::max(s_anchor, s_cur); }
static bool has_range() { return s_anchor != s_cur; }
static std::vector<uint8_t> s_prev;            // 前回見たときの内容
static std::vector<uint8_t> s_age;             // 変わってからの残り表示回数（赤く示す時間。約 1 秒）
static std::vector<uint8_t> s_snap;            // 検索バッファ
static std::vector<uint32_t> s_cand;           // 候補のアドレス
static bool s_searched = false;
static int s_list_top = 0;
static int s_list_sel = -1;
static int s_size = 1;                         // 比較の大きさ（1/2/4）
static WNDPROC s_input_proc = nullptr, s_addr_proc = nullptr;

enum { ID_RESET = 101, ID_FIND, ID_GT, ID_LT, ID_EQ, ID_NE, ID_R1, ID_R2, ID_R4, ID_GO, ID_GAMEONLY, ID_PAUSE, ID_CMD_RUN, ID_TIMER = 1 };
static HWND s_pause = nullptr, s_hint = nullptr;                 // 「操作中はゲームを止める」
static HWND s_cmd = nullptr, s_cmd_edit = nullptr, s_cmd_msg = nullptr;   // コマンド入力の窓
static WNDPROC s_cmd_proc = nullptr;
static HWND s_gameonly = nullptr;
static std::vector<DosMemBlock> s_blocks;      // 「ゲームのメモリだけ」のときの対象（Search RESET のときに決める）
static bool s_use_blocks = false;
static bool in_blocks(uint32_t a) {
    for (auto& b : s_blocks) if (a >= b.start && a < b.start + b.len) return true;
    return false;
}

static uint8_t* ram() { return (g_p && g_p->m) ? g_p->m->ram : nullptr; }
static uint32_t rd(const uint8_t* m, uint32_t a, int n) {
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | m[a + i];
    return v;
}
static void set_status(const std::wstring& t) { if (s_status) SetWindowTextW(s_status, t.c_str()); }
static void update_count(const wchar_t* what) {
    if (!s_searched) { set_status(L"Search RESET を押すと、メモリを検索バッファに写して始めます"); return; }
    wchar_t b[64]; swprintf(b, 64, L" 候補 %u 件", (unsigned)s_cand.size());
    set_status(std::wstring(what ? what : L"") + b);
}
static void set_size(int n) {
    s_size = n;
    for (int i = 0; i < 3; i++) SendMessageW(s_rad[i], BM_SETCHECK, (i == 0 && n == 1) || (i == 1 && n == 2) || (i == 2 && n == 4), 0);
}
static void snap_now() {
    uint8_t* m = ram(); if (!m) return;
    s_snap.assign(m, m + MEM_SIZE);
}
static void search_reset() {
    snap_now();
    s_cand.clear();
    s_use_blocks = false;
    std::wstring what = L"検索バッファを初期化しました（640KB 全体）。";
    if (SendMessageW(s_gameonly, BM_GETCHECK, 0, 0) == BST_CHECKED && g_p && g_p->m) {
        // ゲームのメモリだけ: 実行中のプログラムと親たちのメモリブロック（シェル・常駐ドライバ・DOS の領域は除く）
        dos_game_blocks(g_p->m, s_blocks);
        uint32_t total = 0;
        for (auto& b : s_blocks) total += b.len;
        if (!s_blocks.empty()) {
            s_use_blocks = true;
            s_cand.reserve(total);
            for (auto& b : s_blocks) for (uint32_t a = b.start; a < b.start + b.len && a < MEM_SIZE; a++) s_cand.push_back(a);
            std::wstring names;
            for (auto& b : s_blocks) {
                std::wstring n = W(b.name.empty() ? std::string("?") : b.name);
                if (names.find(n) == std::wstring::npos) names += (names.empty() ? L"" : L"・") + n;
            }
            wchar_t b[200]; swprintf(b, 200, L"ゲームのメモリ（%ls、%u ブロック %uKB）で初期化しました。", names.c_str(), (unsigned)s_blocks.size(), total / 1024);
            what = b;
        } else what = L"動いているプログラムが見つからないので 640KB 全体で初期化しました。";
    }
    if (!s_use_blocks) { s_cand.resize(MEM_SIZE); for (uint32_t i = 0; i < MEM_SIZE; i++) s_cand[i] = i; }
    s_searched = true;
    s_list_top = 0; s_list_sel = -1;
    update_count(what.c_str());
    InvalidateRect(s_list, nullptr, FALSE);
    InvalidateRect(s_dump, nullptr, FALSE);
}
// 比較: op 0 '>' 1 '<' 2 '=' 3 '!='
static void compare(int op) {
    uint8_t* m = ram(); if (!m) return;
    if (!s_searched) search_reset();
    int n = s_size;
    std::vector<uint32_t> out; out.reserve(s_cand.size());
    for (uint32_t a : s_cand) {
        if (a + (uint32_t)n > MEM_SIZE) continue;
        uint32_t cur = rd(m, a, n), old = rd(s_snap.data(), a, n);
        bool keep = op == 0 ? cur > old : op == 1 ? cur < old : op == 2 ? cur == old : cur != old;
        if (keep) out.push_back(a);
    }
    s_cand.swap(out);
    snap_now();
    s_list_top = 0; s_list_sel = -1;
    static const wchar_t* nm[] = {L"「増加+」:", L"「減少-」:", L"「不動=」:", L"「変動!=」:"};
    wchar_t w[24]; swprintf(w, 24, L" %d バイト", n);
    update_count((std::wstring(nm[op]) + w + L" →").c_str());
    InvalidateRect(s_list, nullptr, FALSE);
    InvalidateRect(s_dump, nullptr, FALSE);
}
// 入力欄の値: 戻り値 false = 書式の誤り
static bool parse_value(const std::wstring& in, uint32_t* val, int* size, std::wstring* err) {
    std::wstring t;
    for (wchar_t c : in) if (c != L' ' && c != L'\t' && c != L'　') t += c;
    if (t.empty()) { *err = L"値を入力してください"; return false; }
    wchar_t p = (wchar_t)towlower(t[0]);
    if ((p == L's' || p == L'm' || p == L'l') && t.size() >= 2) {
        int n = p == L's' ? 1 : p == L'm' ? 2 : 4;
        uint64_t v = 0;
        for (size_t i = 1; i < t.size(); i++) {
            if (t[i] < L'0' || t[i] > L'9') { *err = L"10 進の数字ではありません: " + t; return false; }
            v = v * 10 + (t[i] - L'0');
            if (v > 0xFFFFFFFFull) break;
        }
        uint64_t mx = n == 1 ? 0xFF : n == 2 ? 0xFFFF : 0xFFFFFFFFull;
        if (v > mx) { wchar_t b[96]; swprintf(b, 96, L"%d バイトに入りません（最大 %llu）", n, (unsigned long long)mx); *err = b; return false; }
        *val = (uint32_t)v; *size = n; return true;
    }
    if (t.size() > 2 && t[0] == L'0' && (t[1] == L'x' || t[1] == L'X')) t = t.substr(2);
    else if (!t.empty() && (t[0] == L'$')) t = t.substr(1);
    else if (t.size() > 1 && (t.back() == L'h' || t.back() == L'H')) t.pop_back();
    if (t.empty() || t.size() > 8) { *err = L"16 進は 8 桁（4 バイト）までです"; return false; }
    uint32_t v = 0;
    for (wchar_t c : t) {
        int d = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'a' && c <= L'f') ? c - L'a' + 10 : (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
        if (d < 0) { *err = L"16 進の数字ではありません: " + t + L"（10 進は s99・m999・l99999 のように頭に s/m/l を付けます）"; return false; }
        v = (v << 4) | (uint32_t)d;
    }
    *val = v; *size = (int)((t.size() + 1) / 2);
    return true;
}
static void value_search() {
    uint8_t* m = ram(); if (!m) return;
    wchar_t buf[64] = L"";
    GetWindowTextW(s_input, buf, 64);
    uint32_t v; int n; std::wstring err;
    if (!parse_value(buf, &v, &n, &err)) { set_status(err); MessageBeep(MB_ICONWARNING); return; }
    if (!s_searched) search_reset();
    std::vector<uint32_t> out; out.reserve(s_cand.size() / 4 + 16);
    for (uint32_t a : s_cand) if (a + (uint32_t)n <= MEM_SIZE && rd(m, a, n) == v) out.push_back(a);
    s_cand.swap(out);
    snap_now();
    if (n == 1 || n == 2 || n == 4) set_size(n);
    s_list_top = 0; s_list_sel = -1;
    wchar_t w[96]; swprintf(w, 96, L"値 %0*Xh（%u, %d バイト）:", n * 2, v, v, n);
    update_count(w);
    InvalidateRect(s_list, nullptr, FALSE);
    InvalidateRect(s_dump, nullptr, FALSE);
}

static void open_command();

// ---- ダンプ ---------------------------------------------------------------
static int dump_rows() { RECT r; GetClientRect(s_dump, &r); return std::max(1, (int)(r.bottom / s_ch) - 1); }
static void ensure_visible() {
    int rows = dump_rows();
    uint32_t row = s_cur / ROW_BYTES * ROW_BYTES;
    if (row < s_top) s_top = row;
    if (row >= s_top + (uint32_t)rows * ROW_BYTES) s_top = row - (uint32_t)(rows - 1) * ROW_BYTES;
    uint32_t maxtop = MEM_SIZE - (uint32_t)rows * ROW_BYTES;
    if (s_top > maxtop) s_top = maxtop;
    SetScrollPos(s_dump, SB_VERT, (int)(s_top / ROW_BYTES), TRUE);
}
static void go_to(uint32_t a) {
    if (a >= MEM_SIZE) a = MEM_SIZE - 1;
    s_cur = a; s_anchor = a; s_nib = 0;
    int rows = dump_rows();
    uint32_t row = a / ROW_BYTES * ROW_BYTES;
    s_top = row >= (uint32_t)(rows / 2) * ROW_BYTES ? row - (uint32_t)(rows / 2) * ROW_BYTES : 0;
    ensure_visible();
    InvalidateRect(s_dump, nullptr, FALSE);
}
static bool is_cand(uint32_t a) {
    if (!s_searched || s_cand.size() > 200000) return false;
    return std::binary_search(s_cand.begin(), s_cand.end(), a);
}
static void paint_dump(HDC dc0, const RECT& rc) {
    HDC dc = CreateCompatibleDC(dc0);
    HBITMAP bm = CreateCompatibleBitmap(dc0, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(dc, bm);
    HBRUSH bg = CreateSolidBrush(RGB(20, 22, 30));
    FillRect(dc, &rc, bg); DeleteObject(bg);
    SelectObject(dc, s_font);
    SetBkMode(dc, OPAQUE);
    uint8_t* m = ram();
    if (m && s_prev.size() != MEM_SIZE) { s_prev.assign(m, m + MEM_SIZE); s_age.assign(MEM_SIZE, 0); }
    if (m) {   // 変わったバイトを数回ぶん赤く残す
        for (uint32_t a = 0; a < MEM_SIZE; a++) {
            if (s_prev[a] != m[a]) { s_prev[a] = m[a]; s_age[a] = 10; }
            else if (s_age[a]) s_age[a]--;
        }
    }
    // 見出し
    SetBkColor(dc, RGB(20, 22, 30)); SetTextColor(dc, RGB(130, 140, 170));
    std::wstring hd = L" 番地    ";
    for (int i = 0; i < ROW_BYTES; i++) { wchar_t b[8]; swprintf(b, 8, L"+%X ", i); hd += b; }
    TextOutW(dc, 0, 0, hd.c_str(), (int)hd.size());
    int rows = dump_rows();
    for (int r = 0; r < rows; r++) {
        uint32_t base = s_top + (uint32_t)r * ROW_BYTES;
        if (base >= MEM_SIZE) break;
        int y = (r + 1) * s_ch;
        wchar_t ab[16]; swprintf(ab, 16, L" %05X", base);
        SetBkColor(dc, RGB(20, 22, 30)); SetTextColor(dc, RGB(150, 200, 255));
        TextOutW(dc, 0, y, ab, 6);
        for (int i = 0; i < ROW_BYTES; i++) {
            uint32_t a = base + (uint32_t)i;
            uint8_t v = m ? m[a] : 0;
            bool changed = m && s_age[a] != 0;
            COLORREF fg = changed ? RGB(255, 90, 90) : RGB(225, 228, 235), bk = RGB(20, 22, 30);
            if (s_use_blocks && !changed && !in_blocks(a)) fg = RGB(95, 98, 110);   // サーチの対象外（ゲーム以外）は暗く
            if (is_cand(a)) bk = RGB(70, 60, 20);
            bool insel = has_range() && a >= sel_lo() && a <= sel_hi();
            if (insel) { bk = RGB(45, 75, 130); if (!changed) fg = RGB(255, 255, 255); }   // 範囲指定
            if (a == s_cur) { bk = RGB(80, 150, 255); fg = RGB(255, 255, 255); }
            wchar_t hb[4]; swprintf(hb, 4, L"%02X", v);
            SetBkColor(dc, bk); SetTextColor(dc, fg);
            TextOutW(dc, (9 + i * 3) * s_cw, y, hb, 2);
            if (a == s_cur && s_nib == 1) {   // 下位の 4 ビットを待っている印
                RECT u = {(9 + i * 3) * s_cw + s_cw, y + s_ch - 2, (9 + i * 3) * s_cw + 2 * s_cw, y + s_ch};
                HBRUSH ub = CreateSolidBrush(RGB(255, 230, 120)); FillRect(dc, &u, ub); DeleteObject(ub);
            }
            // 文字（ASCII と半角カナ）
            wchar_t ch = (v >= 0x20 && v < 0x7F) ? (wchar_t)v : (v >= 0xA1 && v <= 0xDF) ? (wchar_t)(0xFF61 + v - 0xA1) : L'.';
            SetBkColor(dc, a == s_cur ? RGB(80, 150, 255) : insel ? RGB(45, 75, 130) : RGB(20, 22, 30));
            SetTextColor(dc, changed ? RGB(255, 90, 90) : RGB(150, 155, 170));
            TextOutW(dc, (9 + ROW_BYTES * 3 + 1 + i) * s_cw, y, &ch, 1);
        }
    }
    BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bm); DeleteDC(dc);
}
static LRESULT CALLBACK dump_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        paint_dump(dc, rc);
        EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_MOUSEMOVE: {
        if (msg == WM_MOUSEMOVE && !s_dragging) return 0;
        if (msg == WM_LBUTTONDOWN) SetFocus(h);
        int px = (short)LOWORD(lp), py = (short)HIWORD(lp);
        int x = px / s_cw, y = (py >= 0 ? py / s_ch : -1) - 1;
        if (msg == WM_MOUSEMOVE) {   // 範囲指定の途中: 上下にはみ出したら 1 行ずつ送る
            int rows = dump_rows();
            if (y < 0 && s_top >= ROW_BYTES) { s_top -= ROW_BYTES; y = 0; }
            else if (y < 0) y = 0;
            if (y >= rows) { if (s_top + (uint32_t)rows * ROW_BYTES < MEM_SIZE) s_top += ROW_BYTES; y = rows - 1; }
            SetScrollPos(h, SB_VERT, (int)(s_top / ROW_BYTES), TRUE);
            if (x < 9) x = 9;
        } else if (y < 0) return 0;
        int col = -1;
        if (x >= 9 && x < 9 + ROW_BYTES * 3) col = (x - 9) / 3;
        else if (x >= 9 + ROW_BYTES * 3 + 1 && x < 9 + ROW_BYTES * 4 + 1) col = x - (9 + ROW_BYTES * 3 + 1);
        else if (msg == WM_MOUSEMOVE) col = x < 9 + ROW_BYTES * 3 ? 0 : ROW_BYTES - 1;
        if (col >= 0) {
            uint32_t a = s_top + (uint32_t)y * ROW_BYTES + (uint32_t)col; if (a >= MEM_SIZE) a = MEM_SIZE - 1;
            s_cur = a; s_nib = 0;
            // クリック = 選び直し（Shift を押しながらなら範囲を広げる）。押したまま動かすと範囲指定
            if (msg == WM_LBUTTONDOWN) {
                if (!(GetKeyState(VK_SHIFT) & 0x8000)) s_anchor = a;
                s_dragging = true; SetCapture(h);
            }
        }
        if (has_range()) {
            wchar_t b[96]; swprintf(b, 96, L"範囲 %05X〜%05X（%u バイト）: W で範囲に書き込み", sel_lo(), sel_hi(), sel_hi() - sel_lo() + 1);
            set_status(b);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    case WM_LBUTTONUP: if (s_dragging) { s_dragging = false; ReleaseCapture(); } return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3;
        int64_t t = (int64_t)s_top - (int64_t)d * ROW_BYTES;
        int64_t maxtop = (int64_t)MEM_SIZE - (int64_t)dump_rows() * ROW_BYTES;
        if (t < 0) t = 0; if (t > maxtop) t = maxtop;
        s_top = (uint32_t)t;
        SetScrollPos(h, SB_VERT, (int)(s_top / ROW_BYTES), TRUE);
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    case WM_VSCROLL: {
        int rows = dump_rows();
        int64_t t = s_top;
        switch (LOWORD(wp)) {
        case SB_LINEUP: t -= ROW_BYTES; break;
        case SB_LINEDOWN: t += ROW_BYTES; break;
        case SB_PAGEUP: t -= (int64_t)rows * ROW_BYTES; break;
        case SB_PAGEDOWN: t += (int64_t)rows * ROW_BYTES; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO si = {sizeof(si), SIF_TRACKPOS}; GetScrollInfo(h, SB_VERT, &si);
            t = (int64_t)si.nTrackPos * ROW_BYTES; break; }
        case SB_TOP: t = 0; break;
        case SB_BOTTOM: t = MEM_SIZE; break;
        }
        int64_t maxtop = (int64_t)MEM_SIZE - (int64_t)rows * ROW_BYTES;
        if (t < 0) t = 0; if (t > maxtop) t = maxtop;
        s_top = (uint32_t)t;
        SetScrollPos(h, SB_VERT, (int)(s_top / ROW_BYTES), TRUE);
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN: {
        int rows = dump_rows();
        int64_t c = s_cur;
        switch (wp) {
        case VK_LEFT: c -= 1; break;
        case VK_RIGHT: c += 1; break;
        case VK_UP: c -= ROW_BYTES; break;
        case VK_DOWN: c += ROW_BYTES; break;
        case VK_PRIOR: c -= (int64_t)rows * ROW_BYTES; s_top = (uint32_t)std::max<int64_t>(0, (int64_t)s_top - (int64_t)rows * ROW_BYTES); break;
        case VK_NEXT: c += (int64_t)rows * ROW_BYTES; s_top += (uint32_t)rows * ROW_BYTES; break;
        case VK_HOME: c -= c % ROW_BYTES; break;
        case VK_END: c = c - c % ROW_BYTES + ROW_BYTES - 1; break;
        default: return 0;
        }
        if (c < 0) c = 0; if (c >= (int64_t)MEM_SIZE) c = MEM_SIZE - 1;
        s_cur = (uint32_t)c; s_nib = 0;
        if (!(GetKeyState(VK_SHIFT) & 0x8000)) s_anchor = s_cur;   // Shift+カーソルキーで範囲を広げる
        ensure_visible();
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    case WM_CHAR: {
        wchar_t c = (wchar_t)wp;
        if (c == L'w' || c == L'W') { open_command(); return 0; }   // 選んだ番地へのコマンド入力
        int d = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'a' && c <= L'f') ? c - L'a' + 10 : (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
        uint8_t* m = ram();
        if (d < 0 || !m) return 0;
        uint8_t v = m[s_cur];
        v = s_nib == 0 ? (uint8_t)((v & 0x0F) | (d << 4)) : (uint8_t)((v & 0xF0) | d);
        m[s_cur] = v;                 // その場で書き換える
        s_anchor = s_cur;             // 1 バイトずつの書き換えは範囲を解く
        if (s_nib == 0) s_nib = 1;
        else { s_nib = 0; if (s_cur + 1 < MEM_SIZE) s_cur++; s_anchor = s_cur; ensure_visible(); }
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- 候補の一覧 ------------------------------------------------------------
}  // namespace memedit
namespace codeedit { static void append_addr(uint32_t a); }   // codeedit.inc にある
namespace memedit {
static int list_rows() { RECT r; GetClientRect(s_list, &r); return std::max(1, (int)(r.bottom / s_ch)); }
static LRESULT CALLBACK list_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc0 = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(dc0);
        HBITMAP bm = CreateCompatibleBitmap(dc0, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(dc, bm);
        HBRUSH bg = CreateSolidBrush(RGB(20, 22, 30)); FillRect(dc, &rc, bg); DeleteObject(bg);
        SelectObject(dc, s_font); SetBkMode(dc, TRANSPARENT);
        uint8_t* m = ram();
        int rows = list_rows();
        if (!s_searched) {
            SetTextColor(dc, RGB(130, 140, 170));
            TextOutW(dc, 4, 2, L"（候補はまだありません）", 12);
        } else if (s_cand.empty()) {
            SetTextColor(dc, RGB(255, 120, 120));
            TextOutW(dc, 4, 2, L"候補がなくなりました", 10);
        }
        for (int r = 0; r < rows && m; r++) {
            size_t k = (size_t)s_list_top + (size_t)r;
            if (k >= s_cand.size()) break;
            uint32_t a = s_cand[k];
            int n = s_size; if (a + (uint32_t)n > MEM_SIZE) n = 1;
            uint32_t cur = rd(m, a, n), old = rd(s_snap.data(), a, n);
            wchar_t b[96];
            int len = swprintf(b, 96, n == 1 ? L"%05X  %02X  %u" : n == 2 ? L"%05X  %04X  %u" : L"%05X  %08X  %u", a, cur, cur);
            if ((int)k == s_list_sel) { RECT sr = {0, r * s_ch, rc.right, (r + 1) * s_ch}; HBRUSH sb = CreateSolidBrush(RGB(60, 90, 150)); FillRect(dc, &sr, sb); DeleteObject(sb); }
            SetTextColor(dc, cur != old ? RGB(255, 90, 90) : RGB(225, 228, 235));
            TextOutW(dc, 4, r * s_ch, b, len);
        }
        BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob); DeleteObject(bm); DeleteDC(dc);
        EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3;
        int64_t t = (int64_t)s_list_top - d;
        int64_t mx = (int64_t)s_cand.size() - list_rows();
        if (t > mx) t = mx; if (t < 0) t = 0;
        s_list_top = (int)t;
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    case WM_LBUTTONDOWN: {
        int r = (short)HIWORD(lp) / s_ch;
        size_t k = (size_t)s_list_top + (size_t)r;
        if (k < s_cand.size()) { s_list_sel = (int)k; go_to(s_cand[k]); SetFocus(s_dump); }   // そのまま 16 進を打てば書き換えられる
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    case WM_LBUTTONDBLCLK: {   // コードエディタの末尾に「番地 」を書き足す
        int r = (short)HIWORD(lp) / s_ch;
        size_t k = (size_t)s_list_top + (size_t)r;
        if (k < s_cand.size()) { s_list_sel = (int)k; go_to(s_cand[k]); InvalidateRect(h, nullptr, FALSE); codeedit::append_addr(s_cand[k]); }
        return 0; }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 入力欄・番地欄で Enter を押したとき
static LRESULT CALLBACK input_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { value_search(); SendMessageW(h, EM_SETSEL, 0, -1); return 0; }   // 次の値はそのまま打てば置き換わる
    if (msg == WM_CHAR && (wp == L'\r' || wp == L'\n')) return 0;
    if (msg == WM_CHAR && wp == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }   // Ctrl+A
    return CallWindowProcW(s_input_proc, h, msg, wp, lp);
}
static void address_go() {
    wchar_t b[32] = L""; GetWindowTextW(s_addr, b, 32);
    std::wstring t = b;
    uint32_t a = 0;
    size_t colon = t.find(L':');
    auto hex = [](const std::wstring& s, bool* ok) {
        uint32_t v = 0; *ok = !s.empty();
        for (wchar_t c : s) {
            int d = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'a' && c <= L'f') ? c - L'a' + 10 : (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
            if (d < 0) { if (c == L' ') continue; *ok = false; return 0u; }
            v = (v << 4) | (uint32_t)d;
        }
        return v;
    };
    bool ok1 = true, ok2 = true;
    if (colon != std::wstring::npos) a = (hex(t.substr(0, colon), &ok1) << 4) + hex(t.substr(colon + 1), &ok2);
    else a = hex(t, &ok1);
    if (!ok1 || !ok2) { set_status(L"番地は 12345 か 1234:5678 のように 16 進で書きます"); MessageBeep(MB_ICONWARNING); return; }
    if (a >= MEM_SIZE) { set_status(L"番地は 00000〜9FFFF（640KB）の範囲です"); MessageBeep(MB_ICONWARNING); return; }
    go_to(a);
    SetFocus(s_dump);
}
static LRESULT CALLBACK addr_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { address_go(); return 0; }
    if (msg == WM_CHAR && (wp == L'\r' || wp == L'\n')) return 0;
    if (msg == WM_CHAR && wp == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    return CallWindowProcW(s_addr_proc, h, msg, wp, lp);
}

// ---- コマンド入力（ダンプで W） ---------------------------------------------
//  選んでいる番地を起点に、1 行のコマンドを実行する。今あるもの:
//    FF442299               16 進のバイト列（2 桁で 1 バイト）を、選んだ番地から順に書き込む
//    s99 / m999 / l99999999   10 進の値を 1 / 2 / 4 バイト（下位バイトが先）で書き込む（S/M/L も可）
//  今後「範囲書き込み」などを足すときは run_command() の解釈を増やす（戻り値の文言がそのまま結果の表示になる）。
struct CmdResult { bool ok; std::wstring msg; };
// 16 進のバイト列（"FF442299" のように 2 桁で 1 バイト。空白は無視）を解く。16 進でなければ false
static bool parse_hex_bytes(const std::wstring& in, std::vector<uint8_t>* out, std::wstring* err) {
    std::wstring t;
    for (wchar_t c : in) if (c != L' ' && c != L'\t' && c != L'　') t += c;
    out->clear();
    if (t.empty()) { *err = L"値がありません"; return false; }
    for (wchar_t c : t) if (!iswxdigit(c)) { *err = L"16 進の数字ではありません: " + t; return false; }
    if (t.size() & 1) { *err = L"16 進は 2 桁で 1 バイトです（桁数が奇数）: " + t; return false; }
    for (size_t i = 0; i < t.size(); i += 2) out->push_back((uint8_t)wcstoul(t.substr(i, 2).c_str(), nullptr, 16));
    return true;
}
static std::wstring hex_list(const std::vector<uint8_t>& b, size_t max = 16) {
    std::wstring r;
    for (size_t i = 0; i < b.size() && i < max; i++) { wchar_t x[4]; swprintf(x, 4, L"%02X", b[i]); if (i) r += L" "; r += x; }
    if (b.size() > max) r += L" …";
    return r;
}
// 範囲 [lo, hi]（両端を含む）を pattern の繰り返しで埋める。最後の 1 回が範囲に入りきらなければ、入るところまで
static std::wstring fill_range(uint32_t lo, uint32_t hi, const std::vector<uint8_t>& pat) {
    uint8_t* m = ram();
    if (!m || pat.empty()) return L"";
    uint32_t n = hi - lo + 1;
    for (uint32_t i = 0; i < n; i++) m[lo + i] = pat[i % pat.size()];
    wchar_t b[200];
    swprintf(b, 200, L"%05X〜%05X（%u バイト）を %ls の繰り返しで埋めました%ls", lo, hi, n, hex_list(pat, 8).c_str(),
             (n % pat.size()) ? L"（最後の 1 回は入るところまで）" : L"");
    return b;
}
static CmdResult run_command(const std::wstring& line, uint32_t addr) {
    uint8_t* m = ram();
    if (!m) return {false, L"メモリがありません"};
    std::wstring t;
    for (wchar_t c : line) if (c != L' ' && c != L'\t' && c != L'　') t += c;
    if (t.empty()) return {false, L"コマンドを入力してください（例: FF4422, s99, m999, l99999）"};
    wchar_t k = (wchar_t)towlower(t[0]);
    if (has_range()) {   // 範囲指定中: 値（10 進でも 16 進のバイト列でも）の繰り返しで範囲を埋める
        std::vector<uint8_t> pat; std::wstring err;
        if ((k == L's' || k == L'm' || k == L'l') && t.size() >= 2) {
            uint32_t v; int n;
            if (!parse_value(t, &v, &n, &err)) return {false, err};
            for (int i = 0; i < n; i++) pat.push_back((uint8_t)(v >> (8 * i)));
        } else if (!parse_hex_bytes(t, &pat, &err)) return {false, err};
        return {true, fill_range(sel_lo(), sel_hi(), pat)};
    }
    if ((k == L's' || k == L'm' || k == L'l') && t.size() >= 2) {
        uint32_t v; int n; std::wstring err;
        if (!parse_value(t, &v, &n, &err)) return {false, err};
        if (addr + (uint32_t)n > MEM_SIZE) return {false, L"640KB の終わりを越えます"};
        for (int i = 0; i < n; i++) m[addr + i] = (uint8_t)(v >> (8 * i));
        std::wstring bytes;
        for (int i = 0; i < n; i++) { wchar_t b[4]; swprintf(b, 4, L"%02X", (v >> (8 * i)) & 0xFF); bytes += (i ? L" " : L"") + std::wstring(b); }
        wchar_t b[160]; swprintf(b, 160, L"%05X に %u（%0*Xh）を %d バイトで書き込みました: %ls", addr, v, n * 2, v, n, bytes.c_str());
        return {true, b};
    }
    {   // 16 進のバイト列: FF442299 → FF 44 22 99 を選んだ番地から順に書く
        std::vector<uint8_t> bytes; std::wstring err;
        if (parse_hex_bytes(t, &bytes, &err)) {
            if (addr + bytes.size() > MEM_SIZE) return {false, L"640KB の終わりを越えます"};
            for (size_t i = 0; i < bytes.size(); i++) m[addr + i] = bytes[i];
            wchar_t b[200]; swprintf(b, 200, L"%05X から %u バイト書き込みました: %ls", addr, (unsigned)bytes.size(), hex_list(bytes).c_str());
            return {true, b};
        }
        if (iswxdigit(t[0]) && t.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos) return {false, err};
    }
    return {false, L"知らないコマンドです: " + t + L"（使えるのは FF4422 のような 16 進のバイト列と、s99・m999・l99999 の 10 進書き込み）"};
}
static void command_exec() {
    wchar_t b[256] = L""; GetWindowTextW(s_cmd_edit, b, 256);
    CmdResult r = run_command(b, s_cur);
    set_status(r.msg);
    if (r.ok) { ShowWindow(s_cmd, SW_HIDE); SetForegroundWindow(s_wnd); SetFocus(s_dump); InvalidateRect(s_dump, nullptr, FALSE); InvalidateRect(s_list, nullptr, FALSE); }
    else { SetWindowTextW(s_cmd_msg, r.msg.c_str()); MessageBeep(MB_ICONWARNING); SendMessageW(s_cmd_edit, EM_SETSEL, 0, -1); }
}
static LRESULT CALLBACK cmd_edit_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { command_exec(); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { ShowWindow(s_cmd, SW_HIDE); SetForegroundWindow(s_wnd); SetFocus(s_dump); return 0; }
    if (msg == WM_CHAR && (wp == L'\r' || wp == L'\n' || wp == 27)) return 0;
    if (msg == WM_CHAR && wp == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    return CallWindowProcW(s_cmd_proc, h, msg, wp, lp);
}
static LRESULT CALLBACK cmd_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: if (LOWORD(wp) == ID_CMD_RUN) { command_exec(); return 0; } if (LOWORD(wp) == IDCANCEL) { ShowWindow(h, SW_HIDE); SetFocus(s_dump); return 0; } break;
    case WM_CTLCOLORSTATIC: { HDC dc = (HDC)wp; SetBkMode(dc, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_CLOSE: ShowWindow(h, SW_HIDE); SetFocus(s_dump); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void open_command() {
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(s_wnd, GWLP_HINSTANCE);
    if (!s_cmd) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = cmd_wnd_proc; wc.hInstance = inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE); wc.lpszClassName = L"PC98PLAYER_MEMCMD";
        RegisterClassExW(&wc);
        RECT wr = {0, 0, S(460), S(110)};
        AdjustWindowRectEx(&wr, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
        s_cmd = CreateWindowExW(WS_EX_TOOLWINDOW, L"PC98PLAYER_MEMCMD", L"", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, s_wnd, nullptr, inst, nullptr);
        auto mk = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int id, int x, int y, int w, int hh) {
            HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | st, x, y, w, hh, s_cmd, (HMENU)(INT_PTR)id, inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)s_ui, TRUE);
            return c;
        };
        mk(L"STATIC", L"FF4422… = 16 進のバイト列　s99 / m999 / l99999 = 10 進（1/2/4 バイト）", SS_LEFT, 0, S(10), S(8), S(440), S(20));
        s_cmd_edit = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0, S(10), S(32), S(350), S(26));
        mk(L"BUTTON", L"送信", BS_DEFPUSHBUTTON, ID_CMD_RUN, S(368), S(32), S(82), S(26));
        s_cmd_msg = mk(L"STATIC", L"", SS_LEFT, 0, S(10), S(66), S(440), S(36));
        s_cmd_proc = (WNDPROC)SetWindowLongPtrW(s_cmd_edit, GWLP_WNDPROC, (LONG_PTR)cmd_edit_sub);
    }
    wchar_t t[96];
    if (has_range()) swprintf(t, 96, L"コマンド入力 - 範囲 %05X〜%05X（%u バイト）に繰り返し書き込み", sel_lo(), sel_hi(), sel_hi() - sel_lo() + 1);
    else swprintf(t, 96, L"コマンド入力 - 番地 %05X", s_cur);
    SetWindowTextW(s_cmd, t);
    SetWindowTextW(s_cmd_msg, L"Enter で送信、Esc でやめる");
    // ダンプの選んだバイトの近くに出す
    RECT dr; GetWindowRect(s_dump, &dr);
    RECT cr; GetWindowRect(s_cmd, &cr);
    int x = dr.left + 40, y = dr.top + 60;
    SetWindowPos(s_cmd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
    (void)cr;
    ShowWindow(s_cmd, SW_SHOW);
    SetForegroundWindow(s_cmd);
    SetFocus(s_cmd_edit);
    SendMessageW(s_cmd_edit, EM_SETSEL, 0, -1);
}
// 「操作中はゲームを止める」: チェックが入っていて、メモリエディタ（またはコマンド入力）を操作しているとき
static bool pause_requested() {
    if (!s_wnd || !s_pause || !IsWindowVisible(s_wnd)) return false;
    if (SendMessageW(s_pause, BM_GETCHECK, 0, 0) != BST_CHECKED) return false;
    HWND f = GetForegroundWindow();
    return f == s_wnd || (s_cmd && f == s_cmd);
}

// ---- 窓 -------------------------------------------------------------------

static void layout(int w, int h) {
    int px = w - PANEL_W;
    int top = S(40);
    MoveWindow(s_dump, S(8), top, px - S(16), h - top - S(8), TRUE);
    if (s_hint) MoveWindow(s_hint, S(440), S(12), std::max(S(40), px - S(448)), S(22), TRUE);
    HWND c;
    int pw = PANEL_W - S(10);
    c = GetDlgItem(s_wnd, ID_RESET); MoveWindow(c, px, S(8), pw, S(28), TRUE);
    MoveWindow(s_gameonly, px, S(40), pw, S(22), TRUE);
    MoveWindow(s_input, px, S(68), pw - S(74), S(26), TRUE);
    c = GetDlgItem(s_wnd, ID_FIND); MoveWindow(c, px + pw - S(68), S(68), S(68), S(26), TRUE);
    int rw = pw / 3;
    for (int i = 0; i < 3; i++) MoveWindow(s_rad[i], px + i * rw, S(102), rw, S(22), TRUE);
    int bw = (pw - 3 * S(6)) / 4;
    for (int i = 0; i < 4; i++) MoveWindow(GetDlgItem(s_wnd, ID_GT + i), px + i * (bw + S(6)), S(130), bw, S(30), TRUE);
    MoveWindow(s_status, px, S(166), pw, S(58), TRUE);
    MoveWindow(s_list, px, S(228), pw, h - S(236), TRUE);
}
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: layout(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(980); mm->ptMinTrackSize.y = S(420); return 0; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_RESET: search_reset(); return 0;
        case ID_FIND: value_search(); SetFocus(s_input); SendMessageW(s_input, EM_SETSEL, 0, -1); return 0;
        case ID_GT: compare(0); return 0;
        case ID_LT: compare(1); return 0;
        case ID_EQ: compare(2); return 0;
        case ID_NE: compare(3); return 0;
        case ID_R1: s_size = 1; InvalidateRect(s_list, nullptr, FALSE); return 0;
        case ID_R2: s_size = 2; InvalidateRect(s_list, nullptr, FALSE); return 0;
        case ID_R4: s_size = 4; InvalidateRect(s_list, nullptr, FALSE); return 0;
        case ID_GO: address_go(); return 0;
        }
        break;
    case WM_TIMER:
        if (IsWindowVisible(h) && !IsIconic(h)) { InvalidateRect(s_dump, nullptr, FALSE); InvalidateRect(s_list, nullptr, FALSE); }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp; SetTextColor(dc, RGB(30, 30, 40)); SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;   // 閉じても中身（候補）は残す。もう一度開ける
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void create(HINSTANCE inst) {
    if (s_wnd) return;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc; wc.hInstance = inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE); wc.lpszClassName = L"PC98PLAYER_MEMEDIT";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    WNDCLASSEXW dc = {sizeof(dc)};
    dc.lpfnWndProc = dump_proc; dc.hInstance = inst; dc.hCursor = LoadCursor(nullptr, IDC_IBEAM); dc.lpszClassName = L"PC98PLAYER_MEMDUMP";
    RegisterClassExW(&dc);
    WNDCLASSEXW lc = {sizeof(lc)};
    lc.lpfnWndProc = list_proc; lc.style = CS_DBLCLKS; lc.hInstance = inst; lc.hCursor = LoadCursor(nullptr, IDC_HAND); lc.lpszClassName = L"PC98PLAYER_MEMLIST";
    RegisterClassExW(&lc);

    { HDC dc = GetDC(nullptr); s_k = GetDeviceCaps(dc, LOGPIXELSY) / 96.0; ReleaseDC(nullptr, dc); if (s_k < 1.0) s_k = 1.0; }
    s_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"ＭＳ ゴシック");
    s_ui = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, FF_DONTCARE, L"Yu Gothic UI");
    HDC sdc = GetDC(nullptr);
    HGDIOBJ of = SelectObject(sdc, s_font);
    TEXTMETRICW tm; GetTextMetricsW(sdc, &tm);
    s_cw = tm.tmAveCharWidth; s_ch = tm.tmHeight;
    SelectObject(sdc, of); ReleaseDC(nullptr, sdc);

    int dump_w = (9 + ROW_BYTES * 4 + 2) * s_cw + GetSystemMetrics(SM_CXVSCROLL) + S(24);
    {   // 上の段（番地・移動・止める・説明）が収まる幅
        HDC dc = GetDC(nullptr); HGDIOBJ o = SelectObject(dc, s_ui);
        SIZE sz; const wchar_t* t = L"0-9 A-F: 書き換え　ドラッグ: 範囲　W: コマンド";
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        SelectObject(dc, o); ReleaseDC(nullptr, dc);
        dump_w = std::max(dump_w, S(448) + (int)sz.cx + S(16));
    }
    RECT wr = {0, 0, dump_w + PANEL_W, 30 * s_ch + S(56)};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    s_wnd = CreateWindowExW(0, L"PC98PLAYER_MEMEDIT", L"メモリエディタ - PC98PLAYER", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, inst, nullptr);
    auto mk = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int id) {
        HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | st, 0, 0, 10, 10, s_wnd, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)s_ui, TRUE);
        return c;
    };
    HWND lab = mk(L"STATIC", L"番地:", SS_LEFT, 0); MoveWindow(lab, S(8), S(12), S(40), S(22), TRUE);
    s_addr = mk(L"EDIT", L"00000", WS_BORDER | ES_AUTOHSCROLL | ES_UPPERCASE, 0); MoveWindow(s_addr, S(50), S(8), S(110), S(26), TRUE);
    HWND go = mk(L"BUTTON", L"移動", BS_PUSHBUTTON, ID_GO); MoveWindow(go, S(166), S(8), S(60), S(26), TRUE);
    s_pause = mk(L"BUTTON", L"操作中はゲームを止める", BS_AUTOCHECKBOX, ID_PAUSE);
    MoveWindow(s_pause, S(236), S(10), S(196), S(22), TRUE);
    s_hint = mk(L"STATIC", L"0-9 A-F: 書き換え　ドラッグ: 範囲　W: コマンド", SS_LEFT, 0);
    s_dump = CreateWindowExW(WS_EX_CLIENTEDGE, L"PC98PLAYER_MEMDUMP", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
                             0, 0, 10, 10, s_wnd, nullptr, inst, nullptr);
    SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, (int)(MEM_SIZE / ROW_BYTES) - 1, 28, 0};
    SetScrollInfo(s_dump, SB_VERT, &si, FALSE);
    mk(L"BUTTON", L"Search RESET", BS_PUSHBUTTON, ID_RESET);
    s_gameonly = mk(L"BUTTON", L"ゲームのメモリだけ探す", BS_AUTOCHECKBOX, ID_GAMEONLY);
    SendMessageW(s_gameonly, BM_SETCHECK, BST_CHECKED, 0);
    s_input = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0);
    mk(L"BUTTON", L"検索", BS_PUSHBUTTON, ID_FIND);
    s_rad[0] = mk(L"BUTTON", L"(1)BYTE", BS_AUTORADIOBUTTON | WS_GROUP, ID_R1);
    s_rad[1] = mk(L"BUTTON", L"(2)WORD", BS_AUTORADIOBUTTON, ID_R2);
    s_rad[2] = mk(L"BUTTON", L"(4)DWORD", BS_AUTORADIOBUTTON, ID_R4);
    mk(L"BUTTON", L"増加+", BS_PUSHBUTTON, ID_GT);
    mk(L"BUTTON", L"減少-", BS_PUSHBUTTON, ID_LT);
    mk(L"BUTTON", L"不動=", BS_PUSHBUTTON, ID_EQ);
    mk(L"BUTTON", L"変動!=", BS_PUSHBUTTON, ID_NE);
    s_status = mk(L"STATIC", L"", SS_LEFT, 0);
    s_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"PC98PLAYER_MEMLIST", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, s_wnd, nullptr, inst, nullptr);
    s_input_proc = (WNDPROC)SetWindowLongPtrW(s_input, GWLP_WNDPROC, (LONG_PTR)input_sub);
    s_addr_proc = (WNDPROC)SetWindowLongPtrW(s_addr, GWLP_WNDPROC, (LONG_PTR)addr_sub);
    set_size(1);
    update_count(nullptr);
    RECT cr; GetClientRect(s_wnd, &cr); layout(cr.right, cr.bottom);
    SetTimer(s_wnd, ID_TIMER, 100, nullptr);
}
static void show(HINSTANCE inst) {
    create(inst);
    ShowWindow(s_wnd, SW_SHOW);
    SetForegroundWindow(s_wnd);
}
// 再起動などでメモリが入れ替わったとき（候補は新しいメモリでは意味がないので消す）
static void on_reboot() {
    s_cand.clear(); s_snap.clear(); s_searched = false; s_prev.clear(); s_age.clear(); s_blocks.clear(); s_use_blocks = false;
    if (s_wnd) { update_count(nullptr); InvalidateRect(s_list, nullptr, FALSE); InvalidateRect(s_dump, nullptr, FALSE); }
}

}  // namespace memedit
