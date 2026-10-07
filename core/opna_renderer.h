// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  opna_renderer.h  --  OPNA 音声生成の差し替え口
//
//  本体（CPU・DOS・タイマ・レジスタ）は音を作らない。ここが唯一の音源境界。
//
//  ・GMPV3_WITH_YMFM を定義してビルドすると ymfm (BSD, Aaron Giles) を使う。
//    ymfm は本リポジトリに同梱していない。tools/fetch_ymfm.ps1 で取得する。
//  ・定義しなければ無音スタブになる。この状態でもリポジトリはそのままビルドでき、
//    ドライバの常駐・レジスタ書き込み・MIDI 出力の確認はすべて行える。
//
//  同梱しない理由は単純で、配布物に他人の著作物を含めないため。ライセンス上は
//  BSD なので同梱してもよいが、含めなければ告知義務の管理そのものが消える。
// -----------------------------------------------------------------------------
#ifndef OPNA_RENDERER_H
#define OPNA_RENDERER_H

#include <stdint.h>
#include <vector>

class OpnaRenderer {
public:
    OpnaRenderer();
    ~OpnaRenderer();

    void init(uint32_t chip_clock, int sample_rate);
    void reset();

    // part: 0 = 188h/18Ah 側、1 = 18Ch/18Eh 側
    void write(int part, uint8_t addr, uint8_t val);

    // 1 フレーム分のステレオサンプルを生成する（-1.0 .. +1.0）
    void render_one(float* left, float* right);

    // スナップショット復元後、レジスタの内容からチップ状態を作り直す
    void reload_from_registers(const uint8_t* part0, const uint8_t* part1);

    // チップ内部（エンベロープ・位相・リサンプル位置まで）を丸ごと保存／復元する。
    // ステートセーブ用。ymfm が無いときは空を返し、復元は何もしない。
    void save_chip(std::vector<uint8_t>& out);
    bool load_chip(const std::vector<uint8_t>& in);

    // FM と SSG の音量比。実機でもボードや本体のつまみで変わる部分なので開ける。
    void set_balance(double fm_gain, double ssg_gain);

    // リズム音源の ADPCM ROM。
    //   ・init() の時点で自前合成の 8KB が入っている（何も渡さなくても鳴る）
    //   ・本物の ROM を渡せばそちらで上書きされる（そちらが優先）
    //   ・data=NULL / len=0 で呼ぶと自前合成へ戻す
    // 戻り値 true = 差し替えた。
    bool set_rhythm_rom(const uint8_t* data, int len);

    // リズムを鳴らすかどうか。false にすると ADPCM-A の読み出しが 0 になり、
    // 本物を知っている人が「代用より無音がよい」を選べる。
    void set_rhythm_enabled(bool on);

    // いま何を使っているか。0 = 切 / 1 = 自前合成 / 2 = 渡された本物の ROM
    int rhythm_source() const;

    // 音源が組み込まれているか（0 = 無音スタブ）
    static int available();

private:
    void* impl_;
    uint32_t clock_;
    int      rate_;
};

#endif
