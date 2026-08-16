#pragma once
// YmzSsg.h
// YMZ705 (SSGS) / YMZ732 (SSGS2) / YMZ771 (SSGS3) の SSG 互換部。
// いずれも YM2149 相当の SSG を 2 系統内蔵し、6 チャンネルそれぞれに
// パンポットを持つ。ADPCM / AMM / シーケンサ部はここでは扱わない。
//
// 3 品種の違いはレジスタ配置とパンポットの分解能だけで、
// 音源そのものは共通である。

#include "../extern/ay8910/ay8910.h"

#include <cstdint>

// =========================================================
//  SSG 1 系統 (YM2149 + チャンネルごとのパンポット)
// =========================================================
class ymz_ssg_device : public ym2149_device
{
public:
    // pan_max : パンポットの最大値 (4bit なら 15、5bit なら 31)
    ymz_ssg_device(unsigned int clock, uint8_t pan_max);

    void start();
    void reset();

    // YM2149 と同じ配置のレジスタ ($00-$0D) を書く
    void write_ssg(uint8_t reg, uint8_t val);
    void set_pan(int chan, uint8_t pan);

    // 1 ステップ生成し、パンポットを適用した結果を L/R に加算する
    void step(float& out_l, float& out_r);

private:
    void pan_gain(uint8_t pan, float& gain_l, float& gain_r) const;

    uint8_t m_pan_max;
    uint8_t m_pan_center;
    uint8_t m_pan[NUM_CHANNELS];
    float   m_dc[NUM_CHANNELS];  // 片極性出力の無音時レベル
};

// =========================================================
//  SSG 2 系統を束ねたチップの共通インターフェイス
// =========================================================
class ymz_ssg_chip
{
public:
    static constexpr int NUM_UNITS = 2;

    virtual ~ymz_ssg_chip() = default;

    virtual void start() = 0;
    virtual void reset() = 0;
    virtual void write(uint8_t reg, uint8_t val) = 0;
    virtual void step(float& out_l, float& out_r) = 0;
};

// =========================================================
//  YMZ705 (SSGS) / YMZ732 (SSGS2)
//  $00-$1F を SSG-1、$20-$3F を SSG-2 に割り当てる。
//  各系統は YM2149 と同じ $00-$0D に加え、$10-$12 が 4bit パンポット。
// =========================================================
class ymz705_device : public ymz_ssg_chip
{
public:
    explicit ymz705_device(unsigned int ssg_clock);

    void start() override;
    void reset() override;
    void write(uint8_t reg, uint8_t val) override;
    void step(float& out_l, float& out_r) override;

private:
    ymz_ssg_device m_ssg[NUM_UNITS];
};

// =========================================================
//  YMZ771 (SSGS3) の SSG 部
//  2 系統をひとつの連続したレジスタ空間 ($10-$32) に並べ替えてある。
//  パンポットは 5bit、$32 に 8bit の SSG トータルボリュームを持つ。
// =========================================================
class ymz771_ssg_device : public ymz_ssg_chip
{
public:
    explicit ymz771_ssg_device(unsigned int ssg_clock);

    void start() override;
    void reset() override;
    void write(uint8_t reg, uint8_t val) override;
    void step(float& out_l, float& out_r) override;

private:
    ymz_ssg_device m_ssg[NUM_UNITS];
    uint8_t        m_vlma;  // SSG トータルボリューム (128 で 100%)
};
