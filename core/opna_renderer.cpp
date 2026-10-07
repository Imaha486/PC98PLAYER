// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  opna_renderer.cpp
//
//  GMPV3_WITH_YMFM が定義されているときだけ ymfm を使い、それ以外は無音を返す。
//  ymfm 側の型はこのファイルの外へ一切漏らさない。
// -----------------------------------------------------------------------------
#include "opna_renderer.h"
#include "rhythm_rom.h"
#include <string.h>
#include <stdlib.h>

#ifdef GMPV3_WITH_YMFM
// tools/fetch_ymfm.ps1 で third_party/ymfm に取得したものを使う
#include "ymfm_opn.h"
#include <vector>

namespace {

// ymfm はホスト側に「外部メモリ（ADPCM ROM/RAM）」の面倒を見るよう求める。
class YmfmHost : public ymfm::ymfm_interface {
public:
    std::vector<uint8_t> rhythm_rom;   // ADPCM-A（リズム音源）
    std::vector<uint8_t> adpcm_ram;    // ADPCM-B
    bool rhythm_on;                    // 切ると ADPCM-A の読み出しが 0 になる
    int  rhythm_kind;                  // 0=切 / 1=自前合成 / 2=渡された本物

    YmfmHost() : adpcm_ram(256 * 1024, 0), rhythm_on(true), rhythm_kind(1) {
        // 何も渡されなくてもリズムが鳴るように、自前合成の 8KB を入れておく。
        // 本物の ROM を渡されたらそちらで上書きされる（rhythm_rom.h を参照）。
        rhythm_rom.resize(RHYTHM_ROM_BYTES);
        if (rhythm_build_rom(rhythm_rom.data(), (int)rhythm_rom.size()) == 0) {
            rhythm_rom.clear();
            rhythm_kind = 0;
        }
    }

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t offset) override {
        if (type == ymfm::ACCESS_ADPCM_A)
            return (rhythm_on && offset < rhythm_rom.size()) ? rhythm_rom[offset] : 0;
        if (type == ymfm::ACCESS_ADPCM_B)
            return offset < adpcm_ram.size() ? adpcm_ram[offset] : 0;
        return 0;
    }
    void ymfm_external_write(ymfm::access_class type, uint32_t offset, uint8_t data) override {
        if (type == ymfm::ACCESS_ADPCM_B && offset < adpcm_ram.size())
            adpcm_ram[offset] = data;
    }
};

struct Impl {
    YmfmHost host;
    ymfm::ym2608 chip;
    ymfm::ym2608::output_data out;
    uint32_t chip_rate;
    int      host_rate;
    double   fm_gain, ssg_gain;   // 音源間のバランス
    double   pos;          // リサンプル位置
    float    last_l, last_r;
    // SSG の既定を 0.20 にしている理由は set_balance のところに書いた。
    // ymfm 基準の 1:1 だと実曲で SSG が FM より 5〜18dB 大きくなる。
    Impl() : chip(host), chip_rate(0), host_rate(44100),
          fm_gain(1.0), ssg_gain(0.20), pos(0), last_l(0), last_r(0) {}
};

} // namespace

OpnaRenderer::OpnaRenderer() : impl_(nullptr), clock_(0), rate_(44100) {}
OpnaRenderer::~OpnaRenderer() { delete (Impl*)impl_; impl_ = nullptr; }

void OpnaRenderer::init(uint32_t chip_clock, int sample_rate) {
    clock_ = chip_clock;
    rate_  = sample_rate;
    delete (Impl*)impl_;
    Impl* p = new Impl();
    p->host_rate = sample_rate;
    p->chip.reset();
    // ゲーム用は MED（SSG を 1/2 に間引く）で十分。MAX は約 1MHz で回るので重い。
    p->chip.set_fidelity(ymfm::OPN_FIDELITY_MED);
    p->chip_rate = p->chip.sample_rate(chip_clock);
    impl_ = p;
}

void OpnaRenderer::reset() {
    Impl* p = (Impl*)impl_;
    if (!p) return;
    p->chip.reset();
    p->pos = 0;
}

void OpnaRenderer::write(int part, uint8_t addr, uint8_t val) {
    Impl* p = (Impl*)impl_;
    if (!p) return;
    if (part == 0) { p->chip.write_address(addr);    p->chip.write_data(val); }
    else           { p->chip.write_address_hi(addr); p->chip.write_data_hi(val); }
    // プリスケーラが変わるとチップのサンプルレートも変わる。取り直さないと
    // リサンプル比がずれて音程とテンポが狂う。
    if (part == 0 && addr >= 0x2D && addr <= 0x2F)
        p->chip_rate = p->chip.sample_rate(clock_);
}

void OpnaRenderer::render_one(float* left, float* right) {
    Impl* p = (Impl*)impl_;
    if (!p) { *left = *right = 0.0f; return; }
    // チップのネイティブレートからホストのレートへ落とす。
    // ymfm の ym2608 は既定が OPN_FIDELITY_MAX で、sample_rate() は
    // 入力クロック / 8 を返す（7.9872MHz なら 998.4kHz）。44.1kHz に対して
    // 22.6 倍なので、区間平均がそのまま効きのよい帯域制限になる。
    // 最近傍だと 1 サンプルおきに捨てることになり、
    // SSG の矩形波のように高い倍音を持つ音でエイリアスが出て、
    // 「音が痩せる／聞こえない」ように感じる。区間内の全サンプルを
    // 平均する（＝箱型フィルタ）だけで、捨てずに済む。
    p->pos += (double)p->chip_rate / p->host_rate;
    double acc_l = 0, acc_r = 0;
    int n = 0;
    while (p->pos >= 1.0) {
        p->pos -= 1.0;
        p->chip.generate(&p->out, 1);
        // ym2608 の出力は 3 本ある:
        //   data[0] = FM 左 / data[1] = FM 右 / data[2] = SSG（モノラルに合成済み）
        // ymfm::ym2608::OUTPUTS = FM_OUTPUTS(2) + SSG_OUTPUTS(1) で 3。
        // data[0],[1] だけ取ると SSG が丸ごと落ちる。実機同様ここで足し合わせる。
        acc_l += p->fm_gain * p->out.data[0] + p->ssg_gain * p->out.data[2];
        acc_r += p->fm_gain * p->out.data[1] + p->ssg_gain * p->out.data[2];
        n++;
    }
    if (n) {
        p->last_l = (float)(acc_l / n / 32768.0);
        p->last_r = (float)(acc_r / n / 32768.0);
    }
    *left = p->last_l;
    *right = p->last_r;
}

void OpnaRenderer::reload_from_registers(const uint8_t* part0, const uint8_t* part1) {
    Impl* p = (Impl*)impl_;
    if (!p) return;
    p->chip.reset();
    // 音色・音量系だけを流し込む。キーオン(28h)とタイマ(24h-27h)は復元しない
    // ――スナップショット直後は VM 側のドライバがすぐ書き直すため。
    for (int a = 0x30; a <= 0xB6; a++) { write(0, (uint8_t)a, part0[a]); write(1, (uint8_t)a, part1[a]); }
    // F-Number は上位(A4h-A6h / ACh-AEh)を先に書かないと下位の書き込みで確定しない。
    // 昇順で流すと上位が捨てられ、音程が化けて（ほぼ）無音になるので組にして書き直す。
    for (int c = 0; c < 3; c++) {
        for (int part = 0; part < 2; part++) {
            const uint8_t* R = part ? part1 : part0;
            write(part, (uint8_t)(0xA4 + c), R[0xA4 + c]); write(part, (uint8_t)(0xA0 + c), R[0xA0 + c]);
        }
        write(0, (uint8_t)(0xAC + c), part0[0xAC + c]); write(0, (uint8_t)(0xA8 + c), part0[0xA8 + c]);
    }
    for (int a = 0x00; a <= 0x0F; a++) { write(0, (uint8_t)a, part0[a]); }
    // リズムの総音量(11h)と楽器ごとの音量・パン(18h-1Dh)も戻す。
    // 10h はキーオンなので戻さない ―― 戻すと復元した瞬間に打楽器が鳴る。
    write(0, 0x11, part0[0x11]);
    for (int a = 0x18; a <= 0x1D; a++) write(0, (uint8_t)a, part0[a]);
    write(0, 0x22, part0[0x22]);
    write(0, 0x29, part0[0x29]);
}

void OpnaRenderer::save_chip(std::vector<uint8_t>& out) {
    Impl* p = (Impl*)impl_;
    out.clear();
    if (!p) return;
    std::vector<uint8_t> buf;
    ymfm::ymfm_saved_state st(buf, true);
    p->chip.save_restore(st);
    // リサンプラの状態も添える
    out.resize(sizeof(double) + sizeof(float) * 2);
    memcpy(out.data(), &p->pos, sizeof(double));
    memcpy(out.data() + sizeof(double), &p->last_l, sizeof(float));
    memcpy(out.data() + sizeof(double) + sizeof(float), &p->last_r, sizeof(float));
    out.insert(out.end(), buf.begin(), buf.end());
}
bool OpnaRenderer::load_chip(const std::vector<uint8_t>& in) {
    Impl* p = (Impl*)impl_;
    size_t hdr = sizeof(double) + sizeof(float) * 2;
    if (!p || in.size() <= hdr) return false;
    memcpy(&p->pos, in.data(), sizeof(double));
    memcpy(&p->last_l, in.data() + sizeof(double), sizeof(float));
    memcpy(&p->last_r, in.data() + sizeof(double) + sizeof(float), sizeof(float));
    std::vector<uint8_t> buf(in.begin() + hdr, in.end());
    ymfm::ymfm_saved_state st(buf, false);
    p->chip.save_restore(st);
    p->chip_rate = p->chip.sample_rate(clock_);
    return true;
}

// 本物の ROM を渡されたらそちらを使う。NULL を渡すと自前合成へ戻る。
//
// 本物を優先するのは当然として、**戻せる**ようにしてあるのも大事です。
// 渡された ROM が壊れていた・別物だったときに、取り外して元へ戻す道が
// 無いと、再起動するしかなくなります。
bool OpnaRenderer::set_rhythm_rom(const uint8_t* data, int len) {
    Impl* p = (Impl*)impl_;
    if (!p) return false;
    if (!data || len <= 0) {
        p->host.rhythm_rom.assign(RHYTHM_ROM_BYTES, 0);
        p->host.rhythm_kind =
            rhythm_build_rom(p->host.rhythm_rom.data(), RHYTHM_ROM_BYTES) ? 1 : 0;
        return true;
    }
    p->host.rhythm_rom.assign(data, data + len);
    p->host.rhythm_kind = 2;
    return true;
}

void OpnaRenderer::set_rhythm_enabled(bool on) {
    Impl* p = (Impl*)impl_;
    if (p) p->host.rhythm_on = on;
}

int OpnaRenderer::rhythm_source() const {
    Impl* p = (Impl*)impl_;
    if (!p || !p->host.rhythm_on) return 0;
    return p->host.rhythm_kind;
}

// -----------------------------------------------------------------------------
//  FM と SSG の音量比
//
//  ymfm の ym2608 は FM も SSG も 16bit 全振幅で出す（GeneralInfo.md の表）。
//  ymfm 自身の例 (vgmrender) も YM2608 では `out0 + out2` と 1:1 で混ぜている。
//  つまり 1:1 が「チップ本来の比」なのだが、**実際の曲では SSG が大きすぎる**。
//  同じ曲を FM だけ／SSG だけでレンダリングして RMS を測った結果:
//
//    瑠璃色の雪 RURI.M t1   SSG/FM = 4.25  (+12.6dB)
//    瑠璃色の雪 RURI.M t7   SSG/FM = 7.79  (+17.8dB)   ← 最も極端
//    LEGAM LG_FM02.FM       SSG/FM = 2.29  ( +7.2dB)
//    M1.PMD (PMDB2)         SSG/FM = 2.24  ( +7.0dB)
//    メルクリウス MP01-03    SSG/FM = 1.86〜2.70 (+5.4〜+8.6dB)
//    下級生 KAKYUSEI t437   SSG/FM = 0.80  ( -2.0dB)
//
//  実機の PC-9801-26K / -86 では FM と SSG が別の系統を通り、SSG 側が
//  落とされて出てきます。曲もその前提で書かれているので、チップの素の比を
//  そのまま出すと SSG だけが前に出ます。PMDWin / FMPMD2000 のような
//  既存のプレイヤーが一様に SSG 音量を別に持ち、既定を下げているのも同じ理由です。
//
//  そこで **比を書き換えてごまかすのではなく**、既定を測った値（0.20 ≒ -14dB）に
//  置き、上限も絞ります。1:1 より上は上の実測どおり使いどころが無いので、
//  SSG は 1.0 で止めます。つまみの全行程を使える範囲が実際に使う範囲になります。
//  逆に FM は素の状態でピークが 0.05〜0.09 しか出ていないので、
//  上へ大きく余裕を持たせます（16 倍 = +24dB）。
// -----------------------------------------------------------------------------
void OpnaRenderer::set_balance(double fm, double ssg) {
    Impl* p = (Impl*)impl_;
    if (!p) return;
    p->fm_gain  = fm  < 0 ? 0 : (fm  > 16.0 ? 16.0 : fm);
    p->ssg_gain = ssg < 0 ? 0 : (ssg >  2.0 ?  2.0 : ssg);   // SSGVolume=1000% で 2.0
}

int OpnaRenderer::available() { return 1; }

#else   // ------------------------- 無音スタブ -------------------------------

OpnaRenderer::OpnaRenderer() : impl_(nullptr), clock_(0), rate_(44100) {}
OpnaRenderer::~OpnaRenderer() {}

void OpnaRenderer::init(uint32_t chip_clock, int sample_rate) {
    clock_ = chip_clock; rate_ = sample_rate;
}
void OpnaRenderer::reset() {}
void OpnaRenderer::write(int, uint8_t, uint8_t) {}
void OpnaRenderer::render_one(float* left, float* right) { *left = 0.0f; *right = 0.0f; }
void OpnaRenderer::reload_from_registers(const uint8_t*, const uint8_t*) {}
bool OpnaRenderer::set_rhythm_rom(const uint8_t*, int) { return false; }
void OpnaRenderer::set_rhythm_enabled(bool) {}
int  OpnaRenderer::rhythm_source() const { return 0; }
void OpnaRenderer::save_chip(std::vector<uint8_t>& out) { out.clear(); }
bool OpnaRenderer::load_chip(const std::vector<uint8_t>&) { return false; }
void OpnaRenderer::set_balance(double, double) {}
int  OpnaRenderer::available() { return 0; }

#endif
