// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  bios.cpp  --  PC-9801 BIOS の HLE（ROM は使わない）
//
//  INT 18h（キーボード / CRT / グラフィック）、INT 1Ch（カレンダ・タイマ）、
//  IRQ1（キーボード）、INT 33h（マウスドライバ）、INT DCh（DOS 拡張）と、
//  MS-DOS のコンソール出力（ESC シーケンスつき）をここで扱う。
// -----------------------------------------------------------------------------
#include "machine.h"
#include "floppy.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

#define ROMSEG 0xF000
static const uint16_t STUB_BASE = 0x1000;   // F000:1000 から各ベクタの入口を並べる

static inline uint16_t AX(Machine* m) { return (uint16_t)m->cpu.r[EAX]; }
static inline uint16_t BX(Machine* m) { return (uint16_t)m->cpu.r[EBX]; }
static inline uint16_t CX(Machine* m) { return (uint16_t)m->cpu.r[ECX]; }
static inline uint16_t DX(Machine* m) { return (uint16_t)m->cpu.r[EDX]; }
static inline uint8_t AH(Machine* m) { return (uint8_t)(m->cpu.r[EAX] >> 8); }
static inline uint8_t AL(Machine* m) { return (uint8_t)m->cpu.r[EAX]; }
static inline void SETAX(Machine* m, uint16_t v) { m->cpu.r[EAX] = (m->cpu.r[EAX] & 0xFFFF0000u) | v; }
static inline void SETBX(Machine* m, uint16_t v) { m->cpu.r[EBX] = (m->cpu.r[EBX] & 0xFFFF0000u) | v; }
static inline void SETCX(Machine* m, uint16_t v) { m->cpu.r[ECX] = (m->cpu.r[ECX] & 0xFFFF0000u) | v; }
static inline void SETDX(Machine* m, uint16_t v) { m->cpu.r[EDX] = (m->cpu.r[EDX] & 0xFFFF0000u) | v; }
static inline void SETAL(Machine* m, uint8_t v) { m->cpu.r[EAX] = (m->cpu.r[EAX] & 0xFFFFFF00u) | v; }
static inline void SETAH(Machine* m, uint8_t v) { m->cpu.r[EAX] = (m->cpu.r[EAX] & 0xFFFF00FFu) | ((uint32_t)v << 8); }
static inline void SETBH(Machine* m, uint8_t v) { m->cpu.r[EBX] = (m->cpu.r[EBX] & 0xFFFF00FFu) | ((uint32_t)v << 8); }
static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }

// ---- 入口の組み立て ----------------------------------------------------------
static uint16_t s_stub_ptr;
static uint16_t put_stub(Machine* m, const uint8_t* code, int len) {
    uint16_t off = s_stub_ptr;
    for (int i = 0; i < len; i++) m->ram[lin(ROMSEG, (uint16_t)(off + i))] = code[i];
    s_stub_ptr = (uint16_t)(s_stub_ptr + len);
    return off;
}
static void set_vec(Machine* m, int vec, uint16_t seg, uint16_t off) {
    m->ram[vec * 4] = (uint8_t)off; m->ram[vec * 4 + 1] = (uint8_t)(off >> 8);
    m->ram[vec * 4 + 2] = (uint8_t)seg; m->ram[vec * 4 + 3] = (uint8_t)(seg >> 8);
}
uint16_t bios_hle_stub(Machine* m, uint8_t n) {
    uint8_t code[3] = {0xF1, n, 0xCF};
    return put_stub(m, code, 3);
}
void bios_hook_vec(Machine* m, int vec, uint8_t n) {
    set_vec(m, vec, ROMSEG, bios_hle_stub(m, n));
}

// ---- キーボード --------------------------------------------------------------
static const uint8_t k_norm[0x60] = {
    0x1B,'1','2','3','4','5','6','7','8','9','0','-','^','\\',0x08,0x09,
    'q','w','e','r','t','y','u','i','o','p','@','[',0x0D,'a','s','d',
    'f','g','h','j','k','l',';',':',']','z','x','c','v','b','n','m',
    ',','.','/','_',' ',0x00,0x00,0x00,0x00,0x7F,0x0B,0x08,0x0C,0x0A,0x1A,0x00,
    '-','/','7','8','9','*','4','5','6','+','1','2','3','=','0',',',
    '.',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
static const uint8_t k_shift[0x60] = {
    0x1B,'!','"','#','$','%','&','\'','(',')','0','=','`','|',0x08,0x09,
    'Q','W','E','R','T','Y','U','I','O','P','~','{',0x0D,'A','S','D',
    'F','G','H','J','K','L','+','*','}','Z','X','C','V','B','N','M',
    '<','>','?','_',' ',0x00,0x00,0x00,0x00,0x7F,0x0B,0x08,0x0C,0x0A,0x1A,0x00,
    '-','/','7','8','9','*','4','5','6','+','1','2','3','=','0',',',
    '.',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

static void kb_push(Machine* m, uint16_t code) {
    uint8_t* r = m->ram;
    uint8_t cnt = r[0x528];
    if (cnt >= 16) return;
    uint16_t tail = (uint16_t)(r[0x526] | (r[0x527] << 8));
    r[tail] = (uint8_t)code; r[tail + 1] = (uint8_t)(code >> 8);
    tail = (uint16_t)(tail + 2);
    if (tail >= 0x522) tail = 0x502;
    r[0x526] = (uint8_t)tail; r[0x527] = (uint8_t)(tail >> 8);
    r[0x528] = (uint8_t)(cnt + 1);
}
int bios_key_available(Machine* m) { return m->ram[0x528]; }
uint16_t bios_key_read(Machine* m, bool remove) {
    uint8_t* r = m->ram;
    if (!r[0x528]) return 0;
    uint16_t head = (uint16_t)(r[0x524] | (r[0x525] << 8));
    uint16_t v = (uint16_t)(r[head] | (r[head + 1] << 8));
    if (remove) {
        head = (uint16_t)(head + 2);
        if (head >= 0x522) head = 0x502;
        r[0x524] = (uint8_t)head; r[0x525] = (uint8_t)(head >> 8);
        r[0x528]--;
    }
    return v;
}
static void kb_clear(Machine* m) {
    m->ram[0x524] = 0x02; m->ram[0x525] = 0x05;
    m->ram[0x526] = 0x02; m->ram[0x527] = 0x05;
    m->ram[0x528] = 0;
}

void bios_key_irq(Machine* m) {
    uint8_t d = io_in8(m, 0x41);
    uint8_t sc = d & 0x7F;
    bool brk = (d & 0x80) != 0;
    uint8_t* bm = &m->ram[0x52A];
    if (brk) bm[sc >> 3] &= (uint8_t)~(1u << (sc & 7));
    else bm[sc >> 3] |= (uint8_t)(1u << (sc & 7));
    uint8_t& sh = m->ram[0x53A];
    if (sc >= 0x70 && sc <= 0x74) {
        uint8_t bit = (uint8_t)(1u << (sc - 0x70));
        if (brk) sh &= (uint8_t)~bit; else sh |= bit;
        return;
    }
    if (brk) return;
    // キーバッファに積むコード（AH = キーコード, AL = 文字コード）。実機の BIOS と同じく、
    // ・カーソル・ROLL UP/DOWN・INS・DEL・HELP は文字コード 0（AX=3A00h など）。
    //   （0Bh・0Ah などへの置き換えは MS-DOS の CON がキーコードを見て行う。BIOS で文字コードを付けると、
    //    INT 18h AH=00h で AX=3A00h などと比べるソフトで矢印キーが効かない）
    // ・XFER・NFER・HOME/CLR・f･1〜f･10 は AH だけ（SHIFT・CTRL で AH が変わる）、AL = 0。
    // ・vf･1〜vf･5 など、コードの無いキーと STOP・COPY は積まない
    bool shift = (sh & 0x01) != 0, ctrl = (sh & 0x10) != 0;
    uint16_t code;
    if (sc == 0x35 || sc == 0x51) {                         // XFER / NFER
        uint8_t base = sc == 0x35 ? 0x35 : 0x51;
        code = (uint16_t)((ctrl ? (base == 0x35 ? 0xB5 : 0xB1) : shift ? (base == 0x35 ? 0xA5 : 0xA1) : base) << 8);
    } else if (sc == 0x3E) {                                // HOME/CLR（SHIFT で CLR = AEh）
        if (ctrl) return;
        code = shift ? 0xAE00 : 0x3E00;
    } else if (sc >= 0x36 && sc <= 0x3F) {                  // ROLL UP/DOWN・INS・DEL・矢印・HELP
        code = (uint16_t)(sc << 8);
    } else if (sc >= 0x62 && sc <= 0x6B) {                  // f･1〜f･10（SHIFT +20h、CTRL +30h）
        code = (uint16_t)((sc + (ctrl ? 0x30 : shift ? 0x20 : 0)) << 8);
    } else if (sc < 0x52) {
        uint8_t ch = shift ? k_shift[sc] : k_norm[sc];
        if (sc == 0x33 && !shift) return;                   // 「＿」のキーは SHIFT なしではコード無し
        if ((sh & 0x02) && ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 0x20);   // CAPS
        if (ctrl && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '@' && ch <= '_'))) ch = (uint8_t)(ch & 0x1F);
        code = (uint16_t)((sc << 8) | ch);
    } else return;                                          // vf･1〜vf･5・STOP・COPY など
    kb_push(m, code);
}

// ---- コンソール（MS-DOS の CON）-------------------------------------------------
struct Console {
    int x, y;
    uint8_t attr;
    int esc;           // 0=通常 1=ESC 2=ESC[ 3=ESC= 4=ESC=y 5=ESC) / ESC(
    char buf[32];
    int blen;
    uint8_t sj1;       // シフト JIS 第 1 バイト待ち
    int sx, sy;
    int rows;
};
static Console s_con;
static bool s_rev_by_bg;   // 反転が 40-47 由来か（con_sgr を参照）

static void con_cell(Machine* m, int x, int y, uint16_t code, uint8_t attr) {
    if (x < 0 || x >= 80 || y < 0 || y >= 25) return;
    uint32_t i = (uint32_t)(y * 80 + x) * 2;
    m->tvram[i] = (uint8_t)code; m->tvram[i + 1] = (uint8_t)(code >> 8);
    m->tvram[0x2000 + i] = attr;
}
static void con_clear_range(Machine* m, int from, int to) {
    for (int i = from; i < to; i++) con_cell(m, i % 80, i / 80, 0x20, s_con.attr);
}
static void con_scroll(Machine* m) {
    int rows = s_con.rows;
    memmove(m->tvram, m->tvram + 160, (size_t)(rows - 1) * 160);
    memmove(m->tvram + 0x2000, m->tvram + 0x2000 + 160, (size_t)(rows - 1) * 160);
    con_clear_range(m, (rows - 1) * 80, rows * 80);
}
static void con_newline(Machine* m) {
    s_con.y++;
    if (s_con.y >= s_con.rows) { s_con.y = s_con.rows - 1; con_scroll(m); }
}
// MS-DOS のワークエリア: 0000:071Ch = カーソルの桁、0000:0710h = カーソルの行（Borland C の conio などが直接読み書きする）
static void con_sync_cursor(Machine* m) {
    m->gdcm.ead = (uint32_t)(s_con.y * 80 + s_con.x);
    m->ram[0x71C] = (uint8_t)s_con.x;
    m->ram[0x710] = (uint8_t)s_con.y;
}
static void con_load_state(Machine* m) {
    s_con.attr = m->ram[0x71D];
    int x = m->ram[0x71C], y = m->ram[0x710];
    if (x < 80) s_con.x = x;
    if (y < s_con.rows) s_con.y = y;
}
void console_reset(Machine* m) {
    memset(&s_con, 0, sizeof(s_con));
    s_con.attr = 0xE1;
    s_con.rows = 25;
    s_rev_by_bg = false;
    (void)m;
}
// 40-47（色つき反転）で付いた反転は、続く 30-37 で外れる（NEC の ANSI の振る舞い。
// Ray のメニューは ESC[46m で強調し ESC[36m で戻す）。ESC[7m の反転はそのまま残す。
static void con_sgr(int n) {
    uint8_t& a = s_con.attr;
    if (n == 0) { a = 0xE1; s_rev_by_bg = false; }
    else if (n == 7) { a |= 0x04; s_rev_by_bg = false; }
    else if (n == 1) a |= 0; // 強調なし
    else if (n == 2) a = (uint8_t)((a & 0x1F) | 0xE0 & 0);   // なし
    else if (n == 4) a |= 0x08;
    else if (n == 5) a |= 0x02;
    else if (n == 8 || n == 16) a &= (uint8_t)~1;
    else if (n >= 30 && n <= 37) {
        static const uint8_t col[8] = {0, 2, 4, 6, 1, 3, 5, 7};   // ANSI -> PC-98 GRB
        if (s_rev_by_bg) { a &= (uint8_t)~0x04; s_rev_by_bg = false; }
        a = (uint8_t)((a & 0x1F) | (col[n - 30] << 5) | 1);
    } else if (n >= 17 && n <= 23) {
        static const uint8_t col[7] = {2, 4, 6, 1, 3, 5, 7};
        a = (uint8_t)((a & 0x1F) | (col[n - 17] << 5) | 1);
    } else if (n >= 40 && n <= 47) {
        static const uint8_t col[8] = {0, 2, 4, 6, 1, 3, 5, 7};
        if (!(a & 0x04)) s_rev_by_bg = true;
        a = (uint8_t)((col[n - 40] << 5) | 0x05);
    }
}
static void con_csi(Machine* m, char fin) {
    int args[8] = {0}, na = 0;
    bool priv = false;
    const char* p = s_con.buf;
    if (*p == '>' || *p == '?') { priv = true; p++; }
    while (*p && na < 8) {
        int v = 0; bool any = false;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = true; }
        args[na++] = any ? v : -1;
        if (*p == ';') p++; else break;
    }
    int a0 = na > 0 && args[0] >= 0 ? args[0] : -1;
    int n1 = a0 > 0 ? a0 : 1;
    switch (fin) {
    case 'H': case 'f': {
        int yy = (na > 0 && args[0] > 0) ? args[0] - 1 : 0;
        int xx = (na > 1 && args[1] > 0) ? args[1] - 1 : 0;
        s_con.x = xx < 80 ? xx : 79; s_con.y = yy < s_con.rows ? yy : s_con.rows - 1; break; }
    case 'A': s_con.y -= n1; if (s_con.y < 0) s_con.y = 0; break;
    case 'B': s_con.y += n1; if (s_con.y >= s_con.rows) s_con.y = s_con.rows - 1; break;
    case 'C': s_con.x += n1; if (s_con.x > 79) s_con.x = 79; break;
    case 'D': s_con.x -= n1; if (s_con.x < 0) s_con.x = 0; break;
    case 'J':
        if (a0 == 2) { con_clear_range(m, 0, 80 * 25); s_con.x = s_con.y = 0; }
        else if (a0 == 1) con_clear_range(m, 0, s_con.y * 80 + s_con.x + 1);
        else con_clear_range(m, s_con.y * 80 + s_con.x, 80 * 25);
        break;
    case 'K':
        if (a0 == 2) con_clear_range(m, s_con.y * 80, s_con.y * 80 + 80);
        else if (a0 == 1) con_clear_range(m, s_con.y * 80, s_con.y * 80 + s_con.x + 1);
        else con_clear_range(m, s_con.y * 80 + s_con.x, s_con.y * 80 + 80);
        break;
    case 'm': if (na == 0) con_sgr(0); for (int i = 0; i < na; i++) con_sgr(args[i] < 0 ? 0 : args[i]); break;
    case 's': s_con.sx = s_con.x; s_con.sy = s_con.y; break;
    case 'u': s_con.x = s_con.sx; s_con.y = s_con.sy; break;
    case 'h': case 'l':
        if (priv) {
            bool set = fin == 'h';
            if (a0 == 5) { if (set) m->gdcm.csrform[0] &= 0x7F; else m->gdcm.csrform[0] |= 0x80; }
            // 1: ファンクションキー表示, 3: 20/25 行 ―― 表示だけの問題なので受け流す
        }
        break;
    case 'M': {   // 行削除
        for (int k = 0; k < n1; k++) {
            int rows = s_con.rows;
            int y = s_con.y;
            memmove(m->tvram + y * 160, m->tvram + (y + 1) * 160, (size_t)(rows - 1 - y) * 160);
            memmove(m->tvram + 0x2000 + y * 160, m->tvram + 0x2000 + (y + 1) * 160, (size_t)(rows - 1 - y) * 160);
            con_clear_range(m, (rows - 1) * 80, rows * 80);
        }
        break; }
    case 'L': {   // 行挿入
        for (int k = 0; k < n1; k++) {
            int rows = s_con.rows, y = s_con.y;
            memmove(m->tvram + (y + 1) * 160, m->tvram + y * 160, (size_t)(rows - 1 - y) * 160);
            memmove(m->tvram + 0x2000 + (y + 1) * 160, m->tvram + 0x2000 + y * 160, (size_t)(rows - 1 - y) * 160);
            con_clear_range(m, y * 80, y * 80 + 80);
        }
        break; }
    }
}

static void console_putc_body(Machine* m, uint8_t c);
// MS-DOS のワークエリア 0000:071Dh が「今の表示属性」。ソフトが直接読み書きするので、ここと常にそろえる
void console_putc(Machine* m, uint8_t c) {
    con_load_state(m);
    console_putc_body(m, c);
    m->ram[0x71D] = s_con.attr;
    m->ram[0x71C] = (uint8_t)s_con.x;
    m->ram[0x710] = (uint8_t)s_con.y;
}
static void console_putc_body(Machine* m, uint8_t c) {
    Console& k = s_con;
    if (k.esc) {
        if (k.esc == 1) {
            if (c == '[') { k.esc = 2; k.blen = 0; k.buf[0] = 0; return; }
            if (c == '=') { k.esc = 3; return; }
            if (c == ')' || c == '(') { k.esc = 5; return; }
            if (c == '*') { con_clear_range(m, 0, 80 * 25); k.x = k.y = 0; k.esc = 0; return; }
            if (c == 'D') { con_newline(m); k.esc = 0; return; }
            if (c == 'E') { k.x = 0; con_newline(m); k.esc = 0; return; }
            if (c == 'M') { if (k.y > 0) k.y--; k.esc = 0; return; }
            k.esc = 0; return;
        }
        if (k.esc == 2) {
            if ((c >= '0' && c <= '9') || c == ';' || c == '>' || c == '?') {
                if (k.blen < 30) { k.buf[k.blen++] = (char)c; k.buf[k.blen] = 0; }
                return;
            }
            con_csi(m, (char)c); k.esc = 0; con_sync_cursor(m); return;
        }
        if (k.esc == 3) { k.sy = c - 0x20; k.esc = 4; return; }
        if (k.esc == 4) { k.y = k.sy; k.x = c - 0x20; if (k.x < 0) k.x = 0; if (k.x > 79) k.x = 79;
                          if (k.y < 0) k.y = 0; if (k.y >= k.rows) k.y = k.rows - 1; k.esc = 0; con_sync_cursor(m); return; }
        if (k.esc == 5) { k.esc = 0; return; }
    }
    if (k.sj1) {
        uint16_t jis = sjis_to_jis((uint16_t)((k.sj1 << 8) | c));
        k.sj1 = 0;
        uint8_t row = (uint8_t)((jis >> 8) - 0x20), cell = (uint8_t)(jis & 0xFF);
        if (row >= 0x09 && row <= 0x0B) {   // 半角
            con_cell(m, k.x, k.y, (uint16_t)((cell << 8) | row), k.attr);
            if (++k.x >= 80) { k.x = 0; con_newline(m); }
        } else {
            if (k.x >= 79) { k.x = 0; con_newline(m); }
            con_cell(m, k.x, k.y, (uint16_t)((cell << 8) | row), k.attr);
            con_cell(m, k.x + 1, k.y, (uint16_t)(((cell | 0x80) << 8) | row), k.attr);
            k.x += 2;
            if (k.x >= 80) { k.x = 0; con_newline(m); }
        }
        con_sync_cursor(m);
        return;
    }
    switch (c) {
    case 0x1B: k.esc = 1; return;
    case 0x07: m->beepw.push_back({m->ticks, 3}); return;   // ベル
    case 0x08: if (k.x > 0) k.x--; con_sync_cursor(m); return;
    case 0x09: k.x = (k.x + 8) & ~7; if (k.x >= 80) { k.x = 0; con_newline(m); } con_sync_cursor(m); return;
    case 0x0A: con_newline(m); con_sync_cursor(m); return;
    case 0x0B: if (k.y > 0) k.y--; con_sync_cursor(m); return;
    case 0x0C: if (k.x < 79) k.x++; con_sync_cursor(m); return;
    case 0x0D: k.x = 0; con_sync_cursor(m); return;
    case 0x1A: con_clear_range(m, 0, 80 * 25); k.x = k.y = 0; con_sync_cursor(m); return;
    case 0x1E: k.x = k.y = 0; con_sync_cursor(m); return;
    }
    if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) { k.sj1 = c; return; }
    con_cell(m, k.x, k.y, c, k.attr);
    if (++k.x >= 80) { k.x = 0; con_newline(m); }
    con_sync_cursor(m);
}

// ---- INT 18h -----------------------------------------------------------------
static void wait_and_retry(Machine* m) {
    // 入力待ち: いまの F1 18 をもう一度実行するように戻し、割込みを許して HLT する
    m->cpu.ip = (uint16_t)(m->cpu.ip - 2);
    m->cpu.fl |= FL_IF;
    m->cpu.halted = 1;
}

static void int18(Machine* m) {
    uint8_t ah = AH(m);
    switch (ah) {
    case 0x00:
        if (!bios_key_available(m)) { wait_and_retry(m); return; }
        SETAX(m, bios_key_read(m, true)); return;
    case 0x01:
        if (bios_key_available(m)) { SETAX(m, bios_key_read(m, false)); SETBH(m, 1); }
        else SETBH(m, 0);
        return;
    case 0x02: SETAL(m, m->ram[0x53A]); return;
    case 0x03: kb_clear(m); return;
    case 0x04: SETAH(m, m->ram[0x52A + (AL(m) & 0x0F)]); return;
    case 0x05:
        if (bios_key_available(m)) { SETAX(m, bios_key_read(m, true)); SETBH(m, 1); }
        else SETBH(m, 0);
        return;
    case 0x0A: {
        uint8_t al = AL(m);
        m->ram[0x53C] = al;
        m->gdcm.csrform[0] = (uint8_t)((m->gdcm.csrform[0] & 0xE0) | ((al & 1) ? 19 : 15));
        m->gdcm.csrform[1] = (uint8_t)(m->gdcm.csrform[1] & 0xE0);                       // カーソルは 0 ライン目から
        m->gdcm.csrform[2] = (uint8_t)((((al & 1) ? 19 : 15) << 3) | 3);                  // 最後のラインまで
        s_con.rows = (al & 1) ? 20 : 25;
        m->ram[0x712] = (uint8_t)(s_con.rows - 1);
        m->modeff[0] = (al & 4) ? 1 : 0;   // 属性 bit4 = 簡易グラフ(1) / バーチカルライン(0)
        m->modeff[2] = (al & 2) ? 1 : 0;   // 40 桁
        m->modeff[5] = (al & 8) ? 1 : 0;   // コードアクセス
        return; }
    case 0x0B: SETAL(m, m->ram[0x53C]); return;
    case 0x0C: m->gdcm.display = 1; return;
    case 0x0D: m->gdcm.display = 0; return;
    case 0x0E: m->gdcm.pram[0] = (uint8_t)(DX(m) >> 1); m->gdcm.pram[1] = (uint8_t)(DX(m) >> 9); return;
    case 0x0F: return;
    case 0x10: return;
    case 0x11: m->gdcm.csrform[0] |= 0x80; return;
    case 0x12: m->gdcm.csrform[0] &= 0x7F; return;
    case 0x13: m->gdcm.ead = (uint32_t)(DX(m) >> 1); return;
    case 0x14: {   // フォント読み出し
        uint16_t code = DX(m);
        { static int n = 0; if (m->cfg.trace && n < 400) { n++; { uint32_t sp = ((uint32_t)m->cpu.sr[SS_] << 4) + (uint16_t)m->cpu.r[ESP]; plog("[bios] INT18 AH=14 DX=%04X → %04X:%04X  from %04X:%04X\n", code, BX(m), CX(m), m->ram[sp + 2] | (m->ram[sp + 3] << 8), m->ram[sp] | (m->ram[sp + 1] << 8)); } } }
        uint32_t buf = lin(BX(m), CX(m));
        // バッファの先頭 2 バイトは「縦の大きさ（8 ドット単位）」「横の大きさ（8 ドット単位）」の順
        //（ワードとして読むと 横*256+縦。全角 0202h、半角 0102h、8x8 は 0101h）。ゲームによっては 2 バイト目で幅を判断する
        uint8_t hi = (uint8_t)(code >> 8);
        if (hi == 0x00) {                    // 8x8 の半角（グラフィック用の小さな文字）: 8x16 の字形を縦に半分にしたもの
            const uint8_t* g = font_get_ank((uint8_t)code);
            mem_wb(m, buf, 1); mem_wb(m, buf + 1, 1);
            for (int i = 0; i < 8; i++) mem_wb(m, buf + 2 + i, (uint8_t)(g[i * 2] | g[i * 2 + 1]));
        } else if (hi == 0x80) {             // 8x16 の半角
            const uint8_t* g = font_get_ank((uint8_t)code);
            mem_wb(m, buf, 2); mem_wb(m, buf + 1, 1);
            for (int i = 0; i < 16; i++) mem_wb(m, buf + 2 + i, g[i]);
        } else {
            uint16_t jis = code & 0x7F7F;
            const uint8_t* g = font_get_kanji(jis);
            uint8_t row = (uint8_t)(jis >> 8);
            if (row >= 0x29 && row <= 0x2B) {   // 半角の漢字 ROM（8x16）
                mem_wb(m, buf, 2); mem_wb(m, buf + 1, 1);
                for (int i = 0; i < 16; i++) mem_wb(m, buf + 2 + i, g[i * 2]);
            } else {
                mem_wb(m, buf, 2); mem_wb(m, buf + 1, 2);
                for (int i = 0; i < 32; i++) mem_wb(m, buf + 2 + i, g[i]);
            }
        }
        return; }
    case 0x16: {
        uint8_t ch = (uint8_t)DX(m), at = (uint8_t)(DX(m) >> 8);
        for (int i = 0; i < 0x1000; i++) { m->tvram[i * 2] = ch; m->tvram[i * 2 + 1] = 0; m->tvram[0x2000 + i * 2] = at; }
        return; }
    case 0x17: m->portc &= (uint8_t)~0x08; m->beepw.push_back({m->ticks, 1}); return;
    case 0x18: m->portc |= 0x08; m->beepw.push_back({m->ticks, 0}); return;
    case 0x1A: {   // 外字の定義: DX=JIS コード（76xx/77xx）, BX:CX=バッファ（先頭 2 バイトの後に 32 バイト）
        uint16_t code = DX(m);
        uint32_t buf = lin(BX(m), CX(m));
        if (fontrom_is_gaiji(code))
            for (int y = 0; y < 16; y++) {
                fontrom_gaiji_write(code, y, true, mem_rb(m, buf + 2 + y * 2));
                fontrom_gaiji_write(code, y, false, mem_rb(m, buf + 3 + y * 2));
            }
        return; }
    case 0x1B:   // KCG のアクセスモード: AL=0 コードアクセス / AL=1 ドットアクセス（0053Ch bit3 とモードF/F の bit5）
        if (AL(m) == 0) { m->ram[0x53C] &= (uint8_t)~0x08; m->modeff[5] = 0; }
        else if (AL(m) == 1) { m->ram[0x53C] |= 0x08; m->modeff[5] = 1; }
        return;
    case 0x40: m->gdcs.display = 1; m->ram[0x54C] |= 0x80; return;
    case 0x41: m->gdcs.display = 0; m->ram[0x54C] &= 0x7F; return;
    case 0x42: {
        uint8_t ch = (uint8_t)(CX(m) >> 8);
        int mode = ch >> 6;
        // CH bit7-6: 01 = VRAM の後半 200 ライン（SAD=8000 ワード）/ 10 = 前半 200 ライン / 11 = 400 ライン
        // （master.lib の graph_200line(1) は前者で、前半を隠し VRAM に使う: 東方夢時空）
        memset(m->gdcs.pram, 0, 4);
        // 実機の BIOS と同じく、200 ラインはグラフィック GDC の CSRFORM（1 行の走査線数 = 2）で作る。
        // （フラグで決め打ちにすると、あとでソフトが GDC に直接 CSRFORM を書いて 400 ラインに戻したときに縦 2 倍のままになる）
        m->gdcs.csrform[0] = (uint8_t)((m->gdcs.csrform[0] & 0xE0) | (mode == 3 ? 0 : 1));
        if (mode == 3) { m->gfx_200 = 0; m->gdcs.zoom = 0; }
        else {
            m->gfx_200 = 1; m->gfx_200_lower = 0; m->gdcs.zoom = 0;
            if (mode == 1) { m->gdcs.pram[0] = (uint8_t)(8000 & 0xFF); m->gdcs.pram[1] = (uint8_t)(8000 >> 8); }
            m->gdcs.pram[3] = 0x40;
        }
        m->gfx_color = (ch & 0x20) ? 0 : 1;
        m->disp_bank = (ch >> 4) & 1;
        // GDC 5MHz の機械（0054Dh bit5）では、400 ライン表示にするとグラフィック GDC を 5MHz にし（bit2、PITCH 80 バイト）、
        // 200 ライン表示に戻すと 2.5MHz（PITCH 40 ワード）に戻す
        if (mode == 3 && (m->ram[0x54D] & 0x24) == 0x20) { m->ram[0x54D] |= 0x04; m->gdc_clk5 = 1; m->gdcs.pitch = 80; }
        else if (mode != 3 && (m->ram[0x54D] & 0x24) == 0x24) { m->ram[0x54D] &= (uint8_t)~0x04; m->gdc_clk5 = 0; m->gdcs.pitch = 40; }
        return; }
    case 0x43: {   // パレット（デジタル）
        uint32_t a = lin(m->cpu.sr[DS_], BX(m));
        for (int i = 0; i < 4; i++) m->degpal[i] = mem_rb(m, a + i);
        return; }
    case 0x44: m->border = (uint8_t)(CX(m) >> 8); return;
    default:
        if (m->cfg.trace) plog("[bios] INT 18h AH=%02Xh 未対応\n", ah);
        return;
    }
}

// ---- INT 1Ch -----------------------------------------------------------------
static inline uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static void int1c(Machine* m) {
    uint8_t ah = AH(m);
    if (ah == 0x00) {
        PcTime lt; machine_now(m, &lt);
        uint32_t a = lin(m->cpu.sr[ES_], BX(m));
        mem_wb(m, a + 0, bcd(lt.year % 100));
        mem_wb(m, a + 1, (uint8_t)((lt.month << 4) | lt.wday));
        mem_wb(m, a + 2, bcd(lt.day));
        mem_wb(m, a + 3, bcd(lt.hour));
        mem_wb(m, a + 4, bcd(lt.min));
        mem_wb(m, a + 5, bcd(lt.sec));
        return;
    }
    if (ah == 0x02 || ah == 0x03) {
        // インターバルタイマ: CX*10ms 後に INT 07h（ES:BX を INT 07h に据える）
        set_vec(m, 0x07, m->cpu.sr[ES_], BX(m));
        m->ram[0x58A] = (uint8_t)CX(m); m->ram[0x58B] = (uint8_t)(CX(m) >> 8);
        io_out8(m, 0x77, 0x36);
        uint16_t cnt = 19968;   // 10ms @ 1.9968MHz
        io_out8(m, 0x71, (uint8_t)cnt); io_out8(m, 0x71, (uint8_t)(cnt >> 8));
        io_out8(m, 0x02, (uint8_t)(io_in8(m, 0x02) & ~1));
        return;
    }
}

// ---- INT 33h（マウスドライバ）-------------------------------------------------
// ---- INT 33h（マウスドライバ）----------------------------------------------------
//  MS-DOS 付属の MOUSE.SYS 相当。座標・ボタンは machine_mouse が直接更新する。
//  ・カーソル（AX=1/2/9）: 表示カウンタと形を持ち、画面への重ね描きは Player が行う（VRAM は汚さない）
//  ・イベントハンドラ（AX=0Ch/14h）: 実機のドライバと同じく IRQ13（INT 15h）をつなぎ、マウスの割込みの
//    たびに、起きた出来事がマスクに合えばユーザのハンドラを far call する（ROM の入口: 下の stub）
static const uint16_t k_arrow[32] = {
    0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F, 0x003F, 0x001F, 0x01FF, 0x10FF, 0x30FF, 0xF87F, 0xF87F, 0xFC3F,
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00, 0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000 };
static void mdrv_unhook(Machine* m) {
    MouseDrv* d = &m->mdrv;
    if (!d->hooked) return;
    // 自分が入れたままなら元に戻す（ゲームが後から付け替えていたら触らない）
    if (mem_rw(m, 0x15 * 4) == d->stub_off && mem_rw(m, 0x15 * 4 + 2) == ROMSEG) {
        m->ram[0x54] = (uint8_t)d->old15_off; m->ram[0x55] = (uint8_t)(d->old15_off >> 8);
        m->ram[0x56] = (uint8_t)d->old15_seg; m->ram[0x57] = (uint8_t)(d->old15_seg >> 8);
    }
    m->mouse.portc |= 0x10;   // マウスの割込みを止める
    d->hooked = 0;
}
static void mdrv_hook(Machine* m) {
    MouseDrv* d = &m->mdrv;
    if (d->hooked || !d->stub_off) return;
    d->old15_off = mem_rw(m, 0x15 * 4); d->old15_seg = mem_rw(m, 0x15 * 4 + 2);
    m->ram[0x54] = (uint8_t)d->stub_off; m->ram[0x55] = (uint8_t)(d->stub_off >> 8);
    m->ram[0x56] = (uint8_t)ROMSEG; m->ram[0x57] = (uint8_t)(ROMSEG >> 8);
    m->mouse.portc &= (uint8_t)~0x10;          // マウスの割込みを許す
    m->pic[1].imr &= (uint8_t)~0x20;           // スレーブの IR5（IRQ13）
    m->pic[0].imr &= (uint8_t)~0x80;           // カスケード
    pic_update_hint(m);
    d->hooked = 1;
}
static void mdrv_set_handler(Machine* m, uint16_t mask, uint16_t seg, uint16_t off) {
    MouseDrv* d = &m->mdrv;
    d->cb_mask = mask; d->cb_seg = seg; d->cb_off = off;
    d->events = 0; d->in_cb = 0;
    if (mask && (seg || off)) mdrv_hook(m); else mdrv_unhook(m);
}
static void mdrv_reset(Machine* m, bool hw) {
    Mouse* ms = &m->mouse;
    MouseDrv* d = &m->mdrv;
    d->show = -1;
    memcpy(d->shape, k_arrow, sizeof(k_arrow)); d->hot_x = 0; d->hot_y = 0;
    mdrv_set_handler(m, 0, 0, 0);
    d->sens_x = d->sens_y = d->sens_d = 50;
    ms->minx = 0; ms->maxx = 639; ms->miny = 0; ms->maxy = 399;
    if (hw) { ms->x = 320; ms->y = 200; }
    ms->mickey_x = 8; ms->mickey_y = 16;
    ms->hle_dx = ms->hle_dy = 0;
    for (int b = 0; b < 2; b++) ms->press_cnt[b] = ms->release_cnt[b] = 0;
}
// IRQ13 の入口から: EOI して、ハンドラを呼ぶなら AX..DI を用意して ZF=0
static void mdrv_irq(Machine* m) {
    for (int i = 0; i < 8; i++) if (m->pic[1].isr & (1u << i)) { m->pic[1].isr &= (uint8_t)~(1u << i); break; }
    if (!m->pic[1].isr) m->pic[0].isr &= (uint8_t)~0x80;
    pic_update_hint(m);
    MouseDrv* d = &m->mdrv;
    Mouse* ms = &m->mouse;
    uint16_t ev = (uint16_t)(d->events & d->cb_mask);
    d->events = 0;
    if (!ev || d->in_cb || !d->cb_mask) { set_zf(m, true); return; }
    uint32_t p = lin(ROMSEG, d->ptr_off);
    m->ram[p] = (uint8_t)d->cb_off; m->ram[p + 1] = (uint8_t)(d->cb_off >> 8);
    m->ram[p + 2] = (uint8_t)d->cb_seg; m->ram[p + 3] = (uint8_t)(d->cb_seg >> 8);
    SETAX(m, ev);
    SETBX(m, (uint16_t)((ms->buttons & 1) | ((ms->buttons & 2) ? 2 : 0)));
    SETCX(m, (uint16_t)ms->x); SETDX(m, (uint16_t)ms->y);
    m->cpu.r[ESI] = (m->cpu.r[ESI] & 0xFFFF0000u) | (uint16_t)d->mick_x;
    m->cpu.r[EDI] = (m->cpu.r[EDI] & 0xFFFF0000u) | (uint16_t)d->mick_y;
    d->in_cb = 1;
    set_zf(m, false);
}
static void int33(Machine* m) {
    Mouse* ms = &m->mouse;
    MouseDrv* d = &m->mdrv;
    uint16_t ax = AX(m);
    auto clamp_pos = [&]() {
        if (ms->x < ms->minx) ms->x = ms->minx; if (ms->x > ms->maxx) ms->x = ms->maxx;
        if (ms->y < ms->miny) ms->y = ms->miny; if (ms->y > ms->maxy) ms->y = ms->maxy;
    };
    auto set_range = [&](bool xaxis) {
        int lo = (int16_t)CX(m), hi = (int16_t)DX(m);
        if (lo > hi) { int t = lo; lo = hi; hi = t; }
        if (xaxis) { ms->minx = lo; ms->maxx = hi; } else { ms->miny = lo; ms->maxy = hi; }
        clamp_pos();
    };
    switch (ax) {
    case 0x00: mdrv_reset(m, true); SETAX(m, 0xFFFF); SETBX(m, 2); return;      // リセット（ボタン 2 つ）
    case 0x21: mdrv_reset(m, false); SETAX(m, 0xFFFF); SETBX(m, 2); return;     // ソフトウェアリセット
    case 0x01: if (d->show < 0) d->show++; return;
    case 0x02: d->show--; return;
    case 0x03: {
        int b = (ms->buttons & 1) | ((ms->buttons & 2) ? 2 : 0);
        SETBX(m, (uint16_t)b); SETCX(m, (uint16_t)ms->x); SETDX(m, (uint16_t)ms->y); return; }
    case 0x04: ms->x = (int16_t)CX(m); ms->y = (int16_t)DX(m); clamp_pos(); return;
    case 0x05: case 0x06: {
        int b = BX(m) & 1;
        int bstate = (ms->buttons & 1) | ((ms->buttons & 2) ? 2 : 0);
        SETAX(m, (uint16_t)bstate);
        if (ax == 5) { SETBX(m, (uint16_t)ms->press_cnt[b]); SETCX(m, (uint16_t)ms->press_x[b]); SETDX(m, (uint16_t)ms->press_y[b]); ms->press_cnt[b] = 0; }
        else { SETBX(m, (uint16_t)ms->release_cnt[b]); SETCX(m, (uint16_t)ms->release_x[b]); SETDX(m, (uint16_t)ms->release_y[b]); ms->release_cnt[b] = 0; }
        return; }
    case 0x07: set_range(true); return;
    case 0x08: set_range(false); return;
    case 0x09: {   // グラフィックカーソルの形: BX,CX = ホットスポット, ES:DX = AND マスク 16 ワード + XOR マスク 16 ワード
        d->hot_x = (int16_t)BX(m); d->hot_y = (int16_t)CX(m);
        uint32_t a = lin(m->cpu.sr[ES_], DX(m));
        for (int i = 0; i < 32; i++) d->shape[i] = mem_rw(m, a + i * 2);
        return; }
    case 0x0A: return;   // テキストカーソル（使わない）
    case 0x0B: SETCX(m, (uint16_t)ms->hle_dx); SETDX(m, (uint16_t)ms->hle_dy); ms->hle_dx = ms->hle_dy = 0; return;
    case 0x0C: mdrv_set_handler(m, CX(m), m->cpu.sr[ES_], DX(m)); return;
    case 0x0F: ms->mickey_x = CX(m); ms->mickey_y = DX(m); return;
    case 0x10: case 0x11: set_range(ax == 0x10); return;   // NEC の MOUSE.COM: 10h = 横の範囲、11h = 縦の範囲
    case 0x13: return;   // 速度 2 倍のしきい値
    case 0x14: {   // ハンドラの入れ替え: 古いものを CX, ES:DX に返す
        uint16_t om = d->cb_mask, os = d->cb_seg, oo = d->cb_off;
        mdrv_set_handler(m, CX(m), m->cpu.sr[ES_], DX(m));
        SETCX(m, om); cpu_setsr(&m->cpu, ES_, os); SETDX(m, oo);
        return; }
    case 0x15: SETBX(m, 64); return;          // 状態の保存に要る大きさ
    case 0x16: case 0x17: return;              // 状態の保存・復元（何もしない）
    case 0x1A: d->sens_x = BX(m); d->sens_y = CX(m); d->sens_d = DX(m); return;
    case 0x1B: SETBX(m, (uint16_t)d->sens_x); SETCX(m, (uint16_t)d->sens_y); SETDX(m, (uint16_t)d->sens_d); return;
    case 0x1C: return;                          // 割込みの速さ
    case 0x1D: return;                          // 表示ページ
    case 0x1E: SETBX(m, 0); return;
    case 0x24: SETBX(m, 0x0700); m->cpu.r[ECX] = (m->cpu.r[ECX] & 0xFFFF0000u) | 0x0100; return;   // 版 7.00、バスマウス
    default:
        if (m->cfg.trace) plog("[bios] INT 33h AX=%04Xh 未対応\n", ax);
        return;
    }
}

// ---- INT DCh（MS-DOS の PC-98 拡張）-------------------------------------------
static void intdc(Machine* m) {
    uint8_t cl = (uint8_t)CX(m);
    if (cl == 0x10) {
        uint8_t ah = AH(m);
        con_load_state(m);
        switch (ah) {
        case 0x00: console_putc(m, (uint8_t)DX(m)); return;
        case 0x01: { uint32_t a = lin(m->cpu.sr[DS_], DX(m)); for (int i = 0; i < 4096; i++) { uint8_t c = mem_rb(m, a + i); if (c == '$') break; console_putc(m, c); } return; }
        case 0x02: s_con.attr = (uint8_t)DX(m); m->ram[0x71D] = s_con.attr; return;
        case 0x03: s_con.x = (uint8_t)DX(m); s_con.y = (uint8_t)(DX(m) >> 8); if (s_con.x > 79) s_con.x = 79; if (s_con.y >= s_con.rows) s_con.y = s_con.rows - 1; con_sync_cursor(m); return;
        case 0x0A: con_clear_range(m, s_con.y * 80 + s_con.x, s_con.y * 80 + 80); return;
        case 0x0A + 1: con_clear_range(m, 0, 80 * 25); return;
        default: return;
        }
    }
    if (cl == 0x12) {
        // MS-DOS の製品番号（拡張機能があるか）: AX に 0 以外を返す。0 だと古い DOS とみなして、
        // ドライブの DA/UA 一覧を 0000:066Ch から直接読むソフトがある（同級生の INSTALL.EXE は、その道で
        // DS を 0 にしたまま自分のデータ域を読み、DGROUP の先頭を壊して R6001 で終わる）
        SETAX(m, 0x0001); SETDX(m, 0x0500);
        return;
    }
    if (cl == 0x13) {
        // ドライブの DA/UA 一覧（MS-DOS 5.0 以降）: DS:DX に 96 バイト
        //  +0: A-P の DA/UA（1 バイトずつ）, +1Ah: A-Z の {属性, DA/UA}（2 バイトずつ）
        // インストーラが「今のドライブがフロッピーか」「ハードディスクはどれか」を調べるのに使う（下級生など）
        uint32_t a = lin(m->cpu.sr[DS_], DX(m));
        for (int i = 0; i < 96; i++) mem_wb(m, a + i, 0);
        for (int i = 0; i < 16; i++) {
            uint8_t da = m->ram[0x66C + i];
            mem_wb(m, a + i, da);
            if (da) { mem_wb(m, a + 0x1A + i * 2, 0x00); mem_wb(m, a + 0x1A + i * 2 + 1, da); }
        }
        if (m->cfg.trace) plog("[bios] INT DCh CL=13h（ドライブの DA/UA 一覧）\n");
        return;
    }
    // 0Ch/0Dh: ファンクションキー・編集キーの割り当ての取得/設定（DOS のコンソール入力が使う）
    if (cl == 0x0C) { dos_keytab_get(m, (uint16_t)m->cpu.r[EAX], lin(m->cpu.sr[DS_], DX(m))); return; }
    if (cl == 0x0D) { dos_keytab_set(m, (uint16_t)m->cpu.r[EAX], lin(m->cpu.sr[DS_], DX(m))); return; }
}

// ---- 振り分け ----------------------------------------------------------------
void bios_hle(Machine* m, uint8_t n) {
    switch (n) {
    case HLE_INT18: int18(m); return;
    case HLE_INT1C: int1c(m); return;
    case HLE_INT09: bios_key_irq(m); machine_eoi(m, 1); return;
    case HLE_IRQ_EOI_M: {
        // 受け付け中の最上位を落とす
        for (int i = 0; i < 8; i++) if (m->pic[0].isr & (1u << i)) { m->pic[0].isr &= (uint8_t)~(1u << i); break; }
        pic_update_hint(m);
        return; }
    case HLE_IRQ_EOI_S: {
        for (int i = 0; i < 8; i++) if (m->pic[1].isr & (1u << i)) { m->pic[1].isr &= (uint8_t)~(1u << i); break; }
        if (!m->pic[1].isr) m->pic[0].isr &= (uint8_t)~0x80;
        pic_update_hint(m);
        return; }
    case HLE_INT1A: SETAH(m, 0x00); return;
    case HLE_INT1B:
        if (floppy::is_fd_da(AL(m))) { floppy::bios_int1b(m); return; }
        {
            // ハードディスク（SASI/IDE, DA 80h・00h のユニット 0 = ゲームのフォルダ）: 「付いている」ことだけ答える。
            // インストーラがハードディスクの有無を SENSE（AH=x4h）で調べるため（下級生など）。
            // セクタの読み書きはできない（ゲームのフォルダは DOS のドライブとしてだけ見せる）
            uint8_t al = AL(m), cmd = (uint8_t)(AH(m) & 0x0F);
            if ((al == 0x80 || al == 0x00)) {
                uint8_t st = 0x60;
                if (cmd == 0x04) st = 0x0F;                           // SENSE: 付いている（容量の種類）
                else if (cmd == 0x03 || cmd == 0x07 || cmd == 0x0E) st = 0x00;   // 初期化・再較正・モード設定
                if (m->cfg.trace) plog("[hd] INT1B AX=%04X → 結果 %02Xh\n", (unsigned)(m->cpu.r[EAX] & 0xFFFF), st);
                SETAH(m, st); set_cf(m, st >= 0x20); return;
            }
        }
        if (m->cfg.trace) plog("[fd] INT1B AX=%04X（フロッピー以外の装置: 未接続を返す）\n", (unsigned)(m->cpu.r[EAX] & 0xFFFF));
        SETAH(m, 0x60); set_cf(m, true); return;
    case HLE_INT1F: {
        // INT 1Fh: AH=90h は 1MB より上とのブロック転送（ES:BX+10h/+18h の記述子にある 24 ビットの
        // ベースと上限、SI/DI がそれぞれのオフセット、CX がバイト数）
        uint8_t ah = AH(m);
        if (!(ah & 0x80)) return;
        if (ah == 0xCC) { set_cf(m, true); return; }
        if (!(ah & 0x10)) { set_cf(m, false); return; }
        if (ah != 0x90) return;
        uint32_t t = ((uint32_t)m->cpu.sr[ES_] << 4) + (uint16_t)BX(m);
        uint8_t w[16];
        for (int i = 0; i < 16; i++) w[i] = mem_rb(m, t + 0x10 + i);
        uint32_t slim = (uint32_t)(w[0] | (w[1] << 8)) + 1, dlim = (uint32_t)(w[8] | (w[9] << 8)) + 1;
        uint32_t sbase = w[2] | (w[3] << 8) | ((uint32_t)w[4] << 16), dbase = w[10] | (w[11] << 8) | ((uint32_t)w[12] << 16);
        uint32_t so = (uint16_t)m->cpu.r[ESI], dof = (uint16_t)m->cpu.r[EDI];
        uint32_t n = (uint32_t)(uint16_t)(CX(m) - 1) + 1;
        if (so >= slim || dof >= dlim || so + n > slim || dof + n > dlim) { set_cf(m, true); return; }
        uint32_t keep = g_addr_mask; g_addr_mask = A20_ON_MASK;
        for (uint32_t i = 0; i < n; i++) mem_wb(m, dbase + dof + i, mem_rb(m, sbase + so + i));
        g_addr_mask = keep;
        { static int k = 0; if (m->cfg.trace && k < 16) { k++; plog("[bios] INT1F AH=90 %06X → %06X（%u バイト）\n", sbase + so, dbase + dof, n); } }
        set_cf(m, false);
        return; }
    case HLE_INT33: if (m->cfg.trace > 1) plog("[mouse] INT 33h AX=%04X BX=%04X CX=%04X DX=%04X\n", AX(m), BX(m), CX(m), DX(m)); int33(m); return;
    case HLE_MSIRQ: mdrv_irq(m); return;
    case HLE_MSIRQ_END: m->mdrv.in_cb = 0; return;
    case HLE_INTDC: intdc(m); return;
    case HLE_SNDBIOS: soundbios_hle(m); return;
    case HLE_INT06: {
        uint32_t a = lin(m->cpu.sr[SS_], (uint16_t)m->cpu.r[ESP]);
        uint16_t ip = mem_rw(m, a), cs = mem_rw(m, a + 2);
        plog("[bios] 未定義命令で停止しました: %04X:%04X (%02X %02X %02X)\n", cs, ip,
             mem_rb(m, lin(cs, ip)), mem_rb(m, lin(cs, (uint16_t)(ip + 1))), mem_rb(m, lin(cs, (uint16_t)(ip + 2))));
        m->status = "未対応の CPU 命令に遭遇しました";
        m->quit = 2;
        return; }
    case HLE_NOP: return;
    default:
        if (n >= HLE_LIO && n < HLE_LIO + 16) { lio_hle(m, n); return; }
        if (m->cfg.trace) plog("[bios] HLE %02X 未対応\n", n);
        return;
    }
}

// ---- 初期化 ------------------------------------------------------------------
void bios_init(Machine* m) {
    s_stub_ptr = STUB_BASE;
    uint8_t* r = m->ram;
    // 既定: 全ベクタは IRET
    uint8_t iret[1] = {0xCF};
    uint16_t iret_off = put_stub(m, iret, 1);
    for (int v = 0; v < 256; v++) set_vec(m, v, ROMSEG, iret_off);

    // ハードウェア割込みの既定（EOI して戻る）
    uint16_t eoi_m = bios_hle_stub(m, HLE_IRQ_EOI_M);
    uint16_t eoi_s = bios_hle_stub(m, HLE_IRQ_EOI_S);
    for (int v = 0x08; v < 0x10; v++) set_vec(m, v, ROMSEG, eoi_m);
    for (int v = 0x10; v < 0x18; v++) set_vec(m, v, ROMSEG, eoi_s);

    // IRQ0: BIOS インターバルタイマ
    static const uint8_t timer_code[] = {
        0x50, 0x1E, 0x31, 0xC0, 0x8E, 0xD8,       // push ax; push ds; xor ax,ax; mov ds,ax
        0xB0, 0x20, 0xE6, 0x00,                   // mov al,20h; out 00h,al
        0xFF, 0x0E, 0x8A, 0x05,                   // dec word [058Ah]
        0x75, 0x08,                               // jnz done
        0xE4, 0x02, 0x0C, 0x01, 0xE6, 0x02,       // in al,02h; or al,1; out 02h,al
        0xCD, 0x07,                               // int 07h
        0x1F, 0x58, 0xCF                          // done: pop ds; pop ax; iret
    };
    set_vec(m, 0x08, ROMSEG, put_stub(m, timer_code, sizeof(timer_code)));
    bios_hook_vec(m, 0x09, HLE_INT09);
    bios_hook_vec(m, 0x06, HLE_INT06);
    bios_hook_vec(m, 0x18, HLE_INT18);
    bios_hook_vec(m, 0x1A, HLE_INT1A);
    bios_hook_vec(m, 0x1B, HLE_INT1B);
    bios_hook_vec(m, 0x1C, HLE_INT1C);
    bios_hook_vec(m, 0x1F, HLE_INT1F);
    bios_hook_vec(m, 0xDC, HLE_INTDC);
    if (m->cfg.emulate_mouse) {
        bios_hook_vec(m, 0x33, HLE_INT33);
        // マウスのイベントハンドラを呼ぶ IRQ13 の入口:
        //   pusha; push ds; push es; HLE MSIRQ; jz skip; call far [cs:ptr]; HLE MSIRQ_END; skip: pop es; pop ds; popa; iret
        static const uint8_t z4[4] = {0, 0, 0, 0};
        m->mdrv.ptr_off = put_stub(m, z4, 4);
        uint8_t code[] = {0x60, 0x1E, 0x06, 0xF1, HLE_MSIRQ, 0x74, 0x07, 0x2E, 0xFF, 0x1E,
                          (uint8_t)m->mdrv.ptr_off, (uint8_t)(m->mdrv.ptr_off >> 8), 0xF1, HLE_MSIRQ_END, 0x07, 0x1F, 0x61, 0xCF};
        m->mdrv.stub_off = put_stub(m, code, sizeof(code));
        mdrv_reset(m, true);
    }

    // BIOS ワークエリア
    r[0x500] = 0x00;
    r[0x501] = 0x80 | 0x04;         // 8MHz 系, メモリ 640KB
    r[0x53C] = 0x00;
    r[0x54C] = 0x4E;                // 16 色ボードあり 等
    r[0x54D] = (uint8_t)(0x40 | (m->cfg.gdc_5mhz ? 0x20 : 0x00));   // EGC あり / bit5 = GDC 5MHz を使える（DIP SW 2-8）/ bit2 = いま 5MHz
    r[0x480] = 0x00;
    // 拡張メモリの大きさ（0401h: 1MB より上を 128KB 単位、0594h: 16MB より上を 1MB 単位）
    r[0x401] = (uint8_t)((m->ram_size - 0x100000u) / 0x20000u);
    if (m->ram_size - 0x100000u <= 0x10000u) r[0x401] = 0;
    r[0x594] = 0;
    r[0x481] = 0x00;
    r[0x458] = 0x00;
    r[0x55D] = 0x01;               // ハードディスク（SASI/IDE）がユニット 0 に付いている（ゲームのフォルダ）
    r[0x55C] = 0x03;               // 1MB の FDD がユニット 0・1 に付いている（仮想フロッピーはユニット 0）
    r[0x712] = 24;                 // MS-DOS: テキストの行数-1（master.lib などが参照）
    r[0x71D] = 0xE1;               // 同: 今の表示属性（白）。Borland C の conio などが初期値として読む
    r[0x714] = 0xE1;               // 同: 画面消去に使う属性
    r[0x710] = 0;                  // 同: カーソル表示など
    kb_clear(m);
    console_reset(m);
    // テキスト VRAM を空白に
    for (int i = 0; i < 0x1000; i++) { m->tvram[i * 2] = 0x20; m->tvram[i * 2 + 1] = 0; m->tvram[0x2000 + i * 2] = 0xE1; }
    // リセットベクタ（使わないが念のため）
    r[0xFFFF0] = 0xF4;
    lio_init(m);
    soundbios_init(m);
    // PIT のチャンネル 1（ブザーの音程）は BIOS がモード 3・下位/上位の順で 2kHz（998）に設定しておく。
    // 制御語を書かずに 3FDBh へ下位・上位を書くソフト（MIMPI の BEEP 演奏など）がある
    m->pit[1].mode = 3; m->pit[1].access = 3; m->pit[1].reload = m->pit[1].counter = 998; m->pit[1].wr_hi = 0;
    // ROM の識別に使われることがある領域
    r[0xFFFFE] = 0xFE;
}

// ---- ブートモード（フロッピーの IPL から起動する） ---------------------------------
//  PC-98 の起動 ROM と同じく、ユニット 0 の C=0 H=0 R=1 から IPL を読み込んで実行を始める。
//   ・まず MFM（倍密度）で ID を探し、無ければ FM（単密度）で探す
//   ・MFM で N≧1 なら 1024 バイトを 1FC0:0000 へ、FM（N88-BASIC 形式など）や N=0 なら 512 バイトを 1FE0:0000 へ
//  MS-DOS は用意しない。BIOS（INT 18h・1Bh・1Ch など）だけで動くディスク向け。
//  ROM の N88-BASIC は持っていないので、ROM の領域を「踏んだら止まる」命令で埋めておき、
//  そこへ飛び込んできたら（＝BASIC を使うソフト）知らせて止める。
static const uint32_t k_basic_lo = 0xE8000, k_basic_hi = 0xFD000;   // N88-BASIC(86) の ROM のあたり
static const uint32_t k_rom_hi = 0xFFFF0;
static uint32_t s_rom_used_lo, s_rom_used_hi;                          // 自前の入口（スタブ）を置いた範囲
static bool rom_fill_area(uint32_t a) {
    if (a < k_basic_lo || a >= k_rom_hi) return false;
    if (a >= s_rom_used_lo && a < s_rom_used_hi) return false;
    if (a >= 0xF9900 && a < 0xF9A00) return false;                    // LIO の入口
    return true;
}
bool bios_rom_trap(Machine* m) {
    uint32_t a = ((uint32_t)m->cpu.op_cs << 4) + m->cpu.op_ip;
    a &= 0xFFFFF;
    if (!rom_fill_area(a)) return false;
    plog("[boot] ROM の %05X へ飛び込みました（%04X:%04X）\n", a, m->cpu.op_cs, m->cpu.op_ip);
    if (a < k_basic_hi) {
        m->status = "このソフトをプレイするにはPC98実機またはそのROMデータを搭載した仮想マシンが必要です";
        m->quit = 3;
    } else {
        char b[160];
        snprintf(b, sizeof(b), "ソフトが BIOS の ROM（%05X）を直接呼び出しました。PC98PLAYER はこの呼び出し方に対応していません。", a);
        m->status = b;
        m->quit = 2;
    }
    m->cpu.halted = 1;
    return true;
}

bool bios_boot_fd(Machine* m, std::string* why) {
    uint8_t* r = m->ram;
    // ROM の空き（自前の入口・LIO 以外）を「HLE 命令」で埋める。どこから実行しても F1 xx で止まる
    s_rom_used_lo = (uint32_t)ROMSEG << 4;
    s_rom_used_hi = s_rom_used_lo + s_stub_ptr + 16;
    for (uint32_t a = k_basic_lo; a < k_rom_hi; a++) if (rom_fill_area(a)) r[a] = 0xF1;
    r[0x55C] = 0x03;               // 1MB の FDD がユニット 0・1 に付いている
    r[0x55D] = 0x00;               // ハードディスクは無し
    r[0x584] = 0x90;               // 起動したドライブ（DA/UA）: 1MB FDD のユニット 0
    r[0x712] = 0; r[0x71D] = 0; r[0x714] = 0;
    FloppyImage* im = floppy::image_unit(0);
    if (!im) { if (why) *why = "1 台目のドライブにフロッピーが入っていません"; return false; }
    FdTrack* t = im->track(0, 0);
    const FdSector* id = nullptr;
    if (t) for (auto& x : t->secs) if (!x.fm && x.c == 0 && x.h == 0 && x.r == 1) { id = &x; break; }
    if (!id && t) for (auto& x : t->secs) if (x.fm && x.c == 0 && x.h == 0 && x.r == 1) { id = &x; break; }
    if (!id) { if (why) *why = "起動用のセクタ（トラック 0 の 1 番）がありません"; return false; }
    bool fm = id->fm;
    uint8_t N = id->n;
    uint32_t pos = 0x1FC00, remain = 0x400;
    if (!N || fm || im->media == FD_144) { pos = 0x1FE00; remain = 0x200; }
    uint16_t seg = (uint16_t)(pos >> 4);
    // 中身がすべて同じ値（データ用のディスク・未使用）なら、起動できるディスクではない
    {
        bool same = true;
        for (size_t i = 1; i < id->data.size() && same; i++) same = id->data[i] == id->data[0];
        if (same) { if (why) *why = "起動ディスクではありません（起動用のセクタが空です）"; return false; }
    }
    for (uint8_t R = 1; remain > 0; R++) {
        const FdSector* s = nullptr;
        for (auto& x : t->secs) if (x.c == 0 && x.h == 0 && x.r == R && x.n == N && x.fm == fm) { s = &x; break; }
        if (!s) break;
        uint32_t size = 128u << (N < 3 ? N : 3);
        uint32_t n = remain < size ? remain : size;
        for (uint32_t i = 0; i < n; i++) r[pos + i] = i < s->data.size() ? s->data[i] : 0;
        pos += n; remain -= n;
    }
    plog("[boot] IPL を %04X:0000 へ読み込みました（%s, N=%d, %s）\n", seg, fm ? "FM" : "MFM", N, im->path.c_str());
    // PIC の初期マスク（キーボードとスレーブ連結、それと FDC の 640KB/1MB（スレーブの IR2・IR3 = INT 41h・42h）を開ける。
    // 実機の BIOS はフロッピーを割込みで動かすので FDC は開いたまま。ディスクの入れ替えの割込みを見るソフトがある）
    m->pic[0].imr = 0x7D;
    m->pic[1].imr = 0xF3;
    pic_update_hint(m);
    Cpu* c = &m->cpu;
    for (int i = 0; i < 8; i++) c->r[i] = 0;
    c->r[ESP] = 0x0000;
    cpu_setsr(c, CS_, seg); cpu_setsr(c, DS_, 0); cpu_setsr(c, ES_, 0); cpu_setsr(c, SS_, 0x1FC0);
    c->ip = 0;
    c->fl |= FL_IF;
    c->r[EAX] = 0x0090;            // AL = 起動したドライブ
    return true;
}

// ---- ステートセーブ ----------------------------------------------------------
#include "state.h"
void bios_state_save(StateW& w) { w.tag("BIOS"); w.pod(s_con); }
void bios_state_load(StateR& r) { r.tag("BIOS"); r.pod(s_con); }
