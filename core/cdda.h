// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  cdda.h  --  CD-ROM のトラック表（TOC）と CD-DA（音楽トラック）の演奏
//
//  ・CUE シート（CDROM=xxx.CUE）: データトラック（ISO 9660）と音楽トラックの位置を CUE から作る。
//    音楽は BIN（2352 バイト/セクタの生データ）・WAV・MP3・FLAC のどれでも読む
//  ・音楽のフォルダ（CDDA=フォルダ）: 中の WAV / MP3 / FLAC を、ファイル名の末尾の 2 桁の数を
//    トラック番号として並べる（track_02.mp3 → トラック 2）。CUE と一緒なら、同じ番号の音を置き換える
//  ・位置は HSG（論理セクタ。1/75 秒。トラック 1 の先頭が 0）で持つ。Red Book（分:秒:フレーム）は +150
//  ・演奏は MSCDEX の要求（dos.cpp の INT 2Fh AX=1510h）から。音は Player が 1 標本ずつ取り出して混ぜる
// -----------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct StateW;
struct StateR;

namespace cdda {
    enum Src { SRC_NONE = 0, SRC_BIN, SRC_WAV, SRC_MP3, SRC_FLAC };
    struct Track {
        int      no = 0;
        bool     audio = false;
        uint32_t start = 0;        // HSG（INDEX 01 の位置）
        uint32_t length = 0;       // セクタ数（次のトラックの先頭まで）
        int      src = SRC_NONE;   // 音の出どころ
        std::string path;          // ホストのパス
        int64_t  file_off = 0;     // BIN: トラックの先頭（INDEX 01）のファイル内の位置（バイト）
        // データトラック（CUE の TRACK 01 MODE1/2352 など）の読み方
        uint32_t sec_size = 2048, sec_off = 0;
    };

    // ディスクを入れ替える（トラック表を空にして、演奏を止める）
    void reset();
    // CUE シートを読む。データトラック（最初の MODE トラック）のファイル・セクタの形を返す
    bool load_cue(const std::string& cue_host, std::string* err,
                  std::string* data_file, uint32_t* sec_size, uint32_t* sec_off, int64_t* data_off);
    // CUE が無いとき: データトラック 1 つ（sectors セクタ）だけのディスクにする
    void set_data_only(uint32_t sectors);
    // 音楽のフォルダ（CDDA=）を足す。読めた曲の数を返す
    int  add_folder(const std::string& dir, std::string* info);

    bool has_audio();
    int  first_track();
    int  last_track();
    uint32_t leadout();                  // HSG
    const Track* track(int no);          // 無ければ nullptr
    int  track_at(uint32_t hsg);         // その位置のトラック番号（無ければ 0）

    // 演奏（MSCDEX）。start は HSG、count はセクタ数
    void play(uint32_t start, uint32_t count);
    void stop();                         // 演奏中なら一時停止、一時停止中なら止める
    bool resume();
    bool playing();                      // 音が出ている（＝MSCDEX の busy）
    bool paused();
    uint32_t position();                 // 今の位置（HSG）
    void last_range(uint32_t* start, uint32_t* end);   // 直前の演奏（再開）の範囲（HSG）
    void set_volume(uint8_t v0, uint8_t v1);           // IOCTL の音量（0〜255）
    void get_volume(uint8_t* v0, uint8_t* v1);

    // 音を 1 標本ぶん取り出す（rate は出力の標本化周波数。-1〜1 の値を l/r に足す）
    void render_one(int rate, float* l, float* r);
    // 音を作らずに時間だけ進める（早送り）
    void skip_seconds(double sec);

    void state_save(StateW& w);
    void state_load(StateR& r);
}
