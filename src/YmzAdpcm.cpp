// YmzAdpcm.cpp
// YMZ705 / YMZ732 の ADPCM 再生部の実装

#include "YmzAdpcm.h"
#include "MemMap.h"

#include <algorithm>

// =========================================================
//  4bit ADPCM のテーブル (YMZ280B と同一と仮定)
// =========================================================
static constexpr int kIndexScale[8] = { 0x0E6, 0x0E6, 0x0E6, 0x0E6,
                                        0x133, 0x199, 0x200, 0x266 };

static int kDiffLookup[16];

static bool buildTables() {
    for (int nib = 0; nib < 16; ++nib) {
        const int value = (nib & 0x07) * 2 + 1;
        kDiffLookup[nib] = (nib & 0x08) ? -value : value;
    }
    return true;
}

// =========================================================
//  サンプリング周波数選択 S1,S0
//  データシートは 32k / 16k / 8k / 4k の 4 種としか書いていないため、
//  昇順に割り当てる
// =========================================================
static constexpr uint32_t kRateTable[4] = { 4000, 8000, 16000, 32000 };

// ニブル間の補間位置は 1/256 単位で進める
static constexpr int32_t kFracBits = 8;
static constexpr int32_t kFracOne  = 1 << kFracBits;

// =========================================================
//  ボイスデータのアドレステーブル
//  L / M / H が 64 バイトずつ並ぶ
// =========================================================
static constexpr uint32_t kStartTable = 0x000;
static constexpr uint32_t kEndTable   = 0x0C0;

ymz_adpcm_device::ymz_adpcm_device()
    : m_mem(nullptr)
    , m_voice{}
{
    static const bool built = buildTables();
    (void)built;
    reset();
}

void ymz_adpcm_device::set_memory(const MemMap* mem)
{
    m_mem = mem;
    reset();
}

void ymz_adpcm_device::reset()
{
    for (auto& v : m_voice) {
        v.rate_sel    = 0;
        v.number      = 0;
        v.level       = 0;
        v.pan         = 0;
        v.looping     = 0;
        v.playing     = false;
        v.position    = 0;
        v.start       = 0;
        v.stop        = 0;
        v.signal      = 0;
        v.step_size   = 0x7F;
        v.output_step = kFracOne;
        v.output_pos  = kFracOne;
        v.last_sample = 0;
        v.curr_sample = 0;
        update_gain(v);
    }
}

uint8_t ymz_adpcm_device::rom_byte(uint32_t offset) const
{
    return m_mem ? m_mem->read(offset) : 0;
}

// テーブルは L / M / H の順に 64 バイトずつ並ぶ
uint32_t ymz_adpcm_device::voice_address(int number, uint32_t table_base) const
{
    const uint32_t n = (uint32_t)number & 0x3F;
    return (uint32_t)rom_byte(table_base + n)
         | ((uint32_t)rom_byte(table_base + 0x40 + n) << 8)
         | ((uint32_t)rom_byte(table_base + 0x80 + n) << 16);
}

// パンポットは SSG 部と同じ YMZ280B の分配則
void ymz_adpcm_device::update_gain(voice& v)
{
    const float level = (float)v.level / 15.0f;
    if (v.pan == 8) {
        v.gain_l = level;
        v.gain_r = level;
    } else if (v.pan < 8) {
        v.gain_l = level;
        v.gain_r = (v.pan == 0) ? 0.0f : level * (float)(v.pan - 1) / 7.0f;
    } else {
        v.gain_l = level * (float)(15 - v.pan) / 7.0f;
        v.gain_r = level;
    }
}

void ymz_adpcm_device::key_on(voice& v)
{
    const uint32_t start = voice_address(v.number, kStartTable);
    const uint32_t end   = voice_address(v.number, kEndTable);

    v.start    = start * 2;          // ニブル単位
    v.stop     = (end + 1) * 2;      // 終了アドレスは最後のバイトを指す
    v.position = v.start;

    v.signal      = 0;
    v.step_size   = 0x7F;
    v.last_sample = 0;
    v.curr_sample = 0;
    v.output_pos  = kFracOne;
    v.playing     = (v.stop > v.start) && m_mem && !m_mem->empty();
}

// 1 ニブル分デコードして次のサンプルを返す
int16_t ymz_adpcm_device::next_nibble(voice& v)
{
    const uint8_t byte = rom_byte(v.position / 2);
    const int     val  = (byte >> ((~v.position & 1) << 2)) & 0x0F;

    v.signal += (v.step_size * kDiffLookup[val]) / 8;
    v.signal  = std::clamp(v.signal, -32768, 32767);

    v.step_size = (v.step_size * kIndexScale[val & 7]) >> 8;
    v.step_size = std::clamp(v.step_size, 0x7F, 0x6000);

    v.position++;
    if (v.position >= v.stop) {
        if (v.looping) {
            v.position  = v.start;
            v.signal    = 0;
            v.step_size = 0x7F;
        } else {
            v.playing = false;
        }
    }
    return (int16_t)v.signal;
}

void ymz_adpcm_device::write(uint8_t reg, uint8_t val)
{
    if (reg < 0x40 || reg > 0xB3) return;
    const int idx = (reg >> 4) - 4;
    if (idx >= NUM_CHANNELS) return;
    const uint8_t sub = reg & 0x0F;
    if (sub > 3) return;

    voice& v = m_voice[idx];
    switch (sub) {
    case 0:
        v.rate_sel    = (val >> 6) & 3;
        v.number      = val & 0x3F;
        v.output_step = (int32_t)(kFracOne * kRateTable[v.rate_sel] / kSampleRate);
        break;
    case 1:
        v.level = val & 0x0F;
        update_gain(v);
        break;
    case 2:
        v.pan = val & 0x0F;
        update_gain(v);
        break;
    case 3:
        v.looping = val & 1;
        if (val & 2)
            key_on(v);
        else
            v.playing = false;
        break;
    }
}

void ymz_adpcm_device::step(float& out_l, float& out_r)
{
    float acc_l = 0.0f;
    float acc_r = 0.0f;

    for (auto& v : m_voice) {
        if (!v.playing && v.curr_sample == 0 && v.last_sample == 0) {
            v.output_pos = kFracOne;
            continue;
        }

        // 発音周波数が DAC より低い分はニブル間を線形補間して埋める
        while (v.output_pos >= kFracOne) {
            v.output_pos -= kFracOne;
            v.last_sample = v.curr_sample;
            v.curr_sample = v.playing ? next_nibble(v) : 0;
        }

        const float t = (float)v.output_pos / (float)kFracOne;
        const float s = (float)v.last_sample * (1.0f - t) + (float)v.curr_sample * t;
        v.output_pos += v.output_step;

        acc_l += s * v.gain_l;
        acc_r += s * v.gain_r;
    }

    out_l = acc_l / 32768.0f;
    out_r = acc_r / 32768.0f;
}
