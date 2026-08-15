// Ymz705.cpp
// YMZ705 (SSGS) の SSG 互換部の実装

#include "Ymz705.h"

// =========================================================
//  パンポット
//  データシートにパンポット値と L/R レベルの対応が記載されていないため、
//  同世代・同ファミリの YMZ280B と同じ 0=左端 / 8=中央 / 15=右端 の
//  分配則を採る (片側は常に全開、反対側を線形に絞る)
// =========================================================
static void panGain(uint8_t pan, float& gain_l, float& gain_r)
{
    if (pan < 8) {
        gain_l = 1.0f;
        gain_r = (float)pan / 8.0f;
    } else {
        gain_l = (float)(15 - pan) / 7.0f;
        gain_r = 1.0f;
    }
}

// =========================================================
//  ymz705_ssg_device
// =========================================================
ymz705_ssg_device::ymz705_ssg_device(unsigned int clock)
    : ym2149_device(clock, false)
    , m_pan{}
    , m_dc{}
{
}

void ymz705_ssg_device::start()
{
    device_start();
    reset();
}

void ymz705_ssg_device::reset()
{
    device_reset();
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        m_pan[ch] = 0;
        m_dc[ch]  = 0.0f;
    }

    // コアの出力は 0V を負値、電源電圧側を正値で表す片極性信号なので、
    // 全チャンネル無音時のレベルをチャンネルごとに控えておく。
    // パンポットを掛ける前に差し引かないと、直流成分が定位に漏れる
    short buf[NUM_CHANNELS] = { 0, 0, 0 };
    sound_stream_update(buf, 1);
    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        m_dc[ch] = (float)buf[ch];
}

void ymz705_ssg_device::write(uint8_t reg, uint8_t val)
{
    if (reg <= 0x0D) {
        address_w(reg);
        data_w(val);
    } else if (reg >= 0x10 && reg <= 0x12) {
        m_pan[reg - 0x10] = val & 0x0F;
    }
}

void ymz705_ssg_device::step(float& out_l, float& out_r)
{
    short buf[NUM_CHANNELS] = { 0, 0, 0 };
    sound_stream_update(buf, 1);

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        const float v = (float)buf[ch] - m_dc[ch];
        float gl, gr;
        panGain(m_pan[ch], gl, gr);
        out_l += v * gl;
        out_r += v * gr;
    }
}

// =========================================================
//  ymz705_device
// =========================================================
ymz705_device::ymz705_device(unsigned int ssg_clock)
    : m_ssg{ ymz705_ssg_device(ssg_clock), ymz705_ssg_device(ssg_clock) }
{
}

void ymz705_device::start()
{
    for (auto& ssg : m_ssg)
        ssg.start();
}

void ymz705_device::reset()
{
    for (auto& ssg : m_ssg)
        ssg.reset();
}

void ymz705_device::write(uint8_t reg, uint8_t val)
{
    if (reg >= 0x40) return;
    m_ssg[(reg >> 5) & 1].write(reg & 0x1F, val);
}

void ymz705_device::step(float& out_l, float& out_r)
{
    for (auto& ssg : m_ssg)
        ssg.step(out_l, out_r);
}
