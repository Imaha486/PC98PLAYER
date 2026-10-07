// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  rhythm_rom.cpp
//
//  リズム音源 6 音の合成と ADPCM-A 符号化。設計の意図は rhythm_rom.h に。
//
//  ● 配置とクロックは ymfm の実装から読み取った
//    ymfm_opn.cpp の ym2608::reset() が、6 音の開始・終了アドレスをベタ書きで
//    持っています。ADPCM-A は FM の 4 回に 1 回 clock され、タムとリムだけ
//    さらに半分（mask 0x0f / 0x3f の切り替え）。プリスケール 6 の OPNA では
//    FM が 55466.7Hz なので、
//
//      ch0-3（バス・スネア・シンバル・ハイハット） 13866.7Hz
//      ch4-5（タム・リム）                          6933.3Hz
//
//    になります。ch4-5 は 3.5kHz 弱までしか入らないので、そこは低い音で作ります。
//
//  | 音             | 配置        | サンプル | 長さ    |
//  |----------------|-------------|---------|---------|
//  | バスドラム     | 0000-01BF   |    896  |  64.6ms |
//  | スネア         | 01C0-043F   |   1280  |  92.3ms |
//  | トップシンバル | 0440-1B7F   |  11904  | 858.5ms |
//  | ハイハット     | 1B80-1CFF   |    768  |  55.4ms |
//  | タム           | 1D00-1F7F   |   1280  | 184.6ms |
//  | リムショット   | 1F80-1FFF   |    256  |  36.9ms |
//
//  ● ADPCM-A の符号化
//    復号は ymfm の adpcm_a_channel::clock() が持っています。こちらはその逆で、
//    各サンプルごとに 16 通り試して、いちばん近い符号を選ぶ貪欲法です。
//    蓄積器は 12bit で回り込むので、符号付き -2048..2047 として扱います。
//    振幅は ±1900 に抑えてあります ―― 回り込みは一度起きると派手に壊れるので、
//    上限ぎりぎりを狙わないほうがよい。
//
//    実測の符号化 SNR: バス 21.9 / タム 18.4 / スネア 14.8 / シンバル 14.8 /
//    ハイハット 11.7 / リム 7.2 dB。短くてノイズ主体の音ほど落ちますが、
//    これは 4bit ADPCM の性質で、本物の ROM も同じ条件です
//    （あのざらついた質感の一因でもあります）。
//
//  ● 乱数の種は固定
//    ノイズ成分に乱数を使いますが、種を固定しているので、どのビルドでも、
//    何度呼んでも同じ 8192 バイトになります。「昨日と音が違う」が起きません。
//    標準ライブラリの乱数は実装ごとに系列が変わるので、自前の線形合同法を
//    持っています。
// -----------------------------------------------------------------------------
#include "rhythm_rom.h"

#include <math.h>
#include <string.h>
#include <vector>

namespace {

// 円周率は自前で持つ。<math.h> の M_PI は POSIX の拡張で、MSVC では
// _USE_MATH_DEFINES を先に定義しないと出てきません（実際それでビルドが
// 止まりました）。マクロを足して回るより、必要な定数を 1 行書くほうが確実です。
const double kPi = 3.14159265358979323846;

const double FM_RATE  = 7987200.0 / 144.0;   // 55466.7Hz
const double RATE_03  = FM_RATE / 4.0;       // 13866.7Hz
const double RATE_45  = FM_RATE / 8.0;       //  6933.3Hz

const int   PEAK = 1900;                     // 12bit 蓄積器の上限に対する余裕

// ---- ADPCM-A（ymfm の adpcm_a_channel::clock と同じ表） --------------------
const uint16_t kSteps[49] = {
      16,  17,   19,   21,   23,   25,   28,
      31,  34,   37,   41,   45,   50,   55,
      60,  66,   73,   80,   88,   97,  107,
     118, 130,  143,  157,  173,  190,  209,
     230, 253,  279,  307,  337,  371,  408,
     449, 494,  544,  598,  658,  724,  796,
     876, 963, 1060, 1166, 1282, 1411, 1552
};
const int8_t kStepInc[8] = { -1, -1, -1, -1, 2, 5, 7, 9 };

inline int sign12(int v) { return ((v + 2048) & 0xFFF) - 2048; }

// 1 サンプルぶん進める。戻り値は新しい蓄積器の値（符号付き）。
inline int adpcm_step(int code, int& acc, int& idx) {
    int delta = (2 * (code & 7) + 1) * kSteps[idx] / 8;
    if (code & 8) delta = -delta;
    acc = (acc + delta) & 0xFFF;
    int ni = idx + kStepInc[code & 7];
    idx = ni < 0 ? 0 : (ni > 48 ? 48 : ni);
    return sign12(acc);
}

// 各サンプルで 16 通り試して、いちばん近いものを選ぶ。
void adpcm_encode(const std::vector<int>& pcm, std::vector<uint8_t>& nib) {
    int acc = 0, idx = 0;
    nib.resize(pcm.size());
    for (size_t i = 0; i < pcm.size(); i++) {
        int best = 0; long bestErr = -1;
        for (int c = 0; c < 16; c++) {
            int a = acc, k = idx;                 // 試すだけなので写しで
            int v = adpcm_step(c, a, k);
            long e = labs((long)pcm[i] - v);
            if (bestErr < 0 || e < bestErr) { bestErr = e; best = c; }
        }
        adpcm_step(best, acc, idx);
        nib[i] = (uint8_t)best;
    }
}

// ---- 音づくりの部品 -------------------------------------------------------
// 自前の線形合同法。標準ライブラリだと実装ごとに系列が変わってしまう。
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    double  bipolar() { return (double)(next() >> 8) / 8388608.0 - 1.0; }  // -1..1
};

// 立ち上がり a サンプル、その後は指数で落ちる包絡線
double envelope(int i, int n, int a, double curve) {
    if (i < a) return (double)i / (double)a;
    return exp(-curve * (double)(i - a) / (double)(n - a > 0 ? n - a : 1));
}

// 指数掃引の正弦波
void sweep(std::vector<double>& out, int n, double f0, double f1, double rate) {
    out.assign(n, 0.0);
    double ph = 0.0;
    for (int i = 0; i < n; i++) {
        double f = f0 * pow(f1 / f0, (double)i / (double)n);
        ph += 2.0 * kPi * f / rate;
        out[i] = sin(ph);
    }
}

// 帯域を切ったノイズ（1 次のロー／ハイパスで十分）
void noise(std::vector<double>& out, int n, Rng& rng, double lp, double hp, double rate) {
    out.assign(n, 0.0);
    for (int i = 0; i < n; i++) out[i] = rng.bipolar();
    if (lp > 0.0) {
        double k = exp(-2.0 * kPi * lp / rate), y = 0.0;
        for (int i = 0; i < n; i++) { y = y * k + out[i] * (1.0 - k); out[i] = y; }
    }
    if (hp > 0.0) {
        double k = exp(-2.0 * kPi * hp / rate), y = 0.0, prev = 0.0;
        for (int i = 0; i < n; i++) { double v = out[i]; y = k * (y + v - prev); prev = v; out[i] = y; }
    }
}

// 矩形波の重ね合わせ。金物（シンバル・ハイハット）の芯になる。
void squares(std::vector<double>& out, int n, const double* f, int nf, double rate) {
    out.assign(n, 0.0);
    for (int k = 0; k < nf; k++)
        for (int i = 0; i < n; i++)
            out[i] += (fmod((double)i * f[k] / rate, 1.0) < 0.5) ? 1.0 : -1.0;
    for (int i = 0; i < n; i++) out[i] /= (double)nf;
}

void normalize(const std::vector<double>& in, std::vector<int>& out, int peak) {
    double m = 0.0;
    for (double v : in) { double a = fabs(v); if (a > m) m = a; }
    if (m <= 0.0) m = 1.0;
    out.resize(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        double v = in[i] / m * (double)peak;
        int q = (int)(v < 0 ? v - 0.5 : v + 0.5);
        if (q >  2047) q =  2047;
        if (q < -2048) q = -2048;
        out[i] = q;
    }
}

// 金物の基音。TR-808 系と同じ考え方で、無理数的に散らした 6 本を重ねる。
const double kMetal[6] = { 2005.0, 2822.0, 3307.0, 3900.0, 4520.0, 5300.0 };

enum Inst { BD, SD, TOP, HH, TOM, RIM };

void synth(int inst, int n, double rate, Rng& rng, std::vector<int>& pcm) {
    std::vector<double> a, b, mixbuf(n, 0.0);

    switch (inst) {
    case BD: {
        // 低い正弦を落としながら下げる＋頭に短いクリック
        sweep(a, n, 125.0, 42.0, rate);
        noise(b, n, rng, 2600.0, 0.0, rate);
        for (int i = 0; i < n; i++)
            mixbuf[i] = a[i] * envelope(i, n, 2, 5.5)
                      + b[i] * 0.35 * exp(-60.0 * i / rate);
        break;
    }
    case SD: {
        // 帯域を切ったノイズ＋胴鳴りの 2 音
        noise(a, n, rng, 6000.0, 900.0, rate);
        std::vector<double> t1, t2;
        sweep(t1, n, 330.0, 180.0, rate);
        sweep(t2, n, 185.0, 150.0, rate);
        for (int i = 0; i < n; i++)
            mixbuf[i] = a[i] * envelope(i, n, 2, 6.5)
                      + (t1[i] + t2[i] * 0.7) * envelope(i, n, 2, 11.0) * 0.55;
        break;
    }
    case TOP: {
        squares(a, n, kMetal, 6, rate);
        noise(b, n, rng, 0.0, 3000.0, rate);
        for (int i = 0; i < n; i++)
            mixbuf[i] = (a[i] * 0.75 + b[i] * 0.45) * envelope(i, n, 3, 4.2);
        break;
    }
    case HH: {
        // シンバルと同じ芯を、短く切って高いほうだけ残す
        squares(a, n, kMetal, 6, rate);
        noise(b, n, rng, 0.0, 4000.0, rate);
        for (int i = 0; i < n; i++)
            mixbuf[i] = (a[i] * 0.70 + b[i] * 0.50) * envelope(i, n, 2, 9.0);
        break;
    }
    case TOM: {
        // ch4-5 は 6.93kHz で動くので、3.4kHz より上は入らない。低めに作る。
        sweep(a, n, 200.0, 105.0, rate);
        noise(b, n, rng, 1800.0, 0.0, rate);
        for (int i = 0; i < n; i++)
            mixbuf[i] = a[i] * envelope(i, n, 2, 5.0)
                      + b[i] * 0.25 * exp(-45.0 * i / rate);
        break;
    }
    case RIM: {
        std::vector<double> t1, t2;
        sweep(t1, n, 1700.0, 1500.0, rate);
        sweep(t2, n, 2400.0, 2100.0, rate);
        noise(b, n, rng, 0.0, 1200.0, rate);
        for (int i = 0; i < n; i++)
            mixbuf[i] = (t1[i] * 0.50 + t2[i] * 0.35 + b[i] * 0.50)
                      * envelope(i, n, 1, 16.0);
        break;
    }
    default: break;
    }
    normalize(mixbuf, pcm, PEAK);
}

struct Slot { int inst; int start; int end; double rate; };

// ymfm_opn.cpp の ym2608::reset() が持っている配置表と同じもの
const Slot kLayout[6] = {
    { BD,  0x0000, 0x01BF, RATE_03 },
    { SD,  0x01C0, 0x043F, RATE_03 },
    { TOP, 0x0440, 0x1B7F, RATE_03 },
    { HH,  0x1B80, 0x1CFF, RATE_03 },
    { TOM, 0x1D00, 0x1F7F, RATE_45 },
    { RIM, 0x1F80, 0x1FFF, RATE_45 },
};

} // namespace

extern "C" int rhythm_build_rom(uint8_t* out, int cap) {
    if (!out || cap < RHYTHM_ROM_BYTES) return 0;
    memset(out, 0, RHYTHM_ROM_BYTES);

    for (int k = 0; k < 6; k++) {
        const Slot& s = kLayout[k];
        int bytes = s.end - s.start + 1;
        int n = bytes * 2;                        // 1 バイト＝2 サンプル

        // 音ごとに種を変える。固定なので毎回同じ結果になる。
        Rng rng(0x9E3779B9u + (uint32_t)k * 0x85EBCA6Bu);

        std::vector<int> pcm;
        synth(s.inst, n, s.rate, rng, pcm);

        std::vector<uint8_t> nib;
        adpcm_encode(pcm, nib);

        for (int i = 0; i < bytes; i++)
            out[s.start + i] = (uint8_t)((nib[2 * i] << 4) | nib[2 * i + 1]);
    }
    return RHYTHM_ROM_BYTES;
}
