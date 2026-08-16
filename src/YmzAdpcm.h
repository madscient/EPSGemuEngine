#pragma once
// YmzAdpcm.h
// YMZ705 (SSGS) / YMZ732 (SSGS2) の ADPCM 再生部。
//
// コーデックはデータシートに記載がないため、同じヤマハの 4bit ADPCM である
// YMZ280B のものと同一と仮定する。ステップ / 差分テーブルと補間の扱いは
// MAME の ymz280b (Aaron Giles, BSD-3-Clause) に倣う。

#include <cstdint>

class ymz_adpcm_device
{
public:
    static constexpr int NUM_CHANNELS = 8;

    // 再生レート。実チップの DAC は 31.25us 周期 (32kHz) で動作する
    static constexpr uint32_t kSampleRate = 32000;

    ymz_adpcm_device();

    void reset();

    // ボイスデータ用 ROM を差し替える。data の寿命は呼び出し元が管理する
    void set_rom(const uint8_t* data, uint32_t size);

    // reg : $40-$B3 (チャンネル n の先頭は $40 + n*0x10)
    void write(uint8_t reg, uint8_t val);

    // 1 サンプル生成する。範囲は [-1.0, 1.0]
    void step(float& out_l, float& out_r);

private:
    struct voice
    {
        uint8_t rate_sel;   // S1,S0
        uint8_t number;     // ボイス番号 (0-63)
        uint8_t level;      // 音量 (0-15)
        uint8_t pan;        // パンポット (0-15)
        uint8_t looping;

        bool playing;

        uint32_t position;    // 現在位置 (ニブル単位)
        uint32_t start;       // 開始位置 (ニブル単位)
        uint32_t stop;        // 終了位置 (ニブル単位)

        int32_t signal;
        int32_t step_size;

        // ニブル間の線形補間状態 (YMZ280B と同じ 1/256 単位)
        int32_t output_step;
        int32_t output_pos;
        int16_t last_sample;
        int16_t curr_sample;

        float gain_l;
        float gain_r;
    };

    uint8_t  rom_byte(uint32_t offset) const;
    uint32_t voice_address(int number, uint32_t table_base) const;
    void     key_on(voice& v);
    void     update_gain(voice& v);
    int16_t  next_nibble(voice& v);

    const uint8_t* m_rom;
    uint32_t       m_rom_size;
    voice          m_voice[NUM_CHANNELS];
};
