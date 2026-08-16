// Ymz770.cpp
// YMZ770 (AMMS) 互換の AMM フレーズ再生部の実装
//
// MAME の src/devices/sound/ymz770.cpp (R. Belmont, MetalliC, BSD-3-Clause)
// の ymz770_device をもとにしている。音量・パン・クリップリミッタの各係数と
// キーオン条件は MAME の実装をそのまま踏襲する。

#include "Ymz770.h"
#include "../extern/mpeg_audio/mpeg_audio.h"

#include <algorithm>
#include <cstring>

ymz770_amm_device::ymz770_amm_device(uint32_t clock, uint32_t divider)
    : m_rom(nullptr)
    , m_rom_size(0)
    , m_forward_write(nullptr)
    , m_forward_context(nullptr)
    , m_clock(clock)
    , m_divider(divider ? divider : 1)
    , m_sclock(clock / (divider ? divider : 1))
    , m_mute(0)
    , m_doen(0)
    , m_vlma(0)
    , m_bsl(0)
    , m_cpl(0)
    , m_channels{}
    , m_sequences{}
{
    reset();
}

ymz770_amm_device::~ymz770_amm_device() = default;

void ymz770_amm_device::set_forward_write(ForwardWriteFn fn, void* context)
{
    m_forward_write   = fn;
    m_forward_context = context;
}

void ymz770_amm_device::set_rom(const uint8_t* data, uint32_t size)
{
    m_rom      = (size != 0) ? data : nullptr;
    m_rom_size = (data != nullptr) ? size : 0;

    // デコーダは ROM 先頭のポインタを保持し続けるため、作り直す
    for (auto& ch : m_channels) {
        ch.decoder.reset();
        if (m_rom)
            ch.decoder = std::make_unique<mpeg_audio>(m_rom, mpeg_audio::AMM, false, 0);
    }
    reset();
}

void ymz770_amm_device::reset()
{
    m_sclock = m_clock / m_divider;
    m_mute = 0;
    m_doen = 0;
    m_vlma = 0;
    m_bsl  = 0;
    m_cpl  = 0;

    for (auto& ch : m_channels) {
        ch.latch.pan     = 64;  // 中央
        ch.latch.volume2 = 0;
        ch.current       = ch.latch;

        ch.phrase = 0;
        ch.volume = 0;
        ch.loop   = 0;

        ch.pending          = false;
        ch.is_playing       = false;
        ch.last_block       = false;
        ch.output_remaining = 0;
        ch.output_ptr       = 0;
        ch.atbl             = 0;
        ch.pptr             = 0;
        if (ch.decoder) ch.decoder->clear();
    }

    for (auto& sq : m_sequences) {
        sq.delay        = 0;
        sq.sequence_no  = 0;
        sq.timer        = 0;
        sq.stopchan     = 0;
        sq.stopchan_ssg = 0;
        sq.loop         = 0;
        sq.offset       = 0;
        sq.is_playing   = false;
    }
}

uint8_t ymz770_amm_device::rom_byte(uint32_t offset) const
{
    if (!m_rom) return 0;
    return m_rom[offset % m_rom_size];
}

uint32_t ymz770_amm_device::phrase_offset(int phrase) const
{
    const uint32_t p = (uint32_t)phrase * 4;
    return (uint32_t)rom_byte(p + 1) << 16 | (uint32_t)rom_byte(p + 2) << 8 | rom_byte(p + 3);
}

uint32_t ymz770_amm_device::sequence_offset(int sqn) const
{
    const uint32_t s = (uint32_t)sqn * 4 + 0x400;
    return (uint32_t)rom_byte(s + 1) << 16 | (uint32_t)rom_byte(s + 2) << 8 | rom_byte(s + 3);
}

// =========================================================
//  シーケンサ
//  ROM 上のシーケンスコードは「レジスタ番号 + データ」の 2 バイト対で、
//  $0F が終端、$0E がウエイトタイマー、それ以外は通常のレジスタ書き込み
// =========================================================
void ymz770_amm_device::sequencer()
{
    for (auto& sq : m_sequences) {
        if (!sq.is_playing) continue;

        if (sq.delay > 0) {
            sq.delay--;
            continue;
        }

        const uint8_t reg  = rom_byte(sq.offset++);
        const uint8_t data = rom_byte(sq.offset++);
        switch (reg) {
        case 0x0F:
            for (int ch = 0; ch < NUM_CHANNELS; ++ch)
                if (sq.stopchan & (1 << ch))
                    m_channels[ch].is_playing = false;
            if (sq.loop)
                sq.offset = sequence_offset(sq.sequence_no);
            else
                sq.is_playing = false;
            break;
        case 0x0E:
            sq.delay = (uint32_t)sq.timer * 32 + 32 - 1;
            break;
        default:
            if (m_forward_write)
                m_forward_write(m_forward_context, reg, data);
            else
                write(reg, data);
            break;
        }
    }
}

// =========================================================
//  レジスタ書き込み
// =========================================================
void ymz770_amm_device::write(uint8_t reg, uint8_t val)
{
    if (reg < 0x40) {
        switch (reg) {
        case 0x00:
            m_mute = val & 1;
            m_doen = (val >> 1) & 1;
            break;
        case 0x01:
            m_vlma = val;
            break;
        case 0x02:
            m_bsl = val & 7;
            m_cpl = (val >> 4) & 7;
            break;
        default:
            break;
        }
        return;
    }

    if (reg < 0x60) {
        channel& ch = m_channels[(reg >> 2) & 0x07];
        switch (reg & 0x03) {
        case 0:
            ch.phrase = val;
            break;
        case 1:
            ch.volume       = 128 << 17;
            ch.latch.volume2 = val;
            break;
        case 2:
            // 0 が左 100%、16 が右 100% になる 5bit 値を 128 スケールへ移す
            ch.latch.pan = (uint8_t)(val << 3);
            break;
        case 3:
            // KON の 2bit がともに 1 なら「再生継続」で、再生中なら鳴らし直さない
            if ((val & 6) == 2 || ((val & 6) == 6 && !ch.is_playing)) {
                ch.pending    = true;
                ch.is_playing = true;
                ch.last_block = false;
            } else if ((val & 6) == 0) {
                ch.is_playing = false;
            }
            ch.loop = (val & 1) ? 255 : 0;
            break;
        }
        return;
    }

    if (reg >= 0x80) {
        sequence& sq = m_sequences[(reg >> 4) & 0x07];
        switch (reg & 0x0F) {
        case 0:  // SQSN
            sq.sequence_no = val;
            break;
        case 1:  // SQON / SQLP
            if ((val & 6) == 2 || ((val & 6) == 6 && !sq.is_playing)) {
                sq.offset     = sequence_offset(sq.sequence_no);
                sq.delay      = 0;
                sq.is_playing = true;
            } else if ((val & 6) == 0 && sq.is_playing) {
                sq.is_playing = false;
                for (int i = 0; i < NUM_CHANNELS; ++i)
                    if (sq.stopchan & (1 << i))
                        m_channels[i].is_playing = false;
            }
            sq.loop = val & 1;
            break;
        case 2:  // TMRH
            sq.timer = (uint16_t)((sq.timer & 0x00FF) | (val << 8));
            break;
        case 3:  // TMRL
            sq.timer = (uint16_t)((sq.timer & 0xFF00) | val);
            break;
        case 6:  // SQOF
            sq.stopchan = val;
            break;
        case 7:  // SQOF_SSG
            sq.stopchan_ssg = val;
            break;
        // TGST / TGEN (トリガ) と TEMPO は動作が判明していないため保持しない
        default:
            break;
        }
    }
}

// =========================================================
//  1 サンプル生成
// =========================================================
void ymz770_amm_device::step(float& out_l, float& out_r)
{
    sequencer();

    int32_t mixl = 0;
    int32_t mixr = 0;

    for (auto& ch : m_channels) {
        // 最終ブロックの後始末・ループと次ブロックのデコードは互いに戻り合う。
        // 2 回目のデコード失敗で is_playing が落ち、次の周回で必ず抜ける
        while (ch.output_remaining == 0 && ch.is_playing && ch.decoder) {
            if (ch.last_block) {
                if (ch.loop) {
                    if (ch.loop != 255) --ch.loop;
                    ch.pending = true;
                } else {
                    ch.is_playing       = false;
                    ch.output_remaining = 0;
                    ch.decoder->clear();
                }
            }
            if (!ch.is_playing) break;

            if (ch.pending) {
                const int phrase = ch.phrase;
                ch.atbl = (rom_byte((uint32_t)phrase * 4) >> 4) & 7;
                ch.pptr = 8 * (int)phrase_offset(phrase);

                // フレーズの切り替わりは通常シームレスだが、音量が変わる
                // 場合は AMM バッファの残りが目立たないよう捨てる
                if (!ch.last_block && ch.latch.volume2 != ch.current.volume2)
                    ch.decoder->clear();

                ch.pending = false;
            }

            int sample_rate   = (int)m_sclock;
            int channel_count = 0;
            const bool ok = ch.decoder->decode_buffer(
                ch.pptr, (int)(m_rom_size * 8), ch.output_data,
                ch.output_remaining, sample_rate, channel_count, ch.atbl);

            if (!ok || ch.output_remaining == 0) {
                ch.is_playing       = !ch.last_block;  // 無限リトライの検出
                ch.last_block       = true;
                ch.output_remaining = 0;
                continue;
            }

            // 分周比の選び方が判明していないため、AMM のヘッダが示す
            // サンプルレートをそのまま採用する
            if (sample_rate > 0) m_sclock = (uint32_t)sample_rate;

            ch.last_block = ch.output_remaining < 1152;
            ch.output_ptr = 0;
            ch.current    = ch.latch;
            break;
        }

        if (ch.output_remaining > 0) {
            int32_t smpl = ch.output_data[ch.output_ptr++];

            // 音量は 0-128 (128 で 100%) の線形
            smpl = (smpl * (ch.volume >> 17)) >> 7;
            smpl = (smpl * ch.current.volume2) >> 7;

            // パンも線形で、0 が左 100%、128 が右 100%、64 で左右半々
            mixr += (smpl * ch.current.pan) >> 7;
            mixl += (smpl * (128 - ch.current.pan)) >> 7;

            ch.output_remaining--;
            if (ch.output_remaining == 0 && !ch.is_playing)
                ch.decoder->clear();
        }
    }

    if (m_mute) {
        mixl = mixr = 0;
    } else {
        // トータルボリュームは 0-255 の線形で 128 が 100%
        mixl = (mixl * m_vlma) >> (7 - m_bsl);
        mixr = (mixr * m_vlma) >> (7 - m_bsl);

        // クリップリミッタ 0:off 1:6.02dB(100%) 2:4.86dB(87.5%) 3:3.52dB(75%)
        constexpr int32_t kClipMax3 = 32768 * 75 / 100;
        constexpr int32_t kClipMax2 = 32768 * 875 / 1000;
        switch (m_cpl) {
        case 3: mixl = std::clamp(mixl, -kClipMax3, kClipMax3);
                mixr = std::clamp(mixr, -kClipMax3, kClipMax3); break;
        case 2: mixl = std::clamp(mixl, -kClipMax2, kClipMax2);
                mixr = std::clamp(mixr, -kClipMax2, kClipMax2); break;
        case 1: mixl = std::clamp(mixl, -32768, 32767);
                mixr = std::clamp(mixr, -32768, 32767); break;
        default: break;
        }
    }

    out_l = (float)mixl / 32768.0f;
    out_r = (float)mixr / 32768.0f;
}
