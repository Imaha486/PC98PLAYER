// SPDX-License-Identifier: MIT
//  gamepad.inc -- Windows のゲームパッドを PC-98 のジョイスティック（とキー）にあてる
//
//  PC-9801-26K / 86 の音源ボードのジョイスティック端子（ATARI 仕様: 上下左右 + トリガ A・B）は
//  YM2608 の SSG I/O ポート A（レジスタ 0Eh）で読む。ここでは Windows のゲームパッドを
//  winmm（joyGetPosEx）で毎フレーム読み、その端子の状態（machine_joystick）にする。
//  ボタンにはトリガ A・B のほか、PC-98 のキーも割り当てられる（ジョイスティック非対応のソフト向け）。
//
//  INI（[PC98PLAYER]）:
//    Joystick=1            0 でゲームパッドを使わない
//    JoyDevice=0           0 = 最初に見つかったもの / 1〜16 = その番号のゲームコントローラ
//    JoyDeadZone=50        スティックの遊び（傾きの % 。これより倒すと入る）
//    JoyDirection=JOY      方向の行き先: JOY（ジョイスティック）/ CURSOR（カーソルキー）/ TENKEY（テンキー 8 4 6 2）
//                          "+" で重ねられる（例 JOY+CURSOR）。NONE で方向を使わない
//    JoyButton1=A          ボタン n（Windows の「ゲームコントローラの設定」の番号）の割り当て。
//    JoyButton2=B            A / B = トリガ、RAPIDA / RAPIDB = 連射、キー名（RETURN・SPACE・ESC・Z・F1 など）、
//    JoyButton3=SPACE        16 進のキーコード（0x1C など）。"+" で重ねられる（例 A+RETURN）。NONE で何もしない
//    JoyButton4=RETURN
//    JoyRapid=4            連射の速さ（何フレームごとに押す／放すを切り替えるか。1〜30）

namespace pad {

enum { T_JOY = 0, T_KEY = 1, T_RAPID = 2 };
struct Target { int kind; int code; };

static bool        s_on = true;
static int         s_cfg_dev = 0;          // 0 = 自動
static int         s_dev = -1;             // 使っている joystick ID（-1 = 無し）
static JOYCAPSW    s_caps;
static int         s_dead = 50;
static int         s_dir = 1;              // bit0 JOY, bit1 CURSOR, bit2 TENKEY
static int         s_rapid = 4;
static std::vector<Target> s_btn[32];
static uint8_t     s_held[128];            // パッドが押している PC-98 のキー
static uint8_t     s_joy = 0;
static DWORD       s_next_scan = 0;
static unsigned    s_frame = 0;
static std::wstring s_name;
static void (*s_notice)(const std::wstring&) = nullptr;

struct KeyName { const char* n; uint8_t sc; };
static const KeyName k_keys[] = {
    {"ESC",0x00},{"1",0x01},{"2",0x02},{"3",0x03},{"4",0x04},{"5",0x05},{"6",0x06},{"7",0x07},{"8",0x08},{"9",0x09},{"0",0x0A},
    {"MINUS",0x0B},{"-",0x0B},{"CARET",0x0C},{"^",0x0C},{"YEN",0x0D},{"\\",0x0D},{"BS",0x0E},{"BACKSPACE",0x0E},{"TAB",0x0F},
    {"Q",0x10},{"W",0x11},{"E",0x12},{"R",0x13},{"T",0x14},{"Y",0x15},{"U",0x16},{"I",0x17},{"O",0x18},{"P",0x19},
    {"AT",0x1A},{"@",0x1A},{"[",0x1B},{"RETURN",0x1C},{"ENTER",0x1C},{"RET",0x1C},
    {"A_KEY",0x1D},{"S",0x1E},{"D",0x1F},{"F",0x20},{"G",0x21},{"H",0x22},{"J",0x23},{"K",0x24},{"L",0x25},
    {"SEMICOLON",0x26},{";",0x26},{"COLON",0x27},{":",0x27},{"]",0x28},
    {"Z",0x29},{"X",0x2A},{"C",0x2B},{"V",0x2C},{"B_KEY",0x2D},{"N",0x2E},{"M",0x2F},
    {"COMMA",0x30},{",",0x30},{"PERIOD",0x31},{".",0x31},{"SLASH",0x32},{"/",0x32},{"UNDERSCORE",0x33},{"_",0x33},
    {"SPACE",0x34},{"XFER",0x35},{"ROLLUP",0x36},{"ROLLDOWN",0x37},{"INS",0x38},{"DEL",0x39},
    {"UP",0x3A},{"LEFT",0x3B},{"RIGHT",0x3C},{"DOWN",0x3D},{"HOME",0x3E},{"CLR",0x3E},{"HELP",0x3F},
    {"NUM-",0x40},{"NUM/",0x41},{"NUM7",0x42},{"NUM8",0x43},{"NUM9",0x44},{"NUM*",0x45},{"NUM4",0x46},{"NUM5",0x47},
    {"NUM6",0x48},{"NUM+",0x49},{"NUM1",0x4A},{"NUM2",0x4B},{"NUM3",0x4C},{"NUM=",0x4D},{"NUM0",0x4E},{"NUM,",0x4F},{"NUM.",0x50},
    {"NFER",0x51},{"VF1",0x52},{"VF2",0x53},{"VF3",0x54},{"VF4",0x55},{"VF5",0x56},
    {"STOP",0x60},{"COPY",0x61},{"F1",0x62},{"F2",0x63},{"F3",0x64},{"F4",0x65},{"F5",0x66},{"F6",0x67},{"F7",0x68},
    {"F8",0x69},{"F9",0x6A},{"F10",0x6B},{"SHIFT",0x70},{"CAPS",0x71},{"KANA",0x72},{"GRPH",0x73},{"CTRL",0x74},
};

// 1 語を解釈する（"A" "RAPIDB" "RETURN" "0x1C" など）。分からなければ false
static bool parse_target(std::string w, Target* t) {
    for (auto& c : w) c = (char)toupper((unsigned char)c);
    if (w == "A" || w == "TRIGGERA" || w == "TRIGA") { *t = {T_JOY, 0x10}; return true; }
    if (w == "B" || w == "TRIGGERB" || w == "TRIGB") { *t = {T_JOY, 0x20}; return true; }
    if (w == "RAPIDA" || w == "TURBOA") { *t = {T_RAPID, 0x10}; return true; }
    if (w == "RAPIDB" || w == "TURBOB") { *t = {T_RAPID, 0x20}; return true; }
    if (w == "JOYUP") { *t = {T_JOY, 0x01}; return true; }
    if (w == "JOYDOWN") { *t = {T_JOY, 0x02}; return true; }
    if (w == "JOYLEFT") { *t = {T_JOY, 0x04}; return true; }
    if (w == "JOYRIGHT") { *t = {T_JOY, 0x08}; return true; }
    if (w.size() > 2 && w[0] == '0' && w[1] == 'X') {
        char* e = nullptr; long v = strtol(w.c_str() + 2, &e, 16);
        if (e && !*e && v >= 0 && v < 0x80) { *t = {T_KEY, (int)v}; return true; }
        return false;
    }
    if (w == "KEYA") w = "A_KEY";      // 文字の A・B キーは KEYA / KEYB と書く（A・B はトリガ）
    if (w == "KEYB") w = "B_KEY";
    for (auto& k : k_keys) if (w == k.n) { *t = {T_KEY, k.sc}; return true; }
    return false;
}

static std::vector<std::string> split_plus(const std::string& s) {
    // "A+RETURN" → A, RETURN。テンキーの "NUM+" の + は区切りにしない
    std::vector<std::string> r; std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') continue;
        bool num = cur.size() == 3 && toupper((unsigned char)cur[0]) == 'N' && toupper((unsigned char)cur[1]) == 'U' && toupper((unsigned char)cur[2]) == 'M';
        if (c == '+' && !cur.empty() && !num) { r.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) r.push_back(cur);
    return r;
}

static void release_all(Machine* m) {
    for (int k = 0; k < 128; k++) if (s_held[k]) { if (m) machine_key(m, (uint8_t)k, false); s_held[k] = 0; }
    s_joy = 0;
    if (m) machine_joystick(m, 0);
}
// PC-98 を作り直したとき（押していた状態は新しい本体には無い）
static void forget() { memset(s_held, 0, sizeof(s_held)); s_joy = 0; }

// INI から設定を読む。bad に解釈できなかった項目を返す
static void configure(const Ini& ini, Machine* m, std::wstring* bad) {
    release_all(m);
    s_on = ini.geti("PC98PLAYER.JOYSTICK", 1) != 0;
    int dev = ini.geti("PC98PLAYER.JOYDEVICE", 0);
    if (dev != s_cfg_dev) s_dev = -1;
    s_cfg_dev = dev < 0 ? 0 : dev > 16 ? 16 : dev;
    s_dead = ini.geti("PC98PLAYER.JOYDEADZONE", 50); s_dead = s_dead < 5 ? 5 : s_dead > 95 ? 95 : s_dead;
    s_rapid = ini.geti("PC98PLAYER.JOYRAPID", 4); s_rapid = s_rapid < 1 ? 1 : s_rapid > 30 ? 30 : s_rapid;
    {
        std::string d = ini.get("PC98PLAYER.JOYDIRECTION", "JOY");
        for (auto& c : d) c = (char)toupper((unsigned char)c);
        s_dir = 0;
        for (auto& w : split_plus(d)) {
            if (w == "JOY" || w == "JOYSTICK" || w == "STICK") s_dir |= 1;
            else if (w == "CURSOR" || w == "ARROW" || w == "KEY" || w == "KEYS") s_dir |= 2;
            else if (w == "TENKEY" || w == "NUMPAD") s_dir |= 4;
            else if (w == "NONE") {}
            else if (bad) *bad += L"JoyDirection=" + W(d) + L"\n";
        }
    }
    static const char* defs[4] = {"A", "B", "SPACE", "RETURN"};
    for (int i = 0; i < 32; i++) {
        s_btn[i].clear();
        char key[40]; snprintf(key, sizeof(key), "PC98PLAYER.JOYBUTTON%d", i + 1);
        std::string v = ini.get(key, i < 4 ? defs[i] : "");
        std::string uv = v; for (auto& c : uv) c = (char)toupper((unsigned char)c);
        if (uv.empty() || uv == "NONE") continue;
        for (auto& w : split_plus(v)) {
            Target t;
            if (parse_target(w, &t)) s_btn[i].push_back(t);
            else if (bad) { char k2[24]; snprintf(k2, sizeof(k2), "JoyButton%d=", i + 1); *bad += W(k2) + W(w) + L"\n"; }
        }
    }
    if (!s_on) s_dev = -1;
}

// 使うゲームパッドを探す（抜き差しに備えて、見つからない間は 2 秒おきに探し直す）
static bool find_device() {
    if (s_dev >= 0) return true;
    DWORD now = GetTickCount();
    if ((LONG)(now - s_next_scan) < 0) return false;
    s_next_scan = now + 2000;
    UINT n = joyGetNumDevs(); if (n > 16) n = 16;
    for (UINT id = 0; id < n; id++) {
        if (s_cfg_dev && (int)id != s_cfg_dev - 1) continue;
        JOYINFOEX ji = {sizeof(ji)}; ji.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &ji) != JOYERR_NOERROR) continue;
        if (joyGetDevCapsW(id, &s_caps, sizeof(s_caps)) != JOYERR_NOERROR) continue;
        s_dev = (int)id;
        s_name = s_caps.szPname;
        if (s_notice) s_notice(L"ゲームパッドを使います: " + s_name);
        return true;
    }
    return false;
}

// 軸の値 → -1 / 0 / +1
static int axis(DWORD v, UINT lo, UINT hi) {
    if (hi <= lo) return 0;
    double c = (lo + hi) / 2.0, h = (hi - lo) / 2.0;
    double r = (v - c) / h;
    if (r <= -s_dead / 100.0) return -1;
    if (r >= s_dead / 100.0) return 1;
    return 0;
}

// 毎フレーム呼ぶ。active=false（窓が裏にいる・メニュー中など）は全部放す
static void poll(Machine* m, bool active) {
    s_frame++;
    if (!m) return;
    if (!s_on || !active || !find_device()) { if (s_joy || std::any_of(s_held, s_held + 128, [](uint8_t v) { return v != 0; })) release_all(m); return; }
    JOYINFOEX ji = {sizeof(ji)}; ji.dwFlags = JOY_RETURNALL | JOY_RETURNPOVCTS;
    if (joyGetPosEx((UINT)s_dev, &ji) != JOYERR_NOERROR) {
        release_all(m);
        if (s_notice) s_notice(L"ゲームパッドが外れました");
        s_dev = -1; s_next_scan = GetTickCount() + 2000;
        return;
    }
    int x = axis(ji.dwXpos, s_caps.wXmin, s_caps.wXmax), y = axis(ji.dwYpos, s_caps.wYmin, s_caps.wYmax);
    if ((s_caps.wCaps & JOYCAPS_HASPOV) && ji.dwPOV != 0xFFFF && ji.dwPOV <= 36000) {
        DWORD p = ji.dwPOV;   // 1/100 度。0 = 上、時計回り
        if (p >= 29250 || p <= 6750) y = -1;
        if (p >= 11250 && p <= 24750) y = 1;
        if (p >= 2250 && p <= 15750) x = 1;
        if (p >= 20250 && p <= 33750) x = -1;
    }
    uint8_t joy = 0;
    bool want[128] = {};
    if (s_dir & 1) joy |= (uint8_t)((y < 0 ? 1 : 0) | (y > 0 ? 2 : 0) | (x < 0 ? 4 : 0) | (x > 0 ? 8 : 0));
    if (s_dir & 2) { if (y < 0) want[0x3A] = true; if (y > 0) want[0x3D] = true; if (x < 0) want[0x3B] = true; if (x > 0) want[0x3C] = true; }
    if (s_dir & 4) { if (y < 0) want[0x43] = true; if (y > 0) want[0x4B] = true; if (x < 0) want[0x46] = true; if (x > 0) want[0x48] = true; }
    bool rapid_on = ((s_frame / (unsigned)s_rapid) & 1) == 0;
    for (int i = 0; i < 32; i++) {
        if (!(ji.dwButtons & (1u << i))) continue;
        for (auto& t : s_btn[i]) {
            if (t.kind == T_JOY) joy |= (uint8_t)t.code;
            else if (t.kind == T_RAPID) { if (rapid_on) joy |= (uint8_t)t.code; }
            else want[t.code] = true;
        }
    }
    if (joy != s_joy) { s_joy = joy; }
    machine_joystick(m, s_joy);
    for (int k = 0; k < 128; k++) {
        if (want[k] && !s_held[k]) { s_held[k] = 1; machine_key(m, (uint8_t)k, true); }
        else if (!want[k] && s_held[k]) { s_held[k] = 0; machine_key(m, (uint8_t)k, false); }
    }
}

} // namespace pad
