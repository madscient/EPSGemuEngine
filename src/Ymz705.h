#pragma once
// Ymz705.h
// YMZ705 (SSGS) の SSG 互換部。
// YM2149 相当の SSG を 2 系統内蔵し、6 チャンネルそれぞれに
// 4bit のパンポットを持つ。ADPCM / シーケンサ部は対象外。

#include "../extern/ay8910/ay8910.h"

#include <cstdint>

// =========================================================
//  SSG 1 系統 (YM2149 + チャンネルごとのパンポット)
// =========================================================
class ymz705_ssg_device : public ym2149_device
{
public:
    explicit ymz705_ssg_device(unsigned int clock);

    void start();
    void reset();

    // reg : $00-$0D は YM2149 と同一、$10-$12 が CH A-C のパンポット。
    //       それ以外は無視する (YMZ705 は I/O ポートを持たない)。
    void write(uint8_t reg, uint8_t val);

    // 1 ステップ生成し、パンポットを適用した結果を L/R に加算する
    void step(float& out_l, float& out_r);

private:
    uint8_t m_pan[NUM_CHANNELS];
    float   m_dc[NUM_CHANNELS];  // 片極性出力の無音時レベル
};

// =========================================================
//  YMZ705 SSG 部全体 (SSG-1 / SSG-2)
// =========================================================
class ymz705_device
{
public:
    static constexpr int NUM_UNITS = 2;

    // ssg_clock : SSG ブロックの動作クロック (実チップでは 2.048MHz 固定)
    explicit ymz705_device(unsigned int ssg_clock);

    void start();
    void reset();

    // reg : $00-$1F を SSG-1、$20-$3F を SSG-2 に振り分ける。
    //       $40 以降は ADPCM / シーケンサ領域のため無視する。
    void write(uint8_t reg, uint8_t val);

    void step(float& out_l, float& out_r);

private:
    ymz705_ssg_device m_ssg[NUM_UNITS];
};
