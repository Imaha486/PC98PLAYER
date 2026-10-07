// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  floppy.h  --  仮想フロッピーディスク（イメージファイル）
//
//  D88 / ヘッダ無しのベタ（HDM・XDF・DUP・TFD 等）/ FDI（Anex86）/ NFD（T98-Next r0・r1）/
//  フラックスイメージ（SCP・HFE。磁気信号を自前で MFM/FM 復調）を読み込み、セクタ単位（ID つき）で
//  読み書きする。書き込みはその場でイメージファイルへ反映する（セクタの位置を覚えておき、同じ場所を書き換える）。
//
//  実機のドライブ（fdreal.cpp）も同じ形で扱う: トラックは初めて使うときに読みに行く（FdSource）。
//   ・USB フロッピードライブ（Windows のドライブ \\.\A: 等）: 論理セクタの読み書き
//   ・Greaseweazle（フラックスを読む USB 基板）: トラックの磁気信号を読んで復調する（読み込みのみ）
//
//  上には 2 つの使い道がある:
//   ・ディスク BIOS（INT 1Bh）: ID（C/H/R/N）で探す。プロテクトの確認にも使われる
//   ・DOS のドライブ（B: など）: FAT12/16 のファイルシステムとして読み書きする（fatfs）
// -----------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct FdSector {
    uint8_t  c, h, r, n;          // ID
    uint8_t  status;              // BIOS の結果コード（00h=正常, B0h=データ CRC 等）
    bool     deleted;             // デリーテッドデータ
    bool     fm;                  // 単密度（FM）
    std::vector<uint8_t> data;    // 実データ（長さは実際に持っているぶん）
    int64_t  file_off;            // イメージ内のデータの位置（-1 = 書き戻せない）
    int      copies;              // NFD r1 の再試行データの数（1 以上）
    int      next_copy;           // 次に返す再試行データ
};

// READ DIAGNOSTIC（ID を見ずにトラックの先頭から読む）の結果を記録したもの（NFD r1）
struct FdDiag {
    uint8_t  c, h, r, n;          // 読んだときに指定した ID
    uint8_t  status;              // BIOS の結果コード
    std::vector<uint8_t> data;
    int      copies, next_copy;
    int64_t  file_off;
};

struct FdTrack {
    std::vector<FdSector> secs;
    std::vector<FdDiag> diags;
    int rot = 0;                  // READ ID の回転位置
};

// 実機のドライブ等、トラックをその場で読みに行く元（fdreal.cpp）
struct FdSource {
    virtual ~FdSource() {}
    // トラックを読む。ディスクが無い等で読めなければ false（次に使うときにまた読みに行く）
    virtual bool fetch(int cyl, int head, FdTrack& out) = 0;
    // セクタを書く（できなければ false）
    virtual bool write(int cyl, int head, const FdSector& s, const uint8_t* buf, int len) { (void)cyl; (void)head; (void)s; (void)buf; (void)len; return false; }
    virtual bool writable() const { return false; }
    virtual void idle(uint32_t now_ms) { (void)now_ms; }   // しばらく使わなければモーターを止める等
    virtual std::string last_error() const { return ""; }
    int media_hint = -1;          // 分かった媒体（FD_2HD 等。-1 = 不明）
    int cyl_hint = 0;             // 分かったシリンダ数（0 = 不明）
};

enum FdMedia { FD_2D = 0, FD_2DD = 1, FD_2HD = 2, FD_144 = 3 };

class FloppyImage {
public:
    static const int MAX_TRACKS = 168;
    bool load(const std::string& path, std::string* err);
    std::string path;           // ホストのパス
    std::string format;         // "D88" 等（表示用）
    bool   wprot = false;
    int    media = FD_2HD;
    int    cylinders = 0, heads = 2;
    FdTrack trk[MAX_TRACKS];    // 添字 = C*2+H（物理トラック）
    ~FloppyImage();

    FdTrack* track(int cyl, int head);
    // 実機のドライブ（FdSource）: トラックは使うときに読む。not_ready = 直前にディスクが無かった
    FdSource* src = nullptr;
    bool loaded[MAX_TRACKS] = {};
    bool not_ready = false;
    bool open_source(FdSource* s, const std::string& name, const std::string& fmt, std::string* err);
    void refetch();             // 読み直す（ディスクを入れ替えた）
    bool check_media_change();  // 実機: トラック 0 を読み直して、ディスクが替わっていたら覚えている中身を捨てる
    // 全トラックを読み（実機なら読みに行き）D88 で書き出す
    bool save_d88(const std::string& out_path, std::string* err);
    FdSector* find(int cyl, int head, uint8_t c, uint8_t h, uint8_t r, uint8_t n);
    // セクタへ書く（イメージへも反映）。失敗なら false
    bool write_sector(FdSector* s, const uint8_t* buf, int len);

    // 論理セクタ（LBA）。DOS から使う。形は起動セクタ／トラック 0 から決める
    int    lsec_size = 1024, lsec_spt = 8, lsec_heads = 2;
    uint32_t lsec_total = 0;
    bool read_lba(uint32_t lba, uint8_t* buf);
    bool write_lba(uint32_t lba, const uint8_t* buf);
    void setup_geometry();

private:
    bool load_d88(const std::vector<uint8_t>& d);
    bool load_fdi(const std::vector<uint8_t>& d);
    bool load_nfd0(const std::vector<uint8_t>& d);
    bool load_nfd1(const std::vector<uint8_t>& d);
    bool load_raw(const std::vector<uint8_t>& d, int64_t base, int cyl, int hd, int spt, int ssize, int media);
    bool load_scp(const std::vector<uint8_t>& d, std::string* err);
    bool load_hfe(const std::vector<uint8_t>& d, std::string* err);
    FdSector* lba_sector(uint32_t lba);
};

// ---- 本体とのつなぎ -----------------------------------------------------------
struct Machine;
namespace floppy {
    // 入れる／取り出す（drive = 'B' など。ユニット 0 に入る）。err に理由
    bool insert(const std::string& path, std::string* err);
    void eject();
    FloppyImage* image();
    // ドライブ（ユニット）を指定して入れる／取り出す。0 = 上と同じ、1 = 2 台目（ブートモード用。DOS からは見えない）
    bool insert_unit(int unit, const std::string& path, std::string* err);
    void eject_unit(int unit);
    void clear_swap_flags();              // 起動時・ステートロードで入れたものは「入れ替え」として扱わない
    FloppyImage* image_unit(int unit);
    std::string current_path_unit(int unit);
    // イメージの中身を調べる（読めなければ false）。dos = MS-DOS のファイル表がある、bootable = IPL がある
    bool probe_image(const std::string& path, bool* dos, bool* bootable);                 // 入っていなければ nullptr（フォルダを入れているときも nullptr）
    // フォルダを入れているとき、そのホストのパス（空 = フォルダではない）。
    // FloppyImage= にフォルダ名を書くと、そのフォルダをフロッピーのドライブとしてそのまま見せる
    //（ファイルを確かめるだけのキーディスク向け。セクタ単位の読み書きはできない）
    const std::string& folder();
    // 入っているもの（イメージのパス・実機の名前・フォルダ）。空 = 入っていない
    std::string current_path();
    char drive_letter();                  // DOS のドライブ名（0 = 使わない）
    void set_drive_letter(char c);
    uint32_t change_count();              // 入れ替えるたびに増える（DOS のキャッシュを捨てる目印）。どちらのドライブでも
    uint32_t change_count_unit(int unit); // そのドライブ（ユニット）だけの入れ替え回数
    // 2 台目のドライブにも DOS のドライブ名を付けられる（FloppyDrive2=）。0 = 付けない
    char drive_letter_unit(int unit);
    void set_drive_letter_unit(int unit, char c);
    int  unit_of_letter(char c);          // そのドライブ名のフロッピーのユニット（-1 = フロッピーではない）
    void bios_int1b(Machine* m);          // INT 1Bh のフロッピー部分
    bool is_fd_da(uint8_t al);            // AL（DA/UA）がフロッピーか
    // 実機のドライブを表す名前か（"FDD:A" "\\.\A:" "GW" "GW:COM5" 等）。ファイル名ではないのでパスを付けない
    bool is_device_spec(const std::string& name);
    void media_changed();                 // 実機のディスクを入れ替えた（読み直す）
    // 実機: しばらく触っていなかったら、ディスクが入れ替わっていないか確かめる（DOS・BIOS の入口で呼ぶ）
    void poll_change();
    void idle();                          // 1 フレームごとに呼ぶ（実機のモーターを止める等）
    // Greaseweazle の設定（INI: GWDrive / GWRevs）
    void set_gw_options(const std::string& drive, int revs);
    // 入れるディスクを必ず書き込み禁止にする（インストールの支援で、インストーラが「ライトプロテクトにして下さい」と言うため）
    void set_force_wprot(bool on);
    bool force_wprot();
}

// ---- フラックス（磁気信号）の復調（fdflux.cpp） -----------------------------------------
namespace fdflux {
    // flux_ns: 反転の間隔（ns）を並べたもの。index: 各回転の始まり（flux_ns の添字）。回転ごとに復調して
    // まとめる（CRC が通った回を採り、どの回も駄目なら「CRC エラー」＋読むたびに変わるデータとして残す）
    // media_out: 推定した媒体（FD_2HD 等。-1 = 不明）
    bool decode_flux(const std::vector<float>& flux_ns, const std::vector<size_t>& index, int cyl, int head,
                     FdTrack& out, int* media_out);
    // セル（クロック込みのビット）列の 1 回転分から復調（HFE 用）。cell_ns はメディア判定用
    bool decode_cells(const std::vector<uint8_t>& cells, double cell_ns, int cyl, int head, FdTrack& out, int* media_out);
}

// ---- FAT ファイルシステム（hostfs から使う） ---------------------------------------
struct HostDirEntry;
namespace fatfs {
    // 仮想パス: "\x01" + ドライブ名 + ("\\" または "/" 区切りの要素。名前は UTF-8)
    bool is_vpath(const std::string& p);
    std::string root_path(char drive);
    bool is_handle(void* h);
    bool list_dir(const std::string& dir, std::vector<HostDirEntry>& out);
    bool stat(const std::string& path, HostDirEntry& e);
    void* open(const std::string& path, int mode, bool create, bool trunc);
    void  close(void* h);
    int   read(void* h, void* buf, int n);
    int   write(void* h, const void* buf, int n);
    int64_t seek(void* h, int64_t off, int whence);
    bool  truncate_here(void* h);
    bool  remove(const std::string& path);
    bool  rename(const std::string& a, const std::string& b);
    bool  mkdir(const std::string& path);
    bool  rmdir(const std::string& path);
    bool  set_time(void* h, uint16_t d, uint16_t t);
    bool  get_time(void* h, uint16_t* d, uint16_t* t);
    // 空き容量（クラスタ数など）。入っていなければ false
    bool  free_space(uint32_t* spc, uint32_t* bps, uint32_t* free_clusters, uint32_t* total_clusters);
    // ボリュームラベル（DOS の検索で返す形 "NAME.EXT"）。無ければ false
    bool  volume_label(std::string* name, uint16_t* date, uint16_t* time);
    // ドライブを指定して（2 台目のフロッピー用）
    bool  free_space_drive(char drive, uint32_t* spc, uint32_t* bps, uint32_t* free_clusters, uint32_t* total_clusters);
    bool  volume_label_drive(char drive, std::string* name, uint16_t* date, uint16_t* time);
    void  reset();                        // ディスク交換時
}
