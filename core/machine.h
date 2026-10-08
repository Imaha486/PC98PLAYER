// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  machine.h  --  仮想 PC-9801 本体
//
//  「エミュレータを丸ごと作る」のではなく、PC-98 のゲームが触る部分だけを
//  ハードウェア（GDC/GRCG/EGC/パレット/8253/8259/キーボード/マウス/OPNA）と
//  HLE（BIOS INT 18h 等、MS-DOS INT 21h）で用意する。ROM は一切要らない。
//  GMPV3 Studio の「必要なデバイスの振る舞いと DOS ファンクションだけを
//  自前で実装する」考え方を、画面と入力まで広げたもの。
// -----------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <string>
#include "cpu.h"
#include "memio.h"

// ---- 時間軸 ------------------------------------------------------------------
// 基準クロック = OPNA クロック = 7.9872MHz。8253 はその 1/4（1.9968MHz）。
#define MASTER_CLOCK     7987200u
#define LINE_TICKS       322           // 24.8kHz 水平周期（≒ 7987200/24830）
#define FRAME_LINES      440
#define DISPLAY_LINES    400
#define FRAME_TICKS      (LINE_TICKS * FRAME_LINES)   // ≒ 56.4Hz

struct Pic {
    uint8_t imr, irr, isr, base;
    uint8_t icw_step, icw4_needed, read_isr, aeoi;
};

struct Pit {
    uint16_t reload, counter, latch;
    uint8_t  mode, access, wr_hi, rd_hi, latched, armed;
    uint8_t  status_latched, status;
};

struct Gdc {
    uint8_t  cmd;              // 実行中のコマンド
    int      pcount;           // 受け取ったパラメータ数
    uint8_t  params[16];
    uint8_t  pram[16];
    int      pram_ptr;
    uint8_t  display;          // START/STOP
    uint8_t  zoom;             // ZOOM の表示倍率（下位 4bit が倍率-1）
    uint8_t  csrform[3];
    uint8_t  sync[8];
    uint8_t  pitch;
    uint32_t ead;              // CSRW で設定したアドレス
    uint8_t  dad;
    // 描画コマンド（VECTW/VECTE/WRITE）用
    uint8_t  vect[11];
    uint8_t  mode_write;       // WRITE の種類（REPLACE/COMPLEMENT/CLEAR/SET）
    uint16_t pattern;          // TEXTW で設定したパターン
    std::vector<uint8_t> fifo; // 読み出し用
};

struct Egc {
    uint16_t access, fgbg, ope, fg, mask, bg, sft, leng;
    uint16_t lastvram[4];
    uint16_t patreg[4];
    uint16_t fgc[4], bgc[4];
    int      func;
    uint32_t remain;
    uint32_t stack;
    uint8_t  buf[4096 / 8 + 4 * 4];
    uint8_t* inptr[4];
    uint8_t* outptr[4];
    uint16_t mask2;
    uint16_t srcmask;
    uint8_t  srcbit, dstbit, sft8bitl, sft8bitr;
    uint32_t inptr_off, outptr_off;
    uint8_t  vram_src[4][2], vram_data[4][2];
};

struct Opna {
    uint8_t  addr[2];
    uint8_t  reg[2][256];
    uint8_t  status;
    uint8_t  irq_enable_a, irq_enable_b, timer_a_run, timer_b_run;
    uint8_t  prescale;
    int32_t  timer_a_period, timer_a_count;
    int32_t  timer_b_period, timer_b_count;
    uint8_t  keyon[8];          // 28h で最後に書いたスロットの組（ch 0-2, 4-6）。ロード時の鳴らし直し用
};

struct RegWrite { uint64_t tick; uint8_t part, addr, val; };

// PC-9801-86 の PCM（pcm86.cpp）
struct Pcm86 {
    uint8_t  ctrl;          // A468h（bit4 の割込みフラグは irqflag に分けて持つ）
    uint8_t  dactrl;        // A46Ah（ctrl bit5=0 のとき）
    uint8_t  vol;           // A466h の音量（0..15）
    uint8_t  irqflag;
    uint16_t thresh;        // 割込みを起こす FIFO 残量
    uint16_t rpos, wpos;
    int32_t  count;
    uint64_t acc;
    uint8_t  fifo[0x8000];
};
struct PcmSample { uint64_t tick; int16_t l, r; };

// MPU-PC98II（MPU-401 互換）。UART モードと、インテリジェントモードのうち
// 「MIDI データの直接送信（D0h-DFh）」とバージョン問い合わせだけを扱う
struct Mpu {
    uint8_t  uart;
    uint8_t  recv[16];
    uint8_t  rcnt, rpos;
    uint8_t  pending_cmd;           // D0h-DFh の直後
    uint8_t  last;                  // 最後に読ませた値
    // インテリジェントモード
    uint8_t  expect;                // 次のデータ（E0h-EFh の値）を待っているコマンド
    uint8_t  msg_left;              // D0h-D7h の後、送る MIDI メッセージの残りバイト数
    uint8_t  sysex;                 // DFh の後、F7h まで送る
    uint8_t  run_status;            // ランニングステータス
    uint8_t  tempo;                 // E0h（既定 100）
    uint8_t  timebase;              // C2h-C8h（既定 120）
    uint8_t  cth_rate;              // E7h（既定 240）: 内部クロック rate/4 ごとに FDh
    uint8_t  cth_on;                // 95h/94h
    uint8_t  rel_tempo;             // E1h 相対テンポ（40h=等倍。0 は旧セーブで 40h 扱い）
    int64_t  cth_acc;               // 次の FDh までの基準クロック
};

struct Config {
    std::string root;            // ゲームのフォルダ（ホストのパス）
    std::string start;           // 最初に実行するもの
    std::string args;
    char     drive = 'A';
    int      cpu_mhz = 16;
    int      gdc_5mhz = 0;          // DIP SW 2-8: グラフィック GDC を 5MHz で使える（0 = 2.5MHz の機械）
    int      sound_irq = 12;     // 既定は PC-9801-86 の INT5
    int      sound_board = 86;   // 86 / 26 / 0
    bool     sound_bios = false; // CC00h にサウンド BIOS（INT D2h）を置く（SoundBIOS=1）
    bool     fm_enable = true;
    int      memory_kb = 640;
    bool     emulate_mouse = true;
    bool     idle_skip = true;       // 割込み待ちの空回りを止めて待つ（IdleSkip=）
    int      key_repeat = 1;
    int      trace = 0;
    int      dos_version = 0x0500;   // 5.00（上位=メジャー）
    int      fake_year = 0;          // 0 以外: 年だけこの値に置き換える（2000 年問題対策）
    std::string floppy_image;        // 起動時に入れるフロッピーイメージ（空なら無し）
    std::string floppy_image2;       // ブートモード: 2 台目のドライブに入れるイメージ
    bool     boot_fd = false;        // ブートモード: MS-DOS を使わず、フロッピーの IPL から起動する
    char     floppy_drive = 'B';     // フロッピーのドライブ名
    char     floppy_drive2 = 0;      // 2 台目のフロッピーのドライブ名（0 = DOS からは見せない）
    char     current_drive = 0;      // 起動時のカレントドライブ（0 = Start= のドライブ、無ければゲームのドライブ）
    std::string current_dir;         // 起動時のカレントディレクトリ（例 A:\NANPA\。空 = 指定なし）
    int      free_mb = 96;           // ゲームのドライブの空き容量として見せる大きさ（MB）
    int      fake_date = 0;          // 0 以外: YYYYMMDD。起動した日をこの日付として、以後は実時間で進める
    int      first_mcb = 0x0200;     // 先頭 MCB のセグメント（実機の DOS に近い位置へ）
    int      midi = 0;               // PC-9801-86 の PCM（pcm86.cpp）
struct Pcm86 {
    uint8_t  ctrl;          // A468h（bit4 の割込みフラグは irqflag に分けて持つ）
    uint8_t  dactrl;        // A46Ah（ctrl bit5=0 のとき）
    uint8_t  vol;           // A466h の音量（0..15）
    uint8_t  irqflag;
    uint16_t thresh;        // 割込みを起こす FIFO 残量
    uint16_t rpos, wpos;
    int32_t  count;
    uint64_t acc;
    uint8_t  fifo[0x8000];
};
struct PcmSample { uint64_t tick; int16_t l, r; };

// MPU-PC98II（E0D0h）を載せる。0 = 無し
    int      midi_irq = 6;           // INT2
    int      midi_speed = 100;       // MIDI の演奏速度の補正（%）。MPU のクロックだけを速める
    bool     ems = true;             // EMM386 相当（LIM EMS 4.0、ページフレーム D000h）
    int      ems_kb = 4096;
    int      xms_kb = 8192;          // HIMEM.SYS 相当（XMS 3.0）。0 で無し
};

// マウスの状態（バスマウス）
struct Mouse {
    int      acc_x, acc_y;       // 前回ラッチから溜まった移動量
    int8_t   lat_x, lat_y;
    uint8_t  buttons;            // bit0=左 bit1=右（1=押下）
    uint8_t  portc;              // 7FDDh の値
    uint8_t  freq;               // BFDBh
    int32_t  timer;
    // INT 33h 側
    int      x, y, minx, maxx, miny, maxy;
    int      mickey_x, mickey_y;
    int      hle_dx, hle_dy;
    int      press_cnt[2], release_cnt[2];
    int      press_x[2], press_y[2], release_x[2], release_y[2];
    int      visible;
};

// INT 33h（マウスドライバ）の HLE の状態
struct MouseDrv {
    int      show;                 // 表示カウンタ（0 で表示、負で非表示。AX=1 で +1、AX=2 で -1）
    uint16_t shape[32];            // カーソルの形: AND マスク 16 ワード + XOR マスク 16 ワード（AX=9）
    int16_t  hot_x, hot_y;
    uint16_t cb_mask, cb_off, cb_seg;   // ユーザのイベントハンドラ（AX=0Ch/14h）
    uint16_t events;               // 溜まった出来事（bit0 移動, 1 左押す, 2 左離す, 3 右押す, 4 右離す）
    uint8_t  hooked, in_cb;        // IRQ13（INT 15h）をつないでいる / ハンドラを呼んでいる最中
    uint16_t old15_off, old15_seg;
    uint16_t stub_off, ptr_off;    // ROM の割込みの入口、ハンドラの far ポインタの置き場所
    int      mick_x, mick_y;       // 累計の移動量（ミッキー）
    int      sens_x, sens_y, sens_d;
};

struct Machine {
    Cpu      cpu;
    uint8_t* ram;               // PC98_RAM_SIZE
    uint8_t  tvram[0x4000];
    uint8_t  gvram[2][4][0x8000];

    Pic      pic[2];
    Pit      pit[3];
    int32_t  pit_frac;
    Gdc      gdcm, gdcs;
    Egc      egc;
    Opna     opna;
    Mpu      mpu;
    Pcm86    pcm86;
    std::vector<PcmSample> pcm_out;  // PCM86 の出力（フレームごとに Player が取り出す）
    std::vector<uint8_t> midi_out;   // ホストへ送る MIDI バイト列（フレームごとに取り出す）
    Mouse    mouse;
    uint8_t  joy = 0;           // ジョイスティック（ホストが毎フレーム設定。1=押している: bit0 上 1 下 2 左 3 右 4 A 5 B）
    MouseDrv mdrv;
    Config   cfg;

    // 画面
    uint8_t  analog;            // 16 色モード
    uint8_t  pal[16][3];        // G,R,B（4bit）
    uint8_t  palidx;
    uint8_t  degpal[4];
    uint8_t  disp_bank, draw_bank;
    uint8_t  modeff[8];         // 68h
    uint8_t  modeff2[8];        // 6Ah の各 bit
    uint8_t  gfx_200;           // 200 ライン表示
    uint8_t  gfx_200_lower;     // 200 ラインで下半分を表示
    uint8_t  gfx_color;         // カラー / モノクロ
    uint8_t  gdc_clk5;          // グラフィック GDC がいま 5MHz（PITCH はバイト単位＝ 8 ドット）。0 = 2.5MHz（ワード単位）
    uint8_t  mpu_off_noted;     // MIDI=0 のときに MPU を探しに来たことを記録したか（Trace の案内を 1 回だけ）
    uint8_t  border;
    uint8_t  egc_enabled;

    // GRCG
    uint8_t  grcg_mode, grcg_tile[4], grcg_idx;

    // CG ROM
    uint16_t cg_code;
    uint8_t  cg_line, cg_left;

    // キーボード（8251）
    std::vector<uint8_t> kb_queue;
    uint8_t  kb_data, kb_ready;
    int32_t  kb_delay;
    uint8_t  kb_down[128];

    // システムポート
    uint8_t  portc;              // 35h（8255 ポート C）
    uint8_t  beep_on;
    uint8_t  a20;

    // 時間
    uint64_t ticks;              // 基準クロックでの通算
    uint32_t frame_pos;          // フレーム内位置（基準クロック）
    uint64_t frame_count;
    uint8_t  vsync_armed;
    uint32_t cpu_hz;
    uint64_t cyc_acc;

    // 音
    std::vector<RegWrite> regw;  // レンダラへ渡す書き込み
    uint64_t audio_tick;         // ここまで音を作った
    std::vector<std::pair<uint64_t,int>> beepw;   // ビープの変化

    // 状態
    int      quit;               // ゲームが終わった（シェルが最後まで行った）
    std::string status;
    unsigned unknown_io;
    uint32_t fd_seen[2];     // フロッピーの入れ替えを見張る（floppy::change_count_unit の前回の値）
    int32_t  fd_irq_wait[2]; // 入れ替えの割込みを出すまでの待ち（MASTER_CLOCK）
    uint8_t  fd_irq_stage[2];// 2 = 抜いた割込みを出す / 1 = 入れた割込みを出す / 0 = なし
};

extern Machine* g_m;

// machine.cpp
Machine* machine_create(const Config& cfg);
void     machine_destroy(Machine* m);
void     machine_run_frame(Machine* m);          // 1 フレームぶん進める
void     machine_raise_irq(Machine* m, int irq);
void     machine_eoi(Machine* m, int irq);
void     machine_key(Machine* m, uint8_t scancode, bool down);
void     machine_mouse(Machine* m, int dx, int dy, int buttons);
void     machine_joystick(Machine* m, uint8_t bits);   // bits: machine.joy と同じ並び
void     pic_update_hint(Machine* m);
void     opna_write(Machine* m, int part, uint8_t addr, uint8_t val);
int      pit0_hz_ok(Machine* m);

// video.cpp
struct FontSource {
    // JIS 区点コード（0x2121..0x7E7E）の 16x16 を 32 バイトで返す（行ごと 左, 右）
    void (*kanji)(void* user, uint16_t jis, uint8_t out[32]);
    // ANK（0x00..0xFF）の 8x16 を 16 バイトで返す
    void (*ank)(void* user, uint8_t code, uint8_t out[16]);
    // 全角の字（JIS）を 8x16 に縦長で描く（任意。無ければ全角を横に畳んで作る）
    void (*narrow)(void* user, uint16_t jis, uint8_t out[16]);
    void* user;
};
void video_set_font(const FontSource& fs);
void video_render(Machine* m, uint32_t* out /* 640x400 ARGB */);
const uint8_t* font_get_kanji(uint16_t jis);   // 32 バイト
const uint8_t* font_get_ank(uint8_t c);        // 16 バイト
uint16_t sjis_to_jis(uint16_t sj);
// fontrom.cpp（擬似漢字 ROM）
void fontrom_set_jis78(bool on);
bool fontrom_is_gaiji(uint16_t jis);
void fontrom_gaiji_write(uint16_t jis, int line, bool left, uint8_t v);
void fontrom_reset_gaiji();
void fontrom_make_sheet(std::vector<uint32_t>& px, int* w, int* h);

// 日付（INI の FakeYear= / FakeDate= を反映した現在時刻）
struct PcTime { int year, month, day, wday, hour, min, sec; };
void     machine_now(Machine* m, PcTime* t);
uint16_t machine_file_date(Machine* m, uint16_t dos_date);   // ファイルの日付を「今日」より後にしない

// bios.cpp
void bios_init(Machine* m);
// ブートモード: ユニット 0 のフロッピーの IPL を読み込んで、そこから実行を始める。読めなければ false と理由
bool bios_boot_fd(Machine* m, std::string* why);
// ブートモードで、ROM（N88-BASIC など、用意していない部分）へ飛び込んだか確かめる。飛び込んだら止める
bool bios_rom_trap(Machine* m);
void bios_hle(Machine* m, uint8_t n);
void lio_init(Machine* m);
void soundbios_init(Machine* m);
void soundbios_hle(Machine* m);
void lio_hle(Machine* m, uint8_t n);
void bios_key_irq(Machine* m);
void console_putc(Machine* m, uint8_t c);
void console_reset(Machine* m);
int  bios_key_available(Machine* m);
uint16_t bios_key_read(Machine* m, bool remove);

// pcm86.cpp
void    pcm86_advance(Machine* m, uint32_t t);
uint8_t pcm86_in(Machine* m, uint16_t port);
void    pcm86_out(Machine* m, uint16_t port, uint8_t v);
void    pcm86_reset(Machine* m);

// xmsems.cpp
void xmsems_init(Machine* m);
void xmsems_hle(Machine* m, uint8_t n);
bool xms_int2f(Machine* m);
bool ems_enabled(Machine* m);

// dos.cpp
void dos_init(Machine* m);
void dos_hle(Machine* m, uint8_t n);
void shell_start(Machine* m, const std::string& cmdline);
// INT DCh CL=0Ch/0Dh: ファンクションキー・編集キーに割り当てた文字列の取得／設定（DOS のコンソール入力で展開する）
void dos_keytab_get(Machine* m, uint16_t ax, uint32_t addr);
// メモリエディタ用: いま動いているゲーム（実行中のプログラムと、それを起動した親たち。シェルと常駐ドライバは除く）が
// 持っているメモリブロック（MCB の中身の範囲、線形アドレス）
struct DosMemBlock { uint32_t start, len; std::string name; };
void dos_game_blocks(Machine* m, std::vector<DosMemBlock>& out);
void dos_keytab_set(Machine* m, uint16_t ax, uint32_t addr);

// 共通
void set_cf(Machine* m, bool on);
void set_zf(Machine* m, bool on);
void plog(const char* fmt, ...);

// HLE トラップ番号の割り振り
enum {
    HLE_INT_BASE   = 0x00,   // 0x00-0x7F: 対応する INT をそのまま（HLE_INT_BASE+vec）ではなく個別に下の表で
    HLE_INT18      = 0x18,
    HLE_INT1C      = 0x1C,
    HLE_INT1A      = 0x1A,
    HLE_INT1B      = 0x1B,
    HLE_INT1F      = 0x1F,
    HLE_INT09      = 0x09,
    HLE_INT06      = 0x06,
    HLE_IRQ_EOI_M  = 0x0A,   // マスタ EOI して戻る
    HLE_IRQ_EOI_S  = 0x10,   // スレーブ EOI して戻る
    HLE_INT20      = 0x20,
    HLE_INT21      = 0x21,
    HLE_INT25      = 0x25,
    HLE_INT26      = 0x26,
    HLE_INT27      = 0x27,
    HLE_INT28      = 0x28,
    HLE_INT29      = 0x29,
    HLE_INT2F      = 0x2F,
    HLE_INT33      = 0x33,
    HLE_SNDBIOS    = 0xD2,   // サウンド BIOS（INT D2h）
    HLE_INTDC      = 0xDC,
    HLE_INT67      = 0x67,
    HLE_LIO        = 0xA0,   // 0xA0-0xAF: グラフィック LIO（INT A0h-AFh）
    HLE_XMS        = 0xE0,
    HLE_MSIRQ      = 0xF0,   // マウスドライバの IRQ13: ユーザのイベントハンドラを呼ぶか決める
    HLE_MSIRQ_END  = 0xF1,   // ハンドラから戻った
    HLE_SHELL      = 0xFD,   // シェル（バッチ）の次の行へ
    HLE_EXIT       = 0xFE,
    HLE_NOP        = 0xFF,
};
