// SPDX-License-Identifier: MIT
//  player.h -- ホスト側（Windows / 検証用ハーネス）が使う一段上の窓口
#pragma once
#include "machine.h"
#include <string>
#include <map>

struct Ini {
    std::map<std::string, std::string> kv;   // "SECTION.KEY"（大文字）→ 値
    bool load(const std::string& path);
    std::string get(const std::string& key, const std::string& def = "") const;
    int geti(const std::string& key, int def) const;
};

struct PlayerSettings {
    Config cfg;
    int    scale = 2;          // x1 = 640x400
    int    volume = 100;
    bool   fullscreen = false;
    bool   smooth = false;
    bool   exit_on_end = true;
    bool   line200_gap = false;
    std::string title;
    std::string font_name;
    bool        font_shift = true;  // ホストのフォントから作る半角文字の左端の列を空ける（実機の ROM と同じに）
    int    sample_rate = 44100;
    int    fm_volume = 100, ssg_volume = 100, beep_volume = 50, pcm_volume = 100;
    bool   jis78 = true;       // 漢字 ROM を旧 JIS（1978）の並びにする
    std::string rhythm_rom;    // 2608_RYM.WAV などの本物（任意）
    int    midi_device = -1;   // MIDI=1 のときの出力先（-1 = MIDI マッパー）
};

#define STATE_SLOTS   8
#define STATE_THUMB_W 160
#define STATE_THUMB_H 100
struct SlotInfo {
    bool used = false;
    std::string when;                      // "2026/09/26 12:34"
    std::vector<uint8_t> thumb;            // STATE_THUMB_W*STATE_THUMB_H*3 (RGB)
};
std::string state_slot_path(const std::string& root, int slot);
bool state_slot_info(const std::string& root, int slot, SlotInfo* info);

std::string floppy_host_path(const std::string& root, const std::string& name);
bool player_settings_from_ini(const Ini& ini, const std::string& root, PlayerSettings* ps);

class OpnaRenderer;
struct Player {
    PlayerSettings ps;
    Machine* m = nullptr;
    OpnaRenderer* opna = nullptr;
    uint32_t fb[640 * 400];
    std::vector<int16_t> audio;     // 直近フレームぶん（ステレオ交互）
    double sample_acc = 0;
    double beep_phase = 0;
    bool   beep_state = false;
    uint16_t beep_reload = 998;   // ブザーの音程（PIT ch1 の値。フレームの途中の変化も追う）
    bool   beep_pwm = false;      // ブザーをパルス幅で鳴らしている（PIT ch1 がワンショット）
    double pwm_lo_s = 0, pwm_lo_e = 0;   // 出力が L の区間（MASTER_CLOCK の時刻）
    float  pwm_x1 = 0, pwm_y1 = 0, pwm_lp = 0;
    int32_t pcm_l = 0, pcm_r = 0;   // PCM86 の直前の標本
    int    pcm_lp_key = -1;          // PCM86 のローパス（標本化周波数ごとに係数を作る）
    float  pcm_b0 = 1, pcm_b1 = 0, pcm_b2 = 0, pcm_a1 = 0, pcm_a2 = 0;
    float  pcm_xl1 = 0, pcm_xl2 = 0, pcm_yl1 = 0, pcm_yl2 = 0, pcm_xr1 = 0, pcm_xr2 = 0, pcm_yr1 = 0, pcm_yr2 = 0;
    // MIDI の出口（ホストが設定）。完結したメッセージ（SysEx は F0..F7 まるごと）を渡す
    void (*midi_sink)(void* user, const uint8_t* msg, int len) = nullptr;
    void* midi_user = nullptr;
    uint8_t midi_status = 0, midi_need = 0;
    std::vector<uint8_t> midi_msg;
    void midi_feed(uint8_t b);
    bool init(const PlayerSettings& s, std::string* err);
    std::string floppy_error;       // 起動時にフロッピーを入れられなかった理由（ホストが知らせる）
    // ステートセーブ（スロット 0..STATE_SLOTS-1）。ファイルはゲームのフォルダの PC98PLAYER.SAV\ に置く
    bool save_state(int slot, std::string* err);
    bool load_state(int slot, std::string* err);
    // スナップショットをメモリ上で取る／戻す（ロード失敗時の巻き戻しにも使う）
    void snapshot(std::vector<uint8_t>& out);
    bool restore(const uint8_t* data, size_t len);
    // render_audio=false: 音を合成しない（早送り用。レジスタへの書き込みだけ音源へ渡す）
    void run_frame(bool render_video = true, bool render_audio = true);
    void shutdown();
    // PC-98 を起動し直す（電源を入れ直したのと同じ。入っているフロッピーはそのまま）
    bool reboot(std::string* err);
    // 設定を差し替えて起動し直す（INI の読み直し用。フロッピーは s.cfg.floppy_image のとおり）
    bool reboot(const PlayerSettings& s, std::string* err);
};
