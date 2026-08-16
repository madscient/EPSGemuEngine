// YmzSsg.cpp
// YMZ705 / YMZ732 / YMZ771 の SSG 互換部の実装

#include "YmzSsg.h"

// =========================================================
//  ymz_ssg_device
// =========================================================
ymz_ssg_device::ymz_ssg_device(unsigned int clock, uint8_t pan_max)
    : ym2149_device(clock, false)
    , m_pan_max(pan_max)
    , m_pan_center((uint8_t)((pan_max + 1) / 2))
    , m_pan{}
    , m_dc{}
{
}

void ymz_ssg_device::start()
{
    device_start();
    reset();
}

void ymz_ssg_device::reset()
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

void ymz_ssg_device::write_ssg(uint8_t reg, uint8_t val)
{
    if (reg > 0x0D) return;  // YMZ 系は I/O ポートを持たない
    address_w(reg);
    data_w(val);
}

void ymz_ssg_device::set_pan(int chan, uint8_t pan)
{
    m_pan[chan] = pan & m_pan_max;
}

// データシートにパンポット値と L/R レベルの対応の記載がないため、
// 同世代・同ファミリの YMZ280B と同じ分配則を採る。
// 中央値で両側全開、そこから離れるほど反対側だけが線形に絞られ、
// 端の 2 段 (0 と 1) はどちらも片側全振りになる
void ymz_ssg_device::pan_gain(uint8_t pan, float& gain_l, float& gain_r) const
{
    if (pan == m_pan_center) {
        gain_l = 1.0f;
        gain_r = 1.0f;
    } else if (pan < m_pan_center) {
        gain_l = 1.0f;
        gain_r = (pan == 0) ? 0.0f
                            : (float)(pan - 1) / (float)(m_pan_center - 1);
    } else {
        gain_l = (float)(m_pan_max - pan) / (float)(m_pan_max - m_pan_center);
        gain_r = 1.0f;
    }
}

void ymz_ssg_device::step(float& out_l, float& out_r)
{
    short buf[NUM_CHANNELS] = { 0, 0, 0 };
    sound_stream_update(buf, 1);

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        const float v = (float)buf[ch] - m_dc[ch];
        float gl, gr;
        pan_gain(m_pan[ch], gl, gr);
        out_l += v * gl;
        out_r += v * gr;
    }
}

// =========================================================
//  ymz705_device (SSGS / SSGS2)
// =========================================================
ymz705_device::ymz705_device(unsigned int ssg_clock)
    : m_ssg{ ymz_ssg_device(ssg_clock, 15), ymz_ssg_device(ssg_clock, 15) }
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
    if (reg >= 0x40) return;  // ADPCM / シーケンサ領域
    ymz_ssg_device& ssg = m_ssg[(reg >> 5) & 1];
    const uint8_t sub = reg & 0x1F;
    if (sub >= 0x10 && sub <= 0x12)
        ssg.set_pan(sub - 0x10, val);
    else
        ssg.write_ssg(sub, val);
}

void ymz705_device::step(float& out_l, float& out_r)
{
    for (auto& ssg : m_ssg)
        ssg.step(out_l, out_r);
}

// =========================================================
//  ymz771_ssg_device (SSGS3 の SSG 部)
// =========================================================
ymz771_ssg_device::ymz771_ssg_device(unsigned int ssg_clock)
    : m_ssg{ ymz_ssg_device(ssg_clock, 31), ymz_ssg_device(ssg_clock, 31) }
    , m_vlma(0)
{
}

void ymz771_ssg_device::start()
{
    for (auto& ssg : m_ssg)
        ssg.start();
    m_vlma = 0;
}

void ymz771_ssg_device::reset()
{
    for (auto& ssg : m_ssg)
        ssg.reset();
    m_vlma = 0;
}

// 2 系統が交互ではなく機能ごとにまとめて並ぶため、
// レジスタ番号から系統と YM2149 レジスタ番号を組み立て直す
void ymz771_ssg_device::write(uint8_t reg, uint8_t val)
{
    if (reg >= 0x10 && reg <= 0x1B) {          // トーン周期
        const uint8_t idx = reg - 0x10;
        m_ssg[idx / 6].write_ssg(idx % 6, val);
    } else if (reg == 0x1C || reg == 0x1D) {   // ノイズ周期
        m_ssg[reg - 0x1C].write_ssg(ay8910_device::AY_NOISEPER, val);
    } else if (reg == 0x1E || reg == 0x1F) {   // ミキサ
        m_ssg[reg - 0x1E].write_ssg(ay8910_device::AY_ENABLE, val);
    } else if (reg >= 0x20 && reg <= 0x25) {   // 音量
        const uint8_t idx = reg - 0x20;
        m_ssg[idx / 3].write_ssg((uint8_t)(ay8910_device::AY_AVOL + (idx % 3)), val);
    } else if (reg >= 0x26 && reg <= 0x29) {   // エンベロープ周期
        const uint8_t idx = reg - 0x26;
        m_ssg[idx / 2].write_ssg((uint8_t)(ay8910_device::AY_EAFINE + (idx % 2)), val);
    } else if (reg == 0x2A || reg == 0x2B) {   // エンベロープ波形
        m_ssg[reg - 0x2A].write_ssg(ay8910_device::AY_EASHAPE, val);
    } else if (reg >= 0x2C && reg <= 0x31) {   // パンポット
        const uint8_t idx = reg - 0x2C;
        m_ssg[idx / 3].set_pan(idx % 3, val);
    } else if (reg == 0x32) {                  // SSG トータルボリューム
        m_vlma = val;
    }
}

void ymz771_ssg_device::step(float& out_l, float& out_r)
{
    float l = 0.0f, r = 0.0f;
    for (auto& ssg : m_ssg)
        ssg.step(l, r);

    // AMM 側の VLMA と同じく 128 で 100% の線形ボリューム
    const float gain = (float)m_vlma / 128.0f;
    out_l += l * gain;
    out_r += r * gain;
}
