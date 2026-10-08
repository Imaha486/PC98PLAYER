// -----------------------------------------------------------------------------
//  settings.inc  --  画面＆サウンド設定（main.cpp から #include する）
//
//  F11 の画面の「その他」→「S: 画面＆サウンド設定」で開く別ウインドウ。
//  INI（PC98PLAYER.INI）の主な項目を画面の部品で変え、INI そのものを書き換えて保存する。
//  ・すぐ反映できる項目（音量・倍率・なめらか・マウスの速さなど）は、変えたその場で効く（音量は聞きながら調整できる）。
//    「キャンセル」で閉じると元に戻す。
//  ・＊の付いた項目（音源ボード・MIDI・CPU の速さ・フォントなど）は、プログラム再起動で反映する。
//    保存したときにそれらが変わっていれば、今すぐ再起動するかを尋ねる。
//  ・INI の書き換えは ini_rewrite.h（値だけを置き換え、コメントや並びは残す）。文字コードは元のまま
//    （Shift_JIS か UTF-8。Shift_JIS で書けない文字が入るときだけ UTF-8 にする）。
// -----------------------------------------------------------------------------

namespace settingsdlg {

static double s_k = 1.0;
static int S(int v) { return (int)(v * s_k + 0.5); }
static HFONT s_ui = nullptr, s_bold = nullptr;
static HWND s_midi_hint = nullptr;
static bool s_guard = false;          // 部品を中から書き換えている間は通知を無視する

enum Kind { K_CHECK, K_LIST, K_EDITLIST, K_NUM, K_VOL };
enum Apply { A_LIVE, A_RESTART, A_START };   // すぐ反映 / 再起動で反映 / 次に起動したときから
struct Opt { std::wstring val, label; };
struct Field {
    const wchar_t* key;
    Kind kind;
    const wchar_t* label;
    std::wstring def;
    Apply ap;
    std::vector<Opt> opts;
    int lo = 0, hi = 1000;
    const wchar_t* unit = L"";
    HWND ctl = nullptr, ctl2 = nullptr;   // ctl2: 音量のスクロールバー
    std::wstring init, cur;
};
static std::vector<Field> s_f;
enum { ID_SAVE = 1, ID_CANCEL = 2, ID_DEFAULT = 3, ID_FIELD = 100 };

static Field* find(const wchar_t* key) {
    for (auto& f : s_f) if (lstrcmpiW(f.key, key) == 0) return &f;
    return nullptr;
}
static int ival(const std::wstring& s, int def) {
    std::wstring t = s;
    while (!t.empty() && (t[0] == L' ' || t[0] == L'x' || t[0] == L'X')) t.erase(0, 1);
    if (t.empty()) return def;
    return _wtoi(t.c_str());
}

// ---- 項目の一覧 ---------------------------------------------------------------------
static int CALLBACK font_enum(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
    auto* v = (std::vector<std::wstring>*)lp;
    std::wstring n = lf->lfFaceName;
    if (n.empty() || n[0] == L'@') return 1;
    for (auto& x : *v) if (x == n) return 1;
    v->push_back(n);
    return 1;
}
static void build_fields() {
    s_f.clear();
    auto num_opts = [](std::initializer_list<int> vs, const wchar_t* suffix) {
        std::vector<Opt> o;
        for (int v : vs) o.push_back({std::to_wstring(v), std::to_wstring(v) + suffix});
        return o;
    };
    // 画面
    {
        std::vector<Opt> sc;
        for (int i = 1; i <= 6; i++) sc.push_back({std::to_wstring(i), std::to_wstring(i) + L" 倍（" + std::to_wstring(640 * i) + L"x" + std::to_wstring(400 * i) + L"）"});
        s_f.push_back({L"Scale", K_LIST, L"表示倍率", L"2", A_LIVE, sc});
    }
    s_f.push_back({L"Smooth", K_CHECK, L"拡大したときになめらかにする", L"0", A_LIVE});
    s_f.push_back({L"FullScreen", K_CHECK, L"全画面で起動する（Alt+Enter でも切替）", L"0", A_START});
    {
        std::vector<std::wstring> names;
        LOGFONTW lf = {}; lf.lfCharSet = SHIFTJIS_CHARSET;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(dc, &lf, font_enum, (LPARAM)&names, 0);
        ReleaseDC(nullptr, dc);
        std::sort(names.begin(), names.end());
        std::vector<Opt> fo;
        for (auto& n : names) fo.push_back({n, n});
        s_f.push_back({L"Font", K_EDITLIST, L"漢字のフォント", L"ＭＳ ゴシック", A_RESTART, fo});
    }
    s_f.push_back({L"FontShift", K_CHECK, L"半角文字を 1 ドット右へ寄せる（実機の ROM 風）", L"1", A_RESTART});
    s_f.push_back({L"GDCClock", K_LIST, L"GDC クロック", L"2.5", A_RESTART,
                   {{L"2.5", L"2.5MHz（DIP SW 2-8 OFF）"}, {L"5", L"5MHz（DIP SW 2-8 ON）"}}});
    s_f.push_back({L"KanjiJIS", K_LIST, L"漢字の字形", L"78", A_RESTART, {{L"78", L"JIS78（当時の PC-98）"}, {L"83", L"JIS83"}}});
    // 動作
    s_f.push_back({L"CpuMHz", K_EDITLIST, L"CPU の速さ", L"16", A_RESTART, num_opts({8, 10, 12, 16, 20, 25, 33, 40, 66, 75, 100, 133}, L""), 1, 1000, L"MHz"});
    s_f.push_back({L"MouseSpeed", K_NUM, L"マウスの速さ", L"100", A_LIVE, {}, 10, 1000, L"%"});
    s_f.push_back({L"KeyRepeat", K_CHECK, L"キーを押し続けたら繰り返す（キーリピート）", L"1", A_LIVE});
    s_f.push_back({L"PauseInactive", K_CHECK, L"ほかの窓を使っている間は一時停止する", L"0", A_LIVE});
    s_f.push_back({L"ExitOnEnd", K_CHECK, L"ゲームが終わったら窓を閉じる", L"1", A_LIVE});
    // サウンド
    s_f.push_back({L"SoundBoard", K_LIST, L"音源ボード", L"86", A_RESTART,
                   {{L"86", L"PC-9801-86（OPNA・PCM）"}, {L"26", L"PC-9801-26K（OPN）"}, {L"0", L"なし"}}});
    s_f.push_back({L"SoundIRQ", K_LIST, L"音源ボードの割込み", L"12", A_RESTART, num_opts({3, 10, 12, 13}, L"")});
    s_f.push_back({L"SoundBIOS", K_CHECK, L"サウンド BIOS を載せる（MIMPI など）", L"0", A_RESTART});
    s_f.push_back({L"Sound", K_CHECK, L"FM 音源を鳴らす", L"1", A_RESTART});
    s_f.push_back({L"SampleRate", K_LIST, L"サンプリング周波数", L"44100", A_RESTART, num_opts({22050, 44100, 48000}, L" Hz")});
    // MIDI
    s_f.push_back({L"MIDI", K_CHECK, L"MIDI を使う（MPU-PC98II を載せる）", L"1", A_RESTART});
    {
        std::vector<Opt> md = {{L"-1", L"既定の出力（Microsoft MIDI マッパー）"}};
        UINT n = midiOutGetNumDevs();
        for (UINT i = 0; i < n && i < 32; i++) {
            MIDIOUTCAPSW caps = {};
            if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) md.push_back({std::to_wstring(i), std::to_wstring(i) + L": " + caps.szPname});
        }
        s_f.push_back({L"MidiDevice", K_LIST, L"MIDI の出力先", L"-1", A_LIVE, md});
    }
    s_f.push_back({L"MidiIRQ", K_LIST, L"MIDI の割込み", L"6", A_RESTART, num_opts({3, 5, 6, 10, 12, 13}, L"")});
    s_f.push_back({L"MidiSpeedFix", K_NUM, L"MIDI の演奏速度", L"100", A_LIVE, {}, 10, 1000, L"%"});
    // 音量
    s_f.push_back({L"Volume", K_VOL, L"全体", L"100", A_LIVE, {}, 0, 1000, L"%"});
    s_f.push_back({L"FMVolume", K_VOL, L"FM 音源", L"100", A_LIVE, {}, 0, 1000, L"%"});
    s_f.push_back({L"SSGVolume", K_VOL, L"SSG（PSG）", L"100", A_LIVE, {}, 0, 1000, L"%"});
    s_f.push_back({L"PCMVolume", K_VOL, L"PCM（86 音源）", L"100", A_LIVE, {}, 0, 1000, L"%"});
    s_f.push_back({L"BeepVolume", K_VOL, L"ビープ", L"50", A_LIVE, {}, 0, 1000, L"%"});
}

// INI の今の値（無ければ既定）。チェックと数は読み方（YES / x2 など）をそろえる
static void read_ini_values() {
    Ini ini;
    load_ini_any(g_ini_path, ini);
    for (auto& f : s_f) {
        std::string k = "PC98PLAYER." + U(f.key);
        std::string raw = ini.get(k, "");
        if (raw.empty() && lstrcmpiW(f.key, L"CpuMHz") == 0) raw = ini.get("PC98PLAYER.CPUCLOCK", "");
        std::wstring v;
        if (f.kind == K_CHECK) {
            int d = _wtoi(f.def.c_str());
            Ini one; one.kv["X.K"] = raw;
            v = (raw.empty() ? d : one.geti("X.K", d)) ? L"1" : L"0";
        } else if (f.kind == K_EDITLIST && lstrcmpiW(f.key, L"Font") == 0) {
            v = raw.empty() ? f.def : W(raw);
        } else {
            Ini one; one.kv["X.K"] = raw;
            v = raw.empty() ? f.def : std::to_wstring(one.geti("X.K", _wtoi(f.def.c_str())));
        }
        if (lstrcmpiW(f.key, L"Scale") == 0) { int s = ival(v, 2); v = std::to_wstring(s < 1 ? 1 : s > 8 ? 8 : s); }
        if (lstrcmpiW(f.key, L"GDCClock") == 0) {
            std::wstring u = W(raw); for (auto& c : u) c = (wchar_t)towupper(c);
            v = (u == L"5" || u == L"5MHZ" || u == L"5.0") ? L"5" : L"2.5";
        }
        if (lstrcmpiW(f.key, L"KanjiJIS") == 0) v = ival(v, 78) == 83 ? L"83" : L"78";
        f.init = f.cur = v;
        if (f.kind == K_LIST) {   // 一覧に無い値（INI に手で書いたもの）も選べるように足す
            bool hit = false;
            for (auto& o : f.opts) if (o.val == v) hit = true;
            if (!hit) f.opts.push_back({v, v + L"（INI の値）"});
        }
    }
}

// ---- すぐ反映する項目 -----------------------------------------------------------------
static void apply_live(const Field& f) {
    if (!g_p || f.ap != A_LIVE) return;
    PlayerSettings& ps = g_p->ps;
    int v = ival(f.cur, _wtoi(f.def.c_str()));
    auto clampv = [](int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; };
    std::wstring k = f.key;
    if (k == L"Scale") {
        ps.scale = clampv(v, 1, 8);
        if (!g_full) {
            int ww, wh; window_size_for_scale(ps.scale, &ww, &wh);
            ShowWindow(g_hwnd, SW_RESTORE);
            SetWindowPos(g_hwnd, nullptr, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    else if (k == L"Smooth") ps.smooth = v != 0;
    else if (k == L"MouseSpeed") g_mouse_speed = clampv(v, 1, 1000);
    else if (k == L"KeyRepeat") { ps.cfg.key_repeat = v; if (g_p->m) g_p->m->cfg.key_repeat = v; }
    else if (k == L"PauseInactive") g_pause_inactive = v != 0;
    else if (k == L"ExitOnEnd") ps.exit_on_end = v != 0;
    else if (k == L"MidiSpeedFix") { int s = clampv(v, 10, 1000); ps.cfg.midi_speed = s; if (g_p->m) g_p->m->cfg.midi_speed = s; }
    else if (k == L"MidiDevice") {
        if (ps.midi_device != v) {
            ps.midi_device = v;
            if (g_midi) {   // MIDI を使っているときだけ、出口をつなぎ直す
                midi_all_off(); midiOutClose(g_midi); g_midi = nullptr;
                UINT dev = v < 0 ? MIDI_MAPPER : (UINT)v;
                if (midiOutOpen(&g_midi, dev, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) g_midi = nullptr;
            }
        }
    }
    else if (k == L"Volume") ps.volume = clampv(v, 0, 1000);
    else if (k == L"PCMVolume") ps.pcm_volume = clampv(v, 0, 1000);
    else if (k == L"BeepVolume") ps.beep_volume = clampv(v, 0, 1000);
    else if (k == L"FMVolume" || k == L"SSGVolume") {
        if (k == L"FMVolume") ps.fm_volume = clampv(v, 0, 1000); else ps.ssg_volume = clampv(v, 0, 1000);
        if (g_p->opna) g_p->opna->set_balance(1.0 * ps.fm_volume / 100.0, 0.20 * ps.ssg_volume / 100.0);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---- 部品 ⇔ 値 ----------------------------------------------------------------------
static void update_midi_hint() {
    Field* m = find(L"MIDI");
    if (!s_midi_hint || !m) return;
    SetWindowTextW(s_midi_hint, m->cur == L"1"
        ? L"ゲーム側の音源設定も MIDI（MPU-401 / MPU-PC98）にしてください。"
        : L"OFF のままだと、ゲームで MIDI を選んでも鳴りません（MPU が無いので止まるソフトもあります）。");
    InvalidateRect(s_midi_hint, nullptr, TRUE);
}
static void set_ctl(Field& f) {
    s_guard = true;
    if (f.kind == K_CHECK) SendMessageW(f.ctl, BM_SETCHECK, f.cur == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    else if (f.kind == K_LIST) {
        int sel = -1;
        for (size_t i = 0; i < f.opts.size(); i++) if (f.opts[i].val == f.cur) sel = (int)i;
        SendMessageW(f.ctl, CB_SETCURSEL, (WPARAM)sel, 0);
    } else if (f.kind == K_EDITLIST || f.kind == K_NUM) SetWindowTextW(f.ctl, f.cur.c_str());
    else if (f.kind == K_VOL) {
        SetWindowTextW(f.ctl, f.cur.c_str());
        SetScrollPos(f.ctl2, SB_CTL, std::min(ival(f.cur, 0), 300), TRUE);
    }
    s_guard = false;
}
static std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return inirw::trim(s);
}
// 部品が変わった
static void on_changed(Field& f, bool from_scroll = false) {
    if (s_guard) return;
    if (f.kind == K_CHECK) f.cur = SendMessageW(f.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0";
    else if (f.kind == K_LIST) {
        int i = (int)SendMessageW(f.ctl, CB_GETCURSEL, 0, 0);
        if (i >= 0 && i < (int)f.opts.size()) f.cur = f.opts[i].val;
    } else if (f.kind == K_EDITLIST) {
        // 一覧から選んだときは、まだ欄の文字が変わっていないので選んだ項目から取る
        int i = (int)SendMessageW(f.ctl, CB_GETCURSEL, 0, 0);
        std::wstring t = get_text(f.ctl);
        if (i >= 0 && i < (int)f.opts.size() && from_scroll) t = f.opts[i].val;
        f.cur = t;
    } else if (f.kind == K_NUM) f.cur = get_text(f.ctl);
    else if (f.kind == K_VOL) {
        if (from_scroll) {
            f.cur = std::to_wstring(GetScrollPos(f.ctl2, SB_CTL));
            s_guard = true; SetWindowTextW(f.ctl, f.cur.c_str()); s_guard = false;
        } else {
            f.cur = get_text(f.ctl);
            int v = ival(f.cur, 0);
            s_guard = true; SetScrollPos(f.ctl2, SB_CTL, std::min(std::max(v, 0), 300), TRUE); s_guard = false;
        }
    }
    if (f.kind == K_NUM || f.kind == K_VOL) {   // 空や範囲外は、すぐ反映するときだけ丸める（欄の文字はそのまま）
        if (f.cur.empty()) return;
    }
    apply_live(f);
    if (lstrcmpiW(f.key, L"MIDI") == 0) update_midi_hint();
}

// 保存する値に直す（数は範囲に丸める）。空なら既定
static std::wstring final_value(const Field& f) {
    if (f.kind == K_NUM || f.kind == K_VOL || (f.kind == K_EDITLIST && lstrcmpiW(f.key, L"CpuMHz") == 0)) {
        int v = ival(f.cur, _wtoi(f.def.c_str()));
        v = v < f.lo ? f.lo : v > f.hi ? f.hi : v;
        return std::to_wstring(v);
    }
    if (f.cur.empty()) return f.def;
    return f.cur;
}

// ---- INI に書く ---------------------------------------------------------------------
static bool write_ini(const std::vector<std::pair<std::wstring, std::wstring>>& sets, std::wstring* err) {
    HANDLE h = CreateFileW(g_ini_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string data;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(h, nullptr), rd = 0;
        data.resize(sz);
        if (sz) ReadFile(h, &data[0], sz, &rd, nullptr);
        CloseHandle(h);
    }
    bool bom = data.size() >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF;
    if (bom) data.erase(0, 3);
    bool ascii = true;
    for (char c : data) if ((uint8_t)c >= 0x80) { ascii = false; break; }
    bool utf8 = bom || (!ascii && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.c_str(), (int)data.size(), nullptr, 0) > 0);
    UINT cp = utf8 ? CP_UTF8 : 932;
    std::wstring text;
    if (!data.empty()) {
        int n = MultiByteToWideChar(cp, 0, data.c_str(), (int)data.size(), nullptr, 0);
        text.assign(n, L'\0');
        MultiByteToWideChar(cp, 0, data.c_str(), (int)data.size(), &text[0], n);
    }
    std::wstring out = inirw::rewrite(text, sets);
    std::string bytes;
    if (!utf8) {   // Shift_JIS で書けない文字があれば UTF-8（BOM 付き）にする
        BOOL lost = FALSE;
        int n = WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, out.c_str(), (int)out.size(), nullptr, 0, nullptr, &lost);
        if (!lost) {
            bytes.resize(n);
            WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, out.c_str(), (int)out.size(), &bytes[0], n, nullptr, nullptr);
        } else { utf8 = true; bom = true; }
    }
    if (utf8) {
        int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), nullptr, 0, nullptr, nullptr);
        bytes.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), &bytes[0], n, nullptr, nullptr);
        if (bom) bytes.insert(0, "\xEF\xBB\xBF");
    }
    std::wstring tmp = g_ini_path + L".tmp";
    HANDLE o = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o == INVALID_HANDLE_VALUE) { *err = L"INI のあるフォルダに書き込めません（読み取り専用の場所かもしれません）"; return false; }
    DWORD wr = 0;
    BOOL ok = WriteFile(o, bytes.data(), (DWORD)bytes.size(), &wr, nullptr) && wr == bytes.size();
    CloseHandle(o);
    if (!ok || !MoveFileExW(tmp.c_str(), g_ini_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        *err = L"INI を書き換えられません（読み取り専用になっていないか確かめてください）";
        return false;
    }
    return true;
}

static bool any_changed() {
    for (auto& f : s_f) if (final_value(f) != f.init) return true;
    return false;
}
static void close_wnd() {
    if (!s_wnd) return;
    HWND w = s_wnd;
    s_wnd = nullptr;
    DestroyWindow(w);
    SetForegroundWindow(g_hwnd);
}
// やめる: すぐ反映した項目を元に戻す
static void revert_and_close() {
    for (auto& f : s_f) if (f.ap == A_LIVE && f.cur != f.init) { f.cur = f.init; apply_live(f); }
    close_wnd();
}
static void do_reboot_now();
static void save_and_close() {
    std::vector<std::pair<std::wstring, std::wstring>> sets;
    std::wstring restart_list;
    for (auto& f : s_f) {
        std::wstring v = final_value(f);
        if (v == f.init) continue;
        sets.push_back({f.key, v});
        if (f.ap == A_RESTART) restart_list += std::wstring(L"\n・") + f.label;
    }
    if (sets.empty()) { close_wnd(); show_toast(L"設定は変わっていません"); return; }
    std::wstring err;
    if (!write_ini(sets, &err)) { MessageBoxW(s_wnd, err.c_str(), L"画面＆サウンド設定", MB_ICONERROR); return; }
    for (auto& f : s_f) { f.cur = final_value(f); apply_live(f); f.init = f.cur; }
    HWND owner = s_wnd;
    if (!restart_list.empty()) {
        std::wstring msg = L"INI に保存しました。\n\n次の項目は、プログラム再起動で反映されます:" + restart_list +
                           L"\n\n今すぐプログラムを再起動しますか？\n（ゲームでセーブしていない進行は失われます。ステートセーブは残ります）";
        int r = MessageBoxW(owner, msg.c_str(), L"画面＆サウンド設定 - プログラム再起動", MB_YESNO | MB_ICONQUESTION);
        close_wnd();
        if (r == IDYES) do_reboot_now();
        else show_toast(L"INI に保存しました（＊の項目は F11 → R のプログラム再起動から反映）");
    } else {
        close_wnd();
        show_toast(L"INI に保存しました");
    }
}
static void set_defaults() {
    for (auto& f : s_f) {
        f.cur = f.def;
        set_ctl(f);
        apply_live(f);
    }
    update_midi_hint();
}

// ---- 窓 -------------------------------------------------------------------------------
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_SAVE) { save_and_close(); return 0; }
        if (id == ID_CANCEL) { revert_and_close(); return 0; }
        if (id == ID_DEFAULT) { set_defaults(); return 0; }
        if (id >= ID_FIELD && id < ID_FIELD + (int)s_f.size()) {
            Field& f = s_f[id - ID_FIELD];
            if ((f.kind == K_CHECK && code == BN_CLICKED) ||
                (f.kind == K_LIST && code == CBN_SELCHANGE) ||
                ((f.kind == K_NUM || f.kind == K_VOL) && code == EN_CHANGE) ||
                (f.kind == K_EDITLIST && code == CBN_EDITCHANGE)) on_changed(f);
            else if (f.kind == K_EDITLIST && code == CBN_SELCHANGE) on_changed(f, true);
        }
        return 0; }
    case WM_HSCROLL: {
        HWND sb = (HWND)lp;
        for (auto& f : s_f) {
            if (f.ctl2 != sb) continue;
            int pos = GetScrollPos(sb, SB_CTL);
            switch (LOWORD(wp)) {
            case SB_LINELEFT: pos -= 1; break;
            case SB_LINERIGHT: pos += 1; break;
            case SB_PAGELEFT: pos -= 10; break;
            case SB_PAGERIGHT: pos += 10; break;
            case SB_LEFT: pos = 0; break;
            case SB_RIGHT: pos = 300; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = HIWORD(wp); break;
            default: return 0;
            }
            pos = pos < 0 ? 0 : pos > 300 ? 300 : pos;
            SetScrollPos(sb, SB_CTL, pos, TRUE);
            on_changed(f, true);
            break;
        }
        return 0; }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if ((HWND)lp == s_midi_hint) {
            Field* m = find(L"MIDI");
            SetTextColor(dc, (m && m->cur == L"1") ? RGB(0, 110, 40) : RGB(190, 40, 20));
        }
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_CLOSE: {
        if (any_changed()) {
            int r = MessageBoxW(h, L"変更を INI に保存しますか？", L"画面＆サウンド設定", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDYES) save_and_close();
            else if (r == IDNO) revert_and_close();
            return 0;
        }
        revert_and_close();
        return 0; }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void open() {
    if (s_wnd) { SetForegroundWindow(s_wnd); return; }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = wnd_proc; wc.hInstance = inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE); wc.lpszClassName = L"PC98PLAYER_SETTINGS";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        HDC dc = GetDC(nullptr); s_k = GetDeviceCaps(dc, LOGPIXELSY) / 96.0; ReleaseDC(nullptr, dc); if (s_k < 1.0) s_k = 1.0;
        s_ui = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, FF_DONTCARE, L"Yu Gothic UI");
        s_bold = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FF_DONTCARE, L"Yu Gothic UI");
        reg = true;
    }
    build_fields();
    read_ini_values();

    const int CW = 772, CH = 618;
    RECT wr = {0, 0, S(CW), S(CH)};
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    AdjustWindowRectEx(&wr, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT mr; GetWindowRect(g_hwnd, &mr);
    int w = wr.right - wr.left, hh = wr.bottom - wr.top;
    int x = (mr.left + mr.right - w) / 2, y = (mr.top + mr.bottom - hh) / 2;
    {   // 画面からはみ出さないように
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            RECT a = mi.rcWork;
            x = std::max((int)a.left, std::min(x, (int)a.right - w));
            y = std::max((int)a.top, std::min(y, (int)a.bottom - hh));
        }
    }
    std::wstring ini_name = g_ini_path.substr(g_ini_path.find_last_of(L"\\/") + 1);
    s_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PC98PLAYER_SETTINGS", (L"画面＆サウンド設定 - " + ini_name).c_str(), style,
                            x, y, w, hh, g_hwnd, nullptr, inst, nullptr);
    auto mk = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int id, int x0, int y0, int w0, int h0, HFONT f = nullptr) {
        HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | st, S(x0), S(y0), S(w0), S(h0), s_wnd, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : s_ui), TRUE);
        return c;
    };
    auto group = [&](const wchar_t* t, int x0, int y0, int w0, int h0) { mk(L"BUTTON", t, BS_GROUPBOX, 0, x0, y0, w0, h0, s_bold); };
    // 1 項目を置く（ラベルの幅 lw、部品の幅 cw）
    auto place = [&](const wchar_t* key, int x0, int y0, int lw, int cw) {
        Field* f = find(key);
        if (!f) return;
        int id = ID_FIELD + (int)(f - &s_f[0]);
        std::wstring lab = f->label;
        if (f->ap == A_RESTART) lab += L" ＊";
        if (f->kind == K_CHECK) {
            f->ctl = mk(L"BUTTON", lab.c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, id, x0, y0 + 2, lw + cw, 22);
        } else {
            mk(L"STATIC", lab.c_str(), SS_LEFT, 0, x0, y0 + 4, lw, 20);
            if (f->kind == K_LIST || f->kind == K_EDITLIST) {
                f->ctl = mk(L"COMBOBOX", L"", (f->kind == K_LIST ? CBS_DROPDOWNLIST : CBS_DROPDOWN) | WS_VSCROLL | WS_TABSTOP | CBS_AUTOHSCROLL,
                            id, x0 + lw, y0, f->unit[0] ? cw - 40 : cw, 300);
                for (auto& o : f->opts) SendMessageW(f->ctl, CB_ADDSTRING, 0, (LPARAM)o.label.c_str());
                SendMessageW(f->ctl, CB_SETDROPPEDWIDTH, (WPARAM)S(f->kind == K_LIST ? 300 : cw), 0);
            } else if (f->kind == K_NUM) {
                f->ctl = mk(L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, id, x0 + lw, y0 + 1, 70, 24);
            }
            if (f->unit[0]) {
                int ux = x0 + lw + (f->kind == K_NUM ? 76 : cw - 34);
                mk(L"STATIC", f->unit, SS_LEFT, 0, ux, y0 + 4, 40, 20);
            }
        }
    };
    auto place_vol = [&](const wchar_t* key, int x0, int y0) {
        Field* f = find(key);
        if (!f) return;
        int id = ID_FIELD + (int)(f - &s_f[0]);
        mk(L"STATIC", f->label, SS_LEFT, 0, x0, y0 + 4, 110, 20);
        f->ctl2 = mk(L"SCROLLBAR", L"", SBS_HORZ | WS_TABSTOP, 0, x0 + 112, y0 + 3, 160, 20);
        SetScrollRange(f->ctl2, SB_CTL, 0, 300, FALSE);
        f->ctl = mk(L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, id, x0 + 280, y0 + 1, 52, 24);
        mk(L"STATIC", L"%", SS_LEFT, 0, x0 + 336, y0 + 4, 20, 20);
    };
    const int LX = 12, RX = 392, GW = 368, R = 28;
    // 左: 画面
    group(L"画面", LX, 6, GW, 24 + 7 * R + 6);
    place(L"Scale", LX + 12, 28, 130, 210);
    place(L"Smooth", LX + 12, 28 + R, 130, 210);
    place(L"FullScreen", LX + 12, 28 + R * 2, 130, 210);
    place(L"Font", LX + 12, 28 + R * 3, 130, 210);
    place(L"FontShift", LX + 12, 28 + R * 4, 130, 210);
    place(L"KanjiJIS", LX + 12, 28 + R * 5, 130, 210);
    place(L"GDCClock", LX + 12, 28 + R * 6, 130, 210);
    // 左: 動作
    int y2 = 6 + 24 + 7 * R + 6 + 8;
    group(L"動作", LX, y2, GW, 24 + 5 * R + 6);
    place(L"CpuMHz", LX + 12, y2 + 22, 130, 150);
    place(L"MouseSpeed", LX + 12, y2 + 22 + R, 130, 150);
    place(L"KeyRepeat", LX + 12, y2 + 22 + R * 2, 130, 210);
    place(L"PauseInactive", LX + 12, y2 + 22 + R * 3, 130, 210);
    place(L"ExitOnEnd", LX + 12, y2 + 22 + R * 4, 130, 210);
    // 右: サウンド
    group(L"サウンド", RX, 6, GW, 24 + 5 * R + 6);
    place(L"SoundBoard", RX + 12, 28, 150, 196);
    place(L"SoundIRQ", RX + 12, 28 + R, 150, 196);
    place(L"SoundBIOS", RX + 12, 28 + R * 2, 150, 196);
    place(L"Sound", RX + 12, 28 + R * 3, 150, 196);
    place(L"SampleRate", RX + 12, 28 + R * 4, 150, 196);
    // 右: MIDI
    int y3 = 6 + 24 + 5 * R + 6 + 8;
    group(L"MIDI", RX, y3, GW, 24 + 4 * R + 48);
    place(L"MIDI", RX + 12, y3 + 22, 150, 196);
    s_midi_hint = mk(L"STATIC", L"", SS_LEFT, 0, RX + 30, y3 + 22 + R - 2, GW - 40, 38);
    place(L"MidiDevice", RX + 12, y3 + 22 + R + 38, 110, 236);
    place(L"MidiIRQ", RX + 12, y3 + 22 + R * 2 + 38, 150, 196);
    place(L"MidiSpeedFix", RX + 12, y3 + 22 + R * 3 + 38, 150, 196);
    // 下: 音量（2 列）
    int y4 = std::max(y2 + 24 + 5 * R + 6, y3 + 24 + 4 * R + 48) + 8;
    group(L"音量（変えるとすぐ聞こえます。100% を超えると大きな山は自動で丸めます）", LX, y4, CW - 24, 24 + 3 * R + 6);
    place_vol(L"Volume", LX + 12, y4 + 22);
    place_vol(L"FMVolume", RX + 12, y4 + 22);
    place_vol(L"SSGVolume", LX + 12, y4 + 22 + R);
    place_vol(L"PCMVolume", RX + 12, y4 + 22 + R);
    place_vol(L"BeepVolume", LX + 12, y4 + 22 + R * 2);
    int y5 = y4 + 24 + 3 * R + 6 + 8;
    mk(L"STATIC", L"＊ の項目は保存後、プログラム再起動で反映されます（保存のときに尋ねます）。ほかはすぐ反映されます。",
       SS_LEFT, 0, LX + 4, y5, CW - 32, 20);
    int yb = y5 + 28;
    mk(L"BUTTON", L"既定値に戻す", BS_PUSHBUTTON | WS_TABSTOP, ID_DEFAULT, LX, yb, 130, 30);
    mk(L"BUTTON", L"保存して閉じる", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_SAVE, CW - 12 - 130 - 8 - 110, yb, 130, 30);
    mk(L"BUTTON", L"キャンセル", BS_PUSHBUTTON | WS_TABSTOP, ID_CANCEL, CW - 12 - 110, yb, 110, 30);
    // 実際に使った高さに合わせる
    {
        RECT r2 = {0, 0, S(CW), S(yb + 30 + 12)};
        AdjustWindowRectEx(&r2, style, FALSE, WS_EX_DLGMODALFRAME);
        SetWindowPos(s_wnd, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    for (auto& f : s_f) if (f.ctl) set_ctl(f);
    update_midi_hint();
    ShowWindow(s_wnd, SW_SHOW);
    SetForegroundWindow(s_wnd);
}

static void do_reboot_now() { do_reboot(); }

}  // namespace settingsdlg

// F11 の画面の「S: 画面＆サウンド設定」
static void open_settings() {
    if (g_ini_path.empty()) {   // インストールアシスタントの最中は INI が無い
        g_menu_note = L"インストール中は設定を変えられません（INI がありません）";
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    menu_close();
    settingsdlg::open();
}
