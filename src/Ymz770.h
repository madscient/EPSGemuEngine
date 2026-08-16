#pragma once
// Ymz770.h
// YMZ770 (AMMS) 互換の AMM フレーズ再生部。
// YMZ771 (SSGS3) のフレーズ再生・シーケンサ部は YMZ770 と互換であり、
// 同じ実装を使う。
//
// MAME の src/devices/sound/ymz770.cpp (R. Belmont, MetalliC, BSD-3-Clause)
// の ymz770_device をもとに、MAME デバイスフレームワーク依存を除いたもの。
// AMM のデコードは extern/mpeg_audio (無改変) が行う。

#include <cstdint>
#include <memory>

class mpeg_audio;

class ymz770_amm_device
{
public:
    static constexpr int NUM_CHANNELS  = 8;
    static constexpr int NUM_SEQUENCES = 8;

    // clock   : マスタークロック Hz
    // divider : clock からフレーズ再生レートを得る分周比
    ymz770_amm_device(uint32_t clock, uint32_t divider);
    ~ymz770_amm_device();

    void reset();

    // フレーズデータ用 ROM を差し替える。data の寿命は呼び出し元が管理する。
    // デコーダが ROM 先頭のポインタを保持するため、差し替え時に作り直す
    void set_rom(const uint8_t* data, uint32_t size);

    void write(uint8_t reg, uint8_t val);

    // シーケンスコードは全レジスタ空間を書けるため、自分の担当外
    // (YMZ771 なら SSG 部) への書き込みをここへ転送する。
    // 未設定なら担当外のレジスタは捨てられる
    using ForwardWriteFn = void (*)(void* context, uint8_t reg, uint8_t val);
    void set_forward_write(ForwardWriteFn fn, void* context);

    // 1 サンプル生成する。範囲は [-1.0, 1.0]
    void step(float& out_l, float& out_r);

    // 現在の再生レート。AMM のフレームヘッダにより再生中に変わり得る
    uint32_t sample_rate() const { return m_sclock; }

private:
    uint8_t  rom_byte(uint32_t offset) const;
    uint32_t phrase_offset(int phrase) const;
    uint32_t sequence_offset(int sqn) const;
    void     sequencer();

    struct channel_params
    {
        uint8_t pan;
        uint8_t volume2;
    };

    struct channel
    {
        channel_params current;
        channel_params latch;

        uint16_t phrase;
        int32_t  volume;
        uint8_t  loop;

        bool pending;
        bool is_playing;
        bool last_block;

        std::unique_ptr<mpeg_audio> decoder;

        int16_t output_data[0x1000];
        int     output_remaining;
        int     output_ptr;
        int     atbl;
        int     pptr;
    };

    struct sequence
    {
        uint32_t delay;
        uint16_t sequence_no;
        uint16_t timer;
        uint16_t stopchan;
        uint8_t  stopchan_ssg;
        uint8_t  loop;
        uint32_t offset;
        bool     is_playing;
    };

    const uint8_t* m_rom;
    uint32_t       m_rom_size;

    ForwardWriteFn m_forward_write;
    void*          m_forward_context;

    uint32_t m_clock;
    uint32_t m_divider;
    uint32_t m_sclock;

    uint8_t m_mute;   // チップ全体のミュート
    uint8_t m_doen;   // デジタル出力イネーブル
    uint8_t m_vlma;   // AMM トータルボリューム (128 で 100%)
    uint8_t m_bsl;    // ブーストレベル
    uint8_t m_cpl;    // クリップリミッタ

    channel  m_channels[NUM_CHANNELS];
    sequence m_sequences[NUM_SEQUENCES];
};
